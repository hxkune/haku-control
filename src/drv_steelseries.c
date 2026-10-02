// SPDX-License-Identifier: GPL-3.0-only
// SteelSeries devices through SteelSeries GG (Engine), over its local GameSense API (github.com/SteelSeries/gamesense-sdk).
// The address is in %PROGRAMDATA%\SteelSeries\SteelSeries Engine 3\coreProps.json ({"address":"127.0.0.1:<port>"}).
// haku registers as the game HAKU_CONTROL and binds its events once (POST /game_metadata, /bind_game_event):
//   KEYS     rgb-per-key-zones in "bitmap" mode: 132 colours, a 22 x 6 grid from the top left (the keyboard's columns)
//   ZONESn   rgb-n-zone devices (mice, headsets, mousepads...: 1, 2, 3, 5, 8, 12, 17 and 24 zones), each zone in
//            "context-color" mode from the frame's colours c0..c7 (the device's 8 LEDs spread over its zones)
// Frames are events (POST /game_event, several at once with /multiple_game_events); GG gives the devices their own
// lighting back when no event came for 15 s, or at once after POST /stop_game. GameSense does not say which devices
// are there, so a scan offers the keyboard and the other devices whenever GG runs.
// Written after SteelSeries' documentation, not yet tried on a SteelSeries device.
#include "devices.h"
#include <stdio.h>
#include <stdlib.h>

#define GAME "HAKU_CONTROL"
#define ZCOLS 8   // the "other devices" device: 8 colours

static const int ZONE_TYPES[] = { 1, 2, 3, 5, 8, 12, 17, 24 };
#define NZT (int)(sizeof(ZONE_TYPES) / sizeof(ZONE_TYPES[0]))
static const char *const ZONE_NAMES[24] = { "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
    "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen", "twenty",
    "twenty-one", "twenty-two", "twenty-three", "twenty-four" };

// what is bound, per GG address (opened on connecting threads, used by the device thread)
static SRWLOCK slk = SRWLOCK_INIT;
static struct { char addr[64], ip[64]; int port, gen, refs, keys, zones, multi; } eng;   // zones: bit per ZONE_TYPES

typedef struct { int gen, fails; } ss_t;

static int engine_addr(char *out, int cap) {
    const char *own = cfg_get("steelseries", "address", "");   // set by hand (or a test engine)
    if (own[0]) { snprintf(out, cap, "%s", own); return 1; }
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PROGRAMDATA", path, MAX_PATH)) return 0;
    wcscat_s(path, MAX_PATH, L"\\SteelSeries\\SteelSeries Engine 3\\coreProps.json");
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    char js[1024]; size_t n = fread(js, 1, sizeof(js) - 1, f); js[n] = 0; fclose(f);
    return json_get_str(js, "address", out, cap) && out[0];
}

static int post(const char *path, const char *body, char *why, int whycap) {
    char buf[512];
    int st = http_call(eng.ip, eng.port, "POST", path, body, buf, sizeof(buf), why, whycap);
    return st;
}

// Under slk: GG's address and the bound events; returns the binding's number (0: GG is not there).
static int engine(void) {
    char addr[64];
    if (!engine_addr(addr, sizeof(addr))) return 0;
    if (eng.gen && !strcmp(addr, eng.addr)) return eng.gen;
    snprintf(eng.addr, sizeof(eng.addr), "%s", addr);
    eng.port = host_port(addr, 0, eng.ip, sizeof(eng.ip));
    eng.keys = eng.zones = eng.multi = 0;
    char why[200], body[4096];
    if (!post("/game_metadata", "{\"game\":\"" GAME "\",\"game_display_name\":\"haku control\",\"developer\":\"haku\"}", why, sizeof(why))) {
        static DWORD said;   // every device retries: in the log once a minute
        if (!said || GetTickCount() - said > 60000) { logf_("steelseries: GG at %s does not answer: %s", addr, why); said = GetTickCount(); }
        eng.addr[0] = 0; return 0;
    }
    if (post("/bind_game_event", "{\"game\":\"" GAME "\",\"event\":\"KEYS\",\"value_optional\":true,\"handlers\":"
             "[{\"device-type\":\"rgb-per-key-zones\",\"zone\":\"all\",\"mode\":\"bitmap\"}]}", why, sizeof(why)) == 200) eng.keys = 1;
    else logf_("steelseries: binding KEYS failed: %s", why);
    for (int t = 0; t < NZT; t++) {
        int z = ZONE_TYPES[t];
        int o = snprintf(body, sizeof(body), "{\"game\":\"" GAME "\",\"event\":\"ZONES%d\",\"value_optional\":true,\"handlers\":[", z);
        for (int k = 0; k < z; k++)   // zone k shows the colour at its place along the 8
            o += snprintf(body + o, sizeof(body) - o, "%s{\"device-type\":\"rgb-%d-zone\",\"zone\":\"%s\",\"mode\":\"context-color\",\"context-frame-key\":\"c%d\"}",
                          k ? "," : "", z, ZONE_NAMES[k], (int)((k + 0.5f) * ZCOLS / z));
        snprintf(body + o, sizeof(body) - o, "]}");
        if (post("/bind_game_event", body, why, sizeof(why)) == 200) eng.zones |= 1 << t;
        else logf_("steelseries: binding ZONES%d failed: %s", z, why);
    }
    char buf[128];
    eng.multi = http_call(eng.ip, eng.port, "GET", "/supports_multiple_game_events", NULL, buf, sizeof(buf), NULL, 0) == 200;
    eng.gen++; eng.refs = 0;
    logf_("steelseries: GG at %s, keyboard %s, zone devices %02x, several events at once %s", addr, eng.keys ? "bound" : "not bound", eng.zones, eng.multi ? "yes" : "no");
    return eng.gen;
}

static int ss_open(ext_dev *d) {
    ss_t *s = calloc(1, sizeof(ss_t));
    if (!s) return 0;
    AcquireSRWLockExclusive(&slk);
    s->gen = engine();
    int bound = d->sub == 1 ? eng.zones != 0 : eng.keys;
    if (s->gen && bound) eng.refs++;
    ReleaseSRWLockExclusive(&slk);
    if (!s->gen || !bound) {
        snprintf(d->info, sizeof(d->info), s->gen ? "SteelSeries GG refused the lighting" : "SteelSeries GG is not running");
        free(s); return 0;
    }
    d->priv = s;
    d->nleds = d->sub == 1 ? ZCOLS : 22;
    snprintf(d->info, sizeof(d->info), "SteelSeries GG · %s", d->sub == 1 ? "mouse, headset, mousepad" : "keyboard");
    return 1;
}

static int ss_send(ext_dev *d, const rgbf *c, int n) {
    ss_t *s = d->priv;
    static char body[8192];   // the device thread only
    char why[200] = "";
    int o = 0, st = 0;
    if (d->sub != 1) {   // the keyboard: each column one colour
        o = snprintf(body, sizeof(body), "{\"game\":\"" GAME "\",\"event\":\"KEYS\",\"data\":{\"frame\":{\"bitmap\":[");
        for (int y = 0; y < 6; y++) for (int x = 0; x < 22; x++) {
            rgbf v = x < n ? c[x] : (rgbf){ 0 };
            o += snprintf(body + o, sizeof(body) - o, "%s[%d,%d,%d]", y || x ? "," : "", to8(v.r), to8(v.g), to8(v.b));
        }
        snprintf(body + o, sizeof(body) - o, "]}}}");
    } else {
        char frame[512]; int f = 0;
        for (int i = 0; i < ZCOLS; i++) {
            rgbf v = i < n ? c[i] : n ? c[n - 1] : (rgbf){ 0 };
            f += snprintf(frame + f, sizeof(frame) - f, "%s\"c%d\":{\"red\":%d,\"green\":%d,\"blue\":%d}", i ? "," : "", i, to8(v.r), to8(v.g), to8(v.b));
        }
        AcquireSRWLockShared(&slk);
        int zones = eng.zones, multi = eng.multi;
        ReleaseSRWLockShared(&slk);
        if (multi) {
            o = snprintf(body, sizeof(body), "{\"game\":\"" GAME "\",\"events\":[");
            for (int t = 0, k = 0; t < NZT; t++) if (zones & 1 << t)
                o += snprintf(body + o, sizeof(body) - o, "%s{\"event\":\"ZONES%d\",\"data\":{\"frame\":{%s}}}", k++ ? "," : "", ZONE_TYPES[t], frame);
            snprintf(body + o, sizeof(body) - o, "]}");
        } else {   // an older GG: one request per event
            AcquireSRWLockShared(&slk);
            int ok = s->gen == eng.gen;
            for (int t = 0; ok && t < NZT; t++) if (zones & 1 << t) {
                snprintf(body, sizeof(body), "{\"game\":\"" GAME "\",\"event\":\"ZONES%d\",\"data\":{\"frame\":{%s}}}", ZONE_TYPES[t], frame);
                if (!(st = post("/game_event", body, why, sizeof(why)))) break;
            }
            ReleaseSRWLockShared(&slk);
            goto done;
        }
    }
    AcquireSRWLockShared(&slk);
    st = s->gen == eng.gen ? post(d->sub == 1 ? "/multiple_game_events" : "/game_event", body, why, sizeof(why)) : 0;
    ReleaseSRWLockShared(&slk);
done:
    if (!st) {   // GG closed (or moved to another port): bind again on the next connection
        AcquireSRWLockExclusive(&slk); if (s->gen == eng.gen) eng.addr[0] = 0, eng.gen++; ReleaseSRWLockExclusive(&slk);
        return 0;
    }
    if (st != 200) { if (s->fails++ % 50 == 0) logf_("dev.%d (steelseries): event refused: %s", d->id, why); }
    else s->fails = 0;
    return 1;
}

static void ss_leave(ext_dev *d, int how) {
    // GG lights the devices itself again after /stop_game (and 15 s after the last event); LEAVE_OFF darkens them until then
    if (how == LEAVE_OFF) { rgbf z[22] = { 0 }; ss_send(d, z, d->nleds); }
}

static void ss_close(ext_dev *d) {
    ss_t *s = d->priv;
    if (!s) return;
    AcquireSRWLockExclusive(&slk);
    if (s->gen == eng.gen && --eng.refs <= 0) {
        char why[160];
        post("/stop_game", "{\"game\":\"" GAME "\"}", why, sizeof(why));
    }
    ReleaseSRWLockExclusive(&slk);
    free(s); d->priv = NULL;
}

static void ss_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    char addr[64], ip[64], buf[256];
    if (!engine_addr(addr, sizeof(addr))) return;
    int port = host_port(addr, 0, ip, sizeof(ip));
    if (!port || !http_call(ip, port, "GET", "/supports_multiple_game_events", NULL, buf, sizeof(buf), NULL, 0)) return;   // GG is not running
    disc_t k = { "steelseries", "gg" };
    k.sub = 0; k.nleds = 22;
    snprintf(k.name, sizeof(k.name), "SteelSeries keyboard");
    snprintf(k.info, sizeof(k.info), "SteelSeries GG · keyboard");
    found(&k);
    disc_t z = { "steelseries", "gg" };
    z.sub = 1; z.nleds = ZCOLS;
    snprintf(z.name, sizeof(z.name), "SteelSeries mouse & co");
    snprintf(z.info, sizeof(z.info), "SteelSeries GG · mouse, headset, mousepad");
    found(&z);
}

const ext_driver drv_steelseries = { "steelseries", "SteelSeries GG", 15, 1, ss_open, ss_send, ss_leave, ss_close, ss_discover };
