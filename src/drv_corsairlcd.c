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

typedef struct { unsigned char *buf; DWORD n; int nfr; cl_frame fr[CL_FRAMES]; } cl_pack;

typedef struct {
    HANDLE h, hf;                  // the output collection; the one with the feature reports (may be the same)
    OVERLAPPED ow, orr;            // the write's and the read's
    HANDLE wake, quit;             // quit: set on stop, it ends a write that is being waited for
    unsigned char wb[4096], rb[1024];   // their buffers: the device has them until the I/O completes
    int wpend;                     // an I/O did not complete even cancelled: nothing more goes out, cl_close keeps u
    int out_len, feat_len, m;
    HANDLE th; volatile LONG stop, dark, br, err, end_dark;   // end_dark: the thread's last frame is the black one
    int took;                      // the thread ran: the screen was haku's, so it is given back
    int heard;                     // the screen's answers logged so far
    cl_pack *volatile next;        // a new pack for the thread, which owns the one it shows
    int busy;                      // a program in the way was seen: the screen is theirs, nothing is given back
    DWORD checked;
    char key[MAX_PATH * 3 + 16];   // [dev.N] media|media_at last loaded
} cl_t;

#ifdef CORSAIR_LCD_TEST
static BOOL fk_write(HANDLE h, LPCVOID b, DWORD n, LPDWORD wr, LPOVERLAPPED o);
static BOOL fk_cancel(HANDLE h, LPOVERLAPPED o);
#define IO_WRITE  fk_write
#define IO_CANCEL fk_cancel
static DWORD cl_write_ms = 1000, cl_cancel_ms = 3000, cl_br_retry_ms = 1000;
#else
#define IO_WRITE  WriteFile
#define IO_CANCEL CancelIoEx
static const DWORD cl_write_ms = 1000, cl_cancel_ms = 3000, cl_br_retry_ms = 1000;
#endif

// An overlapped I/O cancelled, then waited for until the device lets go of it (the kernel writes its OVERLAPPED and
// buffer until then). 0: it did not within cl_cancel_ms; u->wpend says so, and that memory is never freed.
static int cancel_wait(cl_t *u, OVERLAPPED *o) {
    IO_CANCEL(u->h, o);
    if (WaitForSingleObject(o->hEvent, cl_cancel_ms) == WAIT_OBJECT_0) return 1;
    u->wpend = 1;
    logf_("corsairlcd: an I/O did not end %lu ms after it was cancelled: the screen is let go of", cl_cancel_ms);
    return 0;
}

// One report out, whole. 0: refused, unplugged (ERROR_DEVICE_NOT_CONNECTED), short, too slow (cl_write_ms) or, when
// stoppable, stopped (u->quit). Never returns while the device still has u->wb, unless u->wpend is set.
static int write_rep(cl_t *u, const unsigned char *rep, int stoppable) {
    DWORD wr = 0;
    if (u->wpend) return 0;
    memcpy(u->wb, rep, CL_REP); memset(u->wb + CL_REP, 0, sizeof(u->wb) - CL_REP);
    ResetEvent(u->ow.hEvent);
    u->ow.Offset = u->ow.OffsetHigh = 0;
    if (!IO_WRITE(u->h, u->wb, u->out_len, NULL, &u->ow) && GetLastError() != ERROR_IO_PENDING) return 0;
    HANDLE w[2] = { u->ow.hEvent, u->quit };
    if (WaitForMultipleObjects(stoppable ? 2 : 1, w, FALSE, cl_write_ms) != WAIT_OBJECT_0) {
        if (cancel_wait(u, &u->ow)) GetOverlappedResult(u->h, &u->ow, &wr, FALSE);
        SetLastError(ERROR_TIMEOUT);
        return 0;
    }
    if (!GetOverlappedResult(u->h, &u->ow, &wr, FALSE)) return 0;
    if (wr != (DWORD)u->out_len) { logf_("corsairlcd: %lu of %d bytes written", wr, u->out_len); SetLastError(ERROR_WRITE_FAULT); return 0; }
    return 1;
}

static int feature(cl_t *u, unsigned char cmd, unsigned char v) { return cl_feature(u->hf, u->feat_len, cmd, v); }

static int show(cl_t *u, const cl_pack *p, int f, int stoppable) {
    unsigned char rep[CL_REP];
    for (int i = 0, count = 1; i < count; i++) {
        if ((stoppable && u->stop) || (count = cl_chunk(rep, MODELS[u->m].fam, p->buf + p->fr[f].off, (int)p->fr[f].len, i)) < 0 || !write_rep(u, rep, stoppable)) return 0;
    }
    return 1;
}

static void free_pack(cl_pack *p) { if (p) { free(p->buf); free(p); } }

// What the screen sends back after a frame (it has 512-byte input reports): the first answers go to the log, as the
// only sign of how it took the picture (report 711704f653: frames went, the picture did not change)
static void hear(cl_t *u) {
    DWORD got = 0;
    if (u->wpend) return;
    ResetEvent(u->orr.hEvent);
    u->orr.Offset = u->orr.OffsetHigh = 0;
    int ok = ReadFile(u->h, u->rb, sizeof(u->rb), NULL, &u->orr) || GetLastError() == ERROR_IO_PENDING;
    if (ok && WaitForSingleObject(u->orr.hEvent, 100) != WAIT_OBJECT_0) { ok = 0; cancel_wait(u, &u->orr); }
    if (!ok || !GetOverlappedResult(u->h, &u->orr, &got, FALSE)) got = 0;
    char hex[3 * 32 + 1] = "";
    for (DWORD i = 0; i < got && i < 32; i++) snprintf(hex + 3 * i, 4, "%02x ", u->rb[i]);
    if (got) logf_("corsairlcd: the screen answered %lu bytes: %s", got, hex);
    else logf_("corsairlcd: no answer from the screen within 100 ms");
    u->heard++;
}

// The thread that has the screen while there is a pack: all writes go from here, one frame at a time.
// An animation keeps its own clock (due, monotonic), so the time a frame takes to go counts towards its delay.
// Behind: the next one goes at once and the clock starts again from now, so frames never go in a burst to catch up
// (while the link is slower than the animation, it plays slower). A still (or dark) goes again CL_STILL after the
// last one went, as before.
// The backlight counts as sent only once the screen took it: a refused one is tried again, with the latest value,
// every cl_br_retry_ms at most, and logged once until it goes.
static DWORD WINAPI stream(void *arg) {
    cl_t *u = arg;
    cl_pack *p = NULL; int f = 1, sent_br = -1, br_fails = 0;
    ULONGLONG due = 0, br_at = 0;
    if (MODELS[u->m].rot && !feature(u, 0x0c, 0x00)) logf_("corsairlcd: the rotation was not set (error %lu)", GetLastError());
    while (!u->stop) {
        cl_pack *nx = InterlockedExchangePointer((PVOID volatile *)&u->next, NULL);
        if (nx) { free_pack(p); p = nx; f = 1; due = 0; }
        int br = u->dark ? 0 : u->br;
        ULONGLONG now = GetTickCount64();
        if (MODELS[u->m].br && br != sent_br && p && now >= br_at) {
            if (feature(u, 0x0b, (unsigned char)br)) {
                if (br_fails) logf_("corsairlcd: the backlight went after %d refusal(s)", br_fails);
                sent_br = br; br_fails = 0;
            } else {
                if (!br_fails++) logf_("corsairlcd: the backlight was not set (error %lu), it is tried again", GetLastError());
                br_at = now + cl_br_retry_ms;
            }
        }
        if (!p) { WaitForSingleObject(u->wake, 200); continue; }
        int dark = u->dark, still = dark || p->nfr == 2, k = dark ? 0 : f;
        if (still) due = 0;
        else if (due > now) { WaitForSingleObject(u->wake, (DWORD)(due - now)); continue; }   // woken before its time
        else if (!due) due = now;
        if (!show(u, p, k, 1)) { if (!u->stop) { u->err = 1; logf_("corsairlcd: a frame did not go (error %lu)", GetLastError()); } break; }
        if (u->heard < 3) hear(u);
        DWORD wait = CL_STILL;
        if (!still) {
            due += p->fr[k].delay; now = GetTickCount64();
            if (now > due) due = now;
            wait = due > now ? (DWORD)(due - now) : 0;
            if (++f >= p->nfr) f = 1;
        }
        // set on stop, a new pack, a dark / light or backlight change; a retry of the backlight waits for the next frame
        WaitForSingleObject(u->wake, wait);
    }
    if (u->end_dark && p && !u->err) show(u, p, 0, 0);
    free_pack(p);
    return 0;
}

#ifndef CORSAIR_LCD_TEST
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
    u->ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); u->orr.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    u->wake = CreateEventW(NULL, FALSE, FALSE, NULL); u->quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    logf_("corsairlcd: %s %04X #%d: output %d bytes, feature %d bytes%s", MODELS[m].name, pid, d->sub, u->out_len, u->feat_len,
          fi < 0 ? " (none: no backlight, nothing to give back)" : fi == oi ? "" : " (another collection)");
    free(v);
    if (u->h == INVALID_HANDLE_VALUE || !u->ow.hEvent || !u->orr.hEvent || !u->wake || !u->quit) {
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
    u->stop = 1; SetEvent(u->quit); SetEvent(u->wake);
    // every wait of the thread is bounded: a write being waited for ends at once (quit), its cancel within
    // cl_cancel_ms, then the black frame (end_dark) at most cl_write_ms + cl_cancel_ms a chunk: ~7 s at worst
    WaitForSingleObject(u->th, INFINITE);
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
    if (!u->wpend) give_back(u, 0);   // lost (send returned 0) or closed without leave: its own display again
    free_pack(u->next); u->next = NULL;
    if (u->hf && u->hf != INVALID_HANDLE_VALUE && u->hf != u->h) CloseHandle(u->hf);
    if (u->h && u->h != INVALID_HANDLE_VALUE) { CancelIo(u->h); CloseHandle(u->h); }
    d->priv = NULL;
    // shortcut: an I/O the device never ended keeps u (its OVERLAPPED, buffer and event) for good, ~6 KB a time;
    // a screen that hangs like that is rare, fine until it is not
    if (u->wpend) { logf_("corsairlcd: an I/O is still pending at close: its memory is kept"); return; }
    if (u->ow.hEvent) CloseHandle(u->ow.hEvent);
    if (u->orr.hEvent) CloseHandle(u->orr.hEvent);
    if (u->wake) CloseHandle(u->wake);
    if (u->quit) CloseHandle(u->quit);
    free(u);
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
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
static int fk_n, fk_len, fk_fail; static unsigned char fk_b[CL_FEAT];
static volatile LONG fk_br_n, fk_br_v = -1;   // backlight reports tried, the last one the device took
static BOOLEAN __stdcall fake_set(HANDLE h, PVOID b, ULONG n) {
    (void)h; fk_n++; fk_len = (int)n; memset(fk_b, 0xEE, sizeof(fk_b)); memcpy(fk_b, b, n <= CL_FEAT ? n : CL_FEAT);
    if (fk_b[1] == 0x0b) { InterlockedIncrement(&fk_br_n); if (!fk_fail) fk_br_v = fk_b[2]; }
    return !fk_fail;
}
void logf_(const char *fmt, ...) { (void)fmt; }

// The fake transport: WriteFile and CancelIoEx as a HID driver does them. A write pends on its own thread, which
// completes it the way the kernel does (InternalHigh, then Internal, then the event), fk_delay ms later or, once
// cancelled, fk_cancel_ms later (-1: only when fk_release is set). It checks the buffer was not touched meanwhile.
static volatile LONG fk_delay = 10, fk_cancel_ms = 0, fk_status = 0, fk_short, fk_sync_err, fk_pending, fk_writes, fk_cancels, fk_spoilt;
static HANDLE fk_cancel_ev, fk_release;
static double fk_t0;
static struct { double t; int mark; } lg[2048];
static volatile LONG lg_n;
static double ms(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return c.QuadPart * 1000.0 / f.QuadPart - fk_t0; }
typedef struct { OVERLAPPED *o; const unsigned char *b; DWORD n; unsigned char copy[4096]; } fk_io;
static DWORD WINAPI fk_done(void *arg) {
    fk_io *io = arg;
    int cancelled = WaitForSingleObject(fk_cancel_ev, (DWORD)fk_delay) == WAIT_OBJECT_0;
    if (cancelled) { if (fk_cancel_ms < 0) WaitForSingleObject(fk_release, INFINITE); else Sleep((DWORD)fk_cancel_ms); }
    if (memcmp(io->b, io->copy, io->n)) InterlockedIncrement(&fk_spoilt);
    io->o->InternalHigh = cancelled ? 0 : fk_short ? io->n - 1 : io->n;
    MemoryBarrier();
    io->o->Internal = cancelled ? 0xC0000120 : (ULONG_PTR)(ULONG)fk_status;   // STATUS_CANCELLED
    SetEvent(io->o->hEvent);
    InterlockedDecrement(&fk_pending);
    free(io);
    return 0;
}
static BOOL fk_write(HANDLE h, LPCVOID b, DWORD n, LPDWORD wr, LPOVERLAPPED o) {
    (void)h; (void)wr;
    const unsigned char *p = b;
    InterlockedIncrement(&fk_writes);
    if (!p[4] && lg_n < 2048) { lg[lg_n].t = ms(); lg[lg_n].mark = p[8 + 3]; lg_n++; }   // the first chunk of a frame
    if (fk_sync_err) { SetLastError((DWORD)fk_sync_err); return FALSE; }
    fk_io *io = malloc(sizeof(fk_io));
    io->o = o; io->b = p; io->n = n; memcpy(io->copy, p, n);
    ResetEvent(fk_cancel_ev);
    o->Internal = 0x103;   // STATUS_PENDING
    InterlockedIncrement(&fk_pending);
    CloseHandle(CreateThread(NULL, 0, fk_done, io, 0, NULL));
    SetLastError(ERROR_IO_PENDING);
    return FALSE;
}
static BOOL fk_cancel(HANDLE h, LPOVERLAPPED o) { (void)h; (void)o; InterlockedIncrement(&fk_cancels); SetEvent(fk_cancel_ev); return TRUE; }

static cl_frame fr[CL_FRAMES];
static unsigned char jpg[CL_MAXJPG + CL_DATA], pk[2 * (CL_MAXJPG + 16)];
static DWORD put(DWORD at, const unsigned char *j, DWORD len, int delay) {
    pk[at] = (unsigned char)len; pk[at + 1] = (unsigned char)(len >> 8); pk[at + 2] = (unsigned char)(len >> 16); pk[at + 3] = (unsigned char)(len >> 24);
    pk[at + 4] = (unsigned char)delay; pk[at + 5] = (unsigned char)(delay >> 8);
    memcpy(pk + at + 6, j, len);
    return at + 6 + len;
}
// The writes and the frame clock against the fake transport (no device is opened)
static cl_t U;
static HANDLE th;
static void fresh(void) {
    while (fk_pending) Sleep(1);
    HANDLE ow = U.ow.hEvent, q = U.quit, w = U.wake;
    memset(&U, 0, sizeof(U)); U.ow.hEvent = ow; U.quit = q; U.wake = w; ResetEvent(q);
    U.h = (HANDLE)(INT_PTR)12; U.hf = (HANDLE)(INT_PTR)16; U.feat_len = 64; U.out_len = CL_REP; U.br = 100; U.heard = 3;
    fk_delay = 10; fk_cancel_ms = 0; fk_status = 0; fk_short = 0; fk_sync_err = 0; fk_spoilt = 0; fk_fail = 0;
    fk_br_n = 0; fk_br_v = -1; lg_n = 0;
    cl_write_ms = 1000; cl_cancel_ms = 3000; cl_br_retry_ms = 1000;
}
// frame 0 black (marked B0), then nfr frames marked base, base + 1..., their delays d[0], d[1], d[0]...
static cl_pack *mk(int base, int nfr, const int *d) {
    unsigned char j[20] = { 0xFF, 0xD8, 0xFF, 0xB0 };
    memcpy(pk, "HKLCD1\0\0", 8);
    DWORD at = put(8, j, 20, 0);
    for (int i = 0; i < nfr; i++) { j[3] = (unsigned char)(base + i); at = put(at, j, 20, d[i % 2]); }
    cl_pack *p = calloc(1, sizeof(cl_pack));
    p->buf = malloc(at); memcpy(p->buf, pk, at); p->n = at;
    assert((p->nfr = cl_parse(p->buf, p->n, p->fr)) == nfr + 1);
    return p;
}
static void run(cl_pack *p) { U.next = p; th = CreateThread(NULL, 0, stream, &U, 0, NULL); assert(th); }
static double halt(void) {   // as stop_thread: how long the thread took to end
    double t = ms();
    U.stop = 1; SetEvent(U.quit); SetEvent(U.wake);
    assert(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0);
    CloseHandle(th); free_pack(U.next); U.next = NULL;
    t = ms() - t;
    while (fk_pending) Sleep(1);
    return t;
}
static double period(int from, int to) { assert(to - from >= 3); return (lg[to - 1].t - lg[from].t) / (to - 1 - from); }
static double mingap(int from, int to) { double m = 1e9; for (int i = from + 1; i < to; i++) if (lg[i].t - lg[i - 1].t < m) m = lg[i].t - lg[i - 1].t; return m; }

static void io_tests(void) {
    fk_cancel_ev = CreateEventW(NULL, TRUE, FALSE, NULL); fk_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    U.ow.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); U.quit = CreateEventW(NULL, TRUE, FALSE, NULL); U.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    fk_t0 = ms();
    unsigned char r[CL_REP]; memset(r, 0x5A, sizeof(r));
    double t; LONG c0, w0;

    // §1 write_rep. Done at once, done 50 ms later: whole, nothing cancelled, nothing left pending
    fresh(); fk_delay = 0; assert(write_rep(&U, r, 1) && !fk_pending && !fk_spoilt && !fk_cancels);
    fresh(); fk_delay = 50; t = ms(); assert(write_rep(&U, r, 1) && ms() - t >= 45 && !fk_pending && !fk_spoilt);
    // too slow, the cancel ends at once: an error, and the device has let go of the buffer before it returns
    fresh(); cl_write_ms = 100; fk_delay = 5000; c0 = fk_cancels; t = ms();
    assert(!write_rep(&U, r, 1) && GetLastError() == ERROR_TIMEOUT && !fk_pending && fk_cancels == c0 + 1 && !U.wpend && ms() - t < 400);
    // the cancel takes 300 ms (more than the old 100 ms wait): waited for, not left behind
    fresh(); cl_write_ms = 100; fk_delay = 5000; fk_cancel_ms = 300; t = ms();
    assert(!write_rep(&U, r, 1) && !fk_pending && !U.wpend && ms() - t >= 380);
    memset(r, 0x33, sizeof(r)); fk_delay = 10; assert(write_rep(&U, r, 1) && !fk_spoilt);
    // the cancel never ends: nothing goes out from then on, the buffer is left alone
    fresh(); cl_write_ms = 100; cl_cancel_ms = 200; fk_delay = 5000; fk_cancel_ms = -1; ResetEvent(fk_release);
    assert(!write_rep(&U, r, 1) && U.wpend && fk_pending == 1);
    w0 = fk_writes; assert(!write_rep(&U, r, 1) && fk_writes == w0);
    SetEvent(fk_release); while (fk_pending) Sleep(1); assert(!fk_spoilt);
    // unplugged while the write was pending, or at once
    fresh(); fk_delay = 20; fk_status = (LONG)0xC000009D; assert(!write_rep(&U, r, 1) && GetLastError() == ERROR_DEVICE_NOT_CONNECTED);
    fresh(); fk_sync_err = ERROR_DEVICE_NOT_CONNECTED; assert(!write_rep(&U, r, 1) && GetLastError() == ERROR_DEVICE_NOT_CONNECTED && !fk_pending);
    // fewer bytes than the report: not taken as sent
    fresh(); fk_short = 1; assert(!write_rep(&U, r, 1) && GetLastError() == ERROR_WRITE_FAULT);
    // stopped while waiting: at once, not after cl_write_ms; the black frame at the end (not stoppable) still goes
    fresh(); fk_delay = 5000; SetEvent(U.quit); t = ms(); assert(!write_rep(&U, r, 1) && ms() - t < 300 && !fk_pending);
    fresh(); fk_delay = 50; SetEvent(U.quit); assert(write_rep(&U, r, 0));

    // §12 the frame clock. Sent in 40 ms, 100 ms frames: 100 ms apart (it was 140)
    static const int d100[2] = { 100, 100 }, dvar[2] = { 60, 140 };
    int n;
    fresh(); fk_delay = 40; run(mk(1, 2, d100)); Sleep(1300); halt();
    t = period(2, lg_n); printf("corsairlcd: 40 ms transfer, 100 ms frames: %.1f ms apart\n", t); assert(t >= 90 && t <= 115);
    // close to the frame (80 ms): still 100
    fresh(); fk_delay = 80; run(mk(1, 2, d100)); Sleep(1300); halt();
    t = period(2, lg_n); printf("corsairlcd: 80 ms transfer: %.1f ms apart\n", t); assert(t >= 90 && t <= 115);
    // slower than the frame (150 ms): as fast as the link goes; then fast again: no burst to catch up
    fresh(); fk_delay = 150; run(mk(1, 2, d100)); Sleep(1200); n = lg_n;
    t = period(1, n); printf("corsairlcd: 150 ms transfer: %.1f ms apart\n", t); assert(t >= 140 && t <= 190);
    fk_delay = 10; Sleep(800); halt();
    t = mingap(n - 1, lg_n); printf("corsairlcd: then 10 ms: %.1f ms apart at least\n", t); assert(t >= 80);
    // varied delays: each frame its own
    fresh(); fk_delay = 20; run(mk(1, 4, dvar)); Sleep(1500); halt();
    for (int i = 2; i < lg_n; i++) { double want = dvar[(lg[i - 1].mark - 1) % 2], gap = lg[i].t - lg[i - 1].t; assert(gap >= want - 30 && gap <= want + 30); }
    // dark: the black frame only, every CL_STILL after the last went, the backlight 0
    fresh(); U.dark = 1; run(mk(1, 2, d100)); Sleep(800); halt();
    for (int i = 0; i < lg_n; i++) assert(lg[i].mark == 0xB0);
    t = period(1, lg_n); assert(t >= 95 && t <= 145 && fk_br_v == 0);
    // a new pack while playing: its frames within a frame or so, none of the old after
    fresh(); run(mk(1, 2, d100)); Sleep(300);
    U.next = mk(0x21, 2, d100); SetEvent(U.wake); t = ms(); Sleep(400); halt();
    int first = -1;
    for (int i = 0; i < lg_n; i++) if (lg[i].mark == 0x21 && first < 0) first = i;
    assert(first > 0 && lg[first].t - t < 150);
    for (int i = first; i < lg_n; i++) assert(lg[i].mark >= 0x21);
    // stopped while a write hangs: ends at once (the write is cancelled, not waited out)
    fresh(); run(mk(1, 2, d100)); Sleep(250); fk_delay = 5000; Sleep(300);
    t = halt(); printf("corsairlcd: stopped during a pending write in %.0f ms\n", t); assert(t < 500);
    // switched off (end_dark): the last frame is the black one
    fresh(); U.end_dark = 1; run(mk(1, 2, d100)); Sleep(250); halt(); assert(lg_n > 1 && lg[lg_n - 1].mark == 0xB0);
    // unplugged while playing: the thread ends by itself and says so (cl_send then reports the device lost)
    fresh(); run(mk(1, 2, d100)); Sleep(200); fk_status = (LONG)0xC000009D;
    assert(WaitForSingleObject(th, 1000) == WAIT_OBJECT_0 && U.err); halt();

    // §2 the backlight: refused, then taken; tried again every cl_br_retry_ms, not in a loop; not again once taken
    fresh(); cl_br_retry_ms = 200; fk_fail = 1; U.br = 40; run(mk(1, 1, d100)); Sleep(500);
    printf("corsairlcd: backlight tried %ld time(s) in 500 ms while refused\n", fk_br_n);
    assert(fk_br_n >= 2 && fk_br_n <= 4 && fk_br_v == -1);
    fk_fail = 0; Sleep(350); assert(fk_br_v == 40);
    w0 = fk_br_n; Sleep(300); assert(fk_br_n == w0); halt();
    // changed while refused: the latest goes
    fresh(); cl_br_retry_ms = 100; fk_fail = 1; U.br = 40; run(mk(1, 1, d100)); Sleep(100);
    U.br = 70; Sleep(150); fk_fail = 0; Sleep(400); assert(fk_br_v == 70); halt();
    // a reconnect is a new thread: it sends the backlight again even if it is the same
    fresh(); U.br = 70; run(mk(1, 1, d100)); Sleep(200); halt(); assert(fk_br_v == 70 && fk_br_n == 1);
}

int main(void) {
    unsigned char rep[CL_REP];
    timeBeginPeriod(1);   // as main.c: the frame clock and the write waits are timed against a 1 ms timer
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
    io_tests();
    puts("corsairlcd ok");
    return 0;
}
#endif
