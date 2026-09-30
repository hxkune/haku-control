// SPDX-License-Identifier: GPL-3.0-only
// Govee devices through Govee's cloud API (openapi.api.govee.com, "Govee-API-Key"), for the ones without LAN Control,
// such as the AI Sync Box 2 (H6603 / H6604). The key and the device list come from Devices -> Sign-ins (govee.json).
// Two modes ([dev.N] mode):
//   sync:   the device keeps doing its own thing (screen sync / DreamView); haku switches it on with the PC and
//           off when told to (device switch, PC shutdown). Default for devices that have screen sync.
//   colour: one colour from the effect, sent at most every [dev.N] interval seconds (default 3): every command goes
//           over the internet (Govee allows 2 per second per device), so it follows slow effects, not fast ones.
// Commands take up to a second each, so every device has its own thread; send() only hands over the latest colour.
#include "devices.h"
#include <process.h>
#include <stdlib.h>

typedef struct {
    char sku[24], dev[48], key[80];
    int  sync, sync_auto, has_bright;
    HANDLE th, wake;
    volatile LONG run;
    SRWLOCK lk;
    int  want_on, want_rgb, want_bri, have;   // latest wish (lock)
    int  leave;                               // -1, or LEAVE_* to do before the thread ends
    int  sent_on, sent_rgb, sent_bri;         // what Govee has (thread only)
    DWORD last_call;
    char id[64];
} gcloud_t;

static int load(gcloud_t *g, const char *device) {
    wchar_t p[MAX_PATH]; app_data_path(L"govee.json", p);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    static char buf[32 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    json_get_str(buf, "key", g->key, sizeof(g->key));
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\"", device);
    const char *d = strstr(buf, pat);
    if (!d || !g->key[0]) return 0;
    while (d > buf && *d != '{') d--;
    json_get_str(d, "sku", g->sku, sizeof(g->sku));
    snprintf(g->dev, sizeof(g->dev), "%s", device);
    g->has_bright = (int)json_get_num(d, "brightness", 1);
    int dream = (int)json_get_num(d, "dreamview", 0), color = (int)json_get_num(d, "color", 1);
    const char *e = strchr(d, '}');
    if (e && strstr(d, "\"dreamview\"") > e) dream = 0;   // the number belongs to the next device
    g->sync = g->sync_auto = dream || !color;
    SecureZeroMemory(buf, sizeof(buf));
    return 1;
}

// One capability command; returns the HTTP status (0: no answer). Govee takes 2 commands per second per device.
static int control(gcloud_t *g, const char *type, const char *instance, int value) {
    char head[160], body[512], ans[2048];
    DWORD since = GetTickCount() - g->last_call;
    if (g->last_call && since < 600) Sleep(600 - since);
    snprintf(head, sizeof(head), "Govee-API-Key: %s\r\nContent-Type: application/json\r\n", g->key);
    snprintf(body, sizeof(body), "{\"requestId\":\"haku-%lu\",\"payload\":{\"sku\":\"%s\",\"device\":\"%s\",\"capability\":"
             "{\"type\":\"devices.capabilities.%s\",\"instance\":\"%s\",\"value\":%d}}}", GetTickCount(), g->sku, g->dev, type, instance, value);
    int st = acc_https("govee", "POST", "openapi.api.govee.com", "/router/api/v1/device/control", head, body, ans, sizeof(ans));
    g->last_call = GetTickCount();
    SecureZeroMemory(head, sizeof(head));
    if (st != 200) logf_("govee cloud %s: %s=%d refused (%d) %.120s", g->sku, instance, value, st, ans);
    return st;
}

static int mode_sync(const gcloud_t *g, const char *sec) {
    const char *m = cfg_get(sec, "mode", "auto");
    return !_stricmp(m, "colour") || !_stricmp(m, "color") ? 0 : !_stricmp(m, "sync") ? 1 : g->sync_auto;
}

static unsigned __stdcall worker(void *arg) {
    gcloud_t *g = arg;
    DWORD next = 0;
    for (;;) {
        WaitForSingleObject(g->wake, 250);
        AcquireSRWLockShared(&g->lk);
        int on = g->want_on, rgb = g->want_rgb, bri = g->want_bri, have = g->have, leave = g->leave, run = g->run;
        ReleaseSRWLockShared(&g->lk);
        if (!run) {
            if (leave == LEAVE_OFF && g->sent_on != 0) control(g, "on_off", "powerSwitch", 0);
            break;
        }
        char sec[24]; snprintf(sec, sizeof(sec), "dev.%s", g->id);
        float iv = cfg_getf(sec, "interval", 3); if (iv < 1) iv = 1;
        int sync = mode_sync(g, sec);
        if (sync != g->sync) {   // mode changed in the window: back to screen sync, or colours from now on
            g->sync = sync; g->sent_rgb = g->sent_bri = -1;
            if (sync && g->sent_on == 1) control(g, "toggle", "dreamViewToggle", 1);
        }
        if (!have || (int)(GetTickCount() - next) < 0) continue;
        int st = 200;
        if (on != g->sent_on) {
            st = control(g, "on_off", "powerSwitch", on);
            if (st == 200) { g->sent_on = on; if (on && g->sync) control(g, "toggle", "dreamViewToggle", 1); }
        }
        if (on && !g->sync && st == 200) {
            if (rgb != g->sent_rgb) { st = control(g, "color_setting", "colorRgb", rgb); if (st == 200) g->sent_rgb = rgb; }
            if (st == 200 && g->has_bright && abs(bri - g->sent_bri) > 2) { st = control(g, "range", "brightness", bri); if (st == 200) g->sent_bri = bri; }
        }
        next = GetTickCount() + (st == 429 ? 60000 : st != 200 ? 10000 : (DWORD)(iv * 1000));
    }
    return 0;
}

static int gc_open(ext_dev *d) {
    gcloud_t *g = calloc(1, sizeof(gcloud_t));
    if (!g) return 0;
    if (!load(g, d->host)) { SecureZeroMemory(g, sizeof(*g)); free(g); snprintf(d->info, sizeof(d->info), "Sign in to Govee again under Sign-ins"); return 0; }
    char sec[24]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    snprintf(g->id, sizeof(g->id), "%d", d->id);
    g->sync = mode_sync(g, sec);
    InitializeSRWLock(&g->lk);
    g->sent_on = g->sent_rgb = g->sent_bri = -1; g->leave = -1; g->run = 1;
    // sync: on at once and screen sync on (the device's own mode); colour: waits for the first frame
    if (g->sync) {
        if (control(g, "on_off", "powerSwitch", 1) == 0) { SecureZeroMemory(g, sizeof(*g)); free(g); return 0; }
        g->sent_on = 1;
        control(g, "toggle", "dreamViewToggle", 1);
    }
    g->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g->th = (HANDLE)_beginthreadex(NULL, 0, worker, g, 0, NULL);
    d->priv = g; d->nleds = 1;
    snprintf(d->info, sizeof(d->info), "Govee %s · cloud · %s", g->sku, g->sync ? "screen sync" : "colours");
    return 1;
}

static int gc_send(ext_dev *d, const rgbf *c, int n) {
    gcloud_t *g = d->priv;
    if (n < 1 || !g) return 1;
    float r = clampf(c[0].r, 0, 1), gr = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(gr, b));
    AcquireSRWLockExclusive(&g->lk);
    // sync mode: the colour only says on (anything lit) or off (black: switched off / "off" effect)
    g->want_on = mx > 0.02f;
    if (mx > 0.02f) {
        g->want_rgb = (int)(r / mx * 255 + .5f) << 16 | (int)(gr / mx * 255 + .5f) << 8 | (int)(b / mx * 255 + .5f);
        g->want_bri = (int)(mx * 100 + .5f); if (g->want_bri < 1) g->want_bri = 1;
    }
    g->have = 1;
    ReleaseSRWLockExclusive(&g->lk);
    SetEvent(g->wake);
    return 1;
}

static void gc_leave(ext_dev *d, int how) {
    gcloud_t *g = d->priv;
    if (!g) return;
    AcquireSRWLockExclusive(&g->lk); g->leave = how; ReleaseSRWLockExclusive(&g->lk);
}

static void gc_close(ext_dev *d) {
    gcloud_t *g = d->priv;
    if (!g) return;
    AcquireSRWLockExclusive(&g->lk); g->run = 0; if (g->leave < 0) g->leave = LEAVE_KEEP; ReleaseSRWLockExclusive(&g->lk);
    SetEvent(g->wake);
    if (g->th) { WaitForSingleObject(g->th, 4000); CloseHandle(g->th); }
    CloseHandle(g->wake);
    SecureZeroMemory(g, sizeof(*g));
    free(g); d->priv = NULL;
}

// the devices saved at sign-in (no network)
static void gc_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    wchar_t p[MAX_PATH]; app_data_path(L"govee.json", p);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    static char buf[32 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    const char *arr = strstr(buf, "\"devices\"");
    for (const char *o = arr ? strchr(arr, '{') : NULL; o; o = strchr(o + 1, '{')) {
        const char *e = strchr(o, '}'); if (!e) break;
        char one[768]; int l = (int)(e - o); if (l > 767) l = 767;
        memcpy(one, o, l); one[l] = 0;
        disc_t x = { "goveecloud" };
        char sku[24] = "";
        json_get_str(one, "device", x.host, sizeof(x.host));
        json_get_str(one, "name", x.name, sizeof(x.name));
        json_get_str(one, "sku", sku, sizeof(sku));
        if (!x.host[0]) { o = e; continue; }
        if (!x.name[0]) snprintf(x.name, sizeof(x.name), "Govee %s", sku);
        x.sub = -1; x.nleds = 1;
        snprintf(x.info, sizeof(x.info), "Govee %s · cloud%s", sku, strstr(one, "\"dreamview\": 1") ? " · screen sync" : "");
        found(&x);
        o = e;
    }
    SecureZeroMemory(buf, sizeof(buf));
}

const ext_driver drv_goveecloud = { "goveecloud", "Govee (cloud)", 5, 0, gc_open, gc_send, gc_leave, gc_close, gc_discover };
