// SPDX-License-Identifier: GPL-3.0-only
// Philips WiZ bulbs and strips over their local UDP JSON protocol (port 38899), one colour per light, no keys.
// Messages: registration (discovery broadcast), getSystemConfig (model), getPilot (state), setPilot (colour,
// dimming 10..100, state), as used by the WiZ app and the pywizlight project.
#include "devices.h"
#include <stdlib.h>

#define WIZ_PORT 38899

typedef struct { char ip[64], pilot[512]; int have; int last_dim; } wiz_t;

// Sends one request and waits up to ms for the answer with the same method from that host.
static int wiz_call(SOCKET s, const char *ip, const char *req, const char *method, char *out, int cap, int ms) {
    if (!udp_send(s, ip, WIZ_PORT, req, (int)strlen(req))) return 0;
    DWORD end = GetTickCount() + ms;
    char from[48];
    for (int left; (left = (int)(end - GetTickCount())) > 0;) {
        int k = udp_recv(s, out, cap - 1, left, from, sizeof(from));
        if (k <= 0) continue;
        out[k] = 0;
        char m[32]; json_get_str(out, "method", m, sizeof(m));
        if (!strcmp(from, ip) && !strcmp(m, method)) return 1;
    }
    return 0;
}

static void model_name(const char *cfg, char *out, int cap) {
    char mod[48] = "", fw[24] = "";
    const char *r = json_get_obj(cfg, "result");
    json_get_str(r, "moduleName", mod, sizeof(mod));
    json_get_str(r, "fwVersion", fw, sizeof(fw));
    snprintf(out, cap, "WiZ%s%s%s%s", mod[0] ? " · " : "", mod, fw[0] ? " · fw " : "", fw);
}

static void set_pilot(ext_dev *d, const char *params) {
    wiz_t *w = d->priv;
    char m[256]; snprintf(m, sizeof(m), "{\"id\":1,\"method\":\"setPilot\",\"params\":%s}", params);
    udp_send(d->sock, w->ip, WIZ_PORT, m, (int)strlen(m));
}

static int wiz_open(ext_dev *d) {
    wiz_t *w = calloc(1, sizeof(wiz_t));
    if (!w) return 0;
    host_port(d->host, WIZ_PORT, w->ip, sizeof(w->ip));
    d->sock = udp_socket(0, 0);
    char buf[1024];
    if (d->sock == INVALID_SOCKET || !wiz_call(d->sock, w->ip, "{\"method\":\"getPilot\",\"params\":{}}", "getPilot", buf, sizeof(buf), 1200)) {
        if (d->sock != INVALID_SOCKET) closesocket(d->sock);
        d->sock = INVALID_SOCKET; free(w); return 0;
    }
    const char *r = json_get_obj(buf, "result");
    if (r) { snprintf(w->pilot, sizeof(w->pilot), "%s", r); w->have = 1; }
    char cfg[1024];
    if (wiz_call(d->sock, w->ip, "{\"method\":\"getSystemConfig\",\"params\":{}}", "getSystemConfig", cfg, sizeof(cfg), 800)) model_name(cfg, d->info, sizeof(d->info));
    else strcpy_s(d->info, sizeof(d->info), "WiZ");
    d->priv = w; d->nleds = 1;
    return 1;
}

static int wiz_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    wiz_t *w = d->priv;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(g, b));
    char p[128];
    if (mx < 0.02f) {
        if (w->last_dim) set_pilot(d, "{\"state\":false}");
        w->last_dim = 0;
        return 1;
    }
    // brightness goes to dimming (WiZ goes no lower than 10 %), the hue stays at full scale in r/g/b
    int dim = (int)(mx * 100 + 0.5f); if (dim < 10) dim = 10;
    snprintf(p, sizeof(p), "{\"state\":true,\"r\":%d,\"g\":%d,\"b\":%d,\"dimming\":%d}",
             (int)(r / mx * 255 + .5f), (int)(g / mx * 255 + .5f), (int)(b / mx * 255 + .5f), dim);
    set_pilot(d, p);
    w->last_dim = dim;
    return 1;
}

static void wiz_leave(ext_dev *d, int how) {
    wiz_t *w = d->priv;
    if (how == LEAVE_OFF) { set_pilot(d, "{\"state\":false}"); return; }
    if (how != LEAVE_RESTORE || !w->have) return;
    // what it showed before: a scene, a white temperature or a colour, and whether it was on
    const char *pl = w->pilot;
    int on = (int)json_get_num(pl, "state", 1), scene = (int)json_get_num(pl, "sceneId", 0), dim = (int)json_get_num(pl, "dimming", 100);
    int temp = (int)json_get_num(pl, "temp", 0), speed = (int)json_get_num(pl, "speed", 100);
    char p[160];
    if (!on) snprintf(p, sizeof(p), "{\"state\":false}");
    else if (scene > 0) snprintf(p, sizeof(p), "{\"state\":true,\"sceneId\":%d,\"speed\":%d,\"dimming\":%d}", scene, speed, dim);
    else if (temp > 0) snprintf(p, sizeof(p), "{\"state\":true,\"temp\":%d,\"dimming\":%d}", temp, dim);
    else snprintf(p, sizeof(p), "{\"state\":true,\"r\":%d,\"g\":%d,\"b\":%d,\"dimming\":%d}",
                  (int)json_get_num(pl, "r", 255), (int)json_get_num(pl, "g", 255), (int)json_get_num(pl, "b", 255), dim);
    set_pilot(d, p);
}

static void wiz_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

static void wiz_discover(int ms, void (*found)(const disc_t *)) {
    SOCKET s = udp_socket(0, 1);
    if (s == INVALID_SOCKET) return;
    const char *reg = "{\"method\":\"registration\",\"params\":{\"phoneMac\":\"AAAAAAAAAAAA\",\"register\":false,\"phoneIp\":\"1.2.3.4\",\"id\":\"1\"}}";
    udp_send_all_ifaces(s, NULL, WIZ_PORT, reg, (int)strlen(reg));
    char hosts[32][48], macs[32][20]; int nh = 0;
    DWORD end = GetTickCount() + ms / 2;
    char r[1024], from[48];
    for (int left; (left = (int)(end - GetTickCount())) > 0 && nh < 32;) {
        int k = udp_recv(s, r, sizeof(r) - 1, left, from, sizeof(from));
        if (k <= 0) continue;
        r[k] = 0;
        char m[32]; json_get_str(r, "method", m, sizeof(m));
        if (strcmp(m, "registration")) continue;
        int dup = 0;
        for (int i = 0; i < nh; i++) if (!strcmp(hosts[i], from)) dup = 1;
        if (dup) continue;
        json_get_str(json_get_obj(r, "result"), "mac", macs[nh], sizeof(macs[nh]));
        strcpy_s(hosts[nh++], 48, from);
    }
    for (int i = 0; i < nh; i++) {
        disc_t x = { "wiz" };
        strcpy_s(x.host, sizeof(x.host), hosts[i]);
        x.sub = -1; x.nleds = 1;
        int ml = (int)strlen(macs[i]);
        snprintf(x.name, sizeof(x.name), "WiZ %s", ml >= 4 ? macs[i] + ml - 4 : hosts[i]);
        char cfg[1024];
        if (wiz_call(s, hosts[i], "{\"method\":\"getSystemConfig\",\"params\":{}}", "getSystemConfig", cfg, sizeof(cfg), 600)) model_name(cfg, x.info, sizeof(x.info));
        else strcpy_s(x.info, sizeof(x.info), "WiZ");
        found(&x);
    }
    closesocket(s);
}

const ext_driver drv_wiz = { "wiz", "WiZ", 12, 0, wiz_open, wiz_send, wiz_leave, wiz_close, wiz_discover };
