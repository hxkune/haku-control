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
// own LEDs are on the motherboard's ARGB header. Own pictures need ADB (files go to /sdcard/pcMedia), not done.
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

typedef struct {
    HANDLE h;
    int seq, on, br, theme;     // what was last sent (-1: nothing yet)
    DWORD last_state;
    int rn; unsigned char rx[4096];
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
    u->h = f.h; u->on = u->br = u->theme = -1;
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
    return 1;
}

static int tryx_send(ext_dev *d, const rgbf *c, int n) {
    (void)c; (void)n;
    tryx_t *u = d->priv;
    if (!rx(u, NULL, NULL)) return 0;
    char sec[24], b[512]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    // one setting per round at most: the device drops reads that hold more than two frames
    int follow = cfg_geti(sec, "screen_follow", 1), theme = cfg_geti(sec, "theme", 0);
    int on = _stricmp(cfg_get("general", "effect", ""), "off") != 0, br = (int)(app_brightness_now() * 100 + 0.5f);
    if (theme != u->theme && theme >= 1 && theme <= NTHEMES) {
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
    // off: the screen goes dark; otherwise it keeps what it shows (its own "disconnected" screen after a minute)
    if (how == LEAVE_OFF && cfg_geti(sec, "screen_follow", 1)) tx(d->priv, "POST", "waterBlockScreen", "{\"enable\":false}");
}

static void tryx_close(ext_dev *d) {
    tryx_t *u = d->priv;
    if (!u) return;
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
