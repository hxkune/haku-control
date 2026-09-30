// SPDX-License-Identifier: GPL-3.0-only
// Elgato lights (Key Light, Key Light Air / Mini, Ring Light, Light Strip) over their local HTTP API on port 9123,
// found by mDNS (_elg._tcp), no keys: GET /elgato/accessory-info (model), GET / PUT /elgato/lights
// {"numberOfLights":1,"lights":[{"on":1,"brightness":0..100,"temperature":143..344}]}.
// The Key Lights only give white light: a colour becomes its brightness plus the nearest colour temperature
// (2900..7000 K, in mireds), so they are added in the white mode. The Light Strip takes hue / saturation instead.
#include "devices.h"
#include <stdlib.h>

#define ELG_PORT 9123
#define MIRED_MIN 143   // 7000 K
#define MIRED_MAX 344   // 2900 K

typedef struct {
    char ip[64]; int port;
    int colour;              // Light Strip: hue / saturation
    int nlights;
    char saved[1024];        // GET /elgato/lights as it was before (for LEAVE_RESTORE)
    int sent_on, sent_bri, sent_a, sent_b, have_sent, fails;
    DWORD sent_at;
} elg_t;

// The same approximation the effects use for white light (effects.c), so white mode maps back exactly.
static rgbf kelvin_rgb(int k) {
    float t = k / 100.0f, r, g, b;
    if (t <= 66) { r = 255; g = 99.47f * logf(t) - 161.12f; }
    else { r = 329.70f * powf(t - 60, -0.1332f); g = 288.12f * powf(t - 60, -0.0755f); }
    b = t >= 66 ? 255 : t <= 19 ? 0 : 138.52f * logf(t - 10) - 305.04f;
    rgbf c = { clampf(r, 0, 255) / 255, clampf(g, 0, 255) / 255, clampf(b, 0, 255) / 255 };
    return c;
}

// A colour's nearest white, in mireds: compared at full brightness (each colour divided by its largest channel).
static int nearest_mired(float r, float g, float b, float mx) {
    static rgbf tab[MIRED_MAX - MIRED_MIN + 1]; static int ready;
    if (!ready) {
        for (int m = MIRED_MIN; m <= MIRED_MAX; m++) {
            rgbf c = kelvin_rgb(1000000 / m); float k = max(c.r, max(c.g, c.b));
            tab[m - MIRED_MIN].r = c.r / k; tab[m - MIRED_MIN].g = c.g / k; tab[m - MIRED_MIN].b = c.b / k;
        }
        ready = 1;
    }
    r /= mx; g /= mx; b /= mx;
    int best = 213; float bd = 1e9f;
    for (int m = MIRED_MIN; m <= MIRED_MAX; m++) {
        rgbf t = tab[m - MIRED_MIN];
        float d = (r - t.r) * (r - t.r) + (g - t.g) * (g - t.g) + (b - t.b) * (b - t.b);
        if (d < bd) { bd = d; best = m; }
    }
    return best;
}

static void model_info(const char *js, char *info, int icap, char *name, int ncap) {
    char prod[48] = "", disp[64] = "", fw[24] = "";
    json_get_str(js, "productName", prod, sizeof(prod));
    json_get_str(js, "displayName", disp, sizeof(disp));
    json_get_str(js, "firmwareVersion", fw, sizeof(fw));
    snprintf(info, icap, "%s%s%s", prod[0] ? prod : "Elgato", fw[0] ? " · fw " : "", fw);
    if (name) snprintf(name, ncap, "%s", disp[0] ? disp : prod[0] ? prod : "Elgato light");
}

static int put_lights(ext_dev *d, const char *light) {
    elg_t *e = d->priv;
    char body[640], buf[1024];
    int n = snprintf(body, sizeof(body), "{\"numberOfLights\":%d,\"lights\":[", e->nlights);
    for (int i = 0; i < e->nlights && n < (int)sizeof(body) - 140; i++) n += snprintf(body + n, sizeof(body) - n, "%s%s", i ? "," : "", light);
    snprintf(body + n, sizeof(body) - n, "]}");
    int st = http_request(e->ip, e->port, "PUT", "/elgato/lights", body, buf, sizeof(buf));
    return st >= 200 && st < 300;
}

static int elg_open(ext_dev *d) {
    elg_t *e = calloc(1, sizeof(elg_t));
    if (!e) return 0;
    e->port = host_port(d->host, ELG_PORT, e->ip, sizeof(e->ip));
    char buf[2048];
    if (http_request(e->ip, e->port, "GET", "/elgato/lights", NULL, buf, sizeof(buf)) != 200 || !strstr(buf, "\"lights\"")) { free(e); return 0; }
    snprintf(e->saved, sizeof(e->saved), "%s", buf);
    e->nlights = (int)json_get_num(buf, "numberOfLights", 1);
    if (e->nlights < 1 || e->nlights > 4) e->nlights = 1;
    e->colour = strstr(buf, "\"hue\"") != NULL;
    if (http_request(e->ip, e->port, "GET", "/elgato/accessory-info", NULL, buf, sizeof(buf)) == 200) model_info(buf, d->info, sizeof(d->info), NULL, 0);
    else strcpy_s(d->info, sizeof(d->info), "Elgato");
    if (e->colour) strncat_s(d->info, sizeof(d->info), " · colour", _TRUNCATE);
    d->priv = e; d->nleds = 1;
    // the first time a Key Light shows up it is put in the white mode (4500 K): it has no colours of its own
    char sec[16], zone[24]; snprintf(sec, sizeof(sec), "dev.%d", d->id); snprintf(zone, sizeof(zone), "zone.dev%d", d->id);
    if (!e->colour && !cfg_geti(sec, "white_set", 0)) {
        if (!cfg_get(zone, "mode", NULL)) { cfg_set(zone, "mode", "white"); cfg_set(zone, "kelvin", "4500"); }
        cfg_set(sec, "white_set", "1");
        cfg_save_if_dirty();
        app_config_changed(0);
        ui_refresh_state();
    }
    return 1;
}

static int elg_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    elg_t *e = d->priv;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(g, b));
    int on = mx >= 0.02f, bri = (int)(mx * 100 + 0.5f), a = 0, bb = 0;
    if (bri < 1) bri = 1;
    char light[128];
    if (!on) snprintf(light, sizeof(light), "{\"on\":0}");
    else if (e->colour) {   // hue 0..360, saturation 0..100
        float mn = min(r, min(g, b)), dd = mx - mn, h = 0;
        if (dd > 0) h = mx == r ? fmodf((g - b) / dd, 6) : mx == g ? (b - r) / dd + 2 : (r - g) / dd + 4;
        h *= 60; if (h < 0) h += 360;
        a = (int)(h + 0.5f) % 360; bb = (int)(dd / mx * 100 + 0.5f);
        snprintf(light, sizeof(light), "{\"on\":1,\"brightness\":%d,\"hue\":%d,\"saturation\":%d}", bri, a, bb);
    } else {
        a = nearest_mired(r, g, b, mx);
        snprintf(light, sizeof(light), "{\"on\":1,\"brightness\":%d,\"temperature\":%d}", bri, a);
    }
    // an HTTP request per change: nothing is sent while the light stays the same (again every 5 s, in case it
    // was changed in Elgato's own app)
    DWORD now = GetTickCount();
    if (e->have_sent && e->sent_on == on && (!on || (e->sent_bri == bri && e->sent_a == a && e->sent_b == bb)) && now - e->sent_at < 5000) return 1;
    if (!put_lights(d, light)) return ++e->fails < 3;   // one lost request is not a lost light
    e->fails = 0; e->have_sent = 1; e->sent_on = on; e->sent_bri = bri; e->sent_a = a; e->sent_b = bb; e->sent_at = now;
    return 1;
}

static void elg_leave(ext_dev *d, int how) {
    elg_t *e = d->priv;
    if (how == LEAVE_OFF) { put_lights(d, "{\"on\":0}"); return; }
    if (how != LEAVE_RESTORE || !e->saved[0]) return;
    char buf[1024];
    http_request(e->ip, e->port, "PUT", "/elgato/lights", e->saved, buf, sizeof(buf));   // the state as it was read
}

static void elg_close(ext_dev *d) { free(d->priv); d->priv = NULL; }

typedef struct { char ips[16][48]; int n; } elg_hits;
static void elg_hit(const char *ip, void *p) {
    elg_hits *h = p;
    for (int i = 0; i < h->n; i++) if (!strcmp(h->ips[i], ip)) return;
    if (h->n < 16) strcpy_s(h->ips[h->n++], 48, ip);
}

static void elg_discover(int ms, void (*found)(const disc_t *)) {
    elg_hits h = { 0 };
    mdns_browse("_elg._tcp.local", ms / 2, elg_hit, &h);
    for (int i = 0; i < h.n; i++) {
        char buf[2048];
        if (http_request(h.ips[i], ELG_PORT, "GET", "/elgato/accessory-info", NULL, buf, sizeof(buf)) != 200) continue;
        disc_t x = { "elgato" };
        strcpy_s(x.host, sizeof(x.host), h.ips[i]);
        x.sub = -1; x.nleds = 1;
        model_info(buf, x.info, sizeof(x.info), x.name, sizeof(x.name));
        found(&x);
    }
}

const ext_driver drv_elgato = { "elgato", "Elgato", 10, 0, elg_open, elg_send, elg_leave, elg_close, elg_discover };
