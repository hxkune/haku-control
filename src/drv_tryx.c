// SPDX-License-Identifier: GPL-3.0-only
// TRYX PANORAMA liquid coolers' screen over USB: the pump block is a small Android device that shows up as a serial
// port (USB 18D1:2D03 PANORAMA, 18D1:2D04 PANORAMA 240 SE, interface 0, the inbox usbser driver). After the
// community write-ups of the protocol TRYX KANALI speaks (github.com/sallen2/canalis, github.com/heyimmitzu/tryxctl,
// github.com/fadli0029/reed-tpse, all MIT): frames in tryx_frame.h around an HTTP-like text,
//   POST <command> 1\r\nSeqNumber=<n>\r\nDate=<epoch ms>\r\nContentType=json\r\nContentLength=<n>\r\n\r\n<json>
// answered "1 200\r\n<headers>\r\n\r\n<json>" within ~150 ms. conn opens the link, waterBlockScreen {"enable"} turns
// the screen on / off, brightness {"value":0-100}, waterBlockScreenId picks what it shows. The device needs a frame
// at least every 10 s (it restarts its port) and goes to its "disconnected" screen after 60 s, so STATE all (the
// PC's sensors, which TRYX KANALI sends every second) goes out every second.
// What it does here: the screen follows the lights ([dev.N] screen_follow, default on: off when the effect is
// Off, brightness with the app's brightness) and [dev.N] theme=1..14 picks one of the PANORAMA's built-in themes
// (0: leave the screen as TRYX KANALI set it). The haku device itself is one light that shows nothing: the cooler's
// own LEDs are on the motherboard's ARGB header. theme=-1 shows [dev.N] media, a picture / GIF / mp4 on this PC, sent
// over the same port the way tryxctl does it: transport {fileName, fileSize, type "media"}, the raw bytes unframed,
// then transported {fileName, md5}; the device keeps it in sdcard/pcMedia and waterBlockScreenId "Customization"
// shows it. The name on the device is the md5 + the extension, so the same file is not sent twice ([dev.N] media_sent).
// Untested on hardware: every answer that is not 200 is logged.
#include "devices.h"
#include "tryx_frame.h"
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <stdlib.h>

DEFINE_GUID(TX_COMPORT, 0x86E0D1E0, 0x8089, 0x11D0, 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73);   // GUID_DEVINTERFACE_COMPORT

static const char *const THEMES[] = { "Cooling delivery", "Migration", "Quantum time capsule", "Exo-Ecologies", "Racing",
    "Shuttle", "Gift of TRYX", "Sweet Cool Acrylic", "Catch Me If You Can", "The Battle of Ice and Fire", "Oasis",
    "Cyber Bunker", "Edge Of Dream", "Thermal Energy Prohibited" };
#define NTHEMES (int)(sizeof(THEMES) / sizeof(THEMES[0]))

// The device's name for a file: the md5's first 16 hex digits + its extension in lower case; 0: not a kind it shows
// (mp4 plays as a video, the rest go to its picture viewer; .mov / .avi / .mkv show nothing)
static int media_name(const char *path, const char *md5, char *out, int cap) {
    static const char *const EXT[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".mp4" };
    const char *dot = strrchr(path, '.'), *sl = strrchr(path, '\\');
    if (!dot || (sl && dot < sl) || strlen(dot) > 5) return 0;
    char ext[8]; strcpy_s(ext, sizeof(ext), dot); _strlwr_s(ext, sizeof(ext));
    for (int i = 0; i < (int)(sizeof(EXT) / sizeof(EXT[0])); i++)
        if (!strcmp(ext, EXT[i])) return snprintf(out, cap, "%.16s%s", md5, ext) > 0;
    return 0;
}

#ifndef TRYX_TEST
typedef struct {
    HANDLE h;
    int seq, on, br, theme;     // what was last sent (-1: own media, 0: nothing yet)
    DWORD last_state;
    int rn; unsigned char rx[4096];
    // own media: its thread has the port while it runs, tryx_send leaves it alone
    HANDLE up; volatile LONG stop, pct; int id, force, failed; USHORT pid;
    char media[MAX_PATH * 3 + 16], base[96];   // [dev.N] media|media_at last tried; the info line
} tryx_t;

// Walks the TRYX serial ports in a stable order; cb returns 1 to stop.
static int each_port(int (*cb)(const wchar_t *path, USHORT pid, int index, const char *com, void *ctx), void *ctx) {
    HDEVINFO set = SetupDiGetClassDevsW(&TX_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    int count[2] = { 0, 0 }, stop = 0;
    for (DWORD i = 0; !stop && SetupDiEnumDeviceInterfaces(set, NULL, &TX_COMPORT, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        SP_DEVINFO_DATA dev = { sizeof(dev) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, &dev)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        USHORT pid = wcsstr(low, L"vid_18d1&pid_2d03") ? 0x2D03 : wcsstr(low, L"vid_18d1&pid_2d04") ? 0x2D04 : 0;
        if (!pid || (wcsstr(low, L"&mi_") && !wcsstr(low, L"&mi_00"))) continue;
        char com[16] = "";
        HKEY k = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            wchar_t w[16]; DWORD sz = sizeof(w), ty;
            if (!RegQueryValueExW(k, L"PortName", NULL, &ty, (BYTE *)w, &sz) && ty == REG_SZ) { w[15] = 0; WideCharToMultiByte(CP_UTF8, 0, w, -1, com, sizeof(com), NULL, NULL); }
            RegCloseKey(k);
        }
        stop = cb(det->DevicePath, pid, count[pid == 0x2D04]++, com, ctx);
    }
    SetupDiDestroyDeviceInfoList(set);
    return stop;
}

static int port_setup(HANDLE h) {
    DCB dcb = { sizeof(dcb) };
    if (!GetCommState(h, &dcb)) return 0;
    dcb.BaudRate = 115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT; dcb.fBinary = 1;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = dcb.fOutX = dcb.fInX = 0;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    COMMTIMEOUTS to = { MAXDWORD, 0, 0, 0, 500 };   // reads return at once with what is there; writes give up after 0.5 s
    return SetCommState(h, &dcb) && SetCommTimeouts(h, &to);
}

// One message out. The body is padded with spaces when the crc would come out as 5A / 5B (see tryx_crc).
static int tx(tryx_t *u, const char *method, const char *cmd, const char *body) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    unsigned long long ms = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
    char msg[1024]; int n = 0, bl = (int)strlen(body);
    for (int pad = 0; pad < 3; pad++) {
        n = snprintf(msg, sizeof(msg), "%s %s 1\r\nSeqNumber=%d\r\nDate=%llu\r\nContentType=json\r\nContentLength=%d\r\n\r\n%s%*s",
                     method, cmd, u->seq, ms, bl + pad, body, pad, "");
        if (n <= 0 || n >= (int)sizeof(msg)) return 0;
        unsigned char crc = tryx_crc((unsigned char *)msg, n);
        if (crc != 0x5A && crc != 0x5B) break;
    }
    u->seq++;
    unsigned char fr[2 * sizeof(msg) + 8]; int k = tryx_frame((unsigned char *)msg, n, fr);
    DWORD wr = 0;
    if (!WriteFile(u->h, fr, k, &wr, NULL) || (int)wr != k) { logf_("tryx: write failed (error %lu)", GetLastError()); return 0; }
    return 1;
}

// Reads what came in; each whole answer goes to got() (or the log when it is not 200). Returns 0 if the port is gone.
static int rx(tryx_t *u, void (*got)(tryx_t *u, const char *text, void *ctx), void *ctx) {
    DWORD rd = 0;
    if (!ReadFile(u->h, u->rx + u->rn, (DWORD)(sizeof(u->rx) - 1 - u->rn), &rd, NULL)) return 0;
    u->rn += rd;
    for (;;) {
        unsigned char *a = memchr(u->rx, 0x5A, u->rn);
        if (!a) { u->rn = 0; break; }
        unsigned char *b = memchr(a + 1, 0x5A, u->rn - (int)(a + 1 - u->rx));
        if (!b) {   // half a frame: keep it, unless it fills the buffer
            u->rn -= (int)(a - u->rx); memmove(u->rx, a, u->rn);
            if (u->rn >= (int)sizeof(u->rx) - 1) u->rn = 0;
            break;
        }
        int n = tryx_unframe(a + 1, (int)(b - a - 1));
        if (n > 0) {
            char text[4096]; memcpy(text, a + 1, n); text[n] = 0;
            if (strncmp(text, "1 200", 5)) logf_("tryx: answer %.300s", text);
            if (got) got(u, text, ctx);
            u->rn -= (int)(b - u->rx); memmove(u->rx, b, u->rn);   // b may open the next frame
        } else { u->rn -= (int)(b - u->rx); memmove(u->rx, b, u->rn); }
    }
    return 1;
}

static void got_conn(tryx_t *u, const char *text, void *ctx) {
    (void)u;
    const char *js = strstr(text, "\r\n\r\n");
    if (strncmp(text, "1 200", 5) || !js) return;
    logf_("tryx: conn %.400s", js + 4);
    *(int *)ctx = 1;
}

typedef struct { const char *host; int sub; HANDLE h; USHORT pid; char com[16]; DWORD err; } find_ctx;

static int take(const wchar_t *path, USHORT pid, int index, const char *com, void *arg) {
    find_ctx *f = arg;
    char host[16]; snprintf(host, sizeof(host), "18D1:%04X", pid);
    if (_stricmp(host, f->host) || index != (f->sub < 0 ? 0 : f->sub)) return 0;
    f->pid = pid; strcpy_s(f->com, sizeof(f->com), com);
    f->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f->h == INVALID_HANDLE_VALUE) f->err = GetLastError(), logf_("tryx: %s cannot be opened (error %lu): TRYX KANALI has it?", com, f->err);
    return 1;
}

static void tryx_close(ext_dev *d);

static int tryx_open(ext_dev *d) {
    find_ctx f = { d->host, d->sub, INVALID_HANDLE_VALUE };
    if (!each_port(take, &f) || f.h == INVALID_HANDLE_VALUE) {
        // access denied: another program holds the port, in practice TRYX KANALI (it lives on in the tray); the UI words it
        if (f.err == ERROR_ACCESS_DENIED) snprintf(d->info, sizeof(d->info), "%s busy: TRYX KANALI", f.com);
        return 0;
    }
    tryx_t *u = calloc(1, sizeof(tryx_t));
    if (!u) { CloseHandle(f.h); return 0; }
    u->h = f.h; u->on = u->br = -1; u->pid = f.pid;
    d->priv = u;
    // the device ignores the port for a few seconds after it starts: conn until it answers
    int ok = port_setup(u->h), up = 0;
    for (int t = 0; ok && t < 6 && !up; t++) {
        ok = tx(u, "POST", "conn", "{}");
        for (int w = 0; ok && w < 10 && !up; w++) { Sleep(100); ok = rx(u, got_conn, &up); }
    }
    if (!up) { logf_("tryx: %s does not answer conn", f.com); tryx_close(d); return 0; }
    d->nleds = 1;
    snprintf(d->info, sizeof(d->info), "TRYX PANORAMA%s · %s · USB", f.pid == 0x2D04 ? " SE" : "", f.com);
    strcpy_s(u->base, sizeof(u->base), d->info);
    return 1;
}

static void got_any(tryx_t *u, const char *text, void *ctx) { (void)u; *(int *)ctx = strncmp(text, "1 200", 5) ? -1 : 1; }

// Waits for the next answer: 1 when it is 200
static int answer(tryx_t *u, int ms) {
    int r = 0;
    for (int w = 0; !r && w < ms / 20 && !u->stop; w++) { Sleep(20); if (!rx(u, got_any, &r)) return 0; }
    return r == 1;
}

static int show_media(tryx_t *u, const char *name) {
    // ratio: the firmware knows only "" (native), 1:1, 3:2, 4:3 and 16:9 (tryxctl); 16:9 is 2D04's 1920x1080
    char b[512]; int vid = strstr(name, ".mp4") != NULL;
    snprintf(b, sizeof(b), "{\"Type\":\"%s\",\"id\":\"Customization\",\"screenMode\":\"Full Screen\",\"ratio\":\"%s\",\"playMode\":\"Single\","
             "\"media\":[\"%s\"],\"sysinfoDisplay\":[],\"settings\":{\"align\":\"Center\",\"color\":\"#FFFFFF\",\"badges\":[],"
             "\"position\":\"Top\",\"filter\":{\"value\":\"\",\"opacity\":0}}}", vid ? "media" : "image", u->pid == 0x2D04 ? "16:9" : "", name);
    // twice, half a second apart: the device may keep the screen it had cached (reed-tpse)
    int ok = tx(u, "POST", "waterBlockScreenId", b) && answer(u, 1500);
    Sleep(500);
    return tx(u, "POST", "waterBlockScreenId", b) && answer(u, 1500) && ok;
}

// The thread that sends [dev.N] media and shows it
static DWORD WINAPI upload(void *arg) {
    tryx_t *u = arg;
    char sec[24], path[MAX_PATH * 3], md5[33] = "", name[40] = "", b[256];
    snprintf(sec, sizeof(sec), "dev.%d", u->id);
    strcpy_s(path, sizeof(path), cfg_get(sec, "media", ""));
    wchar_t wp[MAX_PATH]; FILE *f = NULL; BYTE *buf = NULL; long size = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH) && (f = _wfopen(wp, L"rb"))) {
        fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
        // shortcut: the whole file in memory, 16 MB at most (the port carries ~40-70 KB/s: 16 MB is 4-7 minutes)
        if (size > 0 && size <= 16 << 20 && (buf = malloc(size)) && fread(buf, 1, size, f) != (size_t)size) { free(buf); buf = NULL; }
        fclose(f);
    }
    BYTE h[16];
    if (!buf || BCryptHash(BCRYPT_MD5_ALG_HANDLE, NULL, 0, buf, (ULONG)size, h, 16)) { logf_("tryx: %s cannot be read or is over 16 MB", path); goto fail; }
    for (int i = 0; i < 16; i++) sprintf(md5 + i * 2, "%02x", h[i]);
    if (!media_name(path, md5, name, sizeof(name))) { logf_("tryx: %s is not a picture, GIF or mp4", path); goto fail; }
    if (u->force || strcmp(cfg_get(sec, "media_sent", ""), name)) {
        logf_("tryx: sending %s as %s (%ld bytes)", path, name, size);
        COMMTIMEOUTS to = { MAXDWORD, 0, 0, 0, 5000 };   // a whole chunk may take ~0.4 s at 115200
        SetCommTimeouts(u->h, &to);
        snprintf(b, sizeof(b), "{\"fileName\":\"%s\",\"fileSize\":%ld,\"type\":\"media\"}", name, size);   // never "firmware"
        if (!tx(u, "POST", "transport", b) || !answer(u, 3000)) { logf_("tryx: transport refused"); goto fail; }
        for (long at = 0; at < size; ) {
            DWORD wr = 0, n = size - at > 4096 ? 4096 : (DWORD)(size - at);
            if (u->stop || !WriteFile(u->h, buf + at, n, &wr, NULL) || !wr) { logf_("tryx: sending stopped at %ld of %ld (error %lu)", at, size, GetLastError()); goto fail; }
            at += wr; u->pct = (LONG)((long long)at * 100 / size);
            Sleep(20);   // tryxctl's pace: the device has no flow control
        }
        int got = answer(u, 15000);   // the device answers once it has fileSize bytes
        snprintf(b, sizeof(b), "{\"fileName\":\"%s\",\"md5\":\"%s\"}", name, md5);
        // a PANORAMA SE took over 5 s to answer for a 2 MB file (a report); a timeout may still mean the file is there
        int done = tx(u, "POST", "transported", b) && answer(u, 20000);
        port_setup(u->h);
        if (done) { cfg_set_and_save(sec, "media_sent", name); logf_("tryx: %s sent", name); }
        else {   // not marked sent (sent again next time); shown if the device took the whole file (it answered) after all
            logf_("tryx: transported not confirmed (%s the file)", got ? "after" : "no answer to");
            if (!got || !show_media(u, name)) { logf_("tryx: %s not shown", name); goto fail; }
            logf_("tryx: %s shown without the confirmation", name);
            free(buf);
            return 0;
        }
    }
    if (!show_media(u, name)) logf_("tryx: %s sent but not shown", name);
    else logf_("tryx: %s shown", name);
    free(buf);
    return 0;
fail:
    free(buf); port_setup(u->h);
    u->failed = 1;
    return 0;
}

static int tryx_send(ext_dev *d, const rgbf *c, int n) {
    (void)c; (void)n;
    tryx_t *u = d->priv;
    if (u->up) {   // own media on its way: the port is the thread's
        if (WaitForSingleObject(u->up, 0) == WAIT_TIMEOUT) { snprintf(d->info, sizeof(d->info), "%s · upload %ld%%", u->base, u->pct); return 1; }
        CloseHandle(u->up); u->up = NULL; u->rn = 0;
        snprintf(d->info, sizeof(d->info), "%s%s", u->base, u->failed ? " · upload failed" : "");
    }
    if (!rx(u, NULL, NULL)) return 0;
    char sec[24], b[512], key[sizeof(u->media)]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    // one setting per round at most: the device drops reads that hold more than two frames
    int follow = cfg_geti(sec, "screen_follow", 1), theme = cfg_geti(sec, "theme", 0);
    int on = _stricmp(cfg_get("general", "effect", ""), "off") != 0, br = (int)(app_brightness_now() * 100 + 0.5f);
    snprintf(key, sizeof(key), "%s|%s", cfg_get(sec, "media", ""), cfg_get(sec, "media_at", ""));
    if (theme == -1 && key[0] != '|' && (u->theme != -1 || strcmp(key, u->media))) {
        // a new pick sends the file again; else what the device already has is shown (media_sent)
        u->force = u->media[0] && strcmp(key, u->media); u->id = d->id; u->pct = 0; u->failed = 0; u->stop = 0;
        strcpy_s(u->media, sizeof(u->media), key); u->theme = -1;
        if (!(u->up = CreateThread(NULL, 0, upload, u, 0, NULL))) logf_("tryx: no upload thread");
    } else if (theme != u->theme && theme >= 1 && theme <= NTHEMES) {
        snprintf(b, sizeof(b), "{\"id\":\"Pre-set %d: %s\",\"settings\":{\"color\":\"#dcdcdc\",\"align\":\"Left\",\"filter\":{\"value\":null,"
                 "\"opacity\":100},\"badges\":[]},\"sysinfoDisplay\":[\"Date&Time\"]}", theme, THEMES[theme - 1]);
        if (!tx(u, "POST", "waterBlockScreenId", b)) return 0;
        u->theme = theme;
    } else if (follow && on != u->on) {
        if (!tx(u, "POST", "waterBlockScreen", on ? "{\"enable\":true}" : "{\"enable\":false}")) return 0;
        u->on = on; u->br = -1;
    } else if (follow && on && br != u->br) {
        snprintf(b, sizeof(b), "{\"value\":%d}", br);
        if (!tx(u, "POST", "brightness", b)) return 0;
        u->br = br;
    } else if (GetTickCount() - u->last_state >= 1000) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        unsigned long long ms = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000;
        float g = app_gpu_temp();
        // shortcut: no CPU sensors here, so the cooler's screen shows no CPU figures; send cpu once haku reads them
        if (g == g) snprintf(b, sizeof(b), "{\"gpu\":{\"temperature\":%.1f},\"timestamp\":%llu}", g, ms);
        else snprintf(b, sizeof(b), "{\"timestamp\":%llu}", ms);
        if (!tx(u, "STATE", "all", b)) return 0;
        u->last_state = GetTickCount();
    }
    return 1;
}

static void tryx_leave(ext_dev *d, int how) {
    char sec[24]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    if (((tryx_t *)d->priv)->up) return;   // the upload has the port; close stops it
    // off: the screen goes dark; otherwise it keeps what it shows (its own "disconnected" screen after a minute)
    if (how == LEAVE_OFF && cfg_geti(sec, "screen_follow", 1)) tx(d->priv, "POST", "waterBlockScreen", "{\"enable\":false}");
}

static void tryx_close(ext_dev *d) {
    tryx_t *u = d->priv;
    if (!u) return;
    if (u->up) { u->stop = 1; WaitForSingleObject(u->up, INFINITE); CloseHandle(u->up); }   // a write gives up within 5 s
    if (u->h && u->h != INVALID_HANDLE_VALUE) CloseHandle(u->h);
    free(u); d->priv = NULL;
}

typedef struct { void (*found)(const disc_t *); } disc_ctx;

static int report(const wchar_t *path, USHORT pid, int index, const char *com, void *arg) {
    (void)path;
    disc_t x = { "tryx" };
    snprintf(x.host, sizeof(x.host), "18D1:%04X", pid);
    x.sub = index; x.nleds = 1;
    char nth[8] = ""; if (index) snprintf(nth, sizeof(nth), " %d", index + 1);
    snprintf(x.name, sizeof(x.name), "TRYX PANORAMA%s%s", pid == 0x2D04 ? " SE" : "", nth);
    snprintf(x.info, sizeof(x.info), "TRYX PANORAMA · %s · USB", com);
    ((disc_ctx *)arg)->found(&x);
    return 0;
}

static void tryx_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    disc_ctx c = { found };
    each_port(report, &c);
}

const ext_driver drv_tryx = { "tryx", "TRYX PANORAMA", 2, 0, tryx_open, tryx_send, tryx_leave, tryx_close, tryx_discover };

#else   // cl /DTRYX_TEST src\drv_tryx.c
#include <assert.h>
int main(void) {
    const char *m = "0123456789abcdef0123456789abcdef"; char n[40];
    assert(media_name("C:\\Pics\\Кот.GIF", m, n, sizeof(n)) && !strcmp(n, "0123456789abcdef.gif"));
    assert(media_name("D:\\v\\clip.mp4", m, n, sizeof(n)) && !strcmp(n, "0123456789abcdef.mp4"));
    assert(media_name("a.jpeg", m, n, sizeof(n)) && !strcmp(n, "0123456789abcdef.jpeg"));
    assert(!media_name("clip.mov", m, n, sizeof(n)) && !media_name("C:\\x.y\\noext", m, n, sizeof(n)) && !media_name("a.firmware", m, n, sizeof(n)));
    puts("tryx ok");
    return 0;
}
#endif
