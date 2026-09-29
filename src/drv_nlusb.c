// SPDX-License-Identifier: GPL-3.0-only
// Nanoleaf USB lights: Pegboard Desk Dock (37FA:8201) and PC Screen Mirror Lightstrip (37FA:8202), after Nanoleaf's
// "USB Lightstrip Communication Protocol" (HID). Messages are TLV: type u8, length u16 big-endian, payload; the reply
// has type 0x80 + request. 0x02 colours (3 bytes per LED), 0x03 LED count, 0x07 power, 0x09 brightness.
// A message longer than one HID report goes out over consecutive reports (report ID 0).
// The device falls back to its own animation when frames stop coming, so the worker's 1 s keep-alive holds it.
// Colours go out green-red-blue: that is what the Pegboard shows correctly (documented as RGB, measured as GRB);
// [dev.N] order=rgb switches it. Full white on every LED at once makes the Pegboard reset (seen with Nanoleaf's
// own app too), so the total brightness is capped.
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <stdlib.h>

#define NL_VID 0x37FA
typedef struct { HANDLE h; OVERLAPPED ow, orr; int out_len, in_len, rgb; unsigned char msg[4 + 3 * 255]; } nlusb_t;

static const struct { USHORT pid; const char *name; } MODELS[] = { { 0x8201, "Pegboard Desk Dock" }, { 0x8202, "PC Screen Mirror Lightstrip" } };

static const char *model_name(USHORT pid) {
    for (int i = 0; i < (int)(sizeof(MODELS) / sizeof(MODELS[0])); i++) if (MODELS[i].pid == pid) return MODELS[i].name;
    return NULL;
}

// Walks the HID interfaces of Nanoleaf USB lights (the one that takes output reports), in a stable order;
// cb returns 1 to stop. The handle passed to cb is open for overlapped I/O; cb owns it when it returns 1.
static int each_device(int (*cb)(HANDLE h, USHORT pid, int index, const HIDP_CAPS *caps, void *ctx), void *ctx) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    int count[2] = { 0, 0 }, stop = 0;
    for (DWORD i = 0; !stop && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        if (!wcsstr(low, L"vid_37fa&pid_820")) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof(at) };
        PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps = { 0 };
        int ok = HidD_GetAttributes(h, &at) && at.VendorID == NL_VID && model_name(at.ProductID) &&
                 HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.OutputReportByteLength > 1;
        if (pp) HidD_FreePreparsedData(pp);
        if (ok) {
            int k = at.ProductID == 0x8202;
            if (cb(h, at.ProductID, count[k]++, &caps, ctx)) { stop = 1; continue; }
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return stop;
}

// One TLV message, split over output reports.
static int write_msg(nlusb_t *u, int type, const unsigned char *payload, int len) {
    u->msg[0] = (unsigned char)type; u->msg[1] = (unsigned char)(len >> 8); u->msg[2] = (unsigned char)len;
    if (len) memcpy(u->msg + 3, payload, len);
    int total = 3 + len, chunk = u->out_len - 1;
    unsigned char rep[1024];
    if (u->out_len > (int)sizeof(rep)) return 0;
    for (int off = 0; off < total; off += chunk) {
        memset(rep, 0, u->out_len);
        memcpy(rep + 1, u->msg + off, min(chunk, total - off));
        DWORD wr = 0;
        ResetEvent(u->ow.hEvent);
        if (!WriteFile(u->h, rep, u->out_len, &wr, &u->ow)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (WaitForSingleObject(u->ow.hEvent, 1000) != WAIT_OBJECT_0) { CancelIo(u->h); return 0; }
            if (!GetOverlappedResult(u->h, &u->ow, &wr, FALSE)) return 0;
        }
    }
    return 1;
}

// Waits up to ms for a reply of the given type; returns its payload length (-1 if none).
static int read_reply(nlusb_t *u, int type, unsigned char *out, int cap, int ms) {
    unsigned char rep[1024];
    if (u->in_len < 4 || u->in_len > (int)sizeof(rep)) return -1;
    DWORD end = GetTickCount() + ms;
    for (int left; (left = (int)(end - GetTickCount())) > 0;) {
        DWORD rd = 0;
        ResetEvent(u->orr.hEvent);
        if (!ReadFile(u->h, rep, u->in_len, &rd, &u->orr)) {
            if (GetLastError() != ERROR_IO_PENDING) return -1;
            if (WaitForSingleObject(u->orr.hEvent, left) != WAIT_OBJECT_0) { CancelIo(u->h); return -1; }
            if (!GetOverlappedResult(u->h, &u->orr, &rd, FALSE)) return -1;
        }
        if (rd >= 4 && rep[1] == type) {
            int len = rep[2] << 8 | rep[3];
            if (len > cap) len = cap;
            if (len > (int)rd - 4) len = (int)rd - 4;
            memcpy(out, rep + 4, len);
            return len;
        }
    }
    return -1;
}

typedef struct { const char *host; int sub; nlusb_t *u; USHORT pid; } find_ctx;

static int take(HANDLE h, USHORT pid, int index, const HIDP_CAPS *caps, void *arg) {
    find_ctx *f = arg;
    char host[16]; snprintf(host, sizeof(host), "37FA:%04X", pid);
    if (_stricmp(host, f->host) || index != (f->sub < 0 ? 0 : f->sub)) return 0;
    f->u->h = h; f->u->out_len = caps->OutputReportByteLength; f->u->in_len = caps->InputReportByteLength; f->pid = pid;
    return 1;
}

static int nlusb_open(ext_dev *d) {
    nlusb_t *u = calloc(1, sizeof(nlusb_t));
    if (!u) return 0;
    find_ctx f = { d->host, d->sub, u, 0 };
    if (!each_device(take, &f)) { free(u); return 0; }
    u->ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    u->orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    char sec[24]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    u->rgb = !_stricmp(cfg_get(sec, "order", "grb"), "rgb");
    d->priv = u;
    unsigned char r[8];
    int n = write_msg(u, 0x03, NULL, 0) ? read_reply(u, 0x83, r, sizeof(r), 500) : -1;
    d->nleds = n >= 2 && !r[0] && r[1] ? r[1] : f.pid == 0x8201 ? 64 : 0;
    const unsigned char on = 1, full = 255;
    write_msg(u, 0x07, &on, 1);
    write_msg(u, 0x09, &full, 1);   // dimming is the app's job
    snprintf(d->info, sizeof(d->info), "Nanoleaf %s · USB", model_name(f.pid));
    return 1;
}

static int nlusb_send(ext_dev *d, const rgbf *c, int n) {
    nlusb_t *u = d->priv;
    if (n > 255) n = 255;
    // keep the sum of all channels at or below half of full white, so the dock never draws enough to reset
    float sum = 0;
    for (int i = 0; i < n; i++) sum += clampf(c[i].r, 0, 1) + clampf(c[i].g, 0, 1) + clampf(c[i].b, 0, 1);
    float k = sum > n * 1.5f ? n * 1.5f / sum : 1;
    unsigned char p[3 * 255];
    for (int i = 0; i < n; i++) {
        unsigned char r = to8(c[i].r * k), g = to8(c[i].g * k), b = to8(c[i].b * k);
        if (u->rgb) { p[i * 3] = r; p[i * 3 + 1] = g; } else { p[i * 3] = g; p[i * 3 + 1] = r; }
        p[i * 3 + 2] = b;
    }
    return write_msg(u, 0x02, p, n * 3);
}

static void nlusb_leave(ext_dev *d, int how) {
    // off: power off; otherwise frames simply stop and the light goes back to its own animation
    if (how == LEAVE_OFF) { const unsigned char off = 0; write_msg(d->priv, 0x07, &off, 1); }
}

static void nlusb_close(ext_dev *d) {
    nlusb_t *u = d->priv;
    if (!u) return;
    if (u->h && u->h != INVALID_HANDLE_VALUE) { CancelIo(u->h); CloseHandle(u->h); }
    if (u->ow.hEvent) CloseHandle(u->ow.hEvent);
    if (u->orr.hEvent) CloseHandle(u->orr.hEvent);
    free(u); d->priv = NULL;
}

typedef struct { void (*found)(const disc_t *); } disc_ctx;

static int report(HANDLE h, USHORT pid, int index, const HIDP_CAPS *caps, void *arg) {
    (void)caps;
    disc_t x = { "nlusb" };
    snprintf(x.host, sizeof(x.host), "37FA:%04X", pid);
    x.sub = index;
    char nth[8] = ""; if (index) snprintf(nth, sizeof(nth), " %d", index + 1);
    snprintf(x.name, sizeof(x.name), "Nanoleaf %s%s", pid == 0x8201 ? "Pegboard" : "Screen Mirror strip", nth);
    // the LED count, asked the same way as on open
    nlusb_t u = { h, { 0 }, { 0 }, caps->OutputReportByteLength, caps->InputReportByteLength, 0 };
    u.ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); u.orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    unsigned char r[8];
    int n = write_msg(&u, 0x03, NULL, 0) ? read_reply(&u, 0x83, r, sizeof(r), 400) : -1;
    CloseHandle(u.ow.hEvent); CloseHandle(u.orr.hEvent);
    x.nleds = n >= 2 && !r[0] && r[1] ? r[1] : pid == 0x8201 ? 64 : 0;
    snprintf(x.info, sizeof(x.info), "Nanoleaf %s · USB", model_name(pid));
    ((disc_ctx *)arg)->found(&x);
    return 0;   // keep going; the handle is closed by each_device
}

static void nlusb_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    disc_ctx c = { found };
    each_device(report, &c);
}

const ext_driver drv_nlusb = { "nlusb", "Nanoleaf USB", 30, 1, nlusb_open, nlusb_send, nlusb_leave, nlusb_close, nlusb_discover };
