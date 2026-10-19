// SPDX-License-Identifier: GPL-3.0-only
// Lian Li wireless fans (UNI FAN SL/TL wireless) through their RF dongle pair: transmitter 0416:8040, receiver 0416:8041,
// both on the inbox WinUSB driver. Once a second the receiver is asked which groups it hears, and answers with one
// 42-byte record per device (address, channel, fan count, rpm and duty per fan, model, the effect it shows). Only while
// some group has a speed set (fans/ll_<mac>, 30..100 %) or is lit by haku (a light device of kind lianli, drv_lianli
// below) is the transmitter opened: then each second it carries the clock broadcast the fans take as a host being
// there, each such group's duty, and colours as uploads (a one-frame animation, tinyuz-compressed). With none of that
// it stays closed and the fans run on their own, as before. The frame layouts come from
// MarcusSanchez/FanControl.LianLiWireless, Yoinky3000/LL-Connect-Wireless and sgtaziz/lian-li-linux, the stream format
// from sisong/tinyuz (all MIT, see THIRD_PARTY_NOTICES.md). Windows gives each dongle to one program at a time: while
// the L-Connect service holds one, the page says so.
#include "devices.h"
#include <setupapi.h>
#include <winusb.h>
#include <process.h>

#define LL_GROUPS 16
#define REC 42
static const GUID ll_guid = { 0x1D4B2365, 0x4749, 0x48EA, { 0xB3, 0x8A, 0x7C, 0x6F, 0xDD, 0xDD, 0x7E, 0x26 } };

// model: the first fan's type byte (LL_MODELS); sync: it follows the board's ARGB (uploads are ignored then); seq: the
// last command number it took; right: chained from the right; fx: the effect it shows (ll_rgb's id)
typedef struct { BYTE mac[6], master[6]; int ch, rx, type, n; int rpm[4], duty[4]; int model, sync, seq, right; BYTE fx[4]; } ll_dev;
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
        v->right = p[19] >= 10;
        for (int f = 0; f < 4; f++) { v->rpm[f] = (p[28 + f * 2] & 0x0F) << 8 | p[29 + f * 2]; v->duty[f] = p[36 + f]; }
        v->model = 0; for (int f = 24; f < 28 && !v->model; f++) v->model = p[f];
        v->sync = p[28] >> 6 & 1; v->seq = p[40]; memcpy(v->fx, p + 20, 4);
    }
    return n;
}

// fan models by type byte: LEDs per fan, name
static const struct { BYTE lo, hi, leds; const char *name; } LL_MODELS[] = {
    { 20, 26, 40, "SL" }, { 27, 35, 26, "TL" }, { 36, 39, 44, "SL-INF" }, { 40, 42, 24, "CL" }, { 43, 50, 44, "SL-INF" },
    { 51, 58, 26, "TL" }, { 59, 62, 52, "SL V4" }, { 63, 63, 9, "P28" }, { 126, 127, 24, "CL" } };
static int ll_model(int m) {
    for (int i = 0; i < (int)(sizeof(LL_MODELS) / sizeof(LL_MODELS[0])); i++) if (m >= LL_MODELS[i].lo && m <= LL_MODELS[i].hi) return i;
    return -1;
}
static int ll_leds(const ll_dev *v) { int k = ll_model(v->model); return k < 0 ? 0 : v->n * LL_MODELS[k].leds; }
#define LL_LEDS (4 * 52)   // the most a group has: 4 SL V4 fans

// tinyuz's stream (sisong/tinyuz 1.1, built with its defaults as lian-li-linux does; the fans' firmware unpacks it):
// the largest match distance as 4 bytes LE, then items. Type bits go LSB first into bytes placed in the stream where
// the next bit is first needed; data bytes go straight in. Literal: bit 1, the byte. Match: bit 0, length-2 (one less
// past distance 2687), then after a literal a bit 1 for "the last distance again", else the distance as a byte below
// 128 or 1xxxxxxx and the rest as a number. Numbers: groups of 1 (lengths) or 2 bits, each with a bit "more", the high
// group first, +1 per extra group. A distance 0 is a control: 1 a run of 15+ literal bytes, 3 the end
typedef struct { BYTE *o; int n, cap, at, bits, lit; unsigned back, maxd; } tz_w;
static void tz_byte(tz_w *w, int b) { if (w->n < w->cap) w->o[w->n] = (BYTE)b; w->n++; }
static void tz_bit(tz_w *w, int b) {
    if (!w->bits) { w->at = w->n; tz_byte(w, 0); }
    if (w->at < w->cap) w->o[w->at] |= (BYTE)(b << w->bits);
    w->bits = (w->bits + 1) & 7;
}
static void tz_num(tz_w *w, unsigned v, int pb) {
    int c = 1;
    for (unsigned m; v >= (m = 1u << c * pb); c++) v -= m;
    while (c--) { for (int i = 0; i < pb; i++) tz_bit(w, v >> (c * pb + i) & 1); tz_bit(w, c > 0); }
}
static void tz_ctrl(tz_w *w, int c) { tz_bit(w, 0); tz_num(w, c, 1); if (w->lit) tz_bit(w, 0); tz_byte(w, 0); }
static void tz_lits(tz_w *w, const BYTE *p, int n) {
    if (n >= 15) { tz_ctrl(w, 1); tz_num(w, n - 15, 2); while (n--) tz_byte(w, *p++); w->lit = 1; }
    else while (n-- > 0) { tz_bit(w, 1); tz_byte(w, *p++); w->lit = 1; }
}
static void tz_match(tz_w *w, int len, unsigned d) {
    int same = w->lit && d == w->back;
    tz_bit(w, 0); tz_num(w, len - 2 - (!same && d > 2687), 1);
    if (w->lit) tz_bit(w, same);
    if (!same) { tz_byte(w, d < 128 ? d : ((d - 128) & 127) | 128); if (d >= 128) tz_num(w, (d - 128) >> 7, 2); }
    w->lit = 0; w->back = d; if (d > w->maxd) w->maxd = d;
}
// greedy, the nearest longest match within 4 KB; returns the size, 0 when it does not fit in cap
static int tuz_pack(const BYTE *in, int n, BYTE *out, int cap) {
    tz_w w = { out, 4, cap, 0, 0, 0, 1, 1 };
    int from = 0;   // literals not yet written
    for (int i = 0; i < n;) {
        int best = 0; unsigned bd = 0;
        for (int d = 1; d <= i && d <= 4096 && best < n - i; d++) {   // shortcut: a plain search, inputs are a few hundred bytes
            int l = 0; while (i + l < n && in[i + l] == in[i + l - d]) l++;
            if (l > best && (l >= 3 || d <= 2687)) { best = l; bd = (unsigned)d; }
        }
        if (best >= 2) { tz_lits(&w, in + from, i - from); tz_match(&w, best, bd); i += best; from = i; }
        else i++;
    }
    tz_lits(&w, in + from, n - from);
    tz_ctrl(&w, 3);
    if (w.n > cap || cap < 4) return 0;
    for (int i = 0; i < 4; i++) out[i] = (BYTE)(w.maxd >> i * 8);
    return w.n;
}

// a group's colours as an upload: one frame (3 bytes a LED: R G B, fan by fan) shown for 5 s, which the fans hold.
// Frame 0 is the header, then the stream in 220-byte chunks; each carries the effect's id (FNV-1a over the stream and
// the header's timing, high bit set). Returns the frame count (0: too big), the id in fx
#define LL_UPFRAMES 8
static int ll_rgb(BYTE pl[][240], const ll_dev *v, const BYTE *master, const BYTE *rgb, int leds, BYTE *fx) {
    BYTE raw[LL_LEDS * 3], z[LL_UPFRAMES * 220 - 220];
    int per = leds / (v->n ? v->n : 1), inf = ll_model(v->model) >= 0 && !strcmp(LL_MODELS[ll_model(v->model)].name, "SL-INF");
    if (leds > LL_LEDS) return 0;
    for (int i = 0; i < leds; i++) {   // SL-INF chained from the right: the fans in reverse order
        int j = inf && v->right ? (v->n - 1 - i / per) * per + i % per : i;
        memcpy(raw + j * 3, rgb + i * 3, 3);
    }
    int zn = tuz_pack(raw, leds * 3, z, sizeof(z)), frames = (zn + 219) / 220 + 1;
    if (!zn) return 0;
    // the timing: 5000 ms in 1/160 ms = 8000 ticks, fraction 0; 1 frame; no second layer
    const BYTE t[11] = { 0x1F, 0x40, 0, 0, 1, (BYTE)leds, 0, 0, 0, 0, 0 };
    unsigned h = 0x811c9dc5;
    for (int i = 0; i < zn + 11; i++) h = (h ^ (i < zn ? z[i] : t[i - zn])) * 0x01000193;
    fx[0] = (BYTE)(h >> 24 | 0x80); fx[1] = (BYTE)(h >> 16); fx[2] = (BYTE)(h >> 8); fx[3] = (BYTE)h;
    for (int f = 0; f < frames; f++) {
        BYTE *p = pl[f];
        memset(p, 0, 240); p[0] = 0x12; p[1] = 0x20; memcpy(p + 2, v->mac, 6); memcpy(p + 8, master, 6); memcpy(p + 14, fx, 4);
        p[18] = (BYTE)f; p[19] = (BYTE)frames;
        if (!f) { p[20] = (BYTE)(zn >> 24); p[21] = (BYTE)(zn >> 16); p[22] = (BYTE)(zn >> 8); p[23] = (BYTE)zn; memcpy(p + 25, t + 3, 3); memcpy(p + 32, t, 3); }
        else memcpy(p + 20, z + (f - 1) * 220, zn - (f - 1) * 220 < 220 ? zn - (f - 1) * 220 : 220);
    }
    return frames;
}
// the board's ARGB sync on or off: 12 27 <group> <dongle> <receiver type> <dongle channel> <index: 0-based among the
// groups bound to the dongle> <command number, taken when the group reports it> .. .. <on>
static void ll_sync(BYTE *pl, const ll_dev *v, const BYTE *master, int ch, int idx, int seq, int on) {
    memset(pl, 0, 240); pl[0] = 0x12; pl[1] = 0x27; memcpy(pl + 2, v->mac, 6); memcpy(pl + 8, master, 6);
    pl[14] = (BYTE)v->rx; pl[15] = (BYTE)ch; pl[16] = (BYTE)idx; pl[17] = (BYTE)seq; pl[20] = (BYTE)on;
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
// the groups haku lights (drv_lianli): colours from the devices worker, sent from this thread. todo after close: 1 an
// upload of black still to go, 2 the board's sync to turn back on; fx: the last upload's id, at its time
static struct { BYTE mac[6]; int on, todo, tries, synced, stries; BYTE rgb[LL_LEDS * 3], fx[4]; DWORD at; } L[LL_GROUPS];
static HANDLE rgb_ev;
static int lit(const BYTE *mac) {   // under cs: the slot of a group haku lights, or -1
    for (int k = 0; k < LL_GROUPS; k++) if ((L[k].on || L[k].todo) && !memcmp(L[k].mac, mac, 6)) return k;
    return -1;
}

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
static void lights(const ll_dev *d, int n, int check);
static void drive(const ll_dev *d, int n) {
    int pct[LL_GROUPS], on[LL_GROUPS], any = 0;
    EnterCriticalSection(&cs);
    for (int i = 0; i < n; i++) any |= ((pct[i] = ll_want(&d[i])) > 0) | (on[i] = lit(d[i].mac) >= 0);
    LeaveCriticalSection(&cs);
    if (!any) { if (T.w) tx_close("off", 0); T.tried = 0; return; }
    int ch = 8;   // shortcut: the first set group's channel for the dongle's own; every receiver seen so far reports one channel
    for (int i = 0; i < n; i++) if (pct[i] || on[i]) { ch = d[i].ch; break; }
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
    lights(d, n, 1);
}

// colours to the lit groups bound to this dongle: an upload when they changed, or (check, after a poll) again when
// the group still shows another effect 2 s on (5 times at most). A group following the board's ARGB ignores uploads:
// its sync goes off first, once a second until it reports so
static void lights(const ll_dev *d, int n, int check) {
    if (!T.w) return;
    DWORD now = GetTickCount();
    for (int i = 0, idx = 0; i < n; i++) {
        if (memcmp(d[i].master, T.master, 6)) continue;
        int slot = idx++, leds = ll_leds(&d[i]);
        EnterCriticalSection(&cs);
        int k = lit(d[i].mac);
        BYTE rgb[LL_LEDS * 3], was[4]; int todo = 0, tries = 0, stries = 0; DWORD at = 0;
        if (k >= 0) { memcpy(rgb, L[k].rgb, sizeof(rgb)); memcpy(was, L[k].fx, 4); todo = L[k].todo; tries = L[k].tries; stries = L[k].stries; at = L[k].at; }
        LeaveCriticalSection(&cs);
        if (k < 0 || !leds) continue;
        BYTE pl[LL_UPFRAMES][240], fx[4];
        if (todo == 2 || d[i].sync) {   // 2: let go, back to the board's ARGB
            int on = todo == 2, done = on && d[i].sync;
            if (!done && !check) continue;
            if (!done && stries < 10) {   // the command number taken last, plus one; 254 and 255 are not used
                ll_sync(pl[0], &d[i], T.master, T.ch, slot, d[i].seq + 1 >= 254 ? 1 : d[i].seq + 1, on);
                if (!tx_send(d[i].ch, d[i].rx, pl[0])) return;
                Sleep(10);
                if (!stries) logf_("lianli: %02x:%02x:%02x:%02x:%02x:%02x board ARGB sync %s asked (index %d)", d[i].mac[0], d[i].mac[1], d[i].mac[2], d[i].mac[3], d[i].mac[4], d[i].mac[5], on ? "on" : "off", slot);
            }
            EnterCriticalSection(&cs);
            if (!on) L[k].synced = 1;
            if (done || ++L[k].stries > 10) { L[k].stries = 0; if (on) L[k].todo = 0; }   // shortcut: given up after 10 s; nothing tells the window
            LeaveCriticalSection(&cs);
            continue;
        }
        int fr = ll_rgb(pl, &d[i], T.master, rgb, leds, fx);
        if (!fr) continue;
        int again = !memcmp(fx, was, 4);
        if (again && !(check && memcmp(d[i].fx, fx, 4) && now - at >= 2000)) continue;
        if (again && tries >= 5) continue;
        if (again && ++tries == 5) logf_("lianli: %02x:%02x:%02x:%02x:%02x:%02x does not take the colours", d[i].mac[0], d[i].mac[1], d[i].mac[2], d[i].mac[3], d[i].mac[4], d[i].mac[5]);
        for (int r = 0; r < 4; r++) { if (!tx_send(d[i].ch, d[i].rx, pl[0])) return; Sleep(20); }   // the header 4 times, as L-Connect does
        for (int f = 1; f < fr; f++) if (!tx_send(d[i].ch, d[i].rx, pl[f])) return;
        Sleep(10);
        EnterCriticalSection(&cs);
        memcpy(L[k].fx, fx, 4); L[k].at = now; L[k].tries = again ? tries : 0;
        if (L[k].todo == 1 && !memcmp(L[k].rgb, rgb, sizeof(rgb))) L[k].todo = 0;
        LeaveCriticalSection(&cs);
    }
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

static void lights_now(void) {
    ll_dev d[LL_GROUPS]; int n;
    EnterCriticalSection(&cs); n = S.n; memcpy(d, S.d, sizeof(ll_dev) * n); LeaveCriticalSection(&cs);
    lights(d, n, 0);
}
// the second between polls; colours that come meanwhile go out at once. 0: stopping
static int tick(void) {
    for (DWORD due = GetTickCount() + 1000, now; (int)(due - (now = GetTickCount())) > 0;) {
        HANDLE h[2] = { stop_ev, rgb_ev };
        DWORD w = WaitForMultipleObjects(2, h, FALSE, due - now);
        if (w == WAIT_OBJECT_0) return 0;
        if (w != WAIT_OBJECT_0 + 1) break;
        lights_now();
    }
    return 1;
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
        } while (tick());
        WinUsb_Free(w); CloseHandle(f);
        lights_now();   // what the light devices left on the way out (black on quit)
        if (T.w) tx_close("off", 0);   // stopping: the fans keep their duty until they miss the clock, then run on their own
    } while (WaitForSingleObject(stop_ev, 5000) == WAIT_TIMEOUT);
    return 0;
}

void ll_start(void) {
    InitializeCriticalSection(&cs);
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    rgb_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
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

// a light device per fan group bound to the dongle (host: its address), its LEDs as the model has them
static int ll_mac(const char *s, BYTE *m) {
    unsigned v[6];
    if (sscanf_s(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) m[i] = (BYTE)v[i];
    return 1;
}
static const ll_dev *heard(const BYTE *mac) {   // under cs: the group as last read, bound to this dongle
    for (int i = 0; i < S.n; i++) if (!memcmp(S.d[i].mac, mac, 6) && !memcmp(S.d[i].master, S.master, 6)) return &S.d[i];
    return NULL;
}
static int lo_open(ext_dev *d) {
    BYTE m[6]; int k = -1, leds = 0, fans = 0, model = -1;
    if (!th || !ll_mac(d->host, m)) return 0;
    EnterCriticalSection(&cs);
    const ll_dev *v = heard(m);
    if (v && (leds = ll_leds(v)) > 0) {
        fans = v->n; model = ll_model(v->model);
        if ((k = lit(m)) < 0) for (k = 0; k < LL_GROUPS && (L[k].on || L[k].todo); k++) {}
        if (k < LL_GROUPS) { memset(&L[k], 0, sizeof(L[k])); memcpy(L[k].mac, m, 6); L[k].on = 1; } else k = -1;
    }
    LeaveCriticalSection(&cs);
    if (k < 0) { snprintf(d->info, sizeof(d->info), "Lian Li · %s", v ? "the fan model is not known" : "the group is not heard"); return 0; }
    d->nleds = leds; d->priv = (void *)(INT_PTR)(k + 1);
    snprintf(d->info, sizeof(d->info), "Lian Li · %d × %s · wireless", fans, LL_MODELS[model].name);
    SetEvent(rgb_ev);
    return 1;
}
static int lo_send(ext_dev *d, const rgbf *c, int n) {
    int k = (int)(INT_PTR)d->priv - 1, ok;
    BYTE rgb[LL_LEDS * 3] = { 0 };
    for (int i = 0; i < n && i < LL_LEDS; i++) { rgb[i * 3] = to8(c[i].r); rgb[i * 3 + 1] = to8(c[i].g); rgb[i * 3 + 2] = to8(c[i].b); }
    EnterCriticalSection(&cs);
    if ((ok = heard(L[k].mac) != NULL) && memcmp(L[k].rgb, rgb, sizeof(rgb))) { memcpy(L[k].rgb, rgb, sizeof(rgb)); SetEvent(rgb_ev); }
    LeaveCriticalSection(&cs);
    return ok;
}
// off: black goes out after close; restore: the board's ARGB sync back on if haku turned it off
static void lo_leave(ext_dev *d, int how) {
    int k = (int)(INT_PTR)d->priv - 1;
    if (k < 0) return;
    EnterCriticalSection(&cs);
    if (how == LEAVE_OFF) { memset(L[k].rgb, 0, sizeof(L[k].rgb)); L[k].todo = 1; }
    else if (how == LEAVE_RESTORE && L[k].synced) L[k].todo = 2;
    LeaveCriticalSection(&cs);
    SetEvent(rgb_ev);
}
static void lo_close(ext_dev *d) {
    int k = (int)(INT_PTR)d->priv - 1;
    if (k < 0) return;
    EnterCriticalSection(&cs); L[k].on = 0; LeaveCriticalSection(&cs);
    d->priv = NULL;
}
static void lo_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    disc_t x[LL_GROUPS]; int n = 0;
    if (!th) return;
    EnterCriticalSection(&cs);
    for (int i = 0; i < S.n; i++) {
        const ll_dev *v = &S.d[i]; int k = ll_model(v->model);
        if (memcmp(v->master, S.master, 6) || k < 0 || !v->n) continue;
        disc_t *o = &x[n++]; memset(o, 0, sizeof(*o)); strcpy_s(o->kind, sizeof(o->kind), "lianli");
        mac_s(o->host, v->mac); o->sub = -1; o->nleds = ll_leds(v);
        snprintf(o->name, sizeof(o->name), "Lian Li %s ×%d", LL_MODELS[k].name, v->n);
        snprintf(o->info, sizeof(o->info), "Lian Li · %d × %s · wireless", v->n, LL_MODELS[k].name);
    }
    LeaveCriticalSection(&cs);
    for (int i = 0; i < n; i++) found(&x[i]);
}
const ext_driver drv_lianli = { "lianli", "Lian Li Wireless", 2, 1, lo_open, lo_send, lo_leave, lo_close, lo_discover };

#else   // cl /DLL_TEST src\lianli.c: the reply parser, on records laid out as in the projects the layout comes from
#include <assert.h>
#include <stdlib.h>
// tinyuz's own memory decoder (tuz_dec.c, tuz_decompress_mem), cut to what the stream uses, to check tuz_pack
typedef struct { const BYTE *p, *end; unsigned bits, n; } tz_r;
static int tz_get(tz_r *s, int k) {   // k type bits, LSB first
    int v = 0;
    for (int i = 0; i < k; i++) { if (!s->n) { assert(s->p < s->end); s->bits = *s->p++; s->n = 8; } v |= (s->bits & 1) << i; s->bits >>= 1; s->n--; }
    return v;
}
static unsigned tz_len(tz_r *s, int rb) {
    unsigned v = 0;
    for (;;) { int b = tz_get(s, rb); v = (v << (rb - 1)) + (b & ((1 << (rb - 1)) - 1)); if (!(b & 1 << (rb - 1))) return v; v++; }
}
static int tuz_unpack(const BYTE *in, int n, BYTE *out, int cap) {
    tz_r s = { in + 4, in + n }; BYTE *o = out; unsigned back = 1; int lit = 0;
    for (;;) {
        if (!(tz_get(&s, 1) & 1)) {
            unsigned len = tz_len(&s, 2), pos;
            if (lit && tz_get(&s, 1)) pos = back;
            else { assert(s.p < s.end); pos = *s.p++; if (pos >= 128) pos = ((pos & 127) | tz_len(&s, 3) << 7) + 128; len += pos > 2687; }
            lit = 0;
            if (pos) {
                len += 2; back = pos;
                assert(pos <= (unsigned)(o - out) && (int)len <= cap - (int)(o - out));
                while (len--) { *o = o[-(int)pos]; o++; }
            } else if (len == 1) {
                unsigned k = tz_len(&s, 3) + 15;
                assert((int)k <= s.end - s.p && (int)k <= cap - (int)(o - out));
                memcpy(o, s.p, k); o += k; s.p += k; lit = 1;
            } else { assert(len == 3); back = 1; s.n = 0; return (int)(o - out); }
        } else { assert(o < out + cap && s.p < s.end); *o++ = *s.p++; lit = 1; }
    }
}
static void tz_round(const BYTE *in, int n) {
    static BYTE z[8192], back[8192]; int zn = tuz_pack(in, n, z, sizeof(z));
    assert(zn > 4 && tuz_unpack(z, zn, back, sizeof(back)) == n && !memcmp(in, back, n));
    unsigned dmax = z[0] | z[1] << 8 | z[2] << 16 | z[3] << 24; assert(dmax >= 1 && dmax <= 4096);
}
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
    // model, sync, command number and effect from the record (the parser test's group: sync on, as 0x66 has 0x40)
    memcpy(p - REC + 20, (BYTE[]){ 0x81, 2, 3, 4, 0, 61 }, 6); (p - REC)[40] = 9;
    r[0] = 0x10; assert(ll_parse(r, sizeof(r), 1, d, 4, m, &rep) == 1);
    assert(d[0].model == 61 && d[0].sync && d[0].seq == 9 && d[0].right && !memcmp(d[0].fx, (BYTE[]){ 0x81, 2, 3, 4 }, 4) && ll_leds(&d[0]) == 3 * 52);
    // the stream: tinyuz's own bytes for nothing and for "A", then round trips through its decoder
    BYTE z[64];
    assert(tuz_pack(NULL, 0, z, sizeof(z)) == 6 && !memcmp(z, (BYTE[]){ 1, 0, 0, 0, 0x0C, 0 }, 6));
    assert(tuz_pack((const BYTE *)"A", 1, z, sizeof(z)) == 7 && !memcmp(z, (BYTE[]){ 1, 0, 0, 0, 0x19, 0x41, 0 }, 7));
    assert(!tuz_pack((const BYTE *)"A", 1, z, 6));
    static BYTE in[6000];
    for (int i = 0; i < (int)sizeof(in); i++) in[i] = (BYTE)(i % 3 == 0 ? 255 : i % 3 == 1 ? 40 : 0);
    tz_round(in, LL_LEDS * 3); tz_round(in, 1); tz_round(in, 2); tz_round(in, 3);
    srand(7);
    for (int it = 0; it < 300; it++) {   // random, runs, near repeats, long literal lines, distances past 128 and 2687
        int n = 1 + rand() % (int)sizeof(in), mode = it % 4;
        for (int i = 0; i < n; i++) in[i] = (BYTE)(mode == 0 ? rand() : mode == 1 ? rand() % 3 : mode == 2 ? (i / 7) * 13 : (i > 2800 ? in[i - 2700 - rand() % 2] : rand()));
        tz_round(in, n);
    }
    // an upload: header, chunks, the effect's id; a solid colour of 3 SL V4 fans fits one chunk
    ll_dev u = { { 1, 2, 3, 4, 5, 6 }, { 0 }, 8, 2, 0, 3 }; u.model = 61;
    BYTE ups[LL_UPFRAMES][240], fx[4], col[LL_LEDS * 3];
    for (int i = 0; i < 156; i++) memcpy(col + i * 3, (BYTE[]){ 10, 20, 30 }, 3);
    int fr = ll_rgb(ups, &u, nine, col, 156, fx), zn = ups[0][20] << 24 | ups[0][21] << 16 | ups[0][22] << 8 | ups[0][23];
    assert(fr == 2 && zn > 0 && zn <= 220 && (fx[0] & 0x80));
    assert(!memcmp(ups[0], (BYTE[]){ 0x12, 0x20, 1, 2, 3, 4, 5, 6, 9, 9, 9, 9, 9, 9 }, 14) && !memcmp(ups[0] + 14, fx, 4) && ups[0][18] == 0 && ups[0][19] == 2);
    assert(!memcmp(ups[0] + 25, (BYTE[]){ 0, 1, 156 }, 3) && ups[0][32] == 0x1F && ups[0][33] == 0x40 && ups[0][34] == 0);
    assert(ups[1][18] == 1 && !memcmp(ups[1] + 14, fx, 4));
    BYTE back[LL_LEDS * 3]; assert(tuz_unpack(ups[1] + 20, zn, back, sizeof(back)) == 468 && !memcmp(back, col, 468));
    for (int i = 0; i < 156; i++) memcpy(col + i * 3, (BYTE[]){ (BYTE)(i * 7), (BYTE)(i * 13), (BYTE)(i * 31) }, 3);   // little repetition: several chunks
    fr = ll_rgb(ups, &u, nine, col, 156, fx); zn = ups[0][20] << 24 | ups[0][21] << 16 | ups[0][22] << 8 | ups[0][23];
    assert(fr == (zn + 219) / 220 + 1 && fr > 2);
    static BYTE joined[LL_UPFRAMES * 220];
    for (int c = 1; c < fr; c++) { assert(ups[c][18] == c && ups[c][19] == fr); memcpy(joined + (c - 1) * 220, ups[c] + 20, 220); }
    assert(tuz_unpack(joined, zn, back, sizeof(back)) == 468 && !memcmp(back, col, 468));
    // SL-INF chained from the right: whole fans swap, each fan's own LEDs keep their order
    u.model = 36; u.n = 2; u.right = 1;
    for (int i = 0; i < 88; i++) memcpy(col + i * 3, (BYTE[]){ (BYTE)i, 0, 0 }, 3);
    fr = ll_rgb(ups, &u, nine, col, 88, fx); zn = ups[0][20] << 24 | ups[0][21] << 16 | ups[0][22] << 8 | ups[0][23];
    for (int c = 1; c < fr; c++) memcpy(joined + (c - 1) * 220, ups[c] + 20, 220);
    assert(tuz_unpack(joined, zn, back, sizeof(back)) == 264 && back[0] == 44 && back[43 * 3] == 87 && back[44 * 3] == 0);
    // sync off: the slot among the dongle's groups, the next command number
    ll_sync(pl, &g, nine, 8, 1, 10, 0);
    assert(!memcmp(pl, (BYTE[]){ 0x12, 0x27, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 2, 8, 1, 10, 0, 0, 0 }, 21));
    puts("lianli: ok");
    return 0;
}
#endif
