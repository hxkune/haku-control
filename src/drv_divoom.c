// SPDX-License-Identifier: GPL-3.0-only
// Divoom Times Gate and Times Frame over their local HTTP APIs: JSON commands to http://<ip>/post (Times Gate
// hardware 400) or http://<ip>:9000/divoom_api (hardware 402; the Times Frame, a Linux box, which answers
// {"ReturnCode":0,"DeviceType":"Frame"} pretty-printed, to a POST or a GET with the JSON as its body). The Times Gate wants the
// LocalToken the Divoom app shows in the device's settings ([dev.N] key=), else it answers "DeviceToken is err".
// The Times Gate's RGB lighting follows the effect through Channel/SetRGBInfo. It has two zones, the backlight
// behind the screens and the edge light on the sides, and every call sets both: one zone (the "primary") takes a
// full effect with our colour, the other only one of a few themes of its own; or both take the same effect.
// [dev.N] lights= picks which (see LIGHTS). Brightness is shared. The screens are left alone.
// The Times Frame's light takes Channel/SetAmbientLight ([dev.N] frame_fx= picks its effect); see dv_open.
// Its screen ([dev.N] screen=): own (left alone), dial (one of Divoom's dials, [dev.N] clock=, kept per profile)
// or monitor (haku's own layout: time, date, CPU, memory, GPU temperature, the effect and the profile, through
// Device/EnterCustomControlMode and Device/UpdateDisplayItems). [dev.N] screen_follow=1 (default) switches the
// screen off with the lights. The dial it showed is read at the start and given back when haku lets go.
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
    char cmd[40];   // the light command: Channel/SetRGBInfo, or the Times Frame's Channel/SetAmbientLight
    int frame, fx, had, had_bri, had_cycle, had_eq, had_fx; char had_col[16];   // the Frame: its effect, what it had
    int scr_off, scr_mode, clock_on, orig_clock; DWORD mon_at;   // the Frame's screen: what haku set, what it had
    int lights, frame_dark, sent_on, sent_bri, have_sent, fails, lost; char sent_col[8]; DWORD sent_at;
} dv_t;

// one command (the token added when there is one); returns the HTTP status, the answer in buf
static int call(dv_t *v, const char *cmd_fields, char *buf, int cap) {
    char body[4096];
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

// [dev.N] frame_fx=: the Times Frame's light effect (SelectEffect; its numbers have no names yet)
static int frame_fx_of(int id) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
    int fx = cfg_geti(sec, "frame_fx", 0);
    return fx < 0 || fx > 99 ? 0 : fx;
}

static void rgb_fields(dv_t *v, int id, int on, const char *col, int bri, char *f, int cap) {
    if (v->frame) {
        v->fx = frame_fx_of(id);
        snprintf(f, cap, "\"Command\":\"%s\",\"Brightness\":%d,\"Color\":\"%s\",\"ColorCycle\":0,\"EqOnOff\":0,\"SelectEffect\":%d",
                 v->cmd, on ? bri : 0, col, v->fx);
        return;
    }
    int l = v->lights;
    snprintf(f, cap, "\"Command\":\"%s\",\"OnOff\":%d,\"Color\":\"%s\",\"ColorCycle\":0,\"Brightness\":%d,"
             "\"SelectLightIndex\":%d,\"LightList\":[{\"SelectEffect\":%d},{\"SelectEffect\":%d},{\"SelectEffect\":%d}]",
             v->cmd, on, col, bri, LIGHTS[l].index, LIGHTS[l].l0, LIGHTS[l].l1, LIGHTS[l].l2);
}

static void fetch_clocks(ext_dev *d, dv_t *v, long long devid);

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
        // The Times Frame's light (the DIVOOM letters on its side and the bars under them) takes
        // Channel/SetAmbientLight with Brightness, Color, ColorCycle, EqOnOff and SelectEffect, the fields
        // Channel/GetAmbientLight answers with (found by trying names: this is documented nowhere). What it had
        // is read first, to give back when haku lets go.
        char ans[1024];
        int s2 = call(v, "\"Command\":\"Channel/GetAmbientLight\"", ans, sizeof(ans));
        if (s2 == 200 && answer_ok(ans)) {
            v->had = 1;
            v->had_bri = (int)json_get_num(ans, "Brightness", 100); v->had_cycle = (int)json_get_num(ans, "ColorCycle", 0);
            v->had_eq = (int)json_get_num(ans, "EqOnOff", 0); v->had_fx = (int)json_get_num(ans, "SelectEffect", 0);
            json_get_str(ans, "Color", v->had_col, sizeof(v->had_col));
            if (!d->fails) logf_("dev.%d (divoom frame): its light had effect %d, %s, brightness %d, cycle %d, eq %d", d->id,
                                 v->had_fx, v->had_col, v->had_bri, v->had_cycle, v->had_eq);
        }
        v->frame = 1;
        v->scr_off = -1;   // unknown: the first frame sets it (haku may have switched it off when it last closed)
        s2 = call(v, "\"Command\":\"Channel/GetClockInfo\"", ans, sizeof(ans));
        v->orig_clock = s2 == 200 && answer_ok(ans) ? (int)json_get_num(ans, "ClockId", 0) : 0;
        if (!d->fails) logf_("dev.%d (divoom frame): its screen shows dial %d", d->id, v->orig_clock);
        fetch_clocks(d, v, (long long)json_get_num(buf, "DeviceId", 0));
        snprintf(v->cmd, sizeof(v->cmd), "Channel/SetAmbientLight");
        char f[400]; rgb_fields(v, d->id, 1, "#FFFFFF", 50, f, sizeof(f));
        s2 = call(v, f, ans, sizeof(ans));
        v->frame_dark = !(s2 == 200 && answer_ok(ans));
        if (v->frame_dark && !d->fails) logf_("dev.%d (divoom frame): Channel/SetAmbientLight answered %d: %.200s", d->id, s2, ans);
        snprintf(d->info, sizeof(d->info), "%s", v->frame_dark ? "Times Frame · its light refused haku's colours (see the log)" : "Times Frame · light");
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
    rgb_fields(v, d->id, on, col, bri, f, sizeof(f));
    int st = call(v, f, buf, sizeof(buf));
    if (st != 200 || !answer_ok(buf)) {
        if (!v->fails++ || v->fails % 50 == 0) logf_("dev.%d (divoom): SetRGBInfo answered %d: %.200s", d->id, st, buf);
        return st == 200;
    }
    if (v->fails) { logf_("dev.%d (divoom): SetRGBInfo works again", d->id); v->fails = 0; }
    return 1;
}

// ---------------------------------------------------------------- the Times Frame's screen
enum { SCR_OWN, SCR_MONITOR, SCR_DIAL };

static int screen_mode_of(const char *sec) {
    const char *m = cfg_get(sec, "screen", "own");
    return !_stricmp(m, "monitor") ? SCR_MONITOR : !_stricmp(m, "dial") ? SCR_DIAL : SCR_OWN;
}

static int simple(dv_t *v, const char *fields, const char *what, int id) {
    char ans[512];
    int st = call(v, fields, ans, sizeof(ans)), ok = st == 200 && answer_ok(ans);
    if (!ok) logf_("dev.%d (divoom frame): %s answered %d: %.200s", id, what, st, ans);
    return ok;
}

// the dials Divoom has for this frame (its cloud, as the Divoom app lists them) -> [dev.N] clocks="id:name|..."
static void fetch_clocks(ext_dev *d, dv_t *v, long long devid) {
    static int done[EXT_MAX + 1];
    if (d->id < 0 || d->id > EXT_MAX || done[d->id] || !devid) return;
    done[d->id] = 1;
    (void)v;
    static char js[32768];
    char body[160], url[256];
    snprintf(body, sizeof(body), "{\"DeviceId\":%lld,\"StartNum\":1,\"EndNum\":60,\"DeviceType\":\"Frame\"}", devid);
    const char *base = cfg_get("divoom", "clocks_url", "https://appin.divoom-gz.com/Channel/MyClockGetList");   // (tests)
    if (strlen(base) > 180) return;
    int st = web_call(base, body, js, sizeof(js));
    if (st != 200 || !strstr(js, "\"ClockList\"")) {   // a GET with the fields in the address, as some notes have it
        snprintf(url, sizeof(url), "%s?DeviceId=%lld&StartNum=1&EndNum=60&DeviceType=Frame", base, devid);
        st = web_call(url, NULL, js, sizeof(js));
    }
    const char *p = strstr(js, "\"ClockList\"");
    if (st != 200 || !p) { logf_("dev.%d (divoom frame): Divoom's list of dials answered %d: %.200s", d->id, st, js); return; }
    char out[2048] = ""; int k = 0, n = 0;
    while ((p = strchr(p, '{')) != NULL && n < 60) {
        const char *e = strchr(p, '}');
        if (!e) break;
        char obj[1024]; int len = (int)min(e - p + 1, (ptrdiff_t)sizeof(obj) - 1);
        memcpy(obj, p, len); obj[len] = 0;
        int id = (int)json_get_num(obj, "ClockId", 0);
        char name[64] = ""; json_get_str(obj, "ClockName", name, sizeof(name));
        for (char *c = name; *c; c++) if (*c == '|' || *c == ':' || *c == '"' || *c == '\\' || (unsigned char)*c < 32) *c = ' ';
        if (id > 0 && k < (int)sizeof(out) - 80) { k += snprintf(out + k, sizeof(out) - k, "%s%d:%s", k ? "|" : "", id, name); n++; }
        p = e + 1;
    }
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    if (n && strcmp(cfg_get(sec, "clocks", ""), out)) { cfg_set(sec, "clocks", out); cfg_save_if_dirty(); }
    logf_("dev.%d (divoom frame): %d dials from Divoom", d->id, n);
}

// CPU load since the last call, 0..100
static int cpu_load(void) {
    static ULONGLONG pi, pk, pu;
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return 0;
    ULONGLONG ni = ((ULONGLONG)i.dwHighDateTime << 32) | i.dwLowDateTime, nk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime,
              nu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    ULONGLONG di = ni - pi, dt = (nk - pk) + (nu - pu);   // kernel time includes idle
    pi = ni; pk = nk; pu = nu;
    return dt ? (int)((dt - di) * 100 / dt) : 0;
}

// the monitor's lines: date, CPU, memory, GPU, effect, profile (JSON-escaped)
static void monitor_lines(char t[6][96]) {
    char raw[6][96] = { { 0 } };
    wchar_t w[64];
    if (GetDateFormatEx(L"en-US", 0, NULL, L"dddd, d MMMM", w, 64, NULL)) WideCharToMultiByte(CP_UTF8, 0, w, -1, raw[0], 96, NULL, NULL);
    snprintf(raw[1], 96, "CPU  %d%%", cpu_load());
    MEMORYSTATUSEX ms = { sizeof(ms) }; GlobalMemoryStatusEx(&ms);
    snprintf(raw[2], 96, "RAM  %lu%%", ms.dwMemoryLoad);
    float gt = app_gpu_temp();
    if (gt == gt && gt > 0) snprintf(raw[3], 96, "GPU  %d\xC2\xB0" "C", (int)(gt + 0.5f));   // NAN != NAN
    int e = effect_index(cfg_get("general", "effect", ""));
    if (e >= 0) WideCharToMultiByte(CP_UTF8, 0, g_effects[e].title, -1, raw[4], 96, NULL, NULL);
    int pr = cfg_geti("general", "profile", 0);
    if (pr > 0) { char ps[24]; snprintf(ps, sizeof(ps), "profile.%d", pr); snprintf(raw[5], 96, "%s", cfg_get(ps, "name", "")); }
    for (int i = 0; i < 6; i++) json_escape_to(t[i], 96, raw[i]);
}

static const struct { int id, y, h, size; const char *fg; } MON[6] = {
    { 11, 440, 80, 52, "#9A9AA6" }, { 12, 600, 100, 80, "#F2F2F2" }, { 13, 710, 100, 80, "#F2F2F2" },
    { 14, 820, 100, 80, "#F2F2F2" }, { 15, 960, 80, 60, "ACCENT" }, { 16, 1040, 64, 44, "#8A8A96" },
};

static int monitor_enter(ext_dev *d, dv_t *v) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    const char *bg = cfg_get(sec, "screen_bg", "https://raw.githubusercontent.com/hxkune/haku-control/main/art/frame-bg.jpg");
    if (strpbrk(bg, "\"\\")) bg = "";
    const char *accent = v->sent_col[0] && strcmp(v->sent_col, "#000000") ? v->sent_col : "#00C8FF";
    char t[6][96]; monitor_lines(t);
    static char f[3800];
    int k = snprintf(f, sizeof(f), "\"Command\":\"Device/EnterCustomControlMode\",\"BackgroudImageLocalFlag\":0,\"BackgroudImageAddr\":\"%s\",\"DispList\":["
                     "{\"ID\":1,\"Type\":\"Time\",\"StartX\":0,\"StartY\":120,\"Width\":800,\"Height\":300,\"Align\":2,\"FontSize\":220,\"FontID\":52,"
                     "\"FontColor\":\"#F2F2F2\",\"BgColor\":\"#0C0C0E\"}", bg);
    for (int i = 0; i < 6; i++)
        k += snprintf(f + k, sizeof(f) - k, ",{\"ID\":%d,\"Type\":\"Text\",\"StartX\":0,\"StartY\":%d,\"Width\":800,\"Height\":%d,\"Align\":2,"
                      "\"FontSize\":%d,\"FontID\":52,\"FontColor\":\"%s\",\"BgColor\":\"#0C0C0E\",\"TextMessage\":\"%s\"}",
                      MON[i].id, MON[i].y, MON[i].h, MON[i].size, strcmp(MON[i].fg, "ACCENT") ? MON[i].fg : accent, t[i]);
    snprintf(f + k, sizeof(f) - k, "]");
    int ok = simple(v, f, "Device/EnterCustomControlMode", d->id);
    if (ok) logf_("dev.%d (divoom frame): haku's screen is on", d->id);
    v->mon_at = GetTickCount();
    return ok;
}

static void monitor_update(ext_dev *d, dv_t *v) {
    char t[6][96]; monitor_lines(t);
    char f[1400]; int k = snprintf(f, sizeof(f), "\"Command\":\"Device/UpdateDisplayItems\",\"DispList\":[");
    for (int i = 0; i < 6; i++) k += snprintf(f + k, sizeof(f) - k, "%s{\"ID\":%d,\"TextMessage\":\"%s\"}", i ? "," : "", MON[i].id, t[i]);
    snprintf(f + k, sizeof(f) - k, "]");
    static int told;
    char ans[512]; int st = call(v, f, ans, sizeof(ans));
    if (!(st == 200 && answer_ok(ans)) && !told++) logf_("dev.%d (divoom frame): Device/UpdateDisplayItems answered %d: %.200s", d->id, st, ans);
    v->mon_at = GetTickCount();
}

// leaves what haku put on the screen: its own layout, a dial; back to the dial it had
static void screen_back(ext_dev *d, dv_t *v) {
    char f[120];
    if (v->scr_mode == SCR_MONITOR) simple(v, "\"Command\":\"Device/ExitCustomControlMode\"", "Device/ExitCustomControlMode", d->id);
    if (v->scr_mode != SCR_OWN && v->orig_clock > 0) {
        snprintf(f, sizeof(f), "\"Command\":\"Channel/SetClockSelectId\",\"ClockId\":%d", v->orig_clock);
        simple(v, f, "Channel/SetClockSelectId", d->id);
    }
    v->scr_mode = SCR_OWN; v->clock_on = 0;
}

// once a frame: the screen follows the settings (and the lights, when asked)
static void screen_tick(ext_dev *d, dv_t *v) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    int off = cfg_geti(sec, "screen_follow", 1) && !_stricmp(cfg_get("general", "effect", ""), "off");
    if (off != v->scr_off) {
        char f[80]; snprintf(f, sizeof(f), "\"Command\":\"Channel/OnOffScreen\",\"OnOff\":%d", !off);
        simple(v, f, "Channel/OnOffScreen", d->id);
        v->scr_off = off;
    }
    if (off) return;
    int m = screen_mode_of(sec), clock = cfg_geti(sec, "clock", 0);
    if (m != v->scr_mode || (m == SCR_DIAL && clock != v->clock_on)) {
        if (v->scr_mode == SCR_MONITOR || m == SCR_OWN) screen_back(d, v);
        if (m == SCR_DIAL && clock > 0) {
            char f[120]; snprintf(f, sizeof(f), "\"Command\":\"Channel/SetClockSelectId\",\"ClockId\":%d", clock);
            simple(v, f, "Channel/SetClockSelectId", d->id);
        } else if (m == SCR_MONITOR) monitor_enter(d, v);
        v->scr_mode = m; v->clock_on = clock;   // (tried once: a refusal is logged, not repeated every frame)
    } else if (m == SCR_MONITOR && GetTickCount() - v->mon_at >= 5000) monitor_update(d, v);
}

static int dv_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    dv_t *v = d->priv;
    if (v->frame) screen_tick(d, v);
    if (v->frame_dark) return 1;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1), mx = max(r, max(g, b));
    int on = mx >= 0.02f, bri = (int)(mx * 100 + 0.5f);
    char col[8] = "#000000";
    if (on) snprintf(col, sizeof(col), "#%02X%02X%02X", to8(r / mx), to8(g / mx), to8(b / mx));
    // one HTTP request per change (again every 10 s, in case the device or its app changed it)
    DWORD now = GetTickCount();
    int l = lights_of(d->id);
    if (l != v->lights) { v->lights = l; v->have_sent = 0; }
    if (v->frame && frame_fx_of(d->id) != v->fx) v->have_sent = 0;
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
    if (v->frame) {
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
        if (v->scr_mode != SCR_OWN) screen_back(d, v);
        int off = how == LEAVE_OFF && cfg_geti(sec, "screen_follow", 1);
        if (off != v->scr_off) {
            char f[80]; snprintf(f, sizeof(f), "\"Command\":\"Channel/OnOffScreen\",\"OnOff\":%d", !off);
            simple(v, f, "Channel/OnOffScreen", d->id);
        }
    }
    if (v->frame_dark) return;
    if (how == LEAVE_OFF) set_rgb(d, 0, "#000000", 0);
    else if (how == LEAVE_RESTORE && v->frame && v->had && !strpbrk(v->had_col, "\"\\")) {
        // the Times Frame gets back the light it had
        char f[300], buf[512];
        snprintf(f, sizeof(f), "\"Command\":\"Channel/SetAmbientLight\",\"Brightness\":%d,\"Color\":\"%s\",\"ColorCycle\":%d,\"EqOnOff\":%d,\"SelectEffect\":%d",
                 v->had_bri, v->had_col, v->had_cycle, v->had_eq, v->had_fx);
        call(v, f, buf, sizeof(buf));
    }
    // LEAVE_RESTORE on the Times Gate: its light effect can't be read back over this API, so it keeps the last colour
}

static void dv_close(ext_dev *d) { free(d->priv); d->priv = NULL; }

// one request a second at most: the Times Gate's web server is slow
const ext_driver drv_divoom = { "divoom", "Divoom", 1, 0, dv_open, dv_send, dv_leave, dv_close, dv_discover };
