// SPDX-License-Identifier: GPL-3.0-only
// Logitech devices through Logitech G HUB, with the LED library G HUB itself installs (the one games use through
// Logitech's LED SDK): its path is in HKLM\SOFTWARE\Classes\CLSID\{a6519e67-7632-4375-afdf-caa889744403}\ServerBinary,
// where Logitech's own SDK looks for it. Nothing of Logitech's is shipped with haku. The library has to carry a
// valid Logitech signature (haku runs with administrator rights), and only the machine-wide key is read.
// Calls: LogiLedInitWithName / LogiLedInit, LogiLedSetTargetDevice (2: RGB devices, 4: per-key keyboards),
// LogiLedSetLightingFromBitmap (21 x 6 keys, 4 bytes each: B G R A, from the top left), LogiLedSetLighting (r, g, b
// in percent), LogiLedShutdown (G HUB lights them itself again). The SDK does not say which devices are there, so
// a scan offers the keyboard and the other lights whenever G HUB's library is installed.
// Written after Logitech's SDK documentation, not yet tried on a Logitech device.
#include "devices.h"
#include <stdbool.h>
#include <stdlib.h>

#define LOGI_CLSID L"SOFTWARE\\Classes\\CLSID\\{a6519e67-7632-4375-afdf-caa889744403}\\ServerBinary"
#define T_RGB    2
#define T_PERKEY 4
#define BW 21
#define BH 6

typedef bool (*f_init)(void);
typedef bool (*f_init_name)(const char *);
typedef bool (*f_target)(int);
typedef bool (*f_bitmap)(unsigned char *);
typedef bool (*f_light)(int, int, int);
typedef void (*f_shutdown)(void);

// the library, loaded once and kept (opened on connecting threads, used by the device thread)
static SRWLOCK llk = SRWLOCK_INIT;
static struct {
    HMODULE dll; int inited, refs, gen;
    f_init init; f_init_name init_name; f_target target; f_bitmap bitmap; f_light light; f_shutdown shutdown;
} lg;

typedef struct { int gen, fails; } lg_t;

static int dll_path(wchar_t *out, DWORD cap) {
#ifdef HAKU_DEV
    const char *own = cfg_get("logitech", "dll", "");   // test builds: a stand-in library
    if (own[0]) { MultiByteToWideChar(CP_UTF8, 0, own, -1, out, cap); return 2; }
#endif
    DWORD n = cap * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, LOGI_CLSID, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, out, &n)) return 0;
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// Under llk: the library loaded and started; 0 when G HUB (or its library) is not there.
static int start(char *why, int cap) {
    if (lg.inited) return 1;
    if (!lg.dll) {
        wchar_t path[MAX_PATH], signer[256] = L"";
        int k = dll_path(path, MAX_PATH);
        if (!k) { snprintf(why, cap, "Logitech G HUB is not installed"); return 0; }
        if (k == 1 && (!file_signer(path, signer, 256) || !wcsstr(signer, L"Logitech"))) {
            logf_("logitech: %ls is not signed by Logitech (%ls), not loaded", path, signer[0] ? signer : L"no valid signature");
            snprintf(why, cap, "G HUB's LED library is not signed by Logitech");
            return 0;
        }
        lg.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lg.dll) { logf_("logitech: loading %ls failed (%lu)", path, GetLastError()); snprintf(why, cap, "G HUB's LED library did not load"); return 0; }
        lg.init = (f_init)GetProcAddress(lg.dll, "LogiLedInit");
        lg.init_name = (f_init_name)GetProcAddress(lg.dll, "LogiLedInitWithName");
        lg.target = (f_target)GetProcAddress(lg.dll, "LogiLedSetTargetDevice");
        lg.bitmap = (f_bitmap)GetProcAddress(lg.dll, "LogiLedSetLightingFromBitmap");
        lg.light = (f_light)GetProcAddress(lg.dll, "LogiLedSetLighting");
        lg.shutdown = (f_shutdown)GetProcAddress(lg.dll, "LogiLedShutdown");
        logf_("logitech: loaded %ls%s%ls", path, signer[0] ? L", signed by " : L"", signer);
        if (!(lg.init || lg.init_name) || !lg.target || !lg.bitmap || !lg.light || !lg.shutdown) {
            logf_("logitech: the library lacks the LED SDK's functions");
            FreeLibrary(lg.dll); lg.dll = NULL;
            snprintf(why, cap, "G HUB's LED library is not the expected one");
            return 0;
        }
    }
    // G HUB has to run (and allow apps to control the lighting)
    if (!(lg.init_name ? lg.init_name("haku control") : lg.init())) { snprintf(why, cap, "Logitech G HUB is not running"); return 0; }
    lg.inited = 1; lg.refs = 0; lg.gen++;
    logf_("logitech: connected to G HUB");
    return 1;
}

static int lg_open(ext_dev *d) {
    lg_t *l = calloc(1, sizeof(lg_t));
    if (!l) return 0;
    char why[96] = "";
    AcquireSRWLockExclusive(&llk);
    int ok = start(why, sizeof(why));
    if (ok) { lg.refs++; l->gen = lg.gen; }
    ReleaseSRWLockExclusive(&llk);
    if (!ok) { free(l); snprintf(d->info, sizeof(d->info), "%s", why); return 0; }
    d->priv = l;
    d->nleds = d->sub == 1 ? 1 : BW;
    snprintf(d->info, sizeof(d->info), "Logitech G HUB · %s", d->sub == 1 ? "mouse, headset, speakers" : "keyboard");
    return 1;
}

static int lg_send(ext_dev *d, const rgbf *c, int n) {
    lg_t *l = d->priv;
    int ok;
    AcquireSRWLockExclusive(&llk);   // the target device is the library's state: one call pair at a time
    if (l->gen != lg.gen || !lg.inited) ok = 0;
    else if (d->sub == 1) {
        rgbf v = n ? c[0] : (rgbf){ 0 };
        ok = lg.target(T_RGB) && lg.light((int)(clampf(v.r, 0, 1) * 100 + .5f), (int)(clampf(v.g, 0, 1) * 100 + .5f), (int)(clampf(v.b, 0, 1) * 100 + .5f));
    } else {
        unsigned char bmp[BW * BH * 4];
        for (int x = 0; x < BW; x++) {
            rgbf v = x < n ? c[x] : (rgbf){ 0 };
            for (int y = 0; y < BH; y++) {
                unsigned char *p = bmp + (y * BW + x) * 4;
                p[0] = to8(v.b); p[1] = to8(v.g); p[2] = to8(v.r); p[3] = 255;
            }
        }
        ok = lg.target(T_PERKEY) && lg.bitmap(bmp);
    }
    ReleaseSRWLockExclusive(&llk);
    if (ok) { l->fails = 0; return 1; }
    if (l->fails++ % 50 == 0) logf_("dev.%d (logitech): G HUB refused the frame", d->id);
    return l->fails < 30;   // G HUB went away: connect again
}

static void lg_leave(ext_dev *d, int how) {
    // G HUB lights the devices itself again after LogiLedShutdown; LEAVE_OFF darkens them until then
    if (how == LEAVE_OFF) { rgbf z[BW] = { 0 }; lg_send(d, z, d->nleds); }
}

static void lg_close(ext_dev *d) {
    lg_t *l = d->priv;
    if (!l) return;
    AcquireSRWLockExclusive(&llk);
    if (l->gen == lg.gen && lg.inited && --lg.refs <= 0) {   // the last one: G HUB takes the lighting back
        lg.shutdown();
        lg.inited = 0;
        logf_("logitech: let go of G HUB");
    }
    ReleaseSRWLockExclusive(&llk);
    free(l); d->priv = NULL;
}

// G HUB's LED library installed: the keyboard and the other lights are offered (nothing is loaded for that)
static void lg_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    wchar_t path[MAX_PATH];
    if (!dll_path(path, MAX_PATH)) return;
    disc_t k = { "logitech", "ghub" };
    k.sub = 0; k.nleds = BW;
    snprintf(k.name, sizeof(k.name), "Logitech keyboard");
    snprintf(k.info, sizeof(k.info), "Logitech G HUB · keyboard");
    found(&k);
    disc_t o = { "logitech", "ghub" };
    o.sub = 1; o.nleds = 1;
    snprintf(o.name, sizeof(o.name), "Logitech mouse & co");
    snprintf(o.info, sizeof(o.info), "Logitech G HUB · mouse, headset, speakers");
    found(&o);
}

const ext_driver drv_logitech = { "logitech", "Logitech G HUB", 20, 1, lg_open, lg_send, lg_leave, lg_close, lg_discover };
