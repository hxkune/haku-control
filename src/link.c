// SPDX-License-Identifier: GPL-3.0-only
// iCUE LINK System Hub (USB 1B1C:0C3F): what is on its LINK chain, its fan and pump speeds, temperatures and an HXi
// SHIFT PSU's rails; with [link] ctl = 1 (experimental, off by default) also its fans, pumps and LEDs.
// Bytes as in OpenLinkHub src/devices/lsh/lsh.go (GPL-3.0); none of the control bytes confirmed on a hub here yet.
// Readings ([link] on = 1): the hub answers its device list only in software mode, where its own fan and pump control
// waits for the host. So that is a short visit (about a second, every 10 s), hardware mode goes right after it, again
// until the hub takes it, and when the hub is next opened. Reads that answer in hardware mode stay there.
// Control ([link] ctl = 1): haku keeps the hub in software mode: readings every second, speeds every 2 s (they are what
// the hub runs on), colours as frames. Pumps never under 50 %, everything at 100 % while a pump's coolant is over
// 57 C. Hardware mode back on exit, crash, sleep, ctl off, another program, three rounds without an answer, and a
// device on the chain that is not in TYPES (then readings only). Nothing kept on the hub is written (no 1e, 1d,
// 15 01, 37). Transfers take Corsair's guard mutex, as iCUE's plugins, FanControl and OpenRGB do.
// Never while iCUE, OpenLinkHub, SignalRGB (or FanControl, for control) runs, or OpenRGB lights the hub: they own it.
// OpenRGB's own iCUE LINK detector is switched off while haku drives the hub ([link] orgb_det: 1 haku did it, so it
// goes back on with ctl off; 2 it was off already).
#include "common.h"
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <tlhelp32.h>
#include <process.h>
#include <limits.h>
#include <stdlib.h>
#ifdef LINK_TEST
#include <assert.h>
#include <stdio.h>
#endif

#define VISIT_MS 10000
#define CH 64             // channels a reading may name
#define LK 24             // channels haku drives (a chain has up to 24 devices)
#define NONE INT_MIN
#define SPEED_MS 2000
#define LIST_MS 10000
#define FRAME_MS 33
#define PUMP_MIN 50
#define HOT 570           // a pump's coolant, tenths of a degree: everything at 100 % over it...
#define COOL 520          // ...until every pump is under this
#ifdef HAKU_DEV
#define CAN_HOLD 0        // the test build does not switch the hub's mode
#else
#define CAN_HOLD 1
#endif

enum { L_OFF, L_NOHUB, L_APP, L_ORGB, L_OK, L_ERR, L_DEV };
static const char *const st_name[] = { "off", "nohub", "app", "orgb", "ok", "err", "dev" };
enum { K_UNK, K_FAN, K_PUMP, K_BLOCK, K_PSU, K_LCD };
static const char *const kind_name[] = { "?", "fan", "pump", "block", "psu", "lcd" };
typedef struct { int ch, type, model; } ldev;

// the devices haku drives (types in decimal, as in OpenLinkHub's lsh.json) and their LEDs. Any other one on the chain
// (the XD5 ELITE LCD, whose pump is not known; the LINK adapter, Commander DUO, V2 devices...): readings only.
// The pump's LCD cap (6): speeds yes, colours no (OpenLinkHub and OpenRGB count its LEDs differently)
static const struct { BYTE type, kind, leds; } TYPES[] = {
    { 1, K_FAN, 34 }, { 2, K_FAN, 18 }, { 3, K_FAN, 8 }, { 15, K_FAN, 8 }, { 19, K_FAN, 0 }, { 4, K_FAN, 0 }, { 16, K_FAN, 0 },
    { 7, K_PUMP, 20 }, { 17, K_PUMP, 20 }, { 12, K_PUMP, 22 }, { 25, K_PUMP, 22 },
    { 9, K_BLOCK, 24 }, { 13, K_BLOCK, 16 }, { 11, K_PSU, 0 }, { 6, K_LCD, 0 } };

static CRITICAL_SECTION cs, iocs;   // cs: L and the colours; iocs: the hub's transfers, one thread at a time
static struct { int st, found, fw[3], nd, nr, hw, ctl, held, refuse, hot, leds; DWORD seen; char app[40]; ldev d[CH]; int rpm[CH], t[CH], v[3], a[3]; } L;
static HANDLE th, stop_ev, cut_ev, col_ev, ev, mx, h = INVALID_HANDLE_VALUE;
static volatile LONG paused;
static volatile int held, need_hw;   // set to 1 only on the hub thread, to 0 under iocs
static int started, hello, hw_tried, col_open, black_due, fails, sfails, out_len, in_len;
static DWORD list_at, speed_at, frame_at;
static BYTE rgb[EXT_MAX_LEDS * 3];   // under cs: the last frame from drv_icuelink
static int dirty, lit;               // under cs
static BYTE H[512], R[1020];         // H: the last reply; R: an endpoint's reply with its second packet

static int u16(const BYTE *p) { return p[0] | p[1] << 8; }

static int kind_of(int type, int *leds) {
    for (int i = 0; i < (int)(sizeof(TYPES) / sizeof(TYPES[0])); i++)
        if (TYPES[i].type == type) { if (leds) *leds = TYPES[i].leds; return TYPES[i].kind; }
    if (leds) *leds = 0;
    return K_UNK;
}

// the device list (endpoint 36): channel, type and model of each device; an empty channel has no serial
static int parse_devs(const BYTE *r, int len, ldev *d, int max) {
    int n = 0;
    for (int ch = 1, pos = 7; ch <= r[6] && ch < CH && pos + 8 <= len && n < max; ch++) {   // ch < CH: L.rpm[ch], L.t[ch]
        int id = r[pos + 7];
        if (!id) { pos += 8; continue; }
        d[n].ch = ch; d[n].type = r[pos + 2]; d[n].model = r[pos + 3]; n++;
        pos += 8 + id;
    }
    return n;
}
// speeds (17) and temperatures (21): a 3-byte record per channel, status 0 when it holds a value
static void parse_vals(const BYTE *r, int len, int *out) {
    for (int i = 0; i < CH; i++) out[i] = NONE;
    for (int i = 0; i < r[6] && i < CH && 10 + i * 3 <= len; i++) if (r[7 + i * 3] == 0) out[i] = (short)u16(r + 8 + i * 3);
}
// PSU rails (28 volts, 29 amps): how many at 6, then 4 bytes each with the value at +2
static int parse_rails(const BYTE *r, int *out) {
    int n = u16(r + 6) < 3 ? u16(r + 6) : 3;
    for (int i = 0; i < n; i++) out[i] = u16(r + 8 + i * 4 + 2);
    return n;
}

// what haku may do with a chain: the first channel it does not drive (0: none) and in *leds the LEDs it lights
static int classify(const ldev *d, int nd, int *leds) {
    int refuse = 0, n = 0, col = 1;
    for (int i = 0; i < nd; i++) {
        int l, k = kind_of(d[i].type, &l);
        // a PSU sits past the fans (25 on a real chain) and gets neither speeds nor colours
        if ((k == K_UNK || (d[i].ch > LK && k != K_PSU)) && !refuse) refuse = d[i].ch;
        col &= k != K_LCD; n += l;
    }
    // shortcut: a chain with more LEDs than one ext device holds gets no colours; split it in parts if one turns up
    *leds = col && n <= EXT_MAX_LEDS ? n : 0;
    return refuse;
}

// a colour frame: LL LL 00 00 12 00, R G B per LED, devices in channel order (black when rgb is NULL). OpenLinkHub
// dims a port that carries many LEDs (its limits); channels from 13 (from 7 on firmware 1) are the second port
static int frame(const ldev *d, int nd, int fw, const BYTE *rgb_in, BYTE *out) {
    int per[2] = { 0 }, cut = fw >= 2 ? 13 : 7, n = 0, o = 6;
    for (int i = 0; i < nd; i++) { int l; kind_of(d[i].type, &l); per[d[i].ch >= cut] += l; n += l; }
    out[0] = (BYTE)(n * 3 + 2); out[1] = (BYTE)((n * 3 + 2) >> 8); out[2] = out[3] = 0; out[4] = 0x12; out[5] = 0;
    for (int i = 0; i < nd; i++) {
        int l, p = per[d[i].ch >= cut]; kind_of(d[i].type, &l);
        float f = p > 442 ? 0.1f : p > 340 ? 0.33f : p > 238 ? 0.66f : 1.0f;
        for (int j = 0; j < l * 3; j++, o++) out[o] = rgb_in ? (BYTE)(rgb_in[o - 6] * f) : 0;
    }
    return o;
}

// OpenRGB's settings with its iCUE LINK detector on or off, into out: 1 changed, 2 already so, 0 not understood
static int det_edit(const char *in, char *out, int cap, int on) {
    static const char key[] = "\"Corsair iCUE Link System Hub\"";
    const char *p = strstr(in, key), *q, *r;
    if (p) {
        for (q = p + sizeof(key) - 1; *q && strchr(" \t\r\n:", *q); q++) {}
        int was = !strncmp(q, "true", 4) ? 1 : !strncmp(q, "false", 5) ? 0 : -1;
        if (was < 0) return 0;
        if (was == on) return 2;
        return snprintf(out, cap, "%.*s%s%s", (int)(q - in), in, on ? "true" : "false", q + (was ? 4 : 5)) < cap;
    }
    if (on) return 2;   // not there: OpenRGB has it on
    if ((p = strstr(in, "\"detectors\"")) && (q = strchr(p, '{'))) {
        for (r = q + 1; *r && strchr(" \t\r\n", *r); r++) {}
        return snprintf(out, cap, "%.*s%s:false%s%s", (int)(q + 1 - in), in, key, *r == '}' ? "" : ",", q + 1) < cap;
    }
    for (q = in; *q && strchr(" \t\r\n", *q); q++) {}
    if (!*q) return snprintf(out, cap, "{\"Detectors\":{\"detectors\":{%s:false}}}", key) < cap;
    if (*q != '{') return 0;
    for (r = q + 1; *r && strchr(" \t\r\n", *r); r++) {}
    return snprintf(out, cap, "%.*s\"Detectors\":{\"detectors\":{%s:false}}%s%s", (int)(q + 1 - in), in, key, *r == '}' ? "" : ",", q + 1) < cap;
}

typedef struct { char *app; int cap, ctl, r; DWORD orgb; } scan_t;
// a program that drives the hub itself (FanControl only counts for control: it reads otherwise), and OpenRGB
static void seen_exe(const wchar_t *x, DWORD pid, scan_t *s) {
    if (s->r == L_OK && ((!_wcsnicmp(x, L"icue", 4) && _wcsicmp(x, L"iCUEUpdateService.exe")) || !_wcsicmp(x, L"Corsair.Service.exe")
        || !_wcsicmp(x, L"OpenLinkHub.exe") || !_wcsicmp(x, L"SignalRgb.exe") || (s->ctl && !_wcsicmp(x, L"FanControl.exe")))) {
        s->r = L_APP; WideCharToMultiByte(CP_UTF8, 0, x, -1, s->app, s->cap, NULL, NULL);
    }
    if (!_wcsicmp(x, L"OpenRGB.exe")) s->orgb = pid;
}

#ifndef LINK_TEST
// one report out or in; stop and sleep cut it short unless it is the hardware mode on the way out
static int io(BYTE *b, int wr, int force) {
    OVERLAPPED ov = { 0 }; ov.hEvent = ev; ResetEvent(ev);
    DWORD got = 0, n = wr ? out_len : in_len;
    if (!(wr ? WriteFile(h, b, n, &got, &ov) : ReadFile(h, b, n, &got, &ov)) && GetLastError() != ERROR_IO_PENDING) return 0;
    HANDLE w[3] = { ev, stop_ev, cut_ev };
    if (WaitForMultipleObjects(force ? 1 : 3, w, FALSE, 1000) != WAIT_OBJECT_0) CancelIo(h);
    return GetOverlappedResult(h, &ov, &got, TRUE) && got == n;
}
// b: the report out (n command bytes); the reply lands in H
static int xfer(BYTE *b, int n, int force) {
    (void)n;
    HidD_FlushQueue(h);
    if (!io(b, 1, force)) return 0;
    memset(b, 0, 1025);
    if (!io(b, 0, force)) return 0;
    memcpy(H, b + 1, sizeof(H));
    return 1;
}

static HANDLE open_hub(void) {
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    HANDLE r = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; r == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (void *)buf; d->cbSize = sizeof(*d);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, sizeof(buf), NULL, NULL)) continue;
        if (!wcsstr(_wcslwr(d->DevicePath), L"vid_1b1c&pid_0c3f")) continue;
        r = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (r == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps = { 0 };
        if (HidD_GetPreparsedData(r, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        // the hub has a second interface (33-byte input only): the one with 513-byte reports both ways is the one
        if (caps.OutputReportByteLength != 513 || caps.InputReportByteLength != 513) { CloseHandle(r); r = INVALID_HANDLE_VALUE; continue; }
        out_len = in_len = 513;
    }
    SetupDiDestroyDeviceInfoList(di);
    return r;
}
static void let_go(void) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }

static void procs(scan_t *s) {
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) seen_exe(pe.szExeFile, pe.th32ProcessID, s);
    CloseHandle(snap);
}

static DWORD now(void) { return GetTickCount(); }
// 1: cut short (stop or sleep)
static int nap(DWORD ms) { HANDLE w[2] = { stop_ev, cut_ev }; return WaitForMultipleObjects(2, w, FALSE, ms) != WAIT_TIMEOUT; }

// %APPDATA%\OpenRGB\OpenRGB.json through det_edit; 3: no such file (OpenRGB never ran here)
static int det_file(int on) {
    wchar_t p[MAX_PATH], t[MAX_PATH + 8];
    if (!ExpandEnvironmentStringsW(L"%APPDATA%\\OpenRGB\\OpenRGB.json", p, MAX_PATH)) return 0;
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 3 : 0; }
    DWORD sz = GetFileSize(f, NULL), got = 0;
    char *in = sz < (1u << 20) ? malloc(sz + 1) : NULL, *out = in ? malloc(sz + 160) : NULL;
    int r = in && out && ReadFile(f, in, sz, &got, NULL) && got == sz;
    CloseHandle(f);
    if (r) { in[sz] = 0; r = strlen(in) == sz ? det_edit(in, out, (int)sz + 160, on) : 0; }   // a NUL inside (UTF-16): not a file to cut
    if (r == 1) {   // written next to it, then moved over it: never half a file
        swprintf(t, MAX_PATH + 8, L"%s.haku", p);
        HANDLE w = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD n = (DWORD)strlen(out), put = 0;
        int ok = w != INVALID_HANDLE_VALUE && WriteFile(w, out, n, &put, NULL) && put == n;
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        if (!ok || !MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(t); r = 0; }
    }
    free(in); free(out);
    return r;
}

static HANDLE guard(void) {
    HANDLE m = CreateMutexW(NULL, FALSE, L"Global\\CorsairLinkReadWriteGuardMutex");
    if (!m) m = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\CorsairLinkReadWriteGuardMutex");
    // shortcut: without it transfers go unguarded (only iocs); a report saying so would make it worth more
    if (!m) logf_("link: Corsair's guard mutex not opened (%lu), transfers go without it", GetLastError());
    return m;
}

#else   // LINK_TEST: a hub on the other side of xfer, and the app's functions link.c calls
static struct {
    int present, sw, dead, noread, nospd, col_open, ep, fw, nd, rpm[CH], t[CH];
    ldev d[8];
    BYTE spd[128], fr[2048], acc[2048]; int nspd, speeds, frames, blacks, nframe, nacc, want;
    char log[1 << 18]; int nlog;
} S;
static const wchar_t *T_procs[4];
static int T_orgb = -1, T_ours, T_stopped, T_nofile, C_ctl, C_on, C_det;
static char T_json[1024];
static DWORD T_now = 1000;
static struct { int calls, n, chn[LK], pump[LK], hot, released, want_fan, want_pump; } F;

static void sim_log(const BYTE *c, int n) {
    for (int i = 0; i < n && i < 32 && S.nlog < (int)sizeof(S.log) - 8; i++) S.nlog += sprintf_s(S.log + S.nlog, sizeof(S.log) - S.nlog, i ? " %02X" : "%02X", c[i]);
    if (S.nlog < (int)sizeof(S.log) - 2) { S.log[S.nlog++] = '\n'; S.log[S.nlog] = 0; }
}
static void sim_read(int h1, int e) {
    BYTE *r = H;
    if (S.noread) { r[3] = 1; return; }
    if (h1 == 0x19) {
        static const int V[3] = { 3312, 5046, 12031 }, A[3] = { 175, 550, 1025 };
        r[4] = e == 0x28 ? 0x23 : 0x24; r[6] = 3;
        for (int i = 0; i < 3; i++) { int v = e == 0x28 ? V[i] : A[i]; r[10 + i * 4] = (BYTE)v; r[11 + i * 4] = (BYTE)(v >> 8); }
        return;
    }
    if (e == 0x36) {
        if (!S.sw) { r[3] = 3; return; }
        int top = 0, pos = 7;
        for (int i = 0; i < S.nd; i++) if (S.d[i].ch > top) top = S.d[i].ch;
        r[4] = 0x21; r[6] = (BYTE)top;
        for (int ch = 1; ch <= top; ch++) {
            int k = -1;
            for (int i = 0; i < S.nd; i++) if (S.d[i].ch == ch) k = i;
            if (k < 0) { pos += 8; continue; }
            r[pos + 2] = (BYTE)S.d[k].type; r[pos + 3] = (BYTE)S.d[k].model; r[pos + 7] = 2; r[pos + 8] = 'A'; r[pos + 9] = 'B'; pos += 10;
        }
        return;
    }
    assert(e == 0x17 || e == 0x21);
    const int *v = e == 0x17 ? S.rpm : S.t;
    r[4] = e == 0x17 ? 0x25 : 0x10; r[6] = 25;
    for (int i = 0; i < 25; i++) {
        int on = !i;
        for (int k = 0; k < S.nd; k++) on |= S.d[k].ch == i;
        r[7 + i * 3] = on ? 0 : 1; r[8 + i * 3] = (BYTE)v[i]; r[9 + i * 3] = (BYTE)(v[i] >> 8);
    }
}
static void sim_col(const BYTE *c, int first) {
    if (!S.sw || !S.col_open) { H[3] = 3; return; }
    if (first) { S.nacc = 0; S.want = u16(c) + 4; }
    int k = S.want - S.nacc < 508 ? S.want - S.nacc : 508;
    memcpy(S.acc + S.nacc, c, k); S.nacc += k;
    if (S.nacc < S.want) return;
    int black = 1;
    for (int i = 6; i < S.want; i++) black &= !S.acc[i];
    S.blacks += black; S.frames++; memcpy(S.fr, S.acc, S.want); S.nframe = S.want;
}
static int xfer(BYTE *b, int n, int force) {
    (void)force;
    const BYTE *c = b + 3;
    sim_log(c, n);
    if (S.dead) return 0;
    memset(H, 0, sizeof(H)); H[2] = c[0];
    assert(b[0] == 0 && b[2] == 1 && (b[1] == 0 || b[1] == 0x19));
    assert(!(c[0] == 0x15 && c[1] == 0x01) && c[0] != 0x1e && c[0] != 0x1d && c[0] != 0x37);   // kept on the hub: never
    switch (c[0]) {
    case 0x01: assert(c[1] == 3 && c[2] == 0 && (c[3] == 1 || c[3] == 2)); S.sw = c[3] == 2; if (!S.sw) S.col_open = 0; break;
    case 0x02: assert(c[1] == 0x13); H[4] = (BYTE)S.fw; H[5] = 1; H[6] = 0x10; break;
    case 0x05: assert(c[1] == 1 && c[2] == 1); if (S.ep == c[3]) S.ep = 0; break;
    case 0x0d:
        assert(c[2] != 0x1e && c[2] != 0x1d && c[2] != 0x37);
        if (c[1] == 0) { assert(c[2] == 0x22); if (S.sw) S.col_open = 1; else H[3] = 3; } else { assert(c[1] == 1); S.ep = c[2]; }
        break;
    case 0x08: assert(c[1] == 1 && S.ep == c[2]); sim_read(b[1], c[2]); break;
    case 0x06:
        if (c[1] == 0) { sim_col(c + 2, 1); break; }
        assert(c[1] == 1 && S.ep == 0x18 && c[6] == 0x07 && c[7] == 0 && b[1] == 0);   // speeds are the only endpoint write
        if (!S.sw || S.nospd) { H[3] = S.sw ? 1 : 3; break; }
        S.nspd = u16(c + 2) - 2; memcpy(S.spd, c + 8, S.nspd); S.speeds++;
        break;
    case 0x07: assert(c[1] == 0); sim_col(c + 2, 0); break;
    default: assert(!"a command the simulated hub does not know");
    }
    return 1;
}
static HANDLE open_hub(void) { return S.present ? (HANDLE)(INT_PTR)1 : INVALID_HANDLE_VALUE; }
static void let_go(void) { h = INVALID_HANDLE_VALUE; }
static void procs(scan_t *s) { for (int i = 0; i < 4; i++) if (T_procs[i]) seen_exe(T_procs[i], 100 + i, s); }
static DWORD now(void) { return T_now; }
static int nap(DWORD ms) { T_now += ms; return 0; }
static int det_file(int on) {
    static char out[1200];
    if (T_nofile) return 3;
    int r = det_edit(T_json, out, sizeof(out), on);
    if (r == 1) strcpy_s(T_json, sizeof(T_json), out);
    return r;
}
static HANDLE guard(void) { return CreateMutexW(NULL, FALSE, NULL); }

void logf_(const char *fmt, ...) { va_list a; va_start(a, fmt); fputs("  log: ", stdout); vprintf(fmt, a); va_end(a); putchar('\n'); }
int cfg_geti(const char *s, const char *k, int def) { (void)s; return !strcmp(k, "ctl") ? C_ctl : !strcmp(k, "on") ? C_on : !strcmp(k, "orgb_det") ? C_det : def; }
void cfg_set_and_save(const char *s, const char *k, const char *v) { (void)s; if (!strcmp(k, "orgb_det")) C_det = atoi(v); }
int orgb_list(orgb_ctl *out, int max) {
    if (T_orgb > 0 && max > 0) { memset(out, 0, sizeof(*out)); strcpy_s(out->name, sizeof(out->name), "Corsair iCUE Link System Hub"); }
    return T_orgb;
}
int orgbapp_ours(void) { return T_ours; }
void orgbapp_stop(void) { T_stopped++; }
void fans_link(int n, const int *chn, const int *pump, const int *rpm, int hot, int *want) {
    (void)rpm; F.calls++; F.n = n; F.hot = hot;
    if (!n) { F.released++; return; }
    for (int i = 0; i < n; i++) { F.chn[i] = chn[i]; F.pump[i] = pump[i]; want[i] = pump[i] ? F.want_pump : F.want_fan; }
}
#endif

// out: 00, h1 (0x19 for the PSU), 01, the command; the reply lands in H
static int cmd(int h1, int force, const BYTE *c, int n) {
    BYTE b[1025] = { 0 };
    b[1] = (BYTE)h1; b[2] = 1; memcpy(b + 3, c, n);
    return xfer(b, n, force);
}
#define CMD(h1, ...) cmd(h1, 0, (const BYTE[]){ __VA_ARGS__ }, sizeof((const BYTE[]){ __VA_ARGS__ }))

// Corsair's guard mutex (other programs' transfers) and iocs (this process's threads); 0: another program's
// transfer is on, try again later
static int hub_lock(void) {
    EnterCriticalSection(&iocs);
    if (!mx) return 1;
    DWORD w = WaitForSingleObject(mx, 200);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return 1;
    LeaveCriticalSection(&iocs);
    return 0;
}
static void hub_unlock(void) { if (mx) ReleaseMutex(mx); LeaveCriticalSection(&iocs); }

// close, open, read, close one endpoint as lsh.go does; a reply of the expected type in R (with its second packet if asked)
static int ep(int h1, int e, int type, int more) {
    CMD(h1, 0x05, 0x01, 0x01, (BYTE)e);
    int ok = CMD(h1, 0x0d, 0x01, (BYTE)e) && CMD(h1, 0x08, 0x01, (BYTE)e) && H[3] == 0 && H[4] == type && H[5] == 0;
    if (ok) {
        memcpy(R, H, 512); memset(R + 512, 0, sizeof(R) - 512);
        if (more && CMD(h1, 0x08, 0x01, (BYTE)e)) memcpy(R + 512, H + 4, 508);
    }
    CMD(h1, 0x05, 0x01, 0x01, (BYTE)e);
    return ok;
}
// close, open, write, close: 1 written, 3 the hub was not in software mode, 0 no answer
static int ep_write(int e, int type, const BYTE *data, int n) {
    BYTE b[510] = { 0x06, 0x01, (BYTE)(n + 2), (BYTE)((n + 2) >> 8), 0, 0, (BYTE)type, 0 };
    memcpy(b + 8, data, n);
    CMD(0, 0x05, 0x01, 0x01, (BYTE)e);
    int st = CMD(0, 0x0d, 0x01, (BYTE)e) && cmd(0, 0, b, 8 + n) ? H[3] : -1;
    CMD(0, 0x05, 0x01, 0x01, (BYTE)e);
    return st == 0 ? 1 : st == 3 ? 3 : 0;
}

static int hw_mode(void) { return cmd(0, 1, (const BYTE[]){ 0x01, 0x03, 0x00, 0x01 }, 4); }
// software mode: the colour endpoint opens again, black goes first, the last frame again
static int sw_mode(void) {
    col_open = 0; black_due = 1;
    EnterCriticalSection(&cs); dirty |= lit; LeaveCriticalSection(&cs);
    return CMD(0, 0x01, 0x03, 0x00, 0x02) && !nap(500);
}
// a write the hub turned down for its mode: software mode again and one more try; any other failure, one more try
static int put(int e, int type, const BYTE *data, int n) {
    int r = ep_write(e, type, data, n);
    if (r == 3) { logf_("link: the hub was out of software mode, back to it"); if (!sw_mode()) return 0; }
    return r == 1 || ep_write(e, type, data, n) == 1;
}

static void set_st(int s) {
    EnterCriticalSection(&cs);
    if (L.st != s && (s == L_ERR || s == L_APP || s == L_ORGB)) logf_("link: %s %s", st_name[s], s == L_APP ? L.app : "");
    L.st = s;
    LeaveCriticalSection(&cs);
}

// everything in one go, into L; devs: the device list too (software mode only)
static int read_all(int devs) {
    ldev d[CH]; int nd = L.nd, rpm[CH], t[CH], v[3] = { 0 }, a[3] = { 0 }, nr = 0, psu = 0, leds = L.leds, refuse = L.refuse;
    if (devs) { if (!ep(0, 0x36, 0x21, 1)) return 0; nd = parse_devs(R, sizeof(R), d, CH); refuse = classify(d, nd, &leds); }
    else memcpy(d, L.d, sizeof(d));
    if (!ep(0, 0x17, 0x25, 1)) return 0;
    parse_vals(R, sizeof(R), rpm);
    if (ep(0, 0x21, 0x10, 1)) parse_vals(R, sizeof(R), t); else for (int i = 0; i < CH; i++) t[i] = NONE;
    for (int i = 0; i < nd; i++) psu |= d[i].type == 11;
    if (psu && ep(0x19, 0x28, 0x23, 0)) {
        nr = parse_rails(R, v);
        if (ep(0x19, 0x29, 0x24, 0)) { int na = parse_rails(R, a); if (na < nr) nr = na; } else nr = 0;
    }
    if (refuse && refuse != L.refuse) logf_("link: channel %d has a device haku does not drive: readings only", refuse);
    EnterCriticalSection(&cs);
    memcpy(L.d, d, sizeof(d)); L.nd = nd; memcpy(L.rpm, rpm, sizeof(rpm)); memcpy(L.t, t, sizeof(t));
    memcpy(L.v, v, sizeof(v)); memcpy(L.a, a, sizeof(a)); L.nr = nr; L.seen = now(); L.refuse = refuse; L.leds = leds;
    LeaveCriticalSection(&cs);
    return 1;
}

// a pump's coolant over 57 C: everything at 100 % until every pump is under 52 (one not read keeps it on)
static int coolant(int hot) {
    int over = 0, cool = 1;
    for (int i = 0; i < L.nd; i++) {
        if (kind_of(L.d[i].type, NULL) != K_PUMP) continue;
        int t = L.t[L.d[i].ch];
        if (t != NONE && t > HOT) over = 1;
        if (t == NONE || t >= COOL) cool = 0;
    }
    return hot ? !cool : over;
}

// the fans and pumps at what fans.c wants for them: n {channel 00 % 00}, never the PSU
static int speeds(void) {
    int n = 0, chn[LK], pump[LK], rpm[LK], want[LK];
    for (int i = 0; i < L.nd && n < LK; i++) {
        int k = kind_of(L.d[i].type, NULL);
        if (k != K_FAN && k != K_PUMP) continue;
        chn[n] = L.d[i].ch; pump[n] = k == K_PUMP; rpm[n] = L.rpm[chn[n]]; want[n] = pump[n] ? 100 : 50; n++;
    }
    if (!n) return 1;
    fans_link(n, chn, pump, rpm, L.hot, want);
    BYTE b[1 + LK * 4] = { (BYTE)n };
    for (int i = 0; i < n; i++) {
        int lo = pump[i] ? PUMP_MIN : 0, p = L.hot ? 100 : want[i] < lo ? lo : want[i] > 100 ? 100 : want[i];
        b[1 + i * 4] = (BYTE)chn[i]; b[3 + i * 4] = (BYTE)p;
    }
    return put(0x18, 0x07, b, 1 + n * 4);
}

// under the hub lock
static int take(DWORD t) {
    held = 1;   // first: a switch cut short still gets hardware mode back
    EnterCriticalSection(&cs); L.held = 1; LeaveCriticalSection(&cs);
    list_at = speed_at = t; fails = sfails = 0;
    logf_("link: control of the hub taken (software mode)");
    return sw_mode();
}
static void release(const char *why) {
    held = 0; col_open = 0;
    if (!hw_mode() && !hw_mode()) { need_hw = 1; logf_("link: the hub did not take hardware mode back, trying again"); }
    else logf_("link: the hub has its own control back (%s)", why);
    fans_link(0, NULL, NULL, NULL, 0, NULL);
    EnterCriticalSection(&cs); L.held = 0; L.hot = 0; LeaveCriticalSection(&cs);
}
// from any thread (sleep, exit): without the guard mutex if it is not to be had, as this is the last chance
static void release_now(const char *why) {
    int m = hub_lock();
    if (!m) EnterCriticalSection(&iocs);
    if (held) release(why);
    if (m) hub_unlock(); else LeaveCriticalSection(&iocs);
}
// the hub thread: control given back if held, and the hub let go (close). 0: another program's transfer, next time
static int drop(int close, const char *why) {
    if (held) { if (!hub_lock()) return 0; if (held) release(why); hub_unlock(); }
    if (close) { EnterCriticalSection(&iocs); let_go(); LeaveCriticalSection(&iocs); }
    return 1;
}

// a frame in 508-byte chunks: 06 00 the first, 07 00 the rest; the colour endpoint opened once per software mode
static int col_write(const BYTE *buf, int len) {
    if (!col_open) {
        CMD(0, 0x05, 0x01, 0x01, 0x22);
        if (!CMD(0, 0x0d, 0x00, 0x22) || H[3]) return 0;
        col_open = 1;
    }
    BYTE b[510];
    for (int o = 0; o < len; o += 508) {
        int k = len - o < 508 ? len - o : 508;
        b[0] = o ? 0x07 : 0x06; b[1] = 0; memcpy(b + 2, buf + o, k);
        if (!cmd(0, 0, b, 2 + k) || H[3]) { col_open = 0; return 0; }
    }
    return 1;
}
static void colour(void) {
    static BYTE f[6 + EXT_MAX_LEDS * 3], c[EXT_MAX_LEDS * 3];
    frame_at = now();
    if (!hub_lock()) return;
    if (held && !paused && L.leds) {
        EnterCriticalSection(&cs); memcpy(c, rgb, L.leds * 3); dirty = 0; LeaveCriticalSection(&cs);
        // black first after software mode, as OpenLinkHub does (a chain of QX and RX fans shows wrong colours otherwise)
        int ok = !black_due || (col_write(f, frame(L.d, L.nd, L.fw[0], NULL, f)) && !nap(40));
        if (ok) { black_due = 0; ok = col_write(f, frame(L.d, L.nd, L.fw[0], c, f)); }
        if (!ok) { EnterCriticalSection(&cs); dirty = 1; LeaveCriticalSection(&cs); frame_at = now() + 1000; }
    }
    hub_unlock();
}
// ms until the next frame may go, -1: none to send
static int col_wait(DWORD t) {
    if (!held || paused || !L.leds) return -1;
    EnterCriticalSection(&cs); int d = dirty; LeaveCriticalSection(&cs);
    if (!d) return -1;
    int w = (int)(frame_at + FRAME_MS - t);
    return w > 0 ? w : 0;
}

static int owner(char *app, int cap, int ctl) {
    static DWORD orgb_pid; static int orgb_has;
    scan_t s = { app, cap, ctl, L_OK, 0 };
    procs(&s);
    if (s.r != L_OK || !s.orgb) return s.r;
    // OpenRGB: asked once per run whether it has the hub; while its server is not up yet, left alone (unless haku holds
    // the hub already: a copy starting with its detector off)
    if (s.orgb != orgb_pid) {
        static orgb_ctl c[64]; int n = orgb_list(c, 64);
        if (n < 0) return held ? L_OK : L_ORGB;
        orgb_pid = s.orgb; orgb_has = 0;
        for (int i = 0; i < n; i++) { char low[64]; strcpy_s(low, sizeof(low), c[i].name); _strlwr(low); orgb_has |= strstr(low, "icue link") != NULL; }
    }
    return orgb_has ? L_ORGB : L_OK;
}

// OpenRGB's iCUE LINK detector off while haku drives the hub, on again after if haku turned it off; looked at again
// every 10 s (a copy started later writes its settings then), and at once when ctl changes
static void orgb_detector(int ctl) {
    static int last = -1; static DWORD at;
    int was = cfg_geti("link", "orgb_det", 0);
    if (!ctl && was != 1) { last = ctl; return; }
    if (ctl == last && now() - at < VISIT_MS) return;
    at = now();
    int r = det_file(!ctl);
    if (!r && last != ctl) logf_("link: OpenRGB's settings could not be changed (its iCUE LINK detector)");
    last = ctl;
    if (!ctl) {
        if (r) cfg_set_and_save("link", "orgb_det", "0");
        if (r == 1) logf_("link: OpenRGB's iCUE LINK detector on again");
        return;
    }
    if (r == 1) {
        if (was != 1) cfg_set_and_save("link", "orgb_det", "1");
        logf_("link: OpenRGB's iCUE LINK detector off while haku drives the hub");
        if (orgbapp_ours()) orgbapp_stop();   // haku's own copy had it: started again without it
    } else if (r == 2 && !was) cfg_set_and_save("link", "orgb_det", "2");
}

// readings only, under the hub lock: hardware mode first; once it has answered there, no switching at all (the device
// list from the first visit)
static int visit(void) {
    if (L.nd && L.hw != 0 && read_all(0)) { if (L.hw < 0) logf_("link: reads answer in hardware mode"); L.hw = 1; set_st(L_OK); return VISIT_MS; }
    if (L.nd && !hw_tried) { hw_tried = 1; L.hw = 0; logf_("link: hardware mode reads refused, short software-mode visits"); }
#ifdef HAKU_DEV
    set_st(L_DEV);
#else
    int ok = sw_mode() && read_all(1);
    if (!hw_mode() && !hw_mode()) { need_hw = 1; logf_("link: the hub did not take hardware mode back, trying again"); }
    if (ok) set_st(L_OK); else { let_go(); set_st(L_ERR); }
#endif
    return VISIT_MS;
}

// control: readings every second, speeds every 2 s, the device list every 10 s
static int hold_step(void) {
    if (!hub_lock()) return 1000;
    if (paused) { hub_unlock(); return 1000; }
    DWORD t = now();
    int ok = held || take(t), list = (int)(t - list_at) >= 0;
    if (ok && !read_all(list)) ok = sw_mode() && read_all(list);
    if (ok && list) list_at = t + LIST_MS;
    if (ok && L.refuse) { release("a device on the chain haku does not drive"); hub_unlock(); set_st(L_OK); return VISIT_MS; }
    if (ok) {
        int hot = coolant(L.hot), was = L.hot;
        if (hot != was) {
            logf_(hot ? "link: coolant over 57 C, fans and pumps at 100 %%" : "link: coolant under 52 C again");
            EnterCriticalSection(&cs); L.hot = hot; LeaveCriticalSection(&cs);
        }
        // speeds kept apart from the readings: one refused every round would never reach three failed rounds
        if ((int)(t - speed_at) >= 0 || hot != was) { speed_at = t + SPEED_MS; sfails = speeds() ? 0 : sfails + 1; }
        if (sfails >= 2) { release("the hub did not take the speeds"); let_go(); sfails = 0; hub_unlock(); set_st(L_ERR); return VISIT_MS; }
    }
    fails = ok ? 0 : fails + 1;
    if (fails >= 3) { release("no answer"); let_go(); fails = 0; hub_unlock(); set_st(L_ERR); return VISIT_MS; }
    hub_unlock();
    if (ok) set_st(L_OK);
    return 1000;
}

// one round of the hub thread; ms until the next
static int step(void) {
    if (need_hw) {
        int ok = 0;
        if (h != INVALID_HANDLE_VALUE && hub_lock()) { ok = hw_mode(); hub_unlock(); }
        if (ok) { need_hw = 0; logf_("link: hardware mode taken"); }
        else if (h == INVALID_HANDLE_VALUE) need_hw = 0;   // the hub gets it when it is opened again
        else return 1000;
    }
    if (paused) return 1000;
    if (h == INVALID_HANDLE_VALUE) {   // opened without a word sent: only to know it is there
        EnterCriticalSection(&iocs); h = open_hub(); LeaveCriticalSection(&iocs);
        hello = h != INVALID_HANDLE_VALUE;
    }
    int c = cfg_geti("link", "ctl", 0), on = c || cfg_geti("link", "on", 0);
    orgb_detector(c);
    EnterCriticalSection(&cs); L.found = h != INVALID_HANDLE_VALUE; L.ctl = c; LeaveCriticalSection(&cs);
    if (!on) { if (!drop(1, "off")) return 1000; set_st(L_OFF); return VISIT_MS; }
    char app[40] = "";
    int who = owner(app, sizeof(app), c);
    if (who != L_OK) {
        if (!drop(1, "another program")) return 1000;
        EnterCriticalSection(&cs); strcpy_s(L.app, sizeof(L.app), app); LeaveCriticalSection(&cs);
        set_st(who);
        return VISIT_MS;
    }
    if (h == INVALID_HANDLE_VALUE) { set_st(L_NOHUB); return VISIT_MS; }
    if (hello) {
        if (!hub_lock()) return 1000;
        EnterCriticalSection(&cs); L.nd = 0; L.hw = -1; L.refuse = 0; L.leds = 0; LeaveCriticalSection(&cs);
        hw_tried = 0;
#ifndef HAKU_DEV
        hw_mode();   // left in software mode by haku closed or crashed in the middle: its own control back first
#endif
        int ok = CMD(0, 0x02, 0x13);
        if (ok) { EnterCriticalSection(&cs); L.fw[0] = H[4]; L.fw[1] = H[5]; L.fw[2] = u16(H + 6); LeaveCriticalSection(&cs); }
        else let_go();
        hub_unlock();
        if (!ok) { set_st(L_ERR); return VISIT_MS; }
        hello = 0;
        logf_("link: iCUE LINK hub, firmware %d.%d.%d", L.fw[0], L.fw[1], L.fw[2]);
    }
    if (c && CAN_HOLD && !L.refuse) return hold_step();
    if (!drop(0, "readings only")) return 1000;
    if (!hub_lock()) return 1000;
    int r = visit();
    hub_unlock();
    return r;
}

// the thread's end: control back, hardware mode if a visit left it out; 1 when the hub did not take it
static int finish(void) {
    int left = 0;
    release_now("exit");
    EnterCriticalSection(&iocs);
    if (need_hw && h != INVALID_HANDLE_VALUE && !hw_mode()) { left = 1; logf_("link: hardware mode not taken on exit: iCUE or a restart gives the hub its control back"); }
    let_go();
    LeaveCriticalSection(&iocs);
    // OpenRGB's detector on again if haku turned it off: an uninstall or an old backup must not leave it off;
    // orgb_det stays 1, so the next start with control on turns it off again
    if (cfg_geti("link", "orgb_det", 0) == 1 && det_file(1) == 1) logf_("link: OpenRGB's iCUE LINK detector on again (exit)");
    return left;
}

static unsigned __stdcall link_thread(void *arg) {
    (void)arg;
    for (DWORD due = now();;) {
        DWORD t = now();
        int wc = col_wait(t), ws = (int)(due - t), w;
        if (ws < 0) ws = 0;
        w = wc >= 0 && wc < ws ? wc : ws;
        HANDLE e[2] = { stop_ev, col_ev };
        if (WaitForMultipleObjects(2, e, FALSE, (DWORD)w) == WAIT_OBJECT_0) break;
        t = now();
        if (col_wait(t) == 0) colour();
        if ((int)(t - due) >= 0) due = now() + step();
    }
    return (unsigned)finish();
}

static void init(void) {
    InitializeCriticalSection(&cs); InitializeCriticalSection(&iocs);
    L.hw = -1;
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL); cut_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    col_ev = CreateEventW(NULL, FALSE, FALSE, NULL); ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    mx = guard();
    started = 1;
}
void link_start(void) {
    init();
    th = (HANDLE)_beginthreadex(NULL, 0, link_thread, NULL, 0, NULL);
}

// 0: the hub has its own control (or was never visited); 1: it did not take it back; -1: the thread did not end in time
int link_stop(void) {
    if (!th) return 0;
    SetEvent(stop_ev);
    DWORD code = 0;
    int r = WaitForSingleObject(th, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) ? (int)code : -1;
    CloseHandle(th); th = NULL;
    // stuck (a transfer, OpenRGB's list): hardware mode from here, as on a crash; the thread writes nothing more
    if (r < 0) { link_crash(); logf_(held ? "link: did not end in time, the hub's control back is not confirmed" : "link: did not end in time; the hub's control was given back from the exit instead"); }
    return r;
}

// sleep: the hub's own control back now, nothing sent until resume
void link_suspend(int on) {
    if (!started) return;
    if (on) { paused = 1; SetEvent(cut_ev); release_now("sleep"); }
    else { ResetEvent(cut_ev); paused = 0; }
}

// a crash: hardware mode back if haku held the hub, best effort (no guard mutex: the hub thread may be the one stuck)
void link_crash(void) {
    if (!started) return;
    paused = 1; SetEvent(cut_ev);
    BOOL in = FALSE;
    for (int i = 0; i < 20 && !(in = TryEnterCriticalSection(&iocs)); i++) Sleep(100);
    if (!in) return;
    if (held) { held = 0; hw_mode(); }
    LeaveCriticalSection(&iocs);
}

int link_json(char *out, int cap) {
    if (!started) return jcat(out, cap, 0, "{\"st\":\"off\",\"found\":0}");
    EnterCriticalSection(&cs);
    int n = jcat(out, cap, 0, "{\"st\":\"%s\",\"found\":%d,\"app\":\"%s\",\"fw\":\"%d.%d.%d\",\"hw\":%d,\"age\":%lu,\"ctl\":%d,\"held\":%d,\"refuse\":%d,\"hot\":%d,\"leds\":%d,\"dev\":[",
                     st_name[L.st], L.found, L.app, L.fw[0], L.fw[1], L.fw[2], L.hw, L.seen ? now() - L.seen : 0, L.ctl, L.held, L.refuse, L.hot, L.leds);
    for (int i = 0; i < L.nd; i++) {
        int c = L.d[i].ch, r = L.rpm[c], t = L.t[c], l, k = kind_of(L.d[i].type, &l);
        n = jcat(out, cap, n, "%s{\"ch\":%d,\"type\":%d,\"model\":%d,\"kind\":\"%s\",\"leds\":%d,\"rpm\":", i ? "," : "", c, L.d[i].type, L.d[i].model, kind_name[k], l);
        n = r == NONE ? jcat(out, cap, n, "null") : jcat(out, cap, n, "%d", r);
        n = t == NONE ? jcat(out, cap, n, ",\"t\":null}") : jcat(out, cap, n, ",\"t\":%.1f}", t / 10.0);
    }
    n = jcat(out, cap, n, "],\"hub_t\":");
    n = L.nd && L.t[0] != NONE ? jcat(out, cap, n, "%.1f", L.t[0] / 10.0) : jcat(out, cap, n, "null");
    n = jcat(out, cap, n, ",\"v\":[");
    for (int i = 0; i < L.nr; i++) n = jcat(out, cap, n, "%s%.2f", i ? "," : "", L.v[i] / 1000.0);
    n = jcat(out, cap, n, "],\"a\":[");
    for (int i = 0; i < L.nr; i++) n = jcat(out, cap, n, "%s%.2f", i ? "," : "", L.a[i] / 100.0);
    n = jcat(out, cap, n, "]}");
    LeaveCriticalSection(&cs);
    return n;
}

// the chain's LEDs as one light device, while haku holds the hub; frames go out on the hub thread
static int ic_open(ext_dev *d) {
    if (!started) return 0;
    EnterCriticalSection(&cs);
    int n = L.held ? L.leds : 0;
    const char *why = !L.ctl ? "control is off (Fans page)" : L.st == L_APP || L.st == L_ORGB ? "another program has the hub"
                    : !L.held ? "the hub is not ready" : "no LEDs haku can light on this chain";
    if (n) { lit = 1; dirty = 1; }
    LeaveCriticalSection(&cs);
    if (!n) { snprintf(d->info, sizeof(d->info), "iCUE LINK · %s", why); return 0; }
    d->nleds = n; d->priv = (void *)(INT_PTR)n;
    snprintf(d->info, sizeof(d->info), "iCUE LINK · %d LEDs", n);
    SetEvent(col_ev);
    return 1;
}
static int ic_send(ext_dev *d, const rgbf *c, int n) {
    BYTE b[EXT_MAX_LEDS * 3] = { 0 };
    for (int i = 0; i < n && i < EXT_MAX_LEDS; i++) { b[i * 3] = to8(c[i].r); b[i * 3 + 1] = to8(c[i].g); b[i * 3 + 2] = to8(c[i].b); }
    EnterCriticalSection(&cs);
    int m = L.leds, ok = L.held && m == (int)(INT_PTR)d->priv && n <= m;   // a chain that changed: opened again
    if (ok && memcmp(rgb, b, m * 3)) { memcpy(rgb, b, m * 3); dirty = 1; SetEvent(col_ev); }
    LeaveCriticalSection(&cs);
    return ok;
}
// software mode has no colours of the hub's own to give back: black, unless kept as it is
static void ic_leave(ext_dev *d, int how) {
    (void)d;
    if (how == LEAVE_KEEP) return;
    EnterCriticalSection(&cs); memset(rgb, 0, sizeof(rgb)); dirty = 1; LeaveCriticalSection(&cs);
    SetEvent(col_ev);
}
static void ic_close(ext_dev *d) {
    EnterCriticalSection(&cs); lit = 0; LeaveCriticalSection(&cs);
    d->priv = NULL;
}
static void ic_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    if (!started) return;
    disc_t x = { "icuelink", "hub", -1, "iCUE LINK" };
    EnterCriticalSection(&cs); x.nleds = L.ctl ? L.leds : 0; LeaveCriticalSection(&cs);
    if (!x.nleds) return;
    snprintf(x.info, sizeof(x.info), "iCUE LINK · %d LEDs", x.nleds);
    found(&x);
}
const ext_driver drv_icuelink = { "icuelink", "Corsair iCUE LINK", 30, 1, ic_open, ic_send, ic_leave, ic_close, ic_discover };

#ifdef LINK_TEST   // cl /DLINK_TEST src\link.c: the parsers on replies from a real hub, the rest on the simulated one
static int hex(const char *s, BYTE *b) { int n = 0; unsigned v; while (sscanf(s, " %2x", &v) == 1) { b[n++] = (BYTE)v; s += 3; } return n; }
static int has(const char *s) { return strstr(S.log, s) != NULL; }
static void clear_log(void) { S.nlog = 0; S.log[0] = 0; }
static void chain(const int *ch, const int *type, int n) { S.nd = n; for (int i = 0; i < n; i++) { S.d[i].ch = ch[i]; S.d[i].type = type[i]; S.d[i].model = 0; } }
static int tick(int ms) { T_now += ms; return step(); }
static ldev D[16];
static int devs(const int *ch, const int *type, int n) { for (int i = 0; i < n; i++) { D[i].ch = ch[i]; D[i].type = type[i]; D[i].model = 0; } return n; }
static HANDLE hog_go, hog_ready;
static DWORD WINAPI hog(void *p) { (void)p; WaitForSingleObject(mx, INFINITE); SetEvent(hog_ready); WaitForSingleObject(hog_go, INFINITE); ReleaseMutex(mx); return 0; }

int main(void) {
    static BYTE r[1020]; ldev d[CH]; int v[CH], rails[3];
    // the device list: 2 fans, an empty channel, the PSU (serials shortened to 2 bytes)
    hex("00 00 08 00 21 00 04 00 00 0F 00 00 00 05 02 30 31 00 00 00 00 00 00 00 00 00 00 0B 00 00 00 05 02 41 42", r);
    int n = parse_devs(r, sizeof(r), d, CH);
    assert(n == 2 && d[0].ch == 1 && d[0].type == 15 && d[1].ch == 3 && d[1].type == 11 && d[1].model == 0);
    memset(r, 0, sizeof(r));
    hex("00 00 08 00 25 00 1A 01 00 00 00 37 01 01 00 00", r);
    parse_vals(r, sizeof(r), v);
    assert(v[0] == NONE && v[1] == 311 && v[2] == NONE && v[30] == NONE);
    hex("00 00 08 00 10 00 34 00 00 01 01 00 00 01 00 00", r);
    parse_vals(r, sizeof(r), v);
    assert(v[0] == 256 && v[1] == NONE);
    hex("19 00 08 00 23 00 03 00 02 00 F0 0C 01 00 B6 13 00 00 FF 2E 00 00", r);
    assert(parse_rails(r, rails) == 3 && rails[0] == 3312 && rails[1] == 5046 && rails[2] == 12031);
    hex("19 00 08 00 24 00 03 00 05 00 AF 00 04 00 26 02 03 00 01 04 00 00", r);
    assert(parse_rails(r, rails) == 3 && rails[0] == 175 && rails[1] == 550 && rails[2] == 1025);

    // what a chain allows: an unknown type, the XD5 ELITE LCD (14) and a fan over channel 24 refuse (a PSU there does not); an LCD cap or too many
    // LEDs only take the colours away
    int leds;
    assert(classify(D, devs((int[]){ 1, 2, 3 }, (int[]){ 1, 17, 9 }, 3), &leds) == 0 && leds == 34 + 20 + 24);
    assert(classify(D, devs((int[]){ 1, 4 }, (int[]){ 1, 14 }, 2), &leds) == 4);
    assert(classify(D, devs((int[]){ 1, 2 }, (int[]){ 1, 99 }, 2), &leds) == 2);
    assert(classify(D, devs((int[]){ 1, 25 }, (int[]){ 1, 1 }, 2), &leds) == 25);
    assert(classify(D, devs((int[]){ 1, 25 }, (int[]){ 1, 11 }, 2), &leds) == 0 && leds == 34);
    assert(classify(D, devs((int[]){ 1, 2 }, (int[]){ 7, 6 }, 2), &leds) == 0 && leds == 0);
    assert(classify(D, devs((int[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, (int[]){ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, 16), &leds) == 0 && leds == 0);

    // frames: the port with over 238 LEDs dimmed to 0.66; firmware 1 starts the second port at channel 7
    static BYTE f[6 + EXT_MAX_LEDS * 3], c[EXT_MAX_LEDS * 3];
    memset(c, 200, sizeof(c));
    n = frame(D, devs((int[]){ 1, 2, 3, 4, 5, 6, 7, 8 }, (int[]){ 1, 1, 1, 1, 1, 1, 1, 1 }, 8), 2, c, f);
    assert(n == 6 + 272 * 3 && u16(f) == 272 * 3 + 2 && f[2] == 0 && f[4] == 0x12 && f[5] == 0 && f[6] == 132 && f[n - 1] == 132);
    n = frame(D, 8, 1, c, f);   // 6 fans (204 LEDs) on one port, 2 on the other: neither dimmed
    assert(f[6] == 200 && f[n - 1] == 200);
    n = frame(D, 8, 2, NULL, f);
    assert(n == 6 + 816 && f[6] == 0 && f[400] == 0);

    // OpenRGB's settings
    static char o[512];
    assert(det_edit("{\"Detectors\":{\"detectors\":{\"A\":true,\"Corsair iCUE Link System Hub\": true}}}", o, sizeof(o), 0) == 1
           && !strcmp(o, "{\"Detectors\":{\"detectors\":{\"A\":true,\"Corsair iCUE Link System Hub\": false}}}"));
    assert(det_edit(o, (char *)r, sizeof(r), 0) == 2 && det_edit(o, (char *)r, sizeof(r), 1) == 1 && strstr((char *)r, "Hub\": true}"));
    assert(det_edit("{\"Detectors\":{\"detectors\":{\"A\":true}},\"x\":1}", o, sizeof(o), 0) == 1
           && !strcmp(o, "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false,\"A\":true}},\"x\":1}"));
    assert(det_edit("{\"Detectors\":{\"detectors\":{ }}}", o, sizeof(o), 0) == 1
           && !strcmp(o, "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false{ }}}") == 0);
    assert(!strcmp(o, "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false }}}"));
    assert(det_edit("{\"Theme\":1}", o, sizeof(o), 0) == 1 && !strcmp(o, "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false}},\"Theme\":1}"));
    assert(det_edit(" { } ", o, sizeof(o), 0) == 1 && !strcmp(o, " {\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false}} } "));
    assert(det_edit("", o, sizeof(o), 0) == 1 && !strcmp(o, "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false}}}"));
    assert(det_edit("{\"Theme\":1}", o, sizeof(o), 1) == 2 && det_edit("garbage", o, sizeof(o), 0) == 0);
    assert(det_edit("{\"Corsair iCUE Link System Hub\":null}", o, sizeof(o), 0) == 0 && det_edit("{\"Theme\":1}", o, 20, 0) == 0);

    // control on a chain: 2 QX fans, a TITAN AIO (pump), an XC7 block, the PSU; fans at 40 %, the pump asked for 30
    init();
    S.present = 1; S.fw = 2; F.want_fan = 40; F.want_pump = 30;
    chain((int[]){ 1, 2, 3, 5, 7 }, (int[]){ 1, 1, 17, 9, 11 }, 5);
    S.rpm[1] = 900; S.rpm[2] = 910; S.rpm[3] = 2400; S.t[0] = 300; S.t[3] = 350;
    strcpy_s(T_json, sizeof(T_json), "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":true}}}");
    T_ours = 1; C_ctl = 1;
    assert(step() == 1000 && S.sw && held && L.held && L.st == L_OK && S.speeds == 1 && L.leds == 112 && !L.refuse);
    assert(has("01 03 00 01\n02 13\n01 03 00 02\n"));   // hardware mode first (a crash may have left it), firmware, software
    assert(has("05 01 01 18\n0D 01 18\n06 01 0F 00 00 00 07 00 03 01 00 28 00 02 00 28 00 03 00 32 00\n05 01 01 18\n"));
    assert(F.n == 3 && F.chn[2] == 3 && F.pump[2] && !F.pump[0] && !F.hot);
    assert(C_det == 1 && T_stopped == 1 && strstr(T_json, "Hub\":false"));
    assert(tick(1000) == 1000 && S.speeds == 1 && tick(1000) == 1000 && S.speeds == 2);   // speeds every 2 s
    char js[4096];
    link_json(js, sizeof(js));
    assert(strstr(js, "\"ctl\":1,\"held\":1,\"refuse\":0,\"hot\":0,\"leds\":112") && strstr(js, "\"ch\":3,\"type\":17,\"model\":0,\"kind\":\"pump\",\"leds\":20,\"rpm\":2400,\"t\":35.0"));
    assert(strstr(js, "\"kind\":\"psu\"") && strstr(js, "\"v\":[3.31,5.05,12.03]"));

    // colours: black first, then the frame in one chunk; the same frame again is not sent
    ext_dev dv; memset(&dv, 0, sizeof(dv));
    assert(drv_icuelink.open(&dv) && dv.nleds == 112);
    rgbf red[EXT_MAX_LEDS]; for (int i = 0; i < EXT_MAX_LEDS; i++) red[i] = (rgbf){ 1, 0, 0 };
    assert(drv_icuelink.send(&dv, red, 112) && col_wait(now()) == 0);
    clear_log(); colour();
    assert(S.frames == 2 && S.blacks == 1 && S.nframe == 6 + 336 && S.fr[0] == 0x52 && S.fr[1] == 0x01 && S.fr[4] == 0x12 && S.fr[6] == 255 && S.fr[7] == 0);
    assert(has("05 01 01 22\n0D 00 22\n06 00 52 01 00 00 12 00 00 00 00") && !has("07 00"));
    assert(drv_icuelink.send(&dv, red, 112) && col_wait(now()) == -1);
    assert(drv_icuelink.send(&dv, red, 100));   // fewer LEDs set by hand: the rest stay black

    // coolant over 57 C: everything at 100 % at once; on until under 52
    S.t[3] = 580; tick(1000);
    assert(L.hot && F.hot && S.spd[0] == 3 && S.spd[3] == 100 && S.spd[7] == 100 && S.spd[11] == 100);
    S.t[3] = 540; tick(2000); assert(L.hot && S.spd[3] == 100);
    S.t[3] = 510; tick(1000); assert(!L.hot && S.spd[3] == 40 && S.spd[11] == 50);

    // the hub fell back to hardware mode: software mode again and the speeds resent; the colours start with black again
    int sp = S.speeds; S.sw = 0;
    tick(2000); assert(S.sw && S.speeds == sp + 1);
    red[0].g = 1; drv_icuelink.send(&dv, red, 112); colour(); assert(S.frames == 4 && S.blacks == 2 && S.fr[7] == 255);

    // iCUE starts: control back, fans.c told, colours turned down; it ends: taken again
    T_procs[0] = L"iCUE.exe";
    tick(1000); assert(!S.sw && !held && !L.held && L.st == L_APP && F.released == 1 && h == INVALID_HANDLE_VALUE);
    assert(!drv_icuelink.send(&dv, red, 112) && !drv_icuelink.open(&dv) && strstr(dv.info, "another program"));
    T_procs[0] = NULL; tick(10000); assert(S.sw && held && L.st == L_OK);
    // FanControl counts only for control; OpenRGB with the hub counts always
    T_procs[0] = L"FanControl.exe"; tick(1000); assert(!S.sw && L.st == L_APP);
    T_procs[0] = L"OpenRGB.exe"; T_orgb = 1; C_ctl = 0; C_on = 1; tick(10000); assert(L.st == L_ORGB && !S.sw);
    T_procs[0] = NULL; C_ctl = 1; C_on = 0; tick(10000); assert(S.sw && held);

    // sleep: control back at once, nothing sent while asleep, taken again after
    link_suspend(1); assert(!S.sw && !held && F.released == 3);
    clear_log(); assert(tick(2000) == 1000 && S.nlog == 0);
    link_suspend(0); tick(1000); assert(S.sw && held);
    // a crash
    link_crash(); assert(!S.sw && !held);
    paused = 0; ResetEvent(cut_ev); tick(1000); assert(S.sw && held);

    // control switched off: hardware mode, OpenRGB's detector on again
    C_ctl = 0; tick(1000);
    assert(!S.sw && !held && L.st == L_OFF && C_det == 0 && strstr(T_json, "Hub\":true"));
    // already off in OpenRGB: noted, so it stays off after
    strcpy_s(T_json, sizeof(T_json), "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":false}}}");
    C_ctl = 1; tick(10000); assert(held && C_det == 2);
    C_ctl = 0; tick(1000); assert(!held && C_det == 2 && strstr(T_json, "Hub\":false"));

    // exit: hardware mode; and when the hub does not answer, the exit code says so
    C_ctl = 1; C_det = 0; tick(10000); assert(held);
    assert(finish() == 0 && !S.sw && h == INVALID_HANDLE_VALUE);
    tick(10000); assert(held && S.sw);
    // haku turned OpenRGB's detector off: on again at exit, off again on the next start
    strcpy_s(T_json, sizeof(T_json), "{\"Detectors\":{\"detectors\":{\"Corsair iCUE Link System Hub\":true}}}");
    tick(10000); assert(C_det == 1 && strstr(T_json, "Hub\":false"));
    assert(finish() == 0 && C_det == 1 && strstr(T_json, "Hub\":true"));
    tick(10000); assert(held && strstr(T_json, "Hub\":false"));
    S.dead = 1; assert(finish() == 1 && h == INVALID_HANDLE_VALUE); S.dead = 0;

    // a device haku does not drive: given back before any speed is written, readings only after
    chain((int[]){ 1, 4 }, (int[]){ 1, 14 }, 2); sp = S.speeds;
    tick(10000);
    assert(!S.sw && !held && L.refuse == 4 && S.speeds == sp && L.st == L_OK && need_hw == 0);
    tick(10000); assert(!S.sw && !held && S.speeds == sp && L.hw == 1);

    // another program's transfer holds Corsair's mutex: nothing is sent until it is done
    hog_go = CreateEventW(NULL, TRUE, FALSE, NULL); hog_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE ht = CreateThread(NULL, 0, hog, NULL, 0, NULL);
    WaitForSingleObject(hog_ready, INFINITE);
    clear_log(); assert(tick(10000) == 1000 && S.nlog == 0);
    SetEvent(hog_go); WaitForSingleObject(ht, INFINITE); CloseHandle(ht);

    // three rounds without readings: control back, the hub let go, an error
    chain((int[]){ 1, 2, 3, 5, 7 }, (int[]){ 1, 1, 17, 9, 11 }, 5);
    EnterCriticalSection(&iocs); let_go(); LeaveCriticalSection(&iocs);
    tick(1000); assert(held && S.sw);
    S.noread = 1; tick(1000); tick(1000); assert(held); tick(1000);
    assert(!held && !S.sw && L.st == L_ERR && h == INVALID_HANDLE_VALUE);
    S.noread = 0; tick(10000); assert(held && L.st == L_OK);
    // readings go through but the speeds are refused: control back after two refused rounds, not held on readings alone
    S.nospd = 1; tick(2000); assert(held); clear_log(); tick(2000);
    assert(!held && !S.sw && L.st == L_ERR && has("01 03 00 01\n") && h == INVALID_HANDLE_VALUE);
    S.nospd = 0; tick(10000); assert(held && L.st == L_OK);

    // a chain with more LEDs: the device opens again with the new count; the frame goes in two chunks, dimmed
    chain((int[]){ 1, 2, 3, 4, 5, 6, 7, 8 }, (int[]){ 1, 1, 1, 1, 1, 1, 1, 1 }, 8);
    tick(10000); assert(L.leds == 272 && !drv_icuelink.send(&dv, red, 112));
    drv_icuelink.close(&dv); assert(drv_icuelink.open(&dv) && dv.nleds == 272);
    rgbf white[EXT_MAX_LEDS]; for (int i = 0; i < EXT_MAX_LEDS; i++) white[i] = (rgbf){ 1, 1, 1 };
    clear_log(); assert(drv_icuelink.send(&dv, white, 272)); colour();
    assert(S.nframe == 6 + 816 && S.fr[6] == 168 && S.fr[6 + 815] == 168 && has("\n07 00 "));
    // let go as off: black after close
    drv_icuelink.leave(&dv, LEAVE_OFF); drv_icuelink.close(&dv); T_now += 100; colour();
    assert(S.fr[6] == 0 && S.fr[6 + 815] == 0 && col_wait(now()) == -1);

    // nothing written that the hub keeps (xfer asserts it on every command)
    puts("link: ok");
    return 0;
}
#endif
