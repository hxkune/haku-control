// SPDX-License-Identifier: GPL-3.0-only
// Lian Li wireless fans (UNI FAN SL/TL wireless) through their RF dongle pair: transmitter 0416:8040, receiver 0416:8041,
// both on the inbox WinUSB driver. Once a second the receiver is asked which groups it hears, and answers with one
// 42-byte record per device (address, channel, fan count, rpm and duty per fan). Only while some group has a speed set
// (fans/ll_<mac>, 30..100 %) is the transmitter opened: then each second it carries the clock broadcast the fans take as
// a host being there, and each such group's duty. With none set it stays closed and the fans run on their own, as
// before. The frame layouts come from MarcusSanchez/FanControl.LianLiWireless, Yoinky3000/LL-Connect-Wireless and
// sgtaziz/lian-li-linux (MIT, see THIRD_PARTY_NOTICES.md). Windows gives each dongle to one program at a time: while
// the L-Connect service holds one, the page says so.
#include "common.h"
#include <setupapi.h>
#include <winusb.h>
#include <process.h>

#define LL_GROUPS 16
#define REC 42
static const GUID ll_guid = { 0x1D4B2365, 0x4749, 0x48EA, { 0xB3, 0x8A, 0x7C, 0x6F, 0xDD, 0xDD, 0x7E, 0x26 } };

typedef struct { BYTE mac[6], master[6]; int ch, rx, type, n; int rpm[4], duty[4]; } ll_dev;
// the receiver's reply: header 10 <count> .. .., then records; marker 0x1C at 41, the dongle's own record has type 0xFF
// returns how many devices went into d (masters into *master), -1 when it is not a discovery reply or ends short
static int ll_parse(const BYTE *r, int len, int pages, ll_dev *d, int cap, BYTE *master, int *reported) {
    static const BYTE zero[6];
    if (len < 4 || r[0] != 0x10) return -1;
    int recs = r[1] < pages * 10 ? r[1] : pages * 10, n = 0;
    *reported = r[1];
    if (len < 4 + recs * REC) return -1;
    for (int i = 0; i < recs; i++) {
        const BYTE *p = r + 4 + i * REC;
        if (p[41] != 0x1C || !memcmp(p, zero, 6)) continue;
        if (p[18] == 0xFF) { memcpy(master, p, 6); continue; }
        if (n >= cap) continue;
        ll_dev *v = &d[n++];
        memcpy(v->mac, p, 6); memcpy(v->master, p + 6, 6);
        v->ch = p[12]; v->rx = p[13]; v->type = p[18];
        v->n = p[19] >= 10 ? p[19] - 10 : p[19]; if (v->n > 4) v->n = 4;   // 10 and up: chained from the right
        for (int f = 0; f < 4; f++) { v->rpm[f] = (p[28 + f * 2] & 0x0F) << 8 | p[29 + f * 2]; v->duty[f] = p[36 + f]; }
    }
    return n;
}

// a radio payload (240 bytes) goes to the transmitter as 4 frames: 10 <chunk> <channel> <receiver type> + 60 bytes
static void ll_frames(BYTE f[4][64], int ch, int rx, const BYTE *pl) {
    for (int i = 0; i < 4; i++) { f[i][0] = 0x10; f[i][1] = (BYTE)i; f[i][2] = (BYTE)ch; f[i][3] = (BYTE)rx; memcpy(f[i] + 4, pl + i * 60, 60); }
}
// a group's duty: 12 10 <group> <dongle> <receiver type> <channel> <slot: 1-based among the groups bound to the dongle,
// in reply order> <duty per fan, 0..255; slots past the fan count 0>
static void ll_speed(BYTE *pl, const ll_dev *v, const BYTE *master, int slot, int duty) {
    memset(pl, 0, 240); pl[0] = 0x12; pl[1] = 0x10; memcpy(pl + 2, v->mac, 6); memcpy(pl + 8, master, 6);
    pl[14] = (BYTE)v->rx; pl[15] = (BYTE)v->ch; pl[16] = (BYTE)slot;
    for (int f = 0; f < v->n; f++) pl[17 + f] = (BYTE)duty;
}
// the clock broadcast, once a second: without it the fans take the host as gone and run on their own. 12 14, the dongle
// at 8, then a 220-byte block (sensor readouts for fans with screens, left 0; the date at 32..38). The first one after
// opening marks the head unset (0x14) and carries only the block's tail
static void ll_beat(BYTE *pl, const BYTE *master, const SYSTEMTIME *t, int first) {
    BYTE b[220] = { 0 };
    b[32] = (BYTE)(t->wYear >> 8); b[33] = (BYTE)t->wYear; b[34] = (BYTE)t->wMonth; b[35] = (BYTE)t->wDay;
    b[36] = (BYTE)t->wHour; b[37] = (BYTE)t->wMinute; b[38] = (BYTE)t->wSecond;
    memset(pl, 0, 240); pl[0] = 0x12; pl[1] = 0x14; memcpy(pl + 8, master, 6);
    if (first) { memset(pl + 14, 0x14, 50); memcpy(pl + 64, b + 50, 170); } else memcpy(pl + 14, b, 220);
}

#ifndef LL_TEST
static CRITICAL_SECTION cs;
static HANDLE th, stop_ev;
// st: none (no dongle), busy (another program holds it), err (it does not answer), ok; tx: the transmitter, the same
// or off (closed: no speed set)
static struct { const char *st, *tx; int err, txe, reported, n; BYTE master[6]; ll_dev d[LL_GROUPS]; } S = { "none", "off" };

static int dev_path(const wchar_t *pid, wchar_t *out, int cap) {
    HDEVINFO set = SetupDiGetClassDevsW(&ll_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    SP_DEVICE_INTERFACE_DATA id = { sizeof(id) };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &ll_guid, i, &id); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *dd = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        dd->cbSize = sizeof(*dd);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &id, dd, sizeof(buf), NULL, NULL)) continue;
        wchar_t lo[512]; wcsncpy_s(lo, 512, dd->DevicePath, _TRUNCATE); _wcslwr_s(lo, 512);
        if (wcsstr(lo, pid)) { wcsncpy_s(out, cap, dd->DevicePath, _TRUNCATE); found = 1; }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void timeout_(WINUSB_INTERFACE_HANDLE w, UCHAR pipe, ULONG ms) { WinUsb_SetPipePolicy(w, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(ms), &ms); }
// one read of what is waiting, 0 when nothing came in time, -1 on a failed transfer
static int rd(WINUSB_INTERFACE_HANDLE w, BYTE *b, int n, ULONG ms) {
    ULONG got = 0; timeout_(w, 0x81, ms);
    if (WinUsb_ReadPipe(w, 0x81, b, n, &got, NULL)) return (int)got;
    return GetLastError() == ERROR_SEM_TIMEOUT ? 0 : -1;
}

// the transmitter, open only while some group has a speed set
static struct { HANDLE f; WINUSB_INTERFACE_HANDLE w; BYTE master[6]; int ch, beat; DWORD tried; } T = { INVALID_HANDLE_VALUE };
static struct { BYTE pl[240]; int ch, rx; } sent[LL_GROUPS];   // what went out last tick, for the full-speed send when the receiver is lost
static int nsent;

static void tx_state(const char *st, int e) {
    EnterCriticalSection(&cs);
    if (strcmp(S.tx, st) || S.txe != e) logf_("lianli: transmitter %s (%d)", st, e);
    S.tx = st; S.txe = e;
    LeaveCriticalSection(&cs);
}
static void tx_close(const char *st, int e) {
    if (T.w) WinUsb_Free(T.w);
    if (T.f != INVALID_HANDLE_VALUE) CloseHandle(T.f);
    T.w = NULL; T.f = INVALID_HANDLE_VALUE; nsent = 0;
    tx_state(st, e);
}
static int tx_send(int ch, int rx, const BYTE *pl) {
    BYTE f[4][64]; ll_frames(f, ch, rx, pl);
    timeout_(T.w, 0x01, 1000);
    for (int i = 0; i < 4; i++) {
        ULONG put = 0;
        if (!WinUsb_WritePipe(T.w, 0x01, f[i], 64, &put, NULL) || put != 64) { tx_close("err", GetLastError()); return 0; }
        Sleep(1);
    }
    return 1;
}
// open the transmitter and ask it which dongle it is: 11 <channel> -> 11 <dongle> <status, above 1 when it is on that
// channel> <firmware>. A failed open is tried again after 10 s
static int tx_open(int ch) {
    if (T.tried && GetTickCount() - T.tried < 10000) return 0;
    T.tried = GetTickCount() | 1;
    wchar_t path[512];
    if (!dev_path(L"vid_0416&pid_8040", path, 512)) { tx_state("none", 0); return 0; }
    T.f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (T.f == INVALID_HANDLE_VALUE || !WinUsb_Initialize(T.f, &T.w)) {
        int e = GetLastError(); T.w = NULL;
        tx_close(e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? "busy" : "err", e);
        return 0;
    }
    UCHAR on = 1; WinUsb_SetPipePolicy(T.w, 0x81, AUTO_CLEAR_STALL, 1, &on); WinUsb_SetPipePolicy(T.w, 0x81, ALLOW_PARTIAL_READS, 1, &on);
    BYTE q[64] = { 0x11, (BYTE)ch }, r[64];
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 16 && rd(T.w, r, 64, 5) > 0; i++) {}
        ULONG put = 0; timeout_(T.w, 0x01, 1000);
        if (!WinUsb_WritePipe(T.w, 0x01, q, 64, &put, NULL) || put != 64) { tx_close("err", GetLastError()); return 0; }
        int got = rd(T.w, r, 64, 500);
        if (got >= 13 && r[0] == 0x11 && memcmp(r + 1, (BYTE[6]){ 0 }, 6) && (r[7] | r[8] | r[9] || r[10] > 1)) {
            memcpy(T.master, r + 1, 6); T.ch = ch; T.beat = 0;
            logf_("lianli: transmitter open, dongle %02x:%02x:%02x:%02x:%02x:%02x on channel %d, firmware %d.%d", r[1], r[2], r[3], r[4], r[5], r[6], ch, r[11], r[12]);
            tx_state("ok", 0);
            return 1;
        }
    }
    tx_close("err", 0);
    return 0;
}

static int ll_want(const ll_dev *v) {
    char k[20]; const BYTE *m = v->mac;
    snprintf(k, sizeof(k), "ll_%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    int p = v->type || !v->n ? 0 : cfg_geti("fans", k, 0);   // fan groups only, not coolers or light strips
    return p <= 0 ? 0 : p < 30 ? 30 : p > 100 ? 100 : p;     // shortcut: one floor of 30 % for every fan model, above all of their own; per-model floors if one stalls
}

// after each poll: nothing set closes the transmitter (the fans go back to their own speed); otherwise the clock
// broadcast and each set group's duty, every second, as the firmware wants them repeated
static void drive(const ll_dev *d, int n) {
    int pct[LL_GROUPS], any = 0;
    for (int i = 0; i < n; i++) any |= (pct[i] = ll_want(&d[i])) > 0;
    if (!any) { if (T.w) tx_close("off", 0); T.tried = 0; return; }
    int ch = 8;   // shortcut: the first set group's channel for the dongle's own; every receiver seen so far reports one channel
    for (int i = 0; i < n; i++) if (pct[i]) { ch = d[i].ch; break; }
    if (!T.w && !tx_open(ch)) return;
    SYSTEMTIME t; GetLocalTime(&t);
    BYTE pl[240]; ll_beat(pl, T.master, &t, !T.beat);
    if (!tx_send(T.ch, 0xFF, pl)) return;
    T.beat = 1;
    int k = 0;
    for (int i = 0, slot = 0; i < n; i++) {
        if (memcmp(d[i].master, T.master, 6)) continue;
        slot++;
        if (!pct[i] || k >= LL_GROUPS) continue;
        ll_speed(pl, &d[i], T.master, slot, (pct[i] * 255 + 50) / 100);
        if (k >= nsent || memcmp(sent[k].pl, pl, 240)) logf_("lianli: %02x:%02x:%02x:%02x:%02x:%02x set to %d %% (slot %d)", d[i].mac[0], d[i].mac[1], d[i].mac[2], d[i].mac[3], d[i].mac[4], d[i].mac[5], pct[i], slot);
        if (!tx_send(d[i].ch, d[i].rx, pl)) return;
        memcpy(sent[k].pl, pl, 240); sent[k].ch = d[i].ch; sent[k].rx = d[i].rx; k++;
    }
    nsent = k;
}

// the receiver is lost: what was set can no longer be checked, so the set groups go to full speed, then the transmitter
// closes and the fans run on their own
static void drive_lost(void) {
    if (!T.w) return;
    int k = nsent;
    for (int i = 0; i < k; i++) {
        for (int f = 17; f < 21; f++) if (sent[i].pl[f]) sent[i].pl[f] = 255;
        if (!tx_send(sent[i].ch, sent[i].rx, sent[i].pl)) break;
    }
    logf_("lianli: receiver lost, %d set group(s) sent to full speed", k);
    if (T.w) tx_close("off", 0);
}

// the first replies of a run go to the log as they came, for receivers whose firmware answers otherwise, and then
// (up to 40 more) each reply where a fan's rpm turns 0 or comes back
static int dumps = 6, changes = 40, fail_e;
static unsigned long long zero_was;
static const char *fail_at = "";
static void dump(const BYTE *r, int len, int pages, int n) {
    static char h[(4 + 10 * REC) * 3 + 1]; int k = len < 4 + 10 * REC ? len : 4 + 10 * REC;
    for (int i = 0; i < k; i++) sprintf(h + i * 3, "%02x ", r[i]);
    logf_("lianli: asked %d page(s), %d bytes back, %d device(s) read: %s", pages, len, n, h);
}

// one poll: drop what is stale, ask, gather the reply until it pauses; the device count read decides the next page count
static int poll(WINUSB_INTERFACE_HANDLE w, int *pages) {
    static BYTE r[26 * 512]; BYTE q[64] = { 0x10, (BYTE)*pages }, junk[512];
    int stale = 0;
    for (int i = 0, g; i < 64 && (g = rd(w, junk, sizeof(junk), 5)) > 0; i++) stale += g;
    ULONG put = 0; timeout_(w, 0x01, 1000);
    if (!WinUsb_WritePipe(w, 0x01, q, sizeof(q), &put, NULL) || put != sizeof(q)) { fail_e = GetLastError(); fail_at = "writing the request"; return -1; }
    int len = 0, cap = *pages * 512, got;
    for (ULONG ms = 100; len < cap && (got = rd(w, r + len, cap - len < 64 ? cap - len : 64, ms)) > 0; ms = 10) len += got;
    if (got < 0) { fail_e = GetLastError(); fail_at = "reading the reply"; return -1; }
    ll_dev d[LL_GROUPS]; BYTE m[6] = { 0 }; int rep = 0, n = ll_parse(r, len, *pages, d, LL_GROUPS, m, &rep);
    unsigned long long z = 0;   // one bit per fan reading 0 rpm
    for (int i = 0; i < n; i++) for (int f = 0; f < d[i].n; f++) if (!d[i].rpm[f]) z |= 1ull << (i * 4 + f);
    if (dumps > 0 || (n >= 0 && z != zero_was && changes > 0)) {
        if (dumps > 0) dumps--; else changes--;
        if (stale) logf_("lianli: %d stale bytes dropped first", stale);
        dump(r, len, *pages, n);
    }
    if (n >= 0) zero_was = z;
    if (n < 0) return 0;
    *pages = rep > 10 ? (rep + 9) / 10 > 26 ? 26 : (rep + 9) / 10 : 1;
    EnterCriticalSection(&cs);
    if (strcmp(S.st, "ok") || S.n != n) logf_("lianli: %d group(s) heard, %d record(s) reported", n, rep);
    S.st = "ok"; S.err = 0; S.reported = rep; S.n = n; memcpy(S.master, m, 6); memcpy(S.d, d, sizeof(ll_dev) * n);
    LeaveCriticalSection(&cs);
    drive(d, n);
    return 1;
}

static void set_state(const char *st, int err, const char *at) {
    EnterCriticalSection(&cs);
    if (strcmp(S.st, st) || S.err != err) logf_("lianli: %s (%d)%s%s", st, err, *at ? " " : "", at);
    S.st = st; S.err = err; S.n = 0;
    LeaveCriticalSection(&cs);
}

static unsigned __stdcall ll_thread(void *arg) {
    (void)arg;
    do {
        wchar_t path[512];
        if (!dev_path(L"vid_0416&pid_8041", path, 512)) { set_state("none", 0, ""); continue; }
        HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
        WINUSB_INTERFACE_HANDLE w = NULL;
        if (f == INVALID_HANDLE_VALUE || !WinUsb_Initialize(f, &w)) {
            int e = GetLastError(); const char *at = f == INVALID_HANDLE_VALUE ? "opening the device" : "starting WinUSB";
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            set_state(e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? "busy" : "err", e, at);
            continue;
        }
        UCHAR on = 1; WinUsb_SetPipePolicy(w, 0x81, AUTO_CLEAR_STALL, 1, &on); WinUsb_SetPipePolicy(w, 0x81, ALLOW_PARTIAL_READS, 1, &on);
        USB_INTERFACE_DESCRIPTOR id = { 0 }; WinUsb_QueryInterfaceSettings(w, 0, &id);
        char pp[160] = ""; int pn = 0;
        for (UCHAR i = 0; i < id.bNumEndpoints && i < 8; i++) {
            WINUSB_PIPE_INFORMATION pi;
            if (WinUsb_QueryPipe(w, 0, i, &pi)) pn += snprintf(pp + pn, sizeof(pp) - pn, " %02x:%s/%d", pi.PipeId, pi.PipeType == UsbdPipeTypeBulk ? "bulk" : pi.PipeType == UsbdPipeTypeInterrupt ? "int" : "other", pi.MaximumPacketSize);
        }
        logf_("lianli: receiver open, pipes%s", pn ? pp : " none");
        int pages = 1, silent = 0, r;
        do {
            r = poll(w, &pages);
            if (r > 0) silent = 0;
            else if (r < 0 || ++silent >= 5) { set_state("err", r < 0 ? fail_e : 0, r < 0 ? fail_at : "no reply read in 5 s"); drive_lost(); break; }   // gone, or no answer for 5 s: open again
        } while (WaitForSingleObject(stop_ev, 1000) == WAIT_TIMEOUT);
        WinUsb_Free(w); CloseHandle(f);
        if (T.w) tx_close("off", 0);   // stopping: the fans keep their duty until they miss the clock, then run on their own
    } while (WaitForSingleObject(stop_ev, 5000) == WAIT_TIMEOUT);
    return 0;
}

void ll_start(void) {
    InitializeCriticalSection(&cs);
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, ll_thread, NULL, 0, NULL);
}

void ll_stop(void) {
    if (!th) return;
    SetEvent(stop_ev); WaitForSingleObject(th, 3000); CloseHandle(th); th = NULL;
}

static int mac_s(char *o, const BYTE *m) { return sprintf(o, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }
// {"st":..,"err":..,"rep":..,"g":[{"mac","own":bound to this dongle (or the reply named no dongle: TL receivers leave it out),"ch","type","n","rpm":[],"pct":[]}]}
int ll_json(char *out, int cap) {
    if (!th) return snprintf(out, cap, "{\"st\":\"none\"}");
    EnterCriticalSection(&cs);
    int n = snprintf(out, cap, "{\"st\":\"%s\",\"err\":%d,\"tx\":\"%s\",\"txe\":%d,\"rep\":%d,\"g\":[", S.st, S.err, S.tx, S.txe, S.reported);
    for (int i = 0; i < S.n && n < cap - 200; i++) {
        const ll_dev *v = &S.d[i]; char a[18];
        mac_s(a, v->mac);
        n += snprintf(out + n, cap - n, "%s{\"mac\":\"%s\",\"own\":%d,\"ch\":%d,\"type\":%d,\"n\":%d,\"rpm\":[", i ? "," : "", a, !memcmp(S.master, (BYTE[6]){ 0 }, 6) || !memcmp(v->master, S.master, 6), v->ch, v->type, v->n);
        for (int f = 0; f < v->n; f++) n += snprintf(out + n, cap - n, "%s%d", f ? "," : "", v->rpm[f]);
        n += snprintf(out + n, cap - n, "],\"pct\":[");
        for (int f = 0; f < v->n; f++) n += snprintf(out + n, cap - n, "%s%d", f ? "," : "", (v->duty[f] * 100 + 127) / 255);
        n += snprintf(out + n, cap - n, "]}");
    }
    n += snprintf(out + n, cap - n, "]}");
    LeaveCriticalSection(&cs);
    return n;
}

#else   // cl /DLL_TEST src\lianli.c: the reply parser, on records laid out as in the projects the layout comes from
#include <assert.h>
int main(void) {
    static const BYTE dongle[6] = { 0xbf, 0x3d, 0xca, 0xe5, 0x66, 0xe4 }, ga[6] = { 0x7c, 0x9c, 0x06, 0xf5, 0x17, 0xe1 };
    BYTE r[4 + 3 * REC] = { 0x10, 3 }, m[6] = { 0 }; ll_dev d[4]; int rep;
    BYTE *p = r + 4;   // the dongle's own record
    memcpy(p, dongle, 6); p[12] = 8; p[18] = 0xFF; p[41] = 0x1C;
    p += REC;          // a group of 3, chained from the right, rpm with flag bits in the high nibble
    memcpy(p, ga, 6); memcpy(p + 6, dongle, 6); p[12] = 8; p[13] = 2; p[19] = 13; p[41] = 0x1C;
    memcpy(p + 28, (BYTE[]){ 0x66, 0x72, 0x06, 0x80, 0x07, 0x1c, 0, 0 }, 8); memcpy(p + 36, (BYTE[]){ 201, 201, 201, 0 }, 4);
    p += REC;          // a bad marker: skipped
    memcpy(p, ga, 6); p[41] = 0;
    assert(ll_parse(r, sizeof(r), 1, d, 4, m, &rep) == 1 && rep == 3 && !memcmp(m, dongle, 6));
    assert(!memcmp(d[0].mac, ga, 6) && d[0].n == 3 && d[0].ch == 8 && d[0].type == 0);
    assert(d[0].rpm[0] == 0x672 && d[0].rpm[1] == 0x680 && d[0].rpm[2] == 0x71c && d[0].rpm[3] == 0 && d[0].duty[2] == 201);
    assert(ll_parse(r, sizeof(r) - 1, 1, d, 4, m, &rep) == -1);   // short
    r[0] = 0x11; assert(ll_parse(r, sizeof(r), 1, d, 4, m, &rep) == -1);
    assert(d[0].rx == 2);
    // the speed command and its frames, against the layout the reference projects test
    BYTE pl[240], f[4][64]; ll_dev g = { { 1, 1, 1, 1, 1, 1 }, { 0 }, 8, 2, 0, 3 }; static const BYTE nine[6] = { 9, 9, 9, 9, 9, 9 };
    ll_speed(pl, &g, nine, 3, 6);
    assert(!memcmp(pl, (BYTE[]){ 0x12, 0x10, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 2, 8, 3, 6, 6, 6, 0 }, 21));
    for (int i = 21; i < 240; i++) assert(!pl[i]);
    for (int i = 0; i < 240; i++) pl[i] = (BYTE)i;
    ll_frames(f, 8, 2, pl);
    for (int i = 0; i < 4; i++) assert(f[i][0] == 0x10 && f[i][1] == i && f[i][2] == 8 && f[i][3] == 2 && !memcmp(f[i] + 4, pl + i * 60, 60));
    assert(f[3][63] == 239);
    // the clock broadcast: whole block, then the first one's unset head
    SYSTEMTIME t = { 2031, 12, 0, 25, 23, 59, 58 };
    ll_beat(pl, dongle, &t, 0);
    assert(pl[0] == 0x12 && pl[1] == 0x14 && !memcmp(pl + 8, dongle, 6) && !memcmp(pl + 14 + 32, (BYTE[]){ 0x07, 0xef, 12, 25, 23, 59, 58 }, 7));
    for (int i = 2; i < 8; i++) assert(!pl[i]);
    for (int i = 234; i < 240; i++) assert(!pl[i]);
    ll_beat(pl, dongle, &t, 1);
    for (int i = 14; i < 64; i++) assert(pl[i] == 0x14);
    for (int i = 64; i < 240; i++) assert(!pl[i]);   // the tail past the date is all zero
    puts("lianli: ok");
    return 0;
}
#endif
