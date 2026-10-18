// SPDX-License-Identifier: GPL-3.0-only
// Lian Li wireless fans (UNI FAN SL/TL wireless) through their RF dongle pair: transmitter 0416:8040, receiver 0416:8041,
// both on the inbox WinUSB driver. Read only for now: once a second the receiver is asked which groups it hears, and
// answers with one 42-byte record per device (address, channel, fan count, rpm and duty per fan). Nothing goes on
// the radio: the transmitter is not opened. The frame layouts come from MarcusSanchez/FanControl.LianLiWireless and
// Yoinky3000/LL-Connect-Wireless (MIT, see THIRD_PARTY_NOTICES.md). Windows gives the dongle to one program at a time:
// while the L-Connect service holds it, the page says so.
#include "common.h"
#include <setupapi.h>
#include <winusb.h>
#include <process.h>

#define LL_GROUPS 16
#define REC 42
static const GUID ll_guid = { 0x1D4B2365, 0x4749, 0x48EA, { 0xB3, 0x8A, 0x7C, 0x6F, 0xDD, 0xDD, 0x7E, 0x26 } };

typedef struct { BYTE mac[6], master[6]; int ch, type, n; int rpm[4], duty[4]; } ll_dev;
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
        v->ch = p[12]; v->type = p[18];
        v->n = p[19] >= 10 ? p[19] - 10 : p[19]; if (v->n > 4) v->n = 4;   // 10 and up: chained from the right
        for (int f = 0; f < 4; f++) { v->rpm[f] = (p[28 + f * 2] & 0x0F) << 8 | p[29 + f * 2]; v->duty[f] = p[36 + f]; }
    }
    return n;
}

#ifndef LL_TEST
static CRITICAL_SECTION cs;
static HANDLE th, stop_ev;
// st: none (no dongle), busy (another program holds it), err (it does not answer), ok
static struct { const char *st; int err, reported, n; BYTE master[6]; ll_dev d[LL_GROUPS]; } S = { "none" };

static int rx_path(wchar_t *out, int cap) {
    HDEVINFO set = SetupDiGetClassDevsW(&ll_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    SP_DEVICE_INTERFACE_DATA id = { sizeof(id) };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &ll_guid, i, &id); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *dd = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        dd->cbSize = sizeof(*dd);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &id, dd, sizeof(buf), NULL, NULL)) continue;
        wchar_t lo[512]; wcsncpy_s(lo, 512, dd->DevicePath, _TRUNCATE); _wcslwr_s(lo, 512);
        if (wcsstr(lo, L"vid_0416&pid_8041")) { wcsncpy_s(out, cap, dd->DevicePath, _TRUNCATE); found = 1; }
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

// the first replies of a run go to the log as they came, for receivers whose firmware answers otherwise
static int dumps = 6, fail_e;
static const char *fail_at = "";
static void dump(const BYTE *r, int len, int pages, int n) {
    char h[96 * 3 + 1] = ""; int k = len < 96 ? len : 96;
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
    if (dumps > 0) { dumps--; if (stale) logf_("lianli: %d stale bytes dropped first", stale); dump(r, len, *pages, n); }
    if (n < 0) return 0;
    *pages = rep > 10 ? (rep + 9) / 10 > 26 ? 26 : (rep + 9) / 10 : 1;
    EnterCriticalSection(&cs);
    if (strcmp(S.st, "ok") || S.n != n) logf_("lianli: %d group(s) heard, %d record(s) reported", n, rep);
    S.st = "ok"; S.err = 0; S.reported = rep; S.n = n; memcpy(S.master, m, 6); memcpy(S.d, d, sizeof(ll_dev) * n);
    LeaveCriticalSection(&cs);
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
        if (!rx_path(path, 512)) { set_state("none", 0, ""); continue; }
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
            else if (r < 0 || ++silent >= 5) { set_state("err", r < 0 ? fail_e : 0, r < 0 ? fail_at : "no reply read in 5 s"); break; }   // gone, or no answer for 5 s: open again
        } while (WaitForSingleObject(stop_ev, 1000) == WAIT_TIMEOUT);
        WinUsb_Free(w); CloseHandle(f);
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
// {"st":..,"err":..,"rep":..,"g":[{"mac","own":bound to this dongle,"ch","type","n","rpm":[],"pct":[]}]}
int ll_json(char *out, int cap) {
    if (!th) return snprintf(out, cap, "{\"st\":\"none\"}");
    EnterCriticalSection(&cs);
    int n = snprintf(out, cap, "{\"st\":\"%s\",\"err\":%d,\"rep\":%d,\"g\":[", S.st, S.err, S.reported);
    for (int i = 0; i < S.n && n < cap - 200; i++) {
        const ll_dev *v = &S.d[i]; char a[18];
        mac_s(a, v->mac);
        n += snprintf(out + n, cap - n, "%s{\"mac\":\"%s\",\"own\":%d,\"ch\":%d,\"type\":%d,\"n\":%d,\"rpm\":[", i ? "," : "", a, !memcmp(v->master, S.master, 6), v->ch, v->type, v->n);
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
    puts("lianli: ok");
    return 0;
}
#endif
