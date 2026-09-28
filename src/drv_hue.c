// Philips Hue through the bridge's local REST API (v1). One haku device = one bridge; its colour lights are
// the device's "LEDs". Pairing: when added, the bridge's link button has to be pressed; the app user name the
// bridge hands out is stored in [dev.N] key=. The bridge takes about 10 light commands a second, so each frame
// updates the light whose colour moved the most.
#include "devices.h"
#include <stdlib.h>

#define HUE_MAX 32

typedef struct {
    int n, id[HUE_MAX];
    int on0[HUE_MAX], bri0[HUE_MAX], ct0[HUE_MAX], ctmode[HUE_MAX]; float x0[HUE_MAX], y0[HUE_MAX];
    rgbf sent[HUE_MAX]; int sent_on[HUE_MAX], have_sent[HUE_MAX];
    char ip[64];
} hue_t;

static char big[256 * 1024];   // lights JSON (worker thread only)

static void rgb_to_xy(rgbf c, float *x, float *y) {
    float r = clampf(c.r, 0, 1), g = clampf(c.g, 0, 1), b = clampf(c.b, 0, 1);
    r = r > 0.04045f ? powf((r + 0.055f) / 1.055f, 2.4f) : r / 12.92f;
    g = g > 0.04045f ? powf((g + 0.055f) / 1.055f, 2.4f) : g / 12.92f;
    b = b > 0.04045f ? powf((b + 0.055f) / 1.055f, 2.4f) : b / 12.92f;
    float X = r * 0.664511f + g * 0.154324f + b * 0.162028f;
    float Y = r * 0.283881f + g * 0.668433f + b * 0.047685f;
    float Z = r * 0.000088f + g * 0.072310f + b * 0.986039f;
    float s = X + Y + Z;
    if (s <= 0) { *x = 0.3127f; *y = 0.3290f; return; }
    *x = X / s; *y = Y / s;
}

// Walks the top-level object {"1":{...},"2":{...}} and calls fn(key, object start) for each member.
static void each_member(const char *js, void (*fn)(const char *key, const char *obj, void *ctx), void *ctx) {
    int depth = 0, instr = 0;
    char key[32]; int kl = 0, readkey = 0;
    for (const char *p = js; *p; p++) {
        char c = *p;
        if (instr) {
            if (c == '\\' && p[1]) { p++; continue; }
            if (c == '"') { instr = 0; if (readkey) { key[kl] = 0; readkey = 2; } continue; }
            if (readkey == 1 && kl < 31) key[kl++] = c;
            continue;
        }
        if (c == '"') { instr = 1; if (depth == 1) { readkey = 1; kl = 0; } continue; }
        if (c == '{' || c == '[') {
            if (depth == 1 && readkey == 2 && c == '{') fn(key, p, ctx);
            depth++; readkey = 0; continue;
        }
        if (c == '}' || c == ']') { depth--; continue; }
        if (c == ',' && depth == 1) readkey = 0;
    }
}

static void add_light(const char *key, const char *obj, void *p) {
    hue_t *h = p;
    char type[48] = "";
    json_get_str(obj, "type", type, sizeof(type));
    _strlwr_s(type, sizeof(type));
    if (!strstr(type, "color light") || h->n >= HUE_MAX) return;   // "Extended color light", "Color light"
    int i = h->n++;
    h->id[i] = atoi(key);
    const char *st = json_get_obj(obj, "state");
    h->on0[i] = (int)json_get_num(st, "on", 1);
    h->bri0[i] = (int)json_get_num(st, "bri", 254);
    h->ct0[i] = (int)json_get_num(st, "ct", 366);
    char mode[8] = ""; json_get_str(st, "colormode", mode, sizeof(mode));
    h->ctmode[i] = !strcmp(mode, "ct");
    const char *xy = json_get_obj(st, "xy");
    h->x0[i] = 0.3127f; h->y0[i] = 0.3290f;
    if (xy) sscanf_s(xy, "[%f,%f", &h->x0[i], &h->y0[i]);
}

static int hue_open(ext_dev *d) {
    char ip[64]; int port = host_port(d->host, 80, ip, sizeof(ip));
    if (!d->key[0]) {
        char buf[1024];
        int st = http_request(ip, port, "POST", "/api", "{\"devicetype\":\"haku_control#pc\"}", buf, sizeof(buf));
        char user[128] = "";
        if (st == 200 && json_get_str(buf, "username", user, sizeof(user)) && user[0]) {
            ext_save_key(d, user);
            logf_("hue: paired with bridge %s", ip);
        } else {
            strcpy_s(d->info, sizeof(d->info), st ? "Press the link button on the Hue bridge" : "Bridge not reachable");
            return 0;
        }
    }
    char path[200]; snprintf(path, sizeof(path), "/api/%s/lights", d->key);
    if (http_request(ip, port, "GET", path, NULL, big, sizeof(big)) != 200) return 0;
    if (strstr(big, "unauthorized user")) {
        strcpy_s(d->info, sizeof(d->info), "Bridge forgot this app: remove and add it again");
        return 0;
    }
    hue_t *h = calloc(1, sizeof(hue_t));
    if (!h) return 0;
    strcpy_s(h->ip, sizeof(h->ip), d->host);
    each_member(big, add_light, h);
    if (!h->n) { free(h); strcpy_s(d->info, sizeof(d->info), "No colour lights on this bridge"); return 0; }
    d->nleds = h->n;
    d->priv = h;
    snprintf(d->info, sizeof(d->info), "Philips Hue · %d colour light%s", h->n, h->n == 1 ? "" : "s");
    return 1;
}

static int put_state(ext_dev *d, int id, const char *body) {
    hue_t *h = d->priv;
    char ip[64]; int port = host_port(h->ip, 80, ip, sizeof(ip));
    char path[200], buf[1024];
    snprintf(path, sizeof(path), "/api/%s/lights/%d/state", d->key, id);
    return http_request(ip, port, "PUT", path, body, buf, sizeof(buf)) == 200;
}

static int hue_send(ext_dev *d, const rgbf *c, int n) {
    hue_t *h = d->priv;
    if (n > h->n) n = h->n;
    // the light whose colour changed the most since it was last sent
    int best = -1; float bd = 0.02f;
    for (int i = 0; i < n; i++) {
        float dd = !h->have_sent[i] ? 10 : fabsf(c[i].r - h->sent[i].r) + fabsf(c[i].g - h->sent[i].g) + fabsf(c[i].b - h->sent[i].b);
        if (dd > bd) { bd = dd; best = i; }
    }
    if (best < 0) return 1;
    rgbf col = c[best];
    float mx = max(col.r, max(col.g, col.b));
    char body[160];
    int on = mx > 0.004f;
    if (!on) snprintf(body, sizeof(body), "{\"on\":false,\"transitiontime\":1}");
    else {
        float x, y; rgb_to_xy(col, &x, &y);
        int bri = (int)(mx * 254 + 0.5f); if (bri < 1) bri = 1;
        snprintf(body, sizeof(body), "{%s\"xy\":[%.4f,%.4f],\"bri\":%d,\"transitiontime\":1}",
                 h->have_sent[best] && h->sent_on[best] ? "" : "\"on\":true,", x, y, bri);
    }
    if (!put_state(d, h->id[best], body)) return 0;
    h->sent[best] = col; h->sent_on[best] = on; h->have_sent[best] = 1;
    return 1;
}

static void hue_leave(ext_dev *d, int how) {
    hue_t *h = d->priv;
    char body[160];
    for (int i = 0; i < h->n; i++) {
        if (how == LEAVE_OFF) put_state(d, h->id[i], "{\"on\":false,\"transitiontime\":3}");
        else if (how == LEAVE_RESTORE) {
            if (h->ctmode[i]) snprintf(body, sizeof(body), "{\"on\":%s,\"bri\":%d,\"ct\":%d,\"transitiontime\":3}", h->on0[i] ? "true" : "false", h->bri0[i], h->ct0[i]);
            else snprintf(body, sizeof(body), "{\"on\":%s,\"bri\":%d,\"xy\":[%.4f,%.4f],\"transitiontime\":3}", h->on0[i] ? "true" : "false", h->bri0[i], h->x0[i], h->y0[i]);
            put_state(d, h->id[i], body);
        }
    }
}

static void hue_close(ext_dev *d) { free(d->priv); d->priv = NULL; }

typedef struct { void (*found)(const disc_t *); } hctx;
static void hue_hit(const char *ip, void *p) {
    hctx *c = p;
    char buf[4096];
    if (http_request(ip, 80, "GET", "/api/config", NULL, buf, sizeof(buf)) != 200) return;
    disc_t x = { "hue" };
    strcpy_s(x.host, sizeof(x.host), ip);
    x.sub = -1;
    if (!json_get_str(buf, "name", x.name, sizeof(x.name))) strcpy_s(x.name, sizeof(x.name), "Hue bridge");
    strcpy_s(x.info, sizeof(x.info), "Philips Hue bridge · press its link button after adding");
    c->found(&x);
}

static void hue_discover(int ms, void (*found)(const disc_t *)) {
    hctx c = { found };
    mdns_browse("_hue._tcp.local", ms, hue_hit, &c);
}

const ext_driver drv_hue = { "hue", "Philips Hue", 10, 0, hue_open, hue_send, hue_leave, hue_close, hue_discover };
