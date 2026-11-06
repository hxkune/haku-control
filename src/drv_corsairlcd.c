// SPDX-License-Identifier: GPL-3.0-only
// Corsair's round 480x480 LCD screens over USB HID (VID 1B1C, interface 0): the iCUE LINK LCD (0C4E), the ELITE LCD
// caps of the H100i/H150i/H170i ELITE coolers (0C39, 0C33) and the XC7 ELITE LCD block (0C42). After OpenLinkHub
// (github.com/jurkovic-nikola/OpenLinkHub, GPL-3.0): a picture goes as one 480x480 JPEG in 1024-byte output reports,
//   [0] 02  [1] 05  [2] 01 on iCUE LINK, 00 on the others  [3] 01 on the last chunk  [4] chunk index  [6..7] its length LE
// and up to 1016 bytes of the JPEG from [8]. Feature report 3 sets the backlight (03 0b <0-100> 01; the XC7 has none)
// and gives the screen back to its own display (03 1e 01 01, 03 1d 00 01, then the default backlight). The screen
// keeps nothing: OpenLinkHub sends a still again and again (every 10 ms on the iCUE LINK, 100 ms on the XC7), and an
// animation frame by frame; before the first it sets the rotation (03 0c 00 01; not on the ELITE caps).
// What it shows here: [dev.N] media, the frame pack the screen builder makes (corsairlcd-<N>.hlcd):
//   "HKLCD1\0\0", then for each frame: u32 LE JPEG length, u16 LE delay in ms, the JPEG
// with frame 0 black, shown while the lights are off ([dev.N] screen_follow, default on; brightness follows the app's).
// No media: nothing is sent and the screen keeps its own display. While iCUE, OpenLinkHub or SignalRGB runs the
// screen is theirs and haku waits. Untested on hardware: the caps of what it opens are logged (corsairlcd:).
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <tlhelp32.h>
#include <stdlib.h>

#define CL_VID     0x1B1C
#define CL_REP     1024
#define CL_DATA    (CL_REP - 8)
#define CL_CHUNKS  256                       // the index is one byte
#define CL_MAXJPG  (CL_CHUNKS * CL_DATA)
#define CL_FRAMES  1500
#define CL_MAXPACK (16 << 20)
#define CL_MINGAP  40                        // 25 frames/s at most
#define CL_STILL   100                       // a still (or dark) goes again every 100 ms, as OpenLinkHub's XC7 image mode
                                             // (1 s left an iCUE LINK LCD on its own temperature screen, report 711704f653)

static const struct { USHORT pid; unsigned char fam, br, rot; const char *name; } MODELS[] = {
    { 0x0C4E, 1, 1, 1, "iCUE LINK LCD" },
    { 0x0C39, 0, 1, 0, "ELITE LCD" }, { 0x0C33, 0, 1, 0, "ELITE LCD" },
    { 0x0C42, 0, 0, 1, "XC7 ELITE LCD" },
};
#define NMODELS (int)(sizeof(MODELS) / sizeof(MODELS[0]))
static int model(USHORT pid) { for (int i = 0; i < NMODELS; i++) if (MODELS[i].pid == pid) return i; return -1; }

// Chunk i of a JPEG of n bytes as the screen takes it, in rep (CL_REP bytes, the rest zero); -1: no such chunk, or the
// JPEG is over CL_CHUNKS chunks
static int cl_chunk(unsigned char *rep, unsigned char fam, const unsigned char *jpg, int n, int i) {
    int count = (n + CL_DATA - 1) / CL_DATA;
    if (n <= 0 || count > CL_CHUNKS || i < 0 || i >= count) return -1;
    int len = i == count - 1 ? n - i * CL_DATA : CL_DATA;
    memset(rep, 0, CL_REP);
    rep[0] = 0x02; rep[1] = 0x05; rep[2] = fam; rep[3] = i == count - 1; rep[4] = (unsigned char)i;
    rep[6] = (unsigned char)len; rep[7] = (unsigned char)(len >> 8);
    memcpy(rep + 8, jpg + i * CL_DATA, len);
    return count;
}

typedef struct { DWORD off, len; int delay; } cl_frame;

// The builder's pack checked whole: its frames in fr (CL_FRAMES at most); the count, or 0 when it is not a pack of
// at least the black frame and one more, every frame a JPEG the screen can take
static int cl_parse(const unsigned char *p, DWORD n, cl_frame *fr) {
    if (n < 8 || memcmp(p, "HKLCD1\0\0", 8)) return 0;
    int k = 0;
    for (DWORD at = 8; at < n; k++) {
        if (k >= CL_FRAMES || n - at < 6) return 0;
        DWORD len = p[at] | p[at + 1] << 8 | p[at + 2] << 16 | (DWORD)p[at + 3] << 24;
        int delay = p[at + 4] | p[at + 5] << 8;
        at += 6;
        if (len < 4 || len > CL_MAXJPG || len > n - at || p[at] != 0xFF || p[at + 1] != 0xD8 || p[at + 2] != 0xFF) return 0;
        fr[k].off = at; fr[k].len = len; fr[k].delay = delay < CL_MINGAP ? CL_MINGAP : delay;
        at += len;
    }
    return k >= 2 ? k : 0;
}

#define CL_FEAT 64   // feature report 3 is 4..64 bytes; a collection that says more (or less) is not used for it
static int feat_ok(int n) { return n >= 4 && n <= CL_FEAT; }

typedef struct { wchar_t path[300], base[300]; USHORT pid; int out, feat; } cl_if;

// The index-th screen of a model: its output collection (takes the 1024-byte reports) and its feature one; -1: none
static int pick(const cl_if *v, int n, USHORT pid, int index, int *feat) {
    for (int i = 0, k = 0; i < n; i++) {
        if (v[i].pid != pid || v[i].out < CL_REP || v[i].out > 4096) continue;
        if (k++ != index) continue;
        *feat = feat_ok(v[i].feat) ? i : -1;
        for (int j = 0; j < n && *feat < 0; j++) if (j != i && !wcscmp(v[j].base, v[i].base) && feat_ok(v[j].feat)) *feat = j;
        return i;
    }
    return -1;
}

#ifdef CORSAIR_LCD_TEST
static BOOLEAN __stdcall fake_set(HANDLE h, PVOID b, ULONG n);
#define SET_FEATURE fake_set
#else
#define SET_FEATURE HidD_SetFeature
#endif
// Feature report 3: cmd, v; sent as len bytes (the collection's), never more than the buffer has
static int cl_feature(HANDLE hf, int len, unsigned char cmd, unsigned char v) {
    unsigned char b[CL_FEAT] = { 0x03, cmd, v, 0x01 };
    return hf && hf != INVALID_HANDLE_VALUE && feat_ok(len) && SET_FEATURE(hf, b, (ULONG)len);
}

#ifndef CORSAIR_LCD_TEST
typedef struct { unsigned char *buf; DWORD n; int nfr; cl_frame fr[CL_FRAMES]; } cl_pack;

typedef struct {
    HANDLE h, hf;                  // the output collection; the one with the feature reports (may be the same)
    OVERLAPPED ow; HANDLE wake;
    int out_len, feat_len, m;
    HANDLE th; volatile LONG stop, dark, br, err, end_dark;   // end_dark: the thread's last frame is the black one
    int took;                      // the thread ran: the screen was haku's, so it is given back
    int heard;                     // the screen's answers logged so far
    cl_pack *volatile next;        // a new pack for the thread, which owns the one it shows
    int busy;                      // a program in the way was seen: the screen is theirs, nothing is given back
    DWORD checked;
    char key[MAX_PATH * 3 + 16];   // [dev.N] media|media_at last loaded
} cl_t;

// a program that drives the screen itself, by exe name (as link.c's owner)
static int in_way(char *app, int cap) {
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    int r = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !r; ok = Process32NextW(snap, &pe)) {
        const wchar_t *x = pe.szExeFile;
        if ((!_wcsnicmp(x, L"icue", 4) && _wcsicmp(x, L"iCUEUpdateService.exe")) || !_wcsicmp(x, L"Corsair.Service.exe")
            || !_wcsicmp(x, L"OpenLinkHub.exe") || !_wcsicmp(x, L"SignalRgb.exe")) {
            r = 1; WideCharToMultiByte(CP_UTF8, 0, x, -1, app, cap, NULL, NULL);
        }
    }
    CloseHandle(snap);
    return r;
}

// The HID collections of the screens on interface 0, in a stable order; base: the path without its &colNN, the same
// for the collections of one device
static int list_ifs(cl_if *v, int max) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    int n = 0;
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL) || wcslen(det->DevicePath) >= 300) continue;
        wchar_t low[300]; wcscpy_s(low, 300, det->DevicePath); _wcslwr_s(low, 300);
        if (!wcsstr(low, L"vid_1b1c") || (wcsstr(low, L"&mi_") && !wcsstr(low, L"&mi_00"))) continue;
        // no access asked: attributes and caps only, nothing is written
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at = { sizeof(at) }; PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps = { 0 };
        int ok = HidD_GetAttributes(h, &at) && at.VendorID == CL_VID && model(at.ProductID) >= 0 &&
                 HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (!ok) continue;
        cl_if *e = &v[n++];
        wcscpy_s(e->path, 300, det->DevicePath);
        wcscpy_s(e->base, 300, low);
        wchar_t *c = wcsstr(e->base, L"&col");
        if (c && wcslen(c) >= 6) memmove(c, c + 6, (wcslen(c + 6) + 1) * sizeof(wchar_t));
        e->pid = at.ProductID; e->out = caps.OutputReportByteLength; e->feat = caps.FeatureReportByteLength;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static int write_rep(cl_t *u, const unsigned char *rep) {
    unsigned char b[4096] = { 0 }; DWORD wr = 0;
    memcpy(b, rep, CL_REP);
    ResetEvent(u->ow.hEvent);
    if (!WriteFile(u->h, b, u->out_len, &wr, &u->ow)) {
        if (GetLastError() != ERROR_IO_PENDING) return 0;
        if (WaitForSingleObject(u->ow.hEvent, 1000) != WAIT_OBJECT_0) { CancelIoEx(u->h, &u->ow); WaitForSingleObject(u->ow.hEvent, 100); return 0; }
        if (!GetOverlappedResult(u->h, &u->ow, &wr, FALSE)) return 0;
    }
    return 1;
}

static int feature(cl_t *u, unsigned char cmd, unsigned char v) { return cl_feature(u->hf, u->feat_len, cmd, v); }

static int show(cl_t *u, const cl_pack *p, int f, int stoppable) {
    unsigned char rep[CL_REP];
    for (int i = 0, count = 1; i < count; i++) {
        if ((stoppable && u->stop) ||(count = cl_chunk(rep, MODELS[u->m].fam, p->buf + p->fr[f].off, (int)p->fr[f].len, i)) < 0 || !write_rep(u, rep)) return 0;
    }
    return 1;
}

static void free_pack(cl_pack *p) { if (p) { free(p->buf); free(p); } }

// What the screen sends back after a frame (it has 512-byte input reports): the first answers go to the log, as the
// only sign of how it took the picture (report 711704f653: frames went, the picture did not change)
static void hear(cl_t *u) {
    unsigned char b[1024]; DWORD got = 0; OVERLAPPED o = { 0 };
    if (!(o.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return;
    int ok = ReadFile(u->h, b, sizeof(b), &got, &o) ||
             (GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(o.hEvent, 100) == WAIT_OBJECT_0 && GetOverlappedResult(u->h, &o, &got, FALSE));
    if (!ok) { CancelIoEx(u->h, &o); GetOverlappedResult(u->h, &o, &got, TRUE); got = 0; }
    CloseHandle(o.hEvent);
    char hex[3 * 32 + 1] = "";
    for (DWORD i = 0; i < got && i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", b[i]);
    if (got) logf_("corsairlcd: the screen answered %lu bytes: %s", got, hex);
    else logf_("corsairlcd: no answer from the screen within 100 ms");
    u->heard++;
}

// The thread that has the screen while there is a pack: all writes go from here, one frame at a time
static DWORD WINAPI stream(void *arg) {
    cl_t *u = arg;
    cl_pack *p = NULL; int f = 1, sent_br = -1;
    if (MODELS[u->m].rot && !feature(u, 0x0c, 0x00)) logf_("corsairlcd: the rotation was not set (error %lu)", GetLastError());
    while (!u->stop) {
        cl_pack *nx = InterlockedExchangePointer((PVOID volatile *)&u->next, NULL);
        if (nx) { free_pack(p); p = nx; f = 1; }
        int br = u->dark ? 0 : u->br;
        if (MODELS[u->m].br && br != sent_br && p) {
            if (!feature(u, 0x0b, (unsigned char)br)) logf_("corsairlcd: the backlight was not set (error %lu)", GetLastError());
            sent_br = br;
        }
        if (!p) { WaitForSingleObject(u->wake, 200); continue; }
        int k = u->dark ? 0 : f;
        if (!show(u, p, k, 1)) { if (!u->stop) { u->err = 1; logf_("corsairlcd: a frame did not go (error %lu)", GetLastError()); } break; }
        if (u->heard < 3) hear(u);
        int still = u->dark || p->nfr == 2, wait = still ? CL_STILL : p->fr[k].delay;
        if (!u->dark && ++f >= p->nfr) f = 1;
        WaitForSingleObject(u->wake, wait);   // set on stop, a new pack and a dark / light change
    }
    if (u->end_dark && p && !u->err) show(u, p, 0, 0);
    free_pack(p);
    return 0;
}

static void cl_close(ext_dev *d);

static int cl_open(ext_dev *d) {
    char app[64];
    if (in_way(app, sizeof(app))) { snprintf(d->info, sizeof(d->info), "%s busy", app); return 0; }
    unsigned pid = 0; int m;
    if (sscanf(d->host, "1B1C:%4x", &pid) != 1 || (m = model((USHORT)pid)) < 0) return 0;
    cl_if *v = malloc(32 * sizeof(cl_if)); cl_t *u = calloc(1, sizeof(cl_t));
    int n = v ? list_ifs(v, 32) : 0, fi = -1, oi = n ? pick(v, n, (USHORT)pid, d->sub < 0 ? 0 : d->sub, &fi) : -1;
    if (oi < 0 || !u) { free(v); free(u); return 0; }
    d->priv = u; u->m = m; u->out_len = v[oi].out; u->br = 100;
    u->h = CreateFileW(v[oi].path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (fi >= 0) {
        u->feat_len = v[fi].feat;
        u->hf = fi == oi ? u->h : CreateFileW(v[fi].path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    u->ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); u->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    logf_("corsairlcd: %s %04X #%d: output %d bytes, feature %d bytes%s", MODELS[m].name, pid, d->sub, u->out_len, u->feat_len,
          fi < 0 ? " (none: no backlight, nothing to give back)" : fi == oi ? "" : " (another collection)");
    free(v);
    if (u->h == INVALID_HANDLE_VALUE || !u->ow.hEvent || !u->wake) {
        logf_("corsairlcd: %04X cannot be opened (error %lu)", pid, GetLastError()); cl_close(d); return 0;
    }
    char nth[8] = ""; if (d->sub > 0) snprintf(nth, sizeof(nth), " %d", d->sub + 1);
    snprintf(d->info, sizeof(d->info), "Corsair %s%s · screen · USB", MODELS[m].name, nth);
    d->nleds = 1;
    return 1;
}

// The pack at path (only the builder's .hlcd), or NULL
static cl_pack *load(const char *path) {
    wchar_t wp[MAX_PATH]; FILE *f = NULL; long size = 0;
    const char *dot = strrchr(path, '.');
    if (!dot || _stricmp(dot, ".hlcd") || !MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH) || !(f = _wfopen(wp, L"rb"))) return NULL;
    cl_pack *p = calloc(1, sizeof(cl_pack));
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    // shortcut: the whole pack in memory, 16 MB at most (the builder keeps under it)
    if (p && size > 0 && size <= CL_MAXPACK && (p->buf = malloc(size)) && fread(p->buf, 1, size, f) == (size_t)size) p->n = (DWORD)size;
    fclose(f);
    if (p && p->n && (p->nfr = cl_parse(p->buf, p->n, p->fr))) return p;
    free_pack(p);
    return NULL;
}

static int cl_send(ext_dev *d, const rgbf *c, int n) {
    (void)c; (void)n;
    cl_t *u = d->priv;
    if (u->err) return 0;
    DWORD now = GetTickCount(); char app[64];
    if (now - u->checked >= 2000) {
        u->checked = now;
        if (in_way(app, sizeof(app))) { u->busy = 1; logf_("corsairlcd: %s runs: the screen is its", app); snprintf(d->info, sizeof(d->info), "%s busy", app); return 0; }
    }
    char sec[24], key[sizeof(u->key)]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    int follow = cfg_geti(sec, "screen_follow", 1), on = _stricmp(cfg_get("general", "effect", ""), "off") != 0;
    LONG dark = follow && !on, br = follow ? (LONG)(app_brightness_now() * 100 + 0.5f) : 100;
    if (dark != u->dark || br != u->br) { u->dark = dark; u->br = br; SetEvent(u->wake); }
    snprintf(key, sizeof(key), "%s|%s", cfg_get(sec, "media", ""), cfg_get(sec, "media_at", ""));
    if (strcmp(key, u->key)) {
        strcpy_s(u->key, sizeof(u->key), key);
        cl_pack *p = key[0] != '|' ? load(cfg_get(sec, "media", "")) : NULL;
        if (key[0] != '|' && !p) logf_("corsairlcd: %s is not a screen builder's pack", cfg_get(sec, "media", ""));
        if (p) {
            logf_("corsairlcd: showing %d frame(s), %lu bytes", p->nfr - 1, p->n);
            free_pack(InterlockedExchangePointer((PVOID volatile *)&u->next, p));
            if (!u->th && !(u->th = CreateThread(NULL, 0, stream, u, 0, NULL))) logf_("corsairlcd: no thread");
            u->took |= u->th != NULL;
            SetEvent(u->wake);
        }
    }
    return 1;
}

static void stop_thread(cl_t *u) {
    if (!u->th) return;
    u->stop = 1; SetEvent(u->wake);
    WaitForSingleObject(u->th, INFINITE);   // a write gives up within a second
    CloseHandle(u->th); u->th = NULL;
}

// To its own display (OpenLinkHub's Stop), or dark: the backlight off, or on the XC7 (no backlight) the black frame
// the thread left on it. Only a screen haku showed something on, and not while a program in the way has it.
static void give_back(cl_t *u, int dark) {
    if (!u->took || u->busy) return;
    u->busy = 1;   // once
    if (dark && !MODELS[u->m].br) return;
    feature(u, 0x1e, 0x01); feature(u, 0x1d, 0x00);
    if (MODELS[u->m].br) feature(u, 0x0b, dark ? 0 : MODELS[u->m].fam ? 0x64 : 0x40);   // the backlight OpenLinkHub leaves it at
}

static void cl_leave(ext_dev *d, int how) {
    cl_t *u = d->priv;
    if (!u) return;
    char sec[24]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    int dark = how == LEAVE_OFF && cfg_geti(sec, "screen_follow", 1);   // switched off: dark; else its own display
    u->end_dark = dark && !u->busy;
    stop_thread(u);
    give_back(u, dark);
}

static void cl_close(ext_dev *d) {
    cl_t *u = d->priv;
    if (!u) return;
    stop_thread(u);
    give_back(u, 0);   // lost (send returned 0) or closed without leave: its own display again
    free_pack(u->next);
    if (u->hf && u->hf != INVALID_HANDLE_VALUE && u->hf != u->h) CloseHandle(u->hf);
    if (u->h && u->h != INVALID_HANDLE_VALUE) { CancelIo(u->h); CloseHandle(u->h); }
    if (u->ow.hEvent) CloseHandle(u->ow.hEvent);
    if (u->wake) CloseHandle(u->wake);
    free(u); d->priv = NULL;
}

static void cl_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    cl_if *v = malloc(32 * sizeof(cl_if));
    int n = v ? list_ifs(v, 32) : 0;
    for (int m = 0; m < NMODELS; m++) {
        if (m && MODELS[m].pid == MODELS[m - 1].pid) continue;
        int fi;
        for (int k = 0; pick(v, n, MODELS[m].pid, k, &fi) >= 0; k++) {
            disc_t x = { "corsairlcd" };
            snprintf(x.host, sizeof(x.host), "1B1C:%04X", MODELS[m].pid);
            x.sub = k; x.nleds = 1;
            char nth[8] = ""; if (k) snprintf(nth, sizeof(nth), " %d", k + 1);
            snprintf(x.name, sizeof(x.name), "Corsair %s%s", MODELS[m].name, nth);
            snprintf(x.info, sizeof(x.info), "Corsair %s · screen · USB · experimental", MODELS[m].name);
            found(&x);
        }
    }
    free(v);
}

const ext_driver drv_corsairlcd = { "corsairlcd", "Corsair LCD", 2, 0, cl_open, cl_send, cl_leave, cl_close, cl_discover };

#else   // cl /DCORSAIR_LCD_TEST src\drv_corsairlcd.c
#include <assert.h>
static int fk_n, fk_len, fk_fail; static unsigned char fk_b[CL_FEAT];
static BOOLEAN __stdcall fake_set(HANDLE h, PVOID b, ULONG n) {
    (void)h; fk_n++; fk_len = (int)n; memset(fk_b, 0xEE, sizeof(fk_b)); memcpy(fk_b, b, n <= CL_FEAT ? n : CL_FEAT);
    return !fk_fail;
}
static cl_frame fr[CL_FRAMES];
static unsigned char jpg[CL_MAXJPG + CL_DATA], pk[2 * (CL_MAXJPG + 16)];
static DWORD put(DWORD at, const unsigned char *j, DWORD len, int delay) {
    pk[at] = (unsigned char)len; pk[at + 1] = (unsigned char)(len >> 8); pk[at + 2] = (unsigned char)(len >> 16); pk[at + 3] = (unsigned char)(len >> 24);
    pk[at + 4] = (unsigned char)delay; pk[at + 5] = (unsigned char)(delay >> 8);
    memcpy(pk + at + 6, j, len);
    return at + 6 + len;
}
int main(void) {
    unsigned char rep[CL_REP];
    for (int i = 0; i < (int)sizeof(jpg); i++) jpg[i] = (unsigned char)(i * 7 + 3);
    jpg[0] = 0xFF; jpg[1] = 0xD8; jpg[2] = 0xFF;
    // one short chunk: the last, its length, the LINK's family byte
    assert(cl_chunk(rep, 1, jpg, 100, 0) == 1 && rep[0] == 2 && rep[1] == 5 && rep[2] == 1 && rep[3] == 1 && rep[4] == 0 && rep[5] == 0 &&
           rep[6] == 100 && rep[7] == 0 && !memcmp(rep + 8, jpg, 100) && rep[108] == 0 && rep[CL_REP - 1] == 0);
    // 2500 bytes: 1016 + 1016 + 468, only the third is the last
    assert(cl_chunk(rep, 0, jpg, 2500, 0) == 3 && rep[2] == 0 && rep[3] == 0 && rep[6] == (CL_DATA & 255) && rep[7] == CL_DATA >> 8);
    assert(cl_chunk(rep, 0, jpg, 2500, 1) == 3 && rep[3] == 0 && rep[4] == 1 && !memcmp(rep + 8, jpg + CL_DATA, CL_DATA));
    assert(cl_chunk(rep, 0, jpg, 2500, 2) == 3 && rep[3] == 1 && rep[4] == 2 && rep[6] == (468 & 255) && rep[7] == 468 >> 8 && !memcmp(rep + 8, jpg + 2 * CL_DATA, 468));
    assert(cl_chunk(rep, 0, jpg, 2500, 3) == -1 && cl_chunk(rep, 0, jpg, 0, 0) == -1);
    // an exact multiple of 1016 still marks its last chunk (OpenLinkHub would not)
    assert(cl_chunk(rep, 0, jpg, 2 * CL_DATA, 1) == 2 && rep[3] == 1 && rep[6] == (CL_DATA & 255));
    // 256 chunks fit (index 255), 257 do not
    assert(cl_chunk(rep, 0, jpg, CL_MAXJPG, 255) == 256 && rep[4] == 255 && rep[3] == 1);
    assert(cl_chunk(rep, 0, jpg, CL_MAXJPG + 1, 0) == -1);

    // the pack: black frame, then two frames; the delays kept, under 40 ms raised to 40
    memcpy(pk, "HKLCD1\0\0", 8);
    DWORD n = put(8, jpg, 50, 1000); n = put(n, jpg, 3000, 100); n = put(n, jpg, 10, 5);
    assert(cl_parse(pk, n, fr) == 3 && fr[0].off == 14 && fr[0].len == 50 && fr[1].len == 3000 && fr[1].delay == 100 && fr[2].delay == CL_MINGAP);
    assert(!cl_parse(pk, n - 1, fr) && !cl_parse(pk, n + 3, fr));   // cut short, or bytes after the last frame
    assert(!cl_parse(pk, put(8, jpg, 50, 1000), fr));               // the black frame alone shows nothing
    pk[0] = 'X'; assert(!cl_parse(pk, n, fr)); pk[0] = 'H';
    pk[14 + 50 + 6 + 1] = 0x00; assert(!cl_parse(pk, n, fr)); pk[14 + 50 + 6 + 1] = 0xD8;   // a frame that is not a JPEG
    assert(cl_parse(pk, n, fr) == 3);
    n = put(8, jpg, 50, 0); n = put(n, jpg, CL_MAXJPG + 1, 100);   // a frame over 256 chunks
    assert(!cl_parse(pk, n, fr) && !cl_parse(pk, 7, fr));
    assert(model(0x0C4E) == 0 && MODELS[model(0x0C42)].br == 0 && model(0x0C3F) < 0);   // the LINK hub itself is not a screen

    // which collection takes the feature reports: never one that says more than 64 bytes (or under 4)
    static cl_if v[4]; int fi;
    for (int i = 0; i < 4; i++) { wcscpy_s(v[i].path, 300, L"p"); wcscpy_s(v[i].base, 300, L"a"); v[i].pid = 0x0C4E; v[i].out = 0; v[i].feat = 0; }
    v[0].out = 1024; v[0].feat = 64;
    assert(pick(v, 4, 0x0C4E, 0, &fi) == 0 && fi == 0);                 // the output collection has it itself
    v[0].feat = 65; v[1].feat = 65; v[2].feat = 64;
    assert(pick(v, 4, 0x0C4E, 0, &fi) == 0 && fi == 2);                 // too long on itself and on a sibling: the next one
    v[2].feat = 3; assert(pick(v, 4, 0x0C4E, 0, &fi) == 0 && fi == -1);  // none fits: no backlight, nothing sent
    v[1].feat = 8; wcscpy_s(v[1].base, 300, L"b");
    assert(pick(v, 4, 0x0C4E, 0, &fi) == 0 && fi == -1);                // another device's collection is not used
    wcscpy_s(v[1].base, 300, L"a"); assert(pick(v, 4, 0x0C4E, 0, &fi) == 0 && fi == 1);
    v[3].out = 1024; assert(pick(v, 4, 0x0C4E, 1, &fi) == 3 && pick(v, 4, 0x0C4E, 2, &fi) == -1 && pick(v, 4, 0x0C42, 0, &fi) == -1);
    v[3].out = 4097; assert(pick(v, 4, 0x0C4E, 1, &fi) == -1);           // an output report over the write buffer
    // the report itself, through a fake transport: the collection's length, 03 cmd v 01, nothing past 64 bytes
    HANDLE h = (HANDLE)(INT_PTR)8;
    assert(cl_feature(h, 64, 0x0b, 40) && fk_n == 1 && fk_len == 64 && fk_b[0] == 3 && fk_b[1] == 0x0b && fk_b[2] == 40 && fk_b[3] == 1 && !fk_b[4] && !fk_b[63]);
    assert(cl_feature(h, 4, 0x1e, 1) && fk_n == 2 && fk_len == 4 && fk_b[1] == 0x1e);
    assert(!cl_feature(h, 65, 0x0b, 40) && !cl_feature(h, 4096, 0x0b, 40) && !cl_feature(h, 3, 0x0b, 40) && !cl_feature(h, 0, 0x0b, 40) && fk_n == 2);
    assert(!cl_feature(NULL, 64, 0x0b, 40) && !cl_feature(INVALID_HANDLE_VALUE, 64, 0x0b, 40) && fk_n == 2);
    fk_fail = 1; assert(!cl_feature(h, 64, 0x0b, 40) && fk_n == 3); fk_fail = 0;   // the device refused: said so
    puts("corsairlcd ok");
    return 0;
}
#endif
