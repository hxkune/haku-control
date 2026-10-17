// SPDX-License-Identifier: GPL-3.0-only
// The onboard LEDs of ASUS boards (the ROG logo, the I/O cover...) through their USB "AURA LED Controller" (VID 0B05),
// after OpenRGB's AsusAuraUSB controllers. 65-byte HID reports behind report ID EC:
//   B0          the config table: answer EC 30, the table from byte 4 ([0x02] ARGB headers, [0x1B] onboard LEDs)
//   82          the firmware: answer EC 02, 16 characters
//   35 ch 0 0 m effect channel ch (0: onboard) in mode m (FF: colours sent by the PC)
//   40 ch s n   colours for direct channel ch (4: onboard; | 80 on the last report of a frame) from LED s, n LEDs as RGB,
//               20 a report
// Only the onboard LEDs: the ARGB headers stay with OpenRGB (one report each, they do not mix). Some boards report 0
// onboard LEDs (an X870E HERO, 0B05:1AA6, which OpenRGB lists with 0): then the count is a zone set by eye with
// Calibrate. The table goes to the log so a report shows what the board says.
// Written after OpenRGB, not yet tried on an ASUS board.
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>

#define AU_VID   0x0B05
#define REP      65
#define ONBOARD  4      // direct channel of the onboard LEDs
#define PER_REP  20
#define GUESS    16     // onboard LEDs when the table says 0, until Calibrate finds the count
#define MAXLEDS  60

static const USHORT PIDS[] = { 0x18F3, 0x1939, 0x19AF, 0x1AA6 };   // the motherboard ones OpenRGB knows

typedef struct { HANDLE h; OVERLAPPED ow, orr; int n; } au_t;

// Walks the Aura board controllers (usage page FF72) in a stable order; cb returns 1 to keep the handle (overlapped).
typedef int (*au_cb)(HANDLE h, USHORT pid, int index, void *ctx);
static int each_ctl(au_cb cb, void *ctx) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    int index = 0, stop = 0;
    for (DWORD i = 0; !stop && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        if (!wcsstr(low, L"vid_0b05")) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof(at) };
        PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps = { 0 };
        int ok = HidD_GetAttributes(h, &at) && at.VendorID == AU_VID && HidD_GetPreparsedData(h, &pp) &&
                 HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0xFF72 &&
                 caps.OutputReportByteLength == REP && caps.InputReportByteLength == REP;
        if (pp) HidD_FreePreparsedData(pp);
        for (int k = 0; ok && k < (int)(sizeof(PIDS) / sizeof(PIDS[0])); k++)
            if (PIDS[k] == at.ProductID) { if (cb(h, at.ProductID, index++, ctx)) stop = 1; break; }
        if (!stop) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return stop;
}

static int io(HANDLE h, OVERLAPPED *o, unsigned char *b, int rd, int ms) {
    DWORD got = 0;
    ResetEvent(o->hEvent);
    if (!(rd ? ReadFile(h, b, REP, &got, o) : WriteFile(h, b, REP, &got, o))) {
        if (GetLastError() != ERROR_IO_PENDING) return 0;
        if (WaitForSingleObject(o->hEvent, ms) != WAIT_OBJECT_0) { CancelIoEx(h, o); WaitForSingleObject(o->hEvent, 100); return 0; }
        if (!GetOverlappedResult(h, o, &got, FALSE)) return 0;
    }
    return 1;
}

// A request and its answer (the first report starting EC <want>), into b
static int ask(au_t *a, unsigned char cmd, unsigned char want, unsigned char *b) {
    memset(b, 0, REP); b[0] = 0xEC; b[1] = cmd;
    if (!io(a->h, &a->ow, b, 0, 500)) return 0;
    for (int k = 0; k < 8; k++) {   // OpenRGB may be talking to it too: its answers are skipped
        if (!io(a->h, &a->orr, b, 1, 500)) return 0;
        if (b[0] == 0xEC && b[1] == want) return 1;
    }
    return 0;
}

static int send_rep(au_t *a, const unsigned char *b) { unsigned char c[REP]; memcpy(c, b, REP); return io(a->h, &a->ow, c, 0, 1000); }

// The config table and the firmware into the log; returns the onboard LEDs the board says (-1: no answer)
static int read_table(au_t *a, const char *who) {
    unsigned char b[REP], t[60]; char fw[17] = "", hex[200]; int o = 0;
    if (ask(a, 0x82, 0x02, b)) { memcpy(fw, b + 2, 16); for (int i = 0; i < 16; i++) if (fw[i] && (fw[i] < 32 || fw[i] > 126)) fw[i] = '?'; }
    if (!ask(a, 0xB0, 0x30, b)) return -1;
    memcpy(t, b + 4, 60);
    for (int i = 0; i < 60; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02X", t[i]);
    logf_("%s (aura): firmware '%s', table %s: %d onboard LEDs, %d ARGB headers", who, fw, hex, t[0x1B], t[0x02]);
    return t[0x1B];
}

typedef struct { const char *host; int sub; au_t *a; } find_ctx;
static int take(HANDLE h, USHORT pid, int index, void *arg) {
    find_ctx *f = arg;
    char host[16]; snprintf(host, sizeof(host), "0B05:%04X", pid);
    if (_stricmp(host, f->host) || index != (f->sub < 0 ? 0 : f->sub)) return 0;
    f->a->h = h;
    return 1;
}

static void au_close(ext_dev *d) {
    au_t *a = d->priv;
    if (!a) return;
    if (a->h && a->h != INVALID_HANDLE_VALUE) { CancelIo(a->h); CloseHandle(a->h); }
    if (a->ow.hEvent) CloseHandle(a->ow.hEvent);
    if (a->orr.hEvent) CloseHandle(a->orr.hEvent);
    free(a); d->priv = NULL;
}

static int au_open(ext_dev *d) {
    au_t *a = calloc(1, sizeof(au_t));
    if (!a) return 0;
    find_ctx f = { d->host, d->sub, a };
    if (!each_ctl(take, &f)) { free(a); return 0; }
    a->ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    a->orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->priv = a;
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    int n = read_table(a, sec);
    if (n < 0) { snprintf(d->info, sizeof(d->info), "ASUS Aura · the controller does not answer"); au_close(d); return 0; }
    char found[40] = "";
    if (!n) {   // the board does not say: a zone the window (Calibrate) sets
        const char *p = strstr(cfg_get(sec, "zones", ""), "Onboard=");
        n = p ? atoi(p + 8) : GUESS;
        if (n < 1) n = 1;
        snprintf(found, sizeof(found), "Onboard:%d:1:%d:0", n, MAXLEDS);
    }
    if (n > MAXLEDS) n = MAXLEDS;
    if (strcmp(cfg_get(sec, "zones_found", ""), found)) { cfg_set(sec, "zones_found", found); cfg_save_if_dirty(); }
    unsigned char b[REP] = { 0xEC, 0x35, 0, 0, 0, 0xFF };   // onboard: colours from the PC
    if (!send_rep(a, b)) { au_close(d); return 0; }
    a->n = d->nleds = n;
    snprintf(d->info, sizeof(d->info), "ASUS Aura · motherboard · USB · onboard LEDs");
    return 1;
}

static int au_send(ext_dev *d, const rgbf *c, int n) {
    au_t *a = d->priv;
    unsigned char b[REP];
    for (int s = 0; s < a->n; s += PER_REP) {
        int k = a->n - s < PER_REP ? a->n - s : PER_REP;
        memset(b, 0, REP);
        b[0] = 0xEC; b[1] = 0x40; b[2] = (unsigned char)(ONBOARD | (s + k >= a->n ? 0x80 : 0)); b[3] = (unsigned char)s; b[4] = (unsigned char)k;
        for (int i = 0; i < k; i++) {
            rgbf v = s + i < n ? c[s + i] : (rgbf){ 0 };
            b[5 + 3 * i] = to8(v.r); b[6 + 3 * i] = to8(v.g); b[7 + 3 * i] = to8(v.b);
        }
        if (!send_rep(a, b)) return 0;
    }
    return 1;
}

// shortcut: LEAVE_RESTORE keeps the last colours (the board's own effect is not read back), its own effect returns at the next boot
static void au_leave(ext_dev *d, int how) {
    if (how == LEAVE_OFF && d->priv) { rgbf z[MAXLEDS] = { 0 }; au_send(d, z, MAXLEDS); }
}

typedef struct { void (*found)(const disc_t *); } disc_ctx;
static int report(HANDLE h, USHORT pid, int index, void *arg) {
    au_t a = { h };
    a.ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    a.orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    disc_t x = { "aura" };
    snprintf(x.host, sizeof(x.host), "0B05:%04X", pid);
    x.sub = index;
    int n = read_table(&a, x.host);
    CloseHandle(a.ow.hEvent); CloseHandle(a.orr.hEvent);
    if (n >= 0) {
        x.nleds = n ? n : GUESS;
        snprintf(x.name, sizeof(x.name), index ? "ASUS board LEDs %d" : "ASUS board LEDs", index + 1);
        snprintf(x.info, sizeof(x.info), "ASUS Aura · motherboard · USB%s", n ? "" : " · count by Calibrate");
        ((disc_ctx *)arg)->found(&x);
    }
    return 0;
}

static void au_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    disc_ctx c = { found };
    each_ctl(report, &c);
}

const ext_driver drv_aura = { "aura", "ASUS Aura (board)", 30, 1, au_open, au_send, au_leave, au_close, au_discover };
