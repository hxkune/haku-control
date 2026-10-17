// SPDX-License-Identifier: GPL-3.0-only
// iCUE LINK System Hub (USB 1B1C:0C3F): what is on its LINK chain, its fan and pump speeds, temperatures and an HXi
// SHIFT PSU's rails, read only. Bytes as in OpenLinkHub src/devices/lsh/lsh.go (GPL-3.0). Nothing that sets a speed,
// a colour or anything kept on the hub is ever sent: only reads and the software/hardware mode switch.
// The hub answers its device list only in software mode, where its own fan and pump control waits for the host. So
// that is a short visit (about a second, every 10 s), hardware mode goes right after it, again until the hub takes it,
// and when the hub is next opened (haku closed in the middle of one). Reads that answer in hardware mode stay there.
// Never while iCUE, OpenLinkHub or SignalRGB runs, or OpenRGB lights the hub: they own it then.
// [link] on = 1 to read it (off by default)
#include "common.h"
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <tlhelp32.h>
#include <process.h>
#include <limits.h>

#define VISIT_MS 10000
#define CH 64             // channels a reading may name
#define NONE INT_MIN

enum { L_OFF, L_NOHUB, L_APP, L_ORGB, L_OK, L_ERR, L_DEV };
static const char *const st_name[] = { "off", "nohub", "app", "orgb", "ok", "err", "dev" };
typedef struct { int ch, type, model; } ldev;

static CRITICAL_SECTION cs;
static struct { int st, found, fw[3], nd, nr, hw; DWORD seen; char app[40]; ldev d[CH]; int rpm[CH], t[CH], v[3], a[3]; } L;
static HANDLE th, stop_ev, ev, h = INVALID_HANDLE_VALUE;
static int paused, out_len, in_len;
static BYTE H[512], R[1020];   // H: the last reply; R: an endpoint's reply with its second packet

static int u16(const BYTE *p) { return p[0] | p[1] << 8; }

// the device list (endpoint 36): channel, type and model of each device; an empty channel has no serial
static int parse_devs(const BYTE *r, int len, ldev *d, int max) {
    int n = 0;
    for (int ch = 1, pos = 7; ch <= r[6] && pos + 8 <= len && n < max; ch++) {
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

#ifndef LINK_TEST
// one report out or in; stop cuts it short unless it is the hardware mode on the way out
static int io(BYTE *b, int wr, int force) {
    OVERLAPPED ov = { 0 }; ov.hEvent = ev; ResetEvent(ev);
    DWORD got = 0, n = wr ? out_len : in_len;
    if (!(wr ? WriteFile(h, b, n, &got, &ov) : ReadFile(h, b, n, &got, &ov)) && GetLastError() != ERROR_IO_PENDING) return 0;
    HANDLE w[2] = { ev, stop_ev };
    if (WaitForMultipleObjects(force ? 1 : 2, w, FALSE, 1000) != WAIT_OBJECT_0) CancelIo(h);
    return GetOverlappedResult(h, &ov, &got, TRUE) && got == n;
}
// out: 00, h1 (0x19 for the PSU), 01, the command; the reply lands in H
static int cmd(int h1, int force, const BYTE *c, int n) {
    BYTE b[1025] = { 0 };
    b[1] = (BYTE)h1; b[2] = 1; memcpy(b + 3, c, n);
    HidD_FlushQueue(h);
    if (!io(b, 1, force)) return 0;
    memset(b, 0, sizeof(b));
    if (!io(b, 0, force)) return 0;
    memcpy(H, b + 1, sizeof(H));
    return 1;
}
#define CMD(h1, ...) cmd(h1, 0, (const BYTE[]){ __VA_ARGS__ }, sizeof((const BYTE[]){ __VA_ARGS__ }))

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

static int hw_mode(void) { return cmd(0, 1, (const BYTE[]){ 0x01, 0x03, 0x00, 0x01 }, 4); }

// everything in one go, into L; devs: the device list too (software mode only)
static int read_all(int devs) {
    ldev d[CH]; int nd = L.nd, rpm[CH], t[CH], v[3] = { 0 }, a[3] = { 0 }, nr = 0, psu = 0;
    if (devs) { if (!ep(0, 0x36, 0x21, 1)) return 0; nd = parse_devs(R, sizeof(R), d, CH); }
    else memcpy(d, L.d, sizeof(d));
    if (!ep(0, 0x17, 0x25, 1)) return 0;
    parse_vals(R, sizeof(R), rpm);
    if (ep(0, 0x21, 0x10, 1)) parse_vals(R, sizeof(R), t); else for (int i = 0; i < CH; i++) t[i] = NONE;
    for (int i = 0; i < nd; i++) psu |= d[i].type == 11;
    if (psu && ep(0x19, 0x28, 0x23, 0)) {
        nr = parse_rails(R, v);
        if (ep(0x19, 0x29, 0x24, 0)) { int na = parse_rails(R, a); if (na < nr) nr = na; } else nr = 0;
    }
    EnterCriticalSection(&cs);
    memcpy(L.d, d, sizeof(d)); L.nd = nd; memcpy(L.rpm, rpm, sizeof(rpm)); memcpy(L.t, t, sizeof(t));
    memcpy(L.v, v, sizeof(v)); memcpy(L.a, a, sizeof(a)); L.nr = nr; L.seen = GetTickCount();
    LeaveCriticalSection(&cs);
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

// a program that drives the hub itself, by exe name; "" when none
static int owner(char *app, int cap) {
    static DWORD orgb_pid; static int orgb_has;
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return L_OK;
    int r = L_OK; DWORD orgb = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && r == L_OK; ok = Process32NextW(snap, &pe)) {
        const wchar_t *x = pe.szExeFile;
        if ((!_wcsnicmp(x, L"icue", 4) && _wcsicmp(x, L"iCUEUpdateService.exe")) || !_wcsicmp(x, L"Corsair.Service.exe")
            || !_wcsicmp(x, L"OpenLinkHub.exe") || !_wcsicmp(x, L"SignalRgb.exe")) {
            r = L_APP; WideCharToMultiByte(CP_UTF8, 0, x, -1, app, cap, NULL, NULL);
        }
        if (!_wcsicmp(x, L"OpenRGB.exe")) orgb = pe.th32ProcessID;
    }
    CloseHandle(snap);
    if (r != L_OK || !orgb) return r;
    // OpenRGB: asked once per run whether it has the hub; while its server is not up yet, left alone
    if (orgb != orgb_pid) {
        static orgb_ctl c[64]; int n = orgb_list(c, 64);
        if (n < 0) return L_ORGB;
        orgb_pid = orgb; orgb_has = 0;
        for (int i = 0; i < n; i++) { char low[64]; strcpy_s(low, sizeof(low), c[i].name); _strlwr(low); orgb_has |= strstr(low, "icue link") != NULL; }
    }
    return orgb_has ? L_ORGB : L_OK;
}

static void set_st(int s) {
    EnterCriticalSection(&cs);
    if (L.st != s && (s == L_ERR || s == L_APP || s == L_ORGB)) logf_("link: %s %s", st_name[s], s == L_APP ? L.app : "");
    L.st = s;
    LeaveCriticalSection(&cs);
}

static void let_go(void) { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }

static unsigned __stdcall link_thread(void *arg) {
    (void)arg;
    int need_hw = 0, hw_tried = 0;   // need_hw: a visit ended without the hub taking hardware mode back
    for (DWORD wait = 0; WaitForSingleObject(stop_ev, wait) != WAIT_OBJECT_0; wait = need_hw ? 1000 : VISIT_MS) {
        if (need_hw) {
            if (h != INVALID_HANDLE_VALUE && hw_mode()) { need_hw = 0; logf_("link: hardware mode taken"); }
            else { if (h == INVALID_HANDLE_VALUE) need_hw = 0; continue; }
        }
        if (paused) continue;
        int fresh = h == INVALID_HANDLE_VALUE;
        if (fresh) h = open_hub();   // opened without a word sent: only to know it is there
        EnterCriticalSection(&cs); L.found = h != INVALID_HANDLE_VALUE; LeaveCriticalSection(&cs);
        if (!cfg_geti("link", "on", 0)) { let_go(); set_st(L_OFF); continue; }
        char app[40] = "";
        int who = owner(app, sizeof(app));
        if (who != L_OK) { let_go(); EnterCriticalSection(&cs); strcpy_s(L.app, sizeof(L.app), app); LeaveCriticalSection(&cs); set_st(who); continue; }
        if (h == INVALID_HANDLE_VALUE) { set_st(L_NOHUB); continue; }
        if (fresh) {
            EnterCriticalSection(&cs); L.nd = 0; L.hw = -1; LeaveCriticalSection(&cs);
            hw_tried = 0;
#ifndef HAKU_DEV
            hw_mode();   // left in software mode by a visit cut short (haku closed or crashed): its own control back first
#endif
            if (!CMD(0, 0x02, 0x13)) { let_go(); set_st(L_ERR); continue; }
            EnterCriticalSection(&cs); L.fw[0] = H[4]; L.fw[1] = H[5]; L.fw[2] = u16(H + 6); LeaveCriticalSection(&cs);
            logf_("link: iCUE LINK hub, firmware %d.%d.%d", L.fw[0], L.fw[1], L.fw[2]);
        }
        // hardware mode first: once it has answered there, no switching at all (the device list from the first visit)
        if (L.nd && L.hw != 0 && read_all(0)) { if (L.hw < 0) logf_("link: reads answer in hardware mode"); L.hw = 1; set_st(L_OK); continue; }
        if (L.nd && !hw_tried) { hw_tried = 1; L.hw = 0; logf_("link: hardware mode reads refused, short software-mode visits"); }
#ifdef HAKU_DEV
        set_st(L_DEV);   // the test build does not switch the hub's mode
#else
        int ok = CMD(0, 0x01, 0x03, 0x00, 0x02);
        if (ok && WaitForSingleObject(stop_ev, 500) != WAIT_OBJECT_0) ok = read_all(1);
        if (!hw_mode() && !hw_mode()) { need_hw = 1; logf_("link: the hub did not take hardware mode back, trying again"); }
        if (ok) set_st(L_OK); else { let_go(); set_st(L_ERR); }
#endif
    }
#ifndef HAKU_DEV
    if (need_hw && h != INVALID_HANDLE_VALUE && !hw_mode()) logf_("link: hardware mode not taken on exit: iCUE or a restart gives the hub its control back");
#endif
    let_go();
    return 0;
}

void link_start(void) {
    InitializeCriticalSection(&cs);
    L.hw = -1;
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL); ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, link_thread, NULL, 0, NULL);
}

void link_stop(void) {
    if (!th) return;
    SetEvent(stop_ev);
    WaitForSingleObject(th, 5000);
    CloseHandle(th); th = NULL;
}

// sleep: no visits (one cut short by sleep would leave the hub in software mode)
void link_suspend(int on) { paused = on; }

int link_json(char *out, int cap) {
    if (!th) return snprintf(out, cap, "{\"st\":\"off\",\"found\":0}");
    EnterCriticalSection(&cs);
    int n = snprintf(out, cap, "{\"st\":\"%s\",\"found\":%d,\"app\":\"%s\",\"fw\":\"%d.%d.%d\",\"hw\":%d,\"age\":%lu,\"dev\":[",
                     st_name[L.st], L.found, L.app, L.fw[0], L.fw[1], L.fw[2], L.hw, L.seen ? GetTickCount() - L.seen : 0);
    for (int i = 0; i < L.nd; i++) {
        int c = L.d[i].ch, r = L.rpm[c], t = L.t[c];
        n += snprintf(out + n, cap - n, "%s{\"ch\":%d,\"type\":%d,\"model\":%d,\"rpm\":", i ? "," : "", c, L.d[i].type, L.d[i].model);
        n += r == NONE ? snprintf(out + n, cap - n, "null") : snprintf(out + n, cap - n, "%d", r);
        n += t == NONE ? snprintf(out + n, cap - n, ",\"t\":null}") : snprintf(out + n, cap - n, ",\"t\":%.1f}", t / 10.0);
    }
    n += snprintf(out + n, cap - n, "],\"hub_t\":");
    n += L.nd && L.t[0] != NONE ? snprintf(out + n, cap - n, "%.1f", L.t[0] / 10.0) : snprintf(out + n, cap - n, "null");
    n += snprintf(out + n, cap - n, ",\"v\":[");
    for (int i = 0; i < L.nr; i++) n += snprintf(out + n, cap - n, "%s%.2f", i ? "," : "", L.v[i] / 1000.0);
    n += snprintf(out + n, cap - n, "],\"a\":[");
    for (int i = 0; i < L.nr; i++) n += snprintf(out + n, cap - n, "%s%.2f", i ? "," : "", L.a[i] / 100.0);
    n += snprintf(out + n, cap - n, "]}");
    LeaveCriticalSection(&cs);
    return n;
}

#else   // cl /DLINK_TEST src\link.c: the parsers on replies taken from a real hub (psuprobe, 2026-10-17)
#include <assert.h>
static int hex(const char *s, BYTE *b) { int n = 0; unsigned v; while (sscanf(s, " %2x", &v) == 1) { b[n++] = (BYTE)v; s += 3; } return n; }
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
    puts("link: ok");
    return 0;
}
#endif
