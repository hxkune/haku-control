// SPDX-License-Identifier: GPL-3.0-only
// Wooting keyboards over USB HID, after Wooting's open RGB SDK (github.com/WootingKb/wooting-rgb-sdk). The lighting
// interface is the one with usage page 0xFF55 (newer firmware: numbered reports) or 0x1337 (older, report ID 0).
// Commands are 8-byte feature reports: report ID, magic D1 DA (D0 DA on 0x1337), command, 4 parameters; the
// keyboard answers each with an input report. Frames: report 5 (0 on 0x1337), magic, command 11, then the whole
// 6 x 21 key matrix, row by row, one RGB565 value per key (little-endian). 33 takes the colours over, 32 gives
// the keyboard its own lighting back. The haku device is the keyboard's columns: each column shows one colour
// from the top row to the bottom, so effects run across the keyboard from left to right.
// Verified on a Wooting 60HE v2 (0xFF55); 0x1337 keyboards follow the SDK and are untested. The first Wooting One /
// Two firmware (VID 03EB) uses another protocol and is not driven (OpenRGB can).
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <stdlib.h>

#define WT_VID 0x31E3
#define ROWS 6
#define COLS 21                // the matrix every frame carries
#define CMD_RAW_COLORS 11
#define CMD_RESET_ALL  32
#define CMD_COLOR_INIT 33

// columns (and rows) that have keys, by model: the product ID without its low 4 bits (gamepad modes)
static const struct { USHORT pid; int cols, rows; } MODELS[] = {
    { 0x1100, 17, 6 },   // One
    { 0x1200, 21, 6 }, { 0x1210, 21, 6 }, { 0x1220, 21, 6 }, { 0x1230, 21, 6 },   // Two, Two LE, Two HE (+ ARM)
    { 0x1300, 14, 6 }, { 0x1310, 14, 6 }, { 0x1320, 14, 6 }, { 0x1340, 14, 6 },   // 60HE, 60HE ARM, 60HE+, 60HE v2
    { 0x1400, 17, 6 },   // 80HE
    { 0x1500, 7, 5 }, { 0x1510, 7, 5 },   // UwU, UwU RGB
};
static void model_size(USHORT pid, int *cols, int *rows) {
    *cols = COLS; *rows = ROWS;
    for (int i = 0; i < (int)(sizeof(MODELS) / sizeof(MODELS[0])); i++)
        if (MODELS[i].pid == (pid & 0xFFF0)) { *cols = MODELS[i].cols; *rows = MODELS[i].rows; }
}

typedef struct {
    HANDLE h; OVERLAPPED ow, orr;
    int v3, out_len, in_len, parts, cols, rows;
    unsigned char rep[4096];
} wt_t;

// Walks the lighting interfaces of Wooting keyboards in a stable order; cb returns 1 to stop and then owns the
// handle (open for overlapped I/O). index counts keyboards of the same model.
typedef int (*wt_cb)(HANDLE h, USHORT pid, int index, int v3, const HIDP_CAPS *caps, void *ctx);
static int each_keyboard(wt_cb cb, void *ctx) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    USHORT seen[16]; int nseen = 0, count[16] = { 0 }, stop = 0;
    for (DWORD i = 0; !stop && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        if (!wcsstr(low, L"vid_31e3")) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof(at) };
        PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps = { 0 };
        int ok = HidD_GetAttributes(h, &at) && at.VendorID == WT_VID && HidD_GetPreparsedData(h, &pp) &&
                 HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && (caps.UsagePage == 0xFF55 || caps.UsagePage == 0x1337) &&
                 caps.OutputReportByteLength > 8 && caps.OutputReportByteLength <= 4096 && caps.FeatureReportByteLength >= 8;
        if (pp) HidD_FreePreparsedData(pp);
        if (ok) {
            USHORT base = at.ProductID & 0xFFF0; int k = 0;
            while (k < nseen && seen[k] != base) k++;
            if (k == nseen && nseen < 16) seen[nseen++] = base;
            if (k < 16 && cb(h, at.ProductID, count[k]++, caps.UsagePage == 0xFF55, &caps, ctx)) { stop = 1; continue; }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return stop;
}

// Waits up to ms for the answer to a command (and drops it).
static void read_answer(wt_t *w, int ms) {
    if (w->in_len < 2 || w->in_len > (int)sizeof(w->rep)) return;
    unsigned char in[4096]; DWORD rd = 0;
    ResetEvent(w->orr.hEvent);
    if (!ReadFile(w->h, in, w->in_len, &rd, &w->orr)) {
        if (GetLastError() != ERROR_IO_PENDING) return;
        if (WaitForSingleObject(w->orr.hEvent, ms) != WAIT_OBJECT_0) { CancelIoEx(w->h, &w->orr); WaitForSingleObject(w->orr.hEvent, 100); return; }
        GetOverlappedResult(w->h, &w->orr, &rd, FALSE);
    }
}

static int command(wt_t *w, unsigned char cmd) {
    unsigned char b[8] = { w->v3 ? 1 : 0, w->v3 ? 0xD1 : 0xD0, 0xDA, cmd, 0, 0, 0, 0 };
    if (!HidD_SetFeature(w->h, b, sizeof(b))) return 0;
    read_answer(w, 300);
    return 1;
}

static int write_report(wt_t *w, const unsigned char *data, int len) {
    DWORD wr = 0;
    ResetEvent(w->ow.hEvent);
    if (!WriteFile(w->h, data, len, &wr, &w->ow)) {
        if (GetLastError() != ERROR_IO_PENDING) return 0;
        if (WaitForSingleObject(w->ow.hEvent, 1000) != WAIT_OBJECT_0) { CancelIoEx(w->h, &w->ow); WaitForSingleObject(w->ow.hEvent, 100); return 0; }
        if (!GetOverlappedResult(w->h, &w->ow, &wr, FALSE)) return 0;
    }
    return 1;
}

// One frame: rgb565[ROWS * COLS] as the keyboard takes it
static int send_matrix(wt_t *w, const unsigned short *m) {
    unsigned char *r = w->rep;
    memset(r, 0, w->out_len);
    r[0] = w->v3 ? 5 : 0; r[1] = w->v3 ? 0xD1 : 0xD0; r[2] = 0xDA; r[3] = CMD_RAW_COLORS;
    for (int i = 0; i < ROWS * COLS; i++) { r[4 + 2 * i] = (unsigned char)m[i]; r[5 + 2 * i] = (unsigned char)(m[i] >> 8); }
    if (!w->parts) return write_report(w, r, w->out_len);
    // older firmware with 64-byte reports: the 257-byte message in four parts, each behind report ID 0
    unsigned char part[65];
    for (int k = 0; k < 4; k++) {
        part[0] = 0; memcpy(part + 1, r + 1 + k * 64, 64);
        if (!write_report(w, part, 65)) return 0;
    }
    return 1;
}

typedef struct { const char *host; int sub; wt_t *w; USHORT pid; } find_ctx;
static int take(HANDLE h, USHORT pid, int index, int v3, const HIDP_CAPS *caps, void *arg) {
    find_ctx *f = arg;
    char host[16]; snprintf(host, sizeof(host), "31E3:%04X", pid & 0xFFF0);
    if (_stricmp(host, f->host) || index != (f->sub < 0 ? 0 : f->sub)) return 0;
    f->w->h = h; f->w->v3 = v3; f->w->out_len = caps->OutputReportByteLength; f->w->in_len = caps->InputReportByteLength;
    f->w->parts = !v3 && caps->OutputReportByteLength <= 65;
    f->pid = pid;
    return 1;
}

static void product(HANDLE h, USHORT pid, char *out, int cap) {
    wchar_t w[64] = L"";
    if (!HidD_GetProductString(h, w, sizeof(w)) || !w[0]) { snprintf(out, cap, "Wooting %04X", pid); return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

static int wt_open(ext_dev *d) {
    wt_t *w = calloc(1, sizeof(wt_t));
    if (!w) return 0;
    find_ctx f = { d->host, d->sub, w, 0 };
    if (!each_keyboard(take, &f)) { free(w); return 0; }
    w->ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    model_size(f.pid, &w->cols, &w->rows);
    d->priv = w;
    if (!command(w, CMD_COLOR_INIT)) { CloseHandle(w->h); CloseHandle(w->ow.hEvent); CloseHandle(w->orr.hEvent); free(w); d->priv = NULL; return 0; }
    char nm[64]; product(w->h, f.pid, nm, sizeof(nm));
    snprintf(d->info, sizeof(d->info), "%s · keyboard · USB", nm);
    d->nleds = w->cols;
    return 1;
}

static int wt_send(ext_dev *d, const rgbf *c, int n) {
    wt_t *w = d->priv;
    unsigned short m[ROWS * COLS] = { 0 };
    for (int x = 0; x < w->cols && x < n; x++) {
        int r = to8(c[x].r), g = to8(c[x].g), b = to8(c[x].b);
        unsigned short v = (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
        for (int y = 0; y < w->rows; y++) m[y * COLS + x] = v;
    }
    return send_matrix(w, m);
}

static void wt_leave(ext_dev *d, int how) {
    wt_t *w = d->priv;
    if (!w) return;
    if (how == LEAVE_OFF) { unsigned short m[ROWS * COLS] = { 0 }; send_matrix(w, m); }
    else if (how == LEAVE_RESTORE) command(w, CMD_RESET_ALL);   // its own lighting (profile) again
}

static void wt_close(ext_dev *d) {
    wt_t *w = d->priv;
    if (!w) return;
    if (w->h && w->h != INVALID_HANDLE_VALUE) { CancelIo(w->h); CloseHandle(w->h); }
    if (w->ow.hEvent) CloseHandle(w->ow.hEvent);
    if (w->orr.hEvent) CloseHandle(w->orr.hEvent);
    free(w); d->priv = NULL;
}

typedef struct { void (*found)(const disc_t *); } disc_ctx;
static int report(HANDLE h, USHORT pid, int index, int v3, const HIDP_CAPS *caps, void *arg) {
    (void)v3; (void)caps;
    disc_t x = { "wooting" };
    snprintf(x.host, sizeof(x.host), "31E3:%04X", pid & 0xFFF0);
    x.sub = index;
    char nm[64]; product(h, pid, nm, sizeof(nm));
    if (index) snprintf(x.name, sizeof(x.name), "%s %d", nm, index + 1); else snprintf(x.name, sizeof(x.name), "%s", nm);
    int cols, rows; model_size(pid, &cols, &rows);
    x.nleds = cols;
    snprintf(x.info, sizeof(x.info), "%s · keyboard · USB", nm);
    ((disc_ctx *)arg)->found(&x);
    return 0;
}

static void wt_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    disc_ctx c = { found };
    each_keyboard(report, &c);
}

const ext_driver drv_wooting = { "wooting", "Wooting", 30, 1, wt_open, wt_send, wt_leave, wt_close, wt_discover };
