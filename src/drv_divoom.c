// SPDX-License-Identifier: GPL-3.0-only
// Divoom Times Gate (and, being found out, Times Frame) over their local HTTP APIs: JSON commands to
// http://<ip>/post (Times Gate hardware 400) or http://<ip>:9000/divoom_api (hardware 402 as a POST; the Times
// Frame, a Linux box, as a GET with the JSON as its body, answering {"ReturnCode":0}). The Times Gate wants the
// LocalToken the Divoom app shows in the device's settings ([dev.N] key=), else it answers "DeviceToken is err".
// The Times Gate's RGB lighting follows the effect through Channel/SetRGBInfo. It has two zones, the backlight
// behind the screens and the edge light on the sides, and every call sets both: one zone (the "primary") takes a
// full effect with our colour, the other only one of a few themes of its own; or both take the same effect.
// [dev.N] lights= picks which (see LIGHTS). Brightness is shared. The screens are left alone.
// The Times Frame's lights are not documented: it is sent the same command and what it answers is logged.
// Written after the community's notes (github.com/averhaegen/hacs-divoom-times-gate-dev, MIT;
// github.com/mfmseth/divoom for the Times Frame) without a device: every answer is logged, for the diagnostics.
// Finding them: Divoom's own service lists the Divoom devices behind the same internet address as the PC, with
// their LAN IPs (the Divoom app finds them this way). A scan asks it, and a device added by its Device ID (the
// number the Divoom app shows) instead of an IP is looked up there each time it connects; the last IP it had is
// kept ([dev.N] ip=) for when the service does not list it.
#include "devices.h"
#include <stdlib.h>

typedef struct { char name[64], ip[64]; long long id; } dv_lan_t;

// the devices Divoom's service sees on this network; [divoom] lan_url= points it elsewhere (tests). It now and
// then answers with an empty list (asked twice in a row, say), so its last full answer stands in for 10 minutes.
static int lan_fetch(dv_lan_t *out, int max);
static int lan_list(dv_lan_t *out, int max) {
    static dv_lan_t last[16]; static int nlast; static DWORD at;
    static SRWLOCK lk = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lk);
    int n = lan_fetch(out, max);
    if (n) { nlast = min(n, 16); memcpy(last, out, nlast * sizeof(dv_lan_t)); at = GetTickCount(); }
    else if (nlast && GetTickCount() - at < 10 * 60 * 1000) { n = min(nlast, max); memcpy(out, last, n * sizeof(dv_lan_t)); }
    ReleaseSRWLockExclusive(&lk);
    return n;
}
static int lan_fetch(dv_lan_t *out, int max) {
    static char js[16384];
    const char *url = cfg_get("divoom", "lan_url", "https://app.divoom-gz.com/Device/ReturnSameLANDevice");
    int st = web_call(url, "{}", js, sizeof(js)), n = 0;
    const char *p = strstr(js, "\"DeviceList\"");
    while (p && n < max && (p = strchr(p, '{')) != NULL) {
        const char *e = strchr(p, '}');
        if (!e) break;
        char obj[1024]; int len = (int)min(e - p + 1, (ptrdiff_t)sizeof(obj) - 1);
        memcpy(obj, p, len); obj[len] = 0;
        dv_lan_t *d = &out[n];
        json_get_str(obj, "DeviceName", d->name, sizeof(d->name));
        json_get_str(obj, "DevicePrivateIP", d->ip, sizeof(d->ip));
        d->id = (long long)json_get_num(obj, "DeviceId", 0);
        if (d->ip[0] && d->id) n++;
        p = e + 1;
    }
    if (st != 200) logf_("divoom: device list answered %d: %.200s", st, js);
    return n;
}

// "585010": a Device ID, not an address
static int is_device_id(const char *h) {
    if (!*h) return 0;
    for (; *h; h++) if (*h < '0' || *h > '9') return 0;
    return 1;
}

static void dv_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    dv_lan_t l[16];
    int n = lan_list(l, 16);
    for (int i = 0; i < n; i++) {
        disc_t d = { 0 };
        snprintf(d.kind, sizeof(d.kind), "divoom");
        snprintf(d.host, sizeof(d.host), "%lld", l[i].id);   // the ID: it stays when the router gives a new IP
        d.sub = -1; d.nleds = 1;
        snprintf(d.name, sizeof(d.name), "%s", l[i].name[0] ? l[i].name : "Divoom");
        snprintf(d.info, sizeof(d.info), "Divoom · %s", l[i].ip);
        found(&d);
    }
}

// [dev.N] lights=: which of the Times Gate's zones takes haku's colour. SelectLightIndex 0 gives both zones
// LightList[0]; 1 makes the edge light primary (LightList[1], its themes for the backlight in [2]: 6 = off);
// 2 the backlight (LightList[2]; the edge light's themes in [1]: 3 static rainbow, 4 slow colour cycle, 5 off).
// Our colour goes to the effects that take one: Bulb (both 4, edge 4 breathing) and the backlight's solid Bulb (5).
static const struct { const char *name; int index, l0, l1, l2; } LIGHTS[] = {
    { "both", 0, 4, 0, 0 }, { "back", 2, 0, 5, 5 }, { "sides", 1, 0, 4, 6 },
    { "back_cycle", 2, 0, 4, 5 }, { "back_rainbow", 2, 0, 3, 5 },
};

enum { EP_GATE, EP_402, EP_FRAME };
static const struct { int port; const char *path, *method; } EP[] = {
    { 80, "/post", "POST" }, { 9000, "/divoom_api", "POST" }, { 9000, "/divoom_api", "GET" },
};

typedef struct {
    char ip[64]; int port, ep, have_token; long token;
    char cmd[40];   // the light command: Channel/SetRGBInfo, or what the Times Frame took ([dev.N] frame_cmd)
    int lights, frame_dark, sent_on, sent_bri, have_sent, fails, lost; char sent_col[8]; DWORD sent_at;
} dv_t;

// one command (the token added when there is one); returns the HTTP status, the answer in buf
static int call(dv_t *v, const char *cmd_fields, char *buf, int cap) {
    char body[768];
    if (v->have_token) snprintf(body, sizeof(body), "{%s,\"LocalToken\":%ld}", cmd_fields, v->token);
    else snprintf(body, sizeof(body), "{%s}", cmd_fields);
    char why[120];
    int st = http_call(v->ip, v->port, EP[v->ep].method, EP[v->ep].path, body, buf, cap, why, sizeof(why));
    if (!st && why[0]) snprintf(buf, cap, "(%s)", why);   // for the log
    return st;
}

// the Times Gate says error_code 0, the Times Frame ReturnCode 0; anything else ("DeviceToken is err", 1...) is not
static int answer_ok(const char *buf) {
    for (int i = 0; i < 2; i++) {
        const char *e = strstr(buf, i ? "\"ReturnCode\"" : "\"error_code\"");
        if (!e) continue;
        e = strchr(e, ':');
        if (!e) return 0;
        for (e++; *e == ' ' || *e == '\t' || *e == '\r' || *e == '\n' || *e == '"'; e++) {}   // the Frame pretty-prints
        return *e == '0';
    }
    return 0;
}
static int answers(const char *buf) { return strstr(buf, "\"error_code\"") || strstr(buf, "\"ReturnCode\""); }

// [dev.N] lights= as an index into LIGHTS (read each time: a change applies at once)
static int lights_of(int id) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
    const char *ln = cfg_get(sec, "lights", "both");
    for (int i = 0; i < (int)(sizeof(LIGHTS) / sizeof(LIGHTS[0])); i++) if (!_stricmp(ln, LIGHTS[i].name)) return i;
    return 0;
}

static void rgb_fields(dv_t *v, int on, const char *col, int bri, char *f, int cap) {
    int l = v->lights;
    snprintf(f, cap, "\"Command\":\"%s\",\"OnOff\":%d,\"Color\":\"%s\",\"ColorCycle\":0,\"Brightness\":%d,"
             "\"SelectLightIndex\":%d,\"LightList\":[{\"SelectEffect\":%d},{\"SelectEffect\":%d},{\"SelectEffect\":%d}]",
             v->cmd, on, col, bri, LIGHTS[l].index, LIGHTS[l].l0, LIGHTS[l].l1, LIGHTS[l].l2);
}

static int dv_open(ext_dev *d) {
    dv_t *v = calloc(1, sizeof(dv_t));
    if (!v) return 0;
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    char host[64]; snprintf(host, sizeof(host), "%s", d->host);
    if (is_device_id(host)) {
        dv_lan_t l[16];
        int n = lan_list(l, 16), at = -1;
        for (int i = 0; i < n; i++) if (l[i].id == _atoi64(host)) at = i;
        const char *last = cfg_get(sec, "ip", "");
        if (at >= 0) {
            if (!d->fails) logf_("dev.%d (divoom %s): device ID %s is at %s", d->id, host, host, l[at].ip);
            if (strcmp(last, l[at].ip)) { cfg_set(sec, "ip", l[at].ip); cfg_save_if_dirty(); }
            snprintf(host, sizeof(host), "%s", l[at].ip);
        } else if (*last) {
            if (!d->fails) logf_("dev.%d (divoom %s): not in Divoom's list right now, trying its last IP %s", d->id, host, last);
            snprintf(host, sizeof(host), "%s", last);
        } else {
            if (!d->fails) {
                char seen[400] = ""; int k = 0;
                for (int i = 0; i < n; i++) k += snprintf(seen + k, sizeof(seen) - k, "%s%s %lld at %s", i ? ", " : "", l[i].name, l[i].id, l[i].ip);
                logf_("dev.%d (divoom %s): not in Divoom's list of devices on this network (it lists: %s)", d->id, host, n ? seen : "none");
            }
            snprintf(d->info, sizeof(d->info), "Device ID %s is not on this network (Divoom's list): is the PC on the same Wi-Fi?", host);
            free(v); return 0;
        }
    }
    int port = host_port(host, 0, v->ip, sizeof(v->ip));
    v->have_token = d->key[0] != 0; v->token = atol(d->key);
    v->lights = lights_of(d->id);
    // the endpoint: the Times Gate's /post on 80, then :9000 as a POST (hardware 402), then as a GET (Times Frame);
    // a port in the address ([dev.N] host=ip:port) is tried with each. What each one answered goes to the log.
    char buf[2048], tried[600] = ""; int st = 0, found = 0, k = 0;
    for (int i = 0; i < 3 && !found; i++) {
        if (port == 80 && i) break;
        if (port == 9000 && !i) continue;
        v->ep = i; v->port = port && port != 80 && port != 9000 ? port : EP[i].port;
        st = call(v, "\"Command\":\"Channel/GetAllConf\"", buf, sizeof(buf));
        if (st == 200 && answers(buf)) found = 1;
        else k += snprintf(tried + k, sizeof(tried) - k, "%s%s :%d%s %d %.80s", k ? "; " : "", EP[i].method, v->port, EP[i].path, st, buf);
    }
    if (!found) {
        if (!d->fails) logf_("dev.%d (divoom %s): no Divoom API answers (%s)", d->id, v->ip, tried);
        free(v); return 0;
    }
    if (!d->fails) logf_("dev.%d (divoom %s): %s :%d%s answers: %.300s", d->id, v->ip, EP[v->ep].method, v->port, EP[v->ep].path, buf);
    // the Times Frame answers {"ReturnCode":0,"DeviceType":"Frame"...} (to a POST as well); the Times Gate error_code
    int frame = strstr(buf, "\"ReturnCode\"") && !strstr(buf, "\"error_code\"");
    if (!frame && !answer_ok(buf)) {
        // reachable, but the token is missing or wrong: shown on the device card until it is fixed
        snprintf(d->info, sizeof(d->info), "%s", d->key[0] ? "LocalToken is wrong: see the Divoom app" : "Enter the LocalToken from the Divoom app");
        free(v); return 0;
    }
    snprintf(v->cmd, sizeof(v->cmd), "Channel/SetRGBInfo");
    if (frame) {
        // The Times Frame's light command is documented nowhere, and it refuses the Times Gate's ("Only accept JSON
        // parameters" is its word for an unknown command). Once per run it is asked a few likely names: readers
        // first (harmless, and their answer would name the fields), then setters with the Times Gate's fields.
        // Every answer is logged; a setter it takes is kept ([dev.N] frame_cmd) and used for the colours.
        static const char *const GETS[] = { "Channel/GetRGBInfo", "Device/GetRGBInfo", "Channel/GetLightInfo", "Device/GetLightInfo",
            "Device/GetLedInfo", "Channel/GetAmbientLight", "Device/GetAmbientLight", "Device/GetRGBLight", "Device/GetLightEffect",
            "Device/GetAllConf", "Sys/GetConf" };
        static const char *const SETS[] = { "Device/SetRGBInfo", "Channel/SetLightInfo", "Device/SetLightInfo", "Channel/SetAmbientLight",
            "Device/SetAmbientLight", "Channel/SetRGBLight", "Device/SetRGBLight", "Device/SetLightColor", "Device/SetLightEffect",
            "Device/SetLedInfo" };
        static int probed[EXT_MAX + 1];
        char f[400], ans[1024];
        const char *kept = cfg_get(sec, "frame_cmd", "");
        int took = 0;
        if (*kept && strlen(kept) < sizeof(v->cmd) && !strpbrk(kept, "\"\\")) {
            snprintf(v->cmd, sizeof(v->cmd), "%s", kept);
            rgb_fields(v, 1, "#FFFFFF", 50, f, sizeof(f));
            int s2 = call(v, f, ans, sizeof(ans));
            took = s2 == 200 && answer_ok(ans);
            if (!d->fails) logf_("dev.%d (divoom frame): %s (kept) answered %d: %.200s", d->id, v->cmd, s2, ans);
        }
        if (!took && d->id >= 0 && d->id <= EXT_MAX && !probed[d->id]) {
            probed[d->id] = 1;
            char line[1200] = ""; int k = 0;
            for (int i = 0; i < (int)(sizeof(GETS) / sizeof(GETS[0])); i++) {
                char c[80]; snprintf(c, sizeof(c), "\"Command\":\"%s\"", GETS[i]);
                int s2 = call(v, c, ans, sizeof(ans));
                if (s2 == 200 && answer_ok(ans)) logf_("dev.%d (divoom frame): %s answers: %.400s", d->id, GETS[i], ans);
                else k += snprintf(line + k, sizeof(line) - k, "%s%s %d", k ? ", " : "", GETS[i], s2);
            }
            for (int i = 0; i < (int)(sizeof(SETS) / sizeof(SETS[0])) && !took; i++) {
                snprintf(v->cmd, sizeof(v->cmd), "%s", SETS[i]);
                rgb_fields(v, 1, "#FFFFFF", 50, f, sizeof(f));
                int s2 = call(v, f, ans, sizeof(ans));
                if (s2 == 200 && answer_ok(ans)) {
                    took = 1; cfg_set(sec, "frame_cmd", SETS[i]); cfg_save_if_dirty();
                    logf_("dev.%d (divoom frame): %s is taken: %.200s", d->id, SETS[i], ans);
                } else k += snprintf(line + k, sizeof(line) - k, "%s%s %d", k ? ", " : "", SETS[i], s2);
            }
            logf_("dev.%d (divoom frame): unknown to it (HTTP 200 = \"Only accept JSON parameters\"): %s", d->id, line);
        }
        v->frame_dark = !took;
        snprintf(d->info, sizeof(d->info), "%s", v->frame_dark ? "Times Frame · its lights don't take haku's colours yet (see the log)" : "Times Frame · lights");
    } else {
        snprintf(d->info, sizeof(d->info), "Divoom · %s%s", v->ep == EP_402 ? "hardware 402" : "hardware 400",
                 strstr(buf, "LightSwitch") ? " · lights" : "");
    }
    d->priv = v; d->nleds = 1;
    return 1;
}

// 1: taken (or refused, which is logged); 0: no answer
static int set_rgb(ext_dev *d, int on, const char *col, int bri) {
    dv_t *v = d->priv;
    char f[400], buf[1024];
    rgb_fields(v, on, col, bri, f, sizeof(f));
    int st = call(v, f, buf, sizeof(buf));
    if (st != 200 || !answer_ok(buf)) {
        if (!v->fails++ || v->fails % 50 == 0) logf_("dev.%d (divoom): SetRGBInfo answered %d: %.200s", d->id, st, buf);
        return st == 200;
    }
    if (v->fails) { logf_("dev.%d (divoom): SetRGBInfo works again", d->id); v->fails = 0; }
    return 1;
}

static int dv_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    dv_t *v = d->priv;
    if (v->frame_dark) return 1;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(g, b));
    int on = mx >= 0.02f, bri = (int)(mx * 100 + 0.5f);
    char col[8] = "#000000";
    if (on) snprintf(col, sizeof(col), "#%02X%02X%02X", to8(r / mx), to8(g / mx), to8(b / mx));
    // one HTTP request per change (again every 10 s, in case the device or its app changed it)
    DWORD now = GetTickCount();
    int l = lights_of(d->id);
    if (l != v->lights) { v->lights = l; v->have_sent = 0; }
    if (v->have_sent && v->sent_on == on && (!on || (abs(v->sent_bri - bri) < 2 && !strcmp(v->sent_col, col))) && now - v->sent_at < 10000) return 1;
    if (!set_rgb(d, on, col, on ? bri : 0)) {
        // the Times Gate's little web server now and then takes no connection: lost only after 3 in a row
        return ++v->lost < 3;
    }
    v->lost = 0;
    v->have_sent = 1; v->sent_on = on; v->sent_bri = bri; strcpy_s(v->sent_col, sizeof(v->sent_col), col); v->sent_at = now;
    return 1;
}

static void dv_leave(ext_dev *d, int how) {
    dv_t *v = d->priv;
    if (how == LEAVE_OFF && !v->frame_dark) set_rgb(d, 0, "#000000", 0);
    // LEAVE_RESTORE: the device's own light effect can't be read back over this API, so it keeps the last colour
}

static void dv_close(ext_dev *d) { free(d->priv); d->priv = NULL; }

// one request a second at most: the Times Gate's web server is slow
const ext_driver drv_divoom = { "divoom", "Divoom", 1, 0, dv_open, dv_send, dv_leave, dv_close, dv_discover };
