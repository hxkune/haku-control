// SPDX-License-Identifier: GPL-3.0-only
// Divoom Times Gate (and, to be seen, Times Frame) over its local HTTP API: JSON commands POSTed to
// http://<ip>/post (hardware 400) or http://<ip>:9000/divoom_api (hardware 402), each with the LocalToken the
// Divoom app shows in the device's settings ([dev.N] key=). Without it the device answers "DeviceToken is err".
// Its RGB lighting follows the effect: Channel/SetRGBInfo with the backlight (behind the screens) as the primary
// zone in the "solid" effect (5) and the colour; the edge light takes a secondary theme ([dev.N] edge=, default 5,
// documented as off). One call always sets both zones, brightness is shared. The screens are left alone.
// Written after the community's notes (github.com/averhaegen/hacs-divoom-times-gate-dev, MIT) without a device:
// every answer is logged, for the diagnostics.
#include "devices.h"
#include <stdlib.h>

typedef struct {
    char ip[64]; int port; char path[24]; long token;
    int edge, sent_on, sent_bri, have_sent, fails; char sent_col[8]; DWORD sent_at;
} dv_t;

// POST one command (the token added); returns the HTTP status, the answer in buf
static int call(dv_t *v, const char *cmd_fields, char *buf, int cap) {
    char body[768];
    snprintf(body, sizeof(body), "{%s,\"LocalToken\":%ld}", cmd_fields, v->token);
    char why[120];
    int st = http_call(v->ip, v->port, "POST", v->path, body, buf, cap, why, sizeof(why));
    if (!st && why[0]) snprintf(buf, cap, "(%s)", why);   // for the log
    return st;
}

// error_code 0 (a number) is success; anything else ("DeviceToken is err", 1, 2...) is not
static int answer_ok(const char *buf) {
    const char *e = strstr(buf, "\"error_code\"");
    if (!e) return 0;
    e += 12; while (*e == ' ' || *e == ':') e++;
    return *e == '0';
}

static int dv_open(ext_dev *d) {
    dv_t *v = calloc(1, sizeof(dv_t));
    if (!v) return 0;
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    int port = host_port(d->host, 0, v->ip, sizeof(v->ip));
    v->token = atol(d->key);
    v->edge = cfg_geti(sec, "edge", 5);
    // the endpoint: as given ([dev.N] host=ip:9000 picks the 402 one), else /post on 80, then 9000
    static const struct { int port; const char *path; } EP[] = { { 80, "/post" }, { 9000, "/divoom_api" } };
    char buf[2048]; int st = 0, found = 0;
    int other = port && port != 80 && port != 9000;   // some other port: /post there
    for (int i = 0; i < 2 && !found; i++) {
        if (port && !other && port != EP[i].port) continue;
        if (other && i) break;
        v->port = other ? port : EP[i].port; snprintf(v->path, sizeof(v->path), "%s", EP[i].path);
        st = call(v, "\"Command\":\"Channel/GetAllConf\"", buf, sizeof(buf));
        if (st == 200 && strstr(buf, "error_code")) found = 1;
    }
    if (!found) { free(v); return 0; }
    if (!d->fails) logf_("dev.%d (divoom %s): %s:%d%s answers: %.300s", d->id, v->ip, v->ip, v->port, v->path, buf);   // (once, not on every retry)
    if (!answer_ok(buf)) {
        // reachable, but the token is missing or wrong: shown on the device card until it is fixed
        snprintf(d->info, sizeof(d->info), "%s", d->key[0] ? "LocalToken is wrong: see the Divoom app" : "Enter the LocalToken from the Divoom app");
        free(v); return 0;
    }
    snprintf(d->info, sizeof(d->info), "Divoom · %s%s", v->port == 9000 ? "hardware 402" : "hardware 400",
             strstr(buf, "LightSwitch") ? " · lights" : "");
    d->priv = v; d->nleds = 1;
    return 1;
}

static int set_rgb(ext_dev *d, int on, const char *col, int bri) {
    dv_t *v = d->priv;
    char f[400], buf[1024];
    snprintf(f, sizeof(f), "\"Command\":\"Channel/SetRGBInfo\",\"OnOff\":%d,\"Color\":\"%s\",\"ColorCycle\":0,\"Brightness\":%d,"
             "\"SelectLightIndex\":2,\"LightList\":[{\"SelectEffect\":0},{\"SelectEffect\":%d},{\"SelectEffect\":5}]", on, col, bri, v->edge);
    int st = call(v, f, buf, sizeof(buf));
    if (st != 200 || !answer_ok(buf)) {
        if (!v->fails++ || v->fails % 50 == 0) logf_("dev.%d (divoom): SetRGBInfo answered %d: %.200s", d->id, st, buf);
        return st == 200;   // a refusal is logged; only a lost connection counts as lost
    }
    if (v->fails) { logf_("dev.%d (divoom): SetRGBInfo works again", d->id); v->fails = 0; }
    return 1;
}

static int dv_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    dv_t *v = d->priv;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(g, b));
    int on = mx >= 0.02f, bri = (int)(mx * 100 + 0.5f);
    char col[8] = "#000000";
    if (on) snprintf(col, sizeof(col), "#%02X%02X%02X", to8(r / mx), to8(g / mx), to8(b / mx));
    // one HTTP request per change (again every 10 s, in case the device or its app changed it)
    DWORD now = GetTickCount();
    if (v->have_sent && v->sent_on == on && (!on || (abs(v->sent_bri - bri) < 2 && !strcmp(v->sent_col, col))) && now - v->sent_at < 10000) return 1;
    if (!set_rgb(d, on, col, on ? bri : 0)) return 0;
    v->have_sent = 1; v->sent_on = on; v->sent_bri = bri; strcpy_s(v->sent_col, sizeof(v->sent_col), col); v->sent_at = now;
    return 1;
}

static void dv_leave(ext_dev *d, int how) {
    if (how == LEAVE_OFF) set_rgb(d, 0, "#000000", 0);
    // LEAVE_RESTORE: the device's own light effect can't be read back over this API, so it keeps the last colour
}

static void dv_close(ext_dev *d) { free(d->priv); d->priv = NULL; }

const ext_driver drv_divoom = { "divoom", "Divoom", 2, 0, dv_open, dv_send, dv_leave, dv_close, NULL };
