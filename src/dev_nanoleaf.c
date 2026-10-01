// SPDX-License-Identifier: GPL-3.0-only
// Nanoleaf (Blocks / Shapes / Canvas / Lines / Elements, Secretlab MAGRGB...) via the local OpenAPI:
//   HTTP :16021 for layout, state and effects,
//   usually: the effect is precomputed (effects_bake) and written once as a looping custom animation that the
//   controller plays itself, so nothing flows over Wi-Fi until the effect, colours, speed or brightness change;
//   live effects (temperature, audio), [nanoleaf] mode=stream, or a controller that refuses custom animations:
//   UDP :60222 extControl v2, at most `rate` frames/s.
// Every controller is its own device with its own thread, layout, zone and settings, in a fixed slot 1..NANO_MAX:
//   slot 1: [nanoleaf], zone.nanoleaf, [layout] nanoleaf_enabled; slot N: [nanoleafN], zone.nanoleafN, nanoleafN_enabled.
// Addresses and auth tokens live in %APPDATA%\haku-control\nanoleaf.json. Pairing (holding the power button, or
// "Connect to API" in Nanoleaf Desktop for strips such as MAGRGB) adds a controller: nano_pair_start().
// The panels' own state (scene / colour / brightness) is captured on connect and restored on exit or when disabled.
#define FD_SETSIZE 256   // subnet scan waits on 254 sockets at once
#include "devices.h"
#include <ws2tcpip.h>
#include <stdlib.h>
#include <process.h>

#define MAX_PANELS NANO_MAX_PANELS

typedef struct { int id, shape; float x, y, path, o; } panel_t;   // o: drawing angle on screen, degrees clockwise
typedef struct { char ip[32], token[64], serial[40]; int slot; } addr_t;

typedef struct {
    int     slot;                  // 1..NANO_MAX, never changes while the controller is paired
    addr_t  cur;
    char    name[64], model[16];
    panel_t panels[MAX_PANELS];
    int     npanels, online;
    float   side_ratio, unit_ratio;   // layout sideLength and one layout unit, in 0..1 map units
    rgbf    target[MAX_PANELS];
    int     ntarget, enabled, have_target;
    // the baked animation (render thread -> this controller's thread)
    rgbf    anim[NANO_MAX_FRAMES * NANO_MAX_PANELS];
    int     anim_frames, anim_panels;
    float   anim_step;
    volatile LONG anim_new, bake_wanted, want_relayout, run, no_custom;
    HANDLE  th, suspend_done;
    // saved panel state
    int     saved, s_on, s_bri, s_hue, s_sat, s_ct;
    char    s_mode[16], s_effect[96];
    char    anim_sel[96];
    char    http_buf[64 * 1024], anim_body[64 * 1024];
} ctl_t;

static ctl_t  *ctl[NANO_MAX];          // by slot - 1; allocated while paired
static SRWLOCK lk = SRWLOCK_INIT;      // the fields other threads read (layout, targets, animation, names)
static SRWLOCK key_lk = SRWLOCK_INIT;  // nanoleaf.json
static volatile LONG layout_new, suspend_req;
static HANDLE  pair_th;
static volatile LONG pair_state;       // PAIR_*
static int     wsa_ok;

enum { PAIR_IDLE, PAIR_SEARCH, PAIR_PRESS, PAIR_OK, PAIR_NOTFOUND, PAIR_TIMEOUT, PAIR_FULL };

// settings section of a slot: [nanoleaf] for the first controller (as before there were several), [nanoleafN]
static void sec_of(int slot, char *out, int cap) { if (slot <= 1) snprintf(out, cap, "nanoleaf"); else snprintf(out, cap, "nanoleaf%d", slot); }

// ---------------------------------------------------------------- HTTP (tiny, blocking, Connection: close)
// Returns the HTTP status (0 on network error); the body is left in buf (NUL-terminated).
static int http(const char *ip, const char *method, const char *path, const char *body, char *buf, int cap) {
    buf[0] = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a = { AF_INET, htons(16021) };
    inet_pton(AF_INET, ip, &a.sin_addr);
    connect(s, (struct sockaddr *)&a, sizeof(a));
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    struct timeval tv = { 2, 0 };
    if (select(0, NULL, &w, NULL, &tv) != 1) { closesocket(s); return 0; }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));

    char req[512];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:16021\r\nContent-Type: application/json\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", method, path, ip, bl);
    if (send(s, req, n, 0) != n) { closesocket(s); return 0; }
    for (int off = 0; off < bl;) {   // bodies can be large (animations)
        int r = send(s, body + off, bl - off, 0);
        if (r <= 0) { closesocket(s); return 0; }
        off += r;
    }
    // Read until the whole answer is in: the controller keeps the connection open despite "Connection: close",
    // so waiting for it to close would cost the full receive timeout (3 s) on every request.
    int got = 0, want = -1;
    for (;;) {
        int r = recv(s, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        buf[got] = 0;
        if (got >= cap - 1) break;
        if (want < 0) {
            char *he = strstr(buf, "\r\n\r\n");
            if (!he) continue;
            int hl = (int)(he + 4 - buf), st = 0, clen = -1;
            sscanf_s(buf, "HTTP/1.%*d %d", &st);
            for (char *p = buf; p < he; p = strstr(p, "\r\n") + 2) {
                if (!_strnicmp(p, "Content-Length:", 15)) { clen = atoi(p + 15); break; }
                if (!strstr(p, "\r\n")) break;
            }
            if (st == 204 || st == 304 || (st >= 100 && st < 200)) clen = 0;
            if (clen >= 0) want = hl + clen;
        }
        if (want >= 0 && got >= want) break;
    }
    closesocket(s);
    buf[got] = 0;
    int status = 0;
    if (sscanf_s(buf, "HTTP/1.%*d %d", &status) != 1) return 0;
    char *b = strstr(buf, "\r\n\r\n");
    if (b) memmove(buf, b + 4, strlen(b + 4) + 1); else buf[0] = 0;
    return status;
}

// Authenticated call to a controller (from its own thread; the answer is in c->http_buf).
static int api(ctl_t *c, const char *method, const char *sub, const char *body) {
    char path[160]; snprintf(path, sizeof(path), "/api/v1/%s%s", c->cur.token, sub);
    return http(c->cur.ip, method, path, body, c->http_buf, sizeof(c->http_buf));
}

// ---------------------------------------------------------------- tiny JSON helpers
// Number (or true/false) after "key": inside [s, ...). Returns def if absent.
static double jnum(const char *s, const char *key, double def) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *v = s ? strstr(s, pat) : NULL;
    if (!v) return def;
    v += strlen(pat);
    while (*v == ' ') v++;
    if (!strncmp(v, "true", 4)) return 1;
    if (!strncmp(v, "false", 5)) return 0;
    return atof(v);
}

// {"key":{"value":N ...}}
static double jvalue(const char *s, const char *key, double def) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":{", key);
    const char *v = s ? strstr(s, pat) : NULL;
    return v ? jnum(v, "value", def) : def;
}

static void jstr(const char *s, const char *key, char *out, int cap) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *v = s ? strstr(s, pat) : NULL;
    out[0] = 0;
    if (!v) return;
    v += strlen(pat);
    while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n' || *v == ':') v++;
    if (*v++ != '"') return;
    int i = 0;
    while (*v && *v != '"' && i < cap - 1) out[i++] = *v++;
    out[i] = 0;
}

// ---------------------------------------------------------------- nanoleaf.json
// {"controllers":[{"slot":1,"ip":"..","token":"..","serial":".."},...]}; the first controller's "ip" and "token"
// come first in the file, so an older version (one controller, {"ip","token"}) still finds that one.
static void key_path(wchar_t *p) { app_data_path(L"nanoleaf.json", p); }

static int load_keys(addr_t *out, int max) {
    wchar_t p[MAX_PATH]; key_path(p);
    AcquireSRWLockShared(&key_lk);
    FILE *f = _wfopen(p, L"rb");
    if (!f) { ReleaseSRWLockShared(&key_lk); if (errno != ENOENT) logf_("nanoleaf: cannot read %ls (errno %d)", p, errno); return 0; }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    ReleaseSRWLockShared(&key_lk);
    int k = 0;
    const char *arr = strstr(buf, "\"controllers\"");
    if (!arr) {   // one controller, the old format
        memset(&out[0], 0, sizeof(out[0]));
        jstr(buf, "ip", out[0].ip, sizeof(out[0].ip));
        jstr(buf, "token", out[0].token, sizeof(out[0].token));
        out[0].slot = 1;
        return out[0].ip[0] && out[0].token[0];
    }
    for (const char *o = strchr(arr, '{'); o && k < max; o = strchr(o + 1, '{')) {
        const char *oe = strchr(o, '}'); if (!oe) break;
        char one[512]; int l = (int)(oe - o); if (l > 511) l = 511;
        memcpy(one, o, l); one[l] = 0;
        addr_t a = { 0 };
        jstr(one, "ip", a.ip, sizeof(a.ip));
        jstr(one, "token", a.token, sizeof(a.token));
        jstr(one, "serial", a.serial, sizeof(a.serial));
        a.slot = (int)jnum(one, "slot", 0);
        if (a.ip[0] && a.token[0] && a.slot >= 1 && a.slot <= NANO_MAX) out[k++] = a;
        o = oe;
    }
    return k;
}

static void save_keys(void) {
    addr_t a[NANO_MAX]; int n = 0;
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < NANO_MAX; i++) if (ctl[i]) a[n++] = ctl[i]->cur;
    ReleaseSRWLockShared(&lk);
    wchar_t p[MAX_PATH]; key_path(p);
    wchar_t d[MAX_PATH]; wcscpy_s(d, MAX_PATH, p);
    wchar_t *s = wcsrchr(d, L'\\'); if (s) { *s = 0; CreateDirectoryW(d, NULL); }
    AcquireSRWLockExclusive(&key_lk);
    FILE *f = _wfopen(p, L"wb");
    if (!f) { ReleaseSRWLockExclusive(&key_lk); logf_("nanoleaf: cannot write key file"); return; }
    fprintf(f, "{\r\n  \"controllers\": [");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\r\n    { \"ip\": \"%s\", \"token\": \"%s\", \"slot\": %d, \"serial\": \"%s\" }", i ? "," : "", a[i].ip, a[i].token, a[i].slot, a[i].serial);
    fprintf(f, "\r\n  ]\r\n}\r\n");
    fclose(f);
    ReleaseSRWLockExclusive(&key_lk);
}

// ---------------------------------------------------------------- layout
// 1D lightstrips report no panel positions at all (Secretlab MAGRGB NL72S2: its layout request answers 500), so
// their zone count comes from the model: [nanoleafN] zones= overrides it (the MAGRGB XL shares the model string
// with 48 zones). Zones are numbered from 0; zone 0 is at the right end.
static int strip_zones(const ctl_t *c, const char *js) {
    char sec[16]; sec_of(c->slot, sec, sizeof(sec));
    int z = cfg_geti(sec, "zones", 0);
    if (z > 0 && z <= MAX_PANELS) return z;
    static const struct { const char *model; int zones; } M[] = { { "NL72S2", 41 } };
    for (int i = 0; i < (int)(sizeof(M) / sizeof(M[0])); i++) if (!_stricmp(c->model, M[i].model)) return M[i].zones;
    int n = (int)jnum(strstr(js, "\"layout\":"), "numPanels", 0);
    return n > 0 && n <= MAX_PANELS ? n : 0;
}

// Panel centres, rotated by the controller's globalOrientation plus the user's rotation, normalised to 0..1 (y = 0 at top).
static void parse_layout(ctl_t *c, const char *js) {
    char sec[16]; sec_of(c->slot, sec, sizeof(sec));
    float rot = (float)jvalue(js, "globalOrientation", 0) + cfg_getf(sec, "rotate", 0);
    int flip = cfg_geti(sec, "flip", 0);
    float side = (float)jnum(strstr(js, "\"layout\":"), "sideLength", 0);
    float cs = cosf(rot * 3.14159265f / 180), sn = sinf(rot * 3.14159265f / 180);
    panel_t p[MAX_PANELS]; int n = 0;
    const char *arr = strstr(js, "\"positionData\"");
    for (const char *o = arr ? strchr(arr, '{') : NULL; o && n < MAX_PANELS; o = strchr(o + 1, '{')) {
        const char *oe = strchr(o, '}'); if (!oe) break;
        char one[256]; int l = (int)(oe - o); if (l > 255) l = 255;
        memcpy(one, o, l); one[l] = 0;
        int id = (int)jnum(one, "panelId", 0), shape = (int)jnum(one, "shapeType", 0);
        // controllers, connectors and caps carry no light (Rhythm 1, Shapes controller 12, Lines connector 16,
        // controller cap 19, power connector 20, Blocks controller 35); lightstrips (MAGRGB) number their zones from 0
        if (shape != 1 && shape != 12 && shape != 16 && shape != 19 && shape != 20 && shape != 35) {
            float x = (float)jnum(one, "x", 0), y = (float)jnum(one, "y", 0);
            // the panel's own orientation goes through the same rotation / mirror as its position; the page draws
            // with y down, where angles turn the other way
            float o = (float)jnum(one, "o", 0) - rot;
            if (flip) o = 180 - o;
            o = fmodf(-o + 720, 360);
            // mirrored, a Shapes triangle's corners (at its angle + 30 on the page) would come out 2 x 30 degrees off:
            // the mirror has to take the 30 with it
            if (flip && (shape == 8 || shape == 9)) o = fmodf(o + 300, 360);
            p[n].id = id; p[n].shape = shape; p[n].o = o;
            p[n].x = x * cs + y * sn;          // layout y points up
            p[n].y = -x * sn + y * cs;
            if (flip) p[n].x = -p[n].x;
            n++;
        }
        o = oe;
        const char *close = strchr(o, ']'), *next = strchr(o, '{');
        if (close && (!next || close < next)) break;
    }
    if (!n) {   // a strip without positions: its zones in a row (the row layout below)
        int z = strip_zones(c, js);
        for (int i = 0; i < z; i++) { p[i].id = i; p[i].shape = 0; p[i].x = p[i].y = p[i].o = 0; }
        n = z;
        if (n) logf_("nanoleaf %s: no panel positions, %d zones for %s", c->cur.ip, n, c->model[0] ? c->model : "this model");
    }
    if (!n) return;
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        x0 = min(x0, p[i].x); x1 = max(x1, p[i].x); y0 = min(y0, p[i].y); y1 = max(y1, p[i].y);
    }
    float w = x1 - x0, h = y1 - y0, span = max(w, h);
    // a strip whose zones all report the same spot: lay them out in a row, zone 0 at the right end (like MAGRGB)
    int row = n > 1 && span < 1;
    if (row) {
        for (int i = 0; i < n; i++) { p[i].x = (float)(flip ? i : n - 1 - i) * 10; p[i].y = 0; p[i].o = 0; }   // flip: mounted the other way round
        x0 = 0; x1 = (float)(n - 1) * 10; y0 = y1 = 0; w = x1; h = 0; span = w;
    }
    if (span < 1) span = 1;
    for (int i = 0; i < n; i++) {
        // keep the aspect ratio: the longer side spans 0..1, the other one is centred
        float nx = (p[i].x - x0 + (span - w) / 2) / span;
        float ny = 1 - (p[i].y - y0 + (span - h) / 2) / span;
        // flowing effects run along the longer side: left -> right, or bottom -> top
        p[i].path = w >= h ? (p[i].x - x0) / span : (p[i].y - y0) / span;
        p[i].x = nx; p[i].y = ny;
    }
    AcquireSRWLockExclusive(&lk);
    memcpy(c->panels, p, sizeof(p)); c->npanels = n;
    c->side_ratio = row ? 0.8f / n : side > 0 ? side / span : 0.2f;
    c->unit_ratio = row ? 0 : 1 / span;
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&layout_new, 1);
    logf_("nanoleaf %s: %d panels, orientation %.0f", c->cur.ip, n, rot);
}

// ---------------------------------------------------------------- state save / restore
static void save_state(ctl_t *c, const char *js) {
    const char *st = strstr(js, "\"state\":");
    const char *fx = strstr(js, "\"effects\":");
    if (!st) return;
    c->s_on  = (int)jvalue(st, "on", 1);
    c->s_bri = (int)jvalue(st, "brightness", 100);
    c->s_hue = (int)jvalue(st, "hue", 0);
    c->s_sat = (int)jvalue(st, "sat", 0);
    c->s_ct  = (int)jvalue(st, "ct", 4000);
    jstr(st, "colorMode", c->s_mode, sizeof(c->s_mode));
    jstr(fx, "select", c->s_effect, sizeof(c->s_effect));
    // left in extControl by a previous run that did not exit cleanly: fall back to white
    if (!strcmp(c->s_effect, "*ExtControl*")) { strcpy_s(c->s_effect, sizeof(c->s_effect), "*Solid*"); strcpy_s(c->s_mode, sizeof(c->s_mode), "hs"); c->s_hue = c->s_sat = 0; }
    c->saved = 1;
    logf_("nanoleaf %s: saved state on=%d bri=%d mode=%s effect=%s", c->cur.ip, c->s_on, c->s_bri, c->s_mode, c->s_effect);
}

static void restore_state(ctl_t *c) {
    if (!c->saved) return;
    char b[160];
    if (c->s_effect[0] && c->s_effect[0] != '*') {
        snprintf(b, sizeof(b), "{\"select\":\"%s\"}", c->s_effect);
        api(c, "PUT", "/effects", b);
    } else if (!strcmp(c->s_mode, "ct")) {
        snprintf(b, sizeof(b), "{\"ct\":{\"value\":%d}}", c->s_ct);
        api(c, "PUT", "/state", b);
    } else {
        snprintf(b, sizeof(b), "{\"hue\":{\"value\":%d},\"sat\":{\"value\":%d}}", c->s_hue, c->s_sat);
        api(c, "PUT", "/state", b);
    }
    snprintf(b, sizeof(b), "{\"brightness\":{\"value\":%d},\"on\":{\"value\":%s}}", c->s_bri, c->s_on ? "true" : "false");
    api(c, "PUT", "/state", b);
    logf_("nanoleaf %s: restored own state", c->cur.ip);
}

static int power(ctl_t *c, int on) {
    return api(c, "PUT", "/state", on ? "{\"on\":{\"value\":true}}" : "{\"on\":{\"value\":false}}") / 100 == 2;
}

static int start_stream(ctl_t *c) {
    int ok = api(c, "PUT", "/effects", "{\"write\":{\"command\":\"display\",\"animType\":\"extControl\",\"extControlVersion\":\"v2\"}}") / 100 == 2;
    if (ok) power(c, 1);   // they may have been switched off by us (device disabled, PC shut down)
    return ok;
}

// What the panels do when the app exits (PC shutdown): [general] on_exit = off | keep | restore
static void exit_action(ctl_t *c) {
    const char *m = cfg_get("general", "on_exit", "off");
    if (!_stricmp(m, "restore")) restore_state(c);
    else if (_stricmp(m, "keep")) power(c, 0);
}

static int on_device(const ctl_t *c) { return !c->no_custom && _stricmp(cfg_get("nanoleaf", "mode", "device"), "stream") != 0; }

// Connect: read name, layout + state, switch to extControl (or wait for the baked loop).
static int connect_panels(ctl_t *c) {
    int st = api(c, "GET", "/", NULL);
    if (st != 200) {
        if (st == 401 || st == 403) logf_("nanoleaf %s: token rejected (%d) — pair again", c->cur.ip, st);
        return 0;
    }
    char nm[64], md[16], sn[40];
    jstr(c->http_buf, "name", nm, sizeof(nm)); jstr(c->http_buf, "model", md, sizeof(md)); jstr(c->http_buf, "serialNo", sn, sizeof(sn));
    AcquireSRWLockExclusive(&lk);
    strcpy_s(c->name, sizeof(c->name), nm[0] ? nm : md); strcpy_s(c->model, sizeof(c->model), md);
    int new_serial = sn[0] && strcmp(sn, c->cur.serial);
    if (new_serial) strcpy_s(c->cur.serial, sizeof(c->cur.serial), sn);
    ReleaseSRWLockExclusive(&lk);
    if (new_serial) save_keys();
    parse_layout(c, c->http_buf);
    if (!c->saved) save_state(c, c->http_buf);
    if (!c->npanels) return 0;
    if (!on_device(c)) return start_stream(c);
    power(c, 1);
    InterlockedExchange(&c->bake_wanted, 1);   // the render thread bakes the current effect, then write_anim()
    return 1;
}

// ---------------------------------------------------------------- the effect on the panels themselves
// The baked loop as a custom animation the controller keeps playing:
// animData = "numPanels  panelId numFrames  R G B W T  R G B W T ...", T = fade time into the frame, in 0.1 s.
// Returns 1 when written, 0 on a network error, -1 when the controller does not take custom animations.
static int write_anim(ctl_t *c) {
    AcquireSRWLockShared(&lk);
    int nf = c->anim_frames, np = c->anim_panels < c->npanels ? c->anim_panels : c->npanels, T = (int)(c->anim_step * 10 + 0.5f);
    int cap = sizeof(c->anim_body) - 64;
    if (T < 1) T = 1;
    int n = snprintf(c->anim_body, sizeof(c->anim_body), "{\"write\":{\"command\":\"display\",\"animType\":\"custom\",\"loop\":true,"
                     "\"palette\":[],\"animData\":\"%d", np);
    for (int p = 0; p < np && n < cap; p++) {
        n += snprintf(c->anim_body + n, sizeof(c->anim_body) - n, " %d %d", c->panels[p].id, nf);
        for (int f = 0; f < nf && n < cap; f++) {
            rgbf k = c->anim[f * NANO_MAX_PANELS + p];
            n += snprintf(c->anim_body + n, sizeof(c->anim_body) - n, " %d %d %d 0 %d", (int)(clampf(k.r, 0, 1) * 255 + .5f),
                          (int)(clampf(k.g, 0, 1) * 255 + .5f), (int)(clampf(k.b, 0, 1) * 255 + .5f), nf == 1 ? 5 : T);
        }
    }
    ReleaseSRWLockShared(&lk);
    if (n >= cap) { logf_("nanoleaf %s: animation too large", c->cur.ip); return 0; }
    snprintf(c->anim_body + n, sizeof(c->anim_body) - n, "\"}}");
    int st = api(c, "PUT", "/effects", c->anim_body);
    if (st >= 400 && st != 401 && st != 403) {
        logf_("nanoleaf %s: custom animations refused (%d), streaming instead", c->cur.ip, st);
        return -1;
    }
    if (st / 100 != 2) { logf_("nanoleaf %s: animation refused (%d): %.160s", c->cur.ip, st, c->http_buf); return 0; }
    c->anim_sel[0] = 0;   // what the controller calls our loop is learnt at the next check
    logf_("nanoleaf %s: playing a %d-frame loop on the panels (%.1f s per frame)", c->cur.ip, nf, T / 10.0);
    return 1;
}

// ---------------------------------------------------------------- finding controllers on the local networks
// Tries every host of the /24 around `base` on port 16021; accept(ip) decides. Collects up to `max` addresses
// (stops at the first when max is 1). Returns how many were found.
static int scan24(ULONG base_host_order, int (*accept)(const char *ip, void *ctx), void *ctx, char (*found)[32], int max) {
    unsigned net = base_host_order & 0xFFFFFF00u;
    SOCKET s[254]; int n = 0, nf = 0;
    for (int h = 1; h < 255; h++) {
        if ((net | h) == base_host_order) continue;   // ourselves
        SOCKET k = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (k == INVALID_SOCKET) continue;
        u_long nb = 1; ioctlsocket(k, FIONBIO, &nb);
        struct sockaddr_in a = { AF_INET, htons(16021) };
        a.sin_addr.s_addr = htonl(net | h);
        connect(k, (struct sockaddr *)&a, sizeof(a));
        s[n++] = k;
    }
    DWORD end = GetTickCount() + 1500;
    int left = n;
    while (left && GetTickCount() < end && nf < max) {
        fd_set w; FD_ZERO(&w);
        for (int i = 0; i < n; i++) if (s[i] != INVALID_SOCKET) FD_SET(s[i], &w);
        struct timeval tv = { 0, 200000 };
        if (select(0, NULL, &w, NULL, &tv) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (s[i] == INVALID_SOCKET || !FD_ISSET(s[i], &w)) continue;
            struct sockaddr_in a; int al = sizeof(a);
            if (nf < max && getpeername(s[i], (struct sockaddr *)&a, &al) == 0) {
                char cand[32]; inet_ntop(AF_INET, &a.sin_addr, cand, sizeof(cand));
                if (accept(cand, ctx)) strcpy_s(found[nf++], 32, cand);
            }
            closesocket(s[i]); s[i] = INVALID_SOCKET; left--;
        }
    }
    for (int i = 0; i < n; i++) if (s[i] != INVALID_SOCKET) closesocket(s[i]);
    return nf;
}

// the controller that knows this token (ctx: the token)
static int accept_token(const char *ip, void *ctx) {
    char b[2048];
    char path[128]; snprintf(path, sizeof(path), "/api/v1/%s/state/on", (const char *)ctx);
    return http(ip, "GET", path, NULL, b, sizeof(b)) == 200;
}

// any Nanoleaf controller: something answering HTTP on port 16021 (panels say 401 / 403 without a token, other
// devices such as the MAGRGB strip may answer differently; only a real controller hands out a token later)
static int accept_any(const char *ip, void *ctx) {
    (void)ctx;
    char b[2048];
    int st = http(ip, "GET", "/api/v1/", NULL, b, sizeof(b));
    if (st) logf_("nanoleaf: %s answers on port 16021 (%d)", ip, st);
    return st != 0;
}

typedef struct { char (*cand)[32]; int *n, max; } mdns_ctx;
static void mdns_hit(const char *ip, void *p) {
    mdns_ctx *m = p;
    for (int i = 0; i < *m->n; i++) if (!strcmp(m->cand[i], ip)) return;
    if (*m->n < m->max) { strcpy_s(m->cand[(*m->n)++], 32, ip); logf_("nanoleaf: %s announces itself (mDNS)", ip); }
}

// the controller moved to another address (DHCP): look for the one that knows our token
static int rescan(ctl_t *c) {
    struct in_addr base; inet_pton(AF_INET, c->cur.ip, &base);
    char found[1][32];
    if (!scan24(ntohl(base.s_addr), accept_token, c->cur.token, found, 1)) return 0;
    AcquireSRWLockExclusive(&lk); strcpy_s(c->cur.ip, sizeof(c->cur.ip), found[0]); ReleaseSRWLockExclusive(&lk);
    save_keys();
    logf_("nanoleaf: slot %d found at %s", c->slot, found[0]);
    return 1;
}

// ---------------------------------------------------------------- one controller's thread
static unsigned __stdcall thread_fn(void *arg) {
    ctl_t *c = arg;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    unsigned char last[2 + MAX_PANELS * 8]; int last_n = -1;
    DWORD retry_at = 0, last_send = 0, last_check = 0;
    int fails = 0, streaming = 0, off_done = 0, live = 0, write_due = 0;

    while (c->run) {
        DWORD now = GetTickCount();
        // PC going to sleep: same as shutting down; streaming restarts after resume
        if (suspend_req) {
            if (streaming) { exit_action(c); streaming = c->online = 0; last_n = -1; logf_("nanoleaf %s: sleep", c->cur.ip); }
            SetEvent(c->suspend_done);
            Sleep(50);
            continue;
        }
        rgbf tg[MAX_PANELS]; int nt, en, have;
        AcquireSRWLockShared(&lk); memcpy(tg, c->target, sizeof(tg)); nt = c->ntarget; en = c->enabled; have = c->have_target; ReleaseSRWLockShared(&lk);

        // switched off in the app: the panels go dark (once), like the other devices
        if (!en) {
            if (!off_done && now >= retry_at) {
                if (power(c, 0)) { off_done = 1; logf_("nanoleaf %s: switched off", c->cur.ip); }
                else retry_at = now + 5000;
            }
            streaming = c->online = 0; last_n = -1;
            Sleep(100);
            continue;
        }
        if (off_done) { off_done = 0; retry_at = 0; }

        if (!streaming) {
            if (now < retry_at) { Sleep(100); continue; }
            if (connect_panels(c)) {
                streaming = c->online = 1; fails = 0; last_n = -1; last_check = now;
                live = !on_device(c); write_due = 0;
                logf_("nanoleaf %s: connected, %s (%s)", c->cur.ip, c->name, live ? "streaming" : "effects on the panels");
            }
            else {
                c->online = 0;
                if (++fails % 3 == 0 && rescan(c)) continue;
                retry_at = now + 10000;
            }
            continue;
        }

        if (InterlockedExchange(&c->want_relayout, 0) && api(c, "GET", "/", NULL) == 200) { parse_layout(c, c->http_buf); InterlockedExchange(&c->bake_wanted, 1); }

        // a new baked loop (or none: the effect is live, or streaming was chosen)
        if (InterlockedExchange(&c->anim_new, 0)) {
            int nf; AcquireSRWLockShared(&lk); nf = c->anim_frames; ReleaseSRWLockShared(&lk);
            if (nf > 0 && on_device(c)) { live = 0; write_due = 1; }
            else if (!live) { live = 1; start_stream(c); last_n = -1; logf_("nanoleaf %s: streaming", c->cur.ip); }
        }
        if (write_due && now >= retry_at) {
            int r = write_anim(c);
            if (r > 0) write_due = 0;
            else if (r < 0) { InterlockedExchange(&c->no_custom, 1); write_due = 0; live = 1; start_stream(c); last_n = -1; }
            else retry_at = now + 5000;
        }

        // someone switched a scene in the Nanoleaf app / on the controller, or the panels vanished
        if (now - last_check > 15000) {
            last_check = now;
            int st = api(c, "GET", "/state/on", NULL);
            if (st != 200) { streaming = c->online = 0; retry_at = now + 3000; logf_("nanoleaf %s: lost connection", c->cur.ip); continue; }
            if (jnum(c->http_buf, "value", 1)) {
                api(c, "GET", "/effects/select", NULL);
                if (live && !strstr(c->http_buf, "*ExtControl*")) { start_stream(c); last_n = -1; }
                // our loop was replaced (a scene picked in the Nanoleaf app): play it again
                if (!live && !write_due) {
                    if (!c->anim_sel[0]) snprintf(c->anim_sel, sizeof(c->anim_sel), "%s", c->http_buf);
                    else if (strcmp(c->http_buf, c->anim_sel)) {
                        logf_("nanoleaf %s: scene changed to %.60s, playing our effect again", c->cur.ip, c->http_buf);
                        write_due = 1;
                    }
                }
            }
        }

        if (!live) { Sleep(50); continue; }   // the panels play the loop themselves

        char sec[16]; sec_of(c->slot, sec, sizeof(sec));
        float rate = cfg_getf(sec, "rate", cfg_getf("nanoleaf", "rate", 10));
        if (rate < 1) rate = 1; if (rate > 30) rate = 30;
        if (!have || now - last_send < (DWORD)(1000 / rate)) { Sleep(15); continue; }

        // extControl v2: nPanels u16, then per panel: id u16, R, G, B, W, transition u16 (x100 ms)
        unsigned char pk[2 + MAX_PANELS * 8]; int k = 2, np = 0;
        AcquireSRWLockShared(&lk);
        for (int i = 0; i < c->npanels && i < nt; i++, np++) {
            rgbf col = tg[i];
            pk[k++] = (unsigned char)(c->panels[i].id >> 8); pk[k++] = (unsigned char)c->panels[i].id;
            pk[k++] = (unsigned char)(clampf(col.r, 0, 1) * 255 + .5f);
            pk[k++] = (unsigned char)(clampf(col.g, 0, 1) * 255 + .5f);
            pk[k++] = (unsigned char)(clampf(col.b, 0, 1) * 255 + .5f);
            pk[k++] = 0;
            pk[k++] = 0; pk[k++] = 1;   // 100 ms fade = one frame at 10 Hz, keeps motion smooth
        }
        ReleaseSRWLockShared(&lk);
        pk[0] = (unsigned char)(np >> 8); pk[1] = (unsigned char)np;
        if (np && (k != last_n || memcmp(pk, last, k))) {
            struct sockaddr_in a = { AF_INET, htons(60222) };
            inet_pton(AF_INET, c->cur.ip, &a.sin_addr);
            sendto(u, (char *)pk, k, 0, (struct sockaddr *)&a, sizeof(a));
            memcpy(last, pk, k); last_n = k;
        }
        last_send = now;
    }
    if (streaming) exit_action(c);
    closesocket(u);
    return 0;
}

static void net_ready(void) { if (!wsa_ok) { WSADATA w; wsa_ok = WSAStartup(MAKEWORD(2, 2), &w) == 0; } }

// A controller in its slot, with its thread running.
static void add_ctl(const addr_t *a) {
    int k = a->slot - 1;
    if (k < 0 || k >= NANO_MAX || ctl[k]) return;
    ctl_t *c = calloc(1, sizeof(ctl_t));
    if (!c) return;
    c->slot = a->slot; c->cur = *a; c->enabled = 1; c->run = 1;
    c->suspend_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    net_ready();
    AcquireSRWLockExclusive(&lk); ctl[k] = c; ReleaseSRWLockExclusive(&lk);
    c->th = (HANDLE)_beginthreadex(NULL, 0, thread_fn, c, 0, NULL);
    InterlockedExchange(&layout_new, 1);
    logf_("nanoleaf: controller %s in slot %d", a->ip, a->slot);
}

static void drop_ctl(int k, int restore) {
    ctl_t *c = ctl[k];
    if (!c) return;
    c->run = 0;
    if (c->th) { WaitForSingleObject(c->th, 5000); CloseHandle(c->th); }
    if (restore) restore_state(c);
    AcquireSRWLockExclusive(&lk); ctl[k] = NULL; ReleaseSRWLockExclusive(&lk);
    if (c->suspend_done) CloseHandle(c->suspend_done);
    free(c);
    InterlockedExchange(&layout_new, 1);
}

// 1: PC is about to sleep — apply the exit action and wait (briefly) until they went out; 0: resumed.
void nano_suspend(int s) {
    HANDLE ev[NANO_MAX]; int n = 0;
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < NANO_MAX; i++) if (ctl[i]) { ResetEvent(ctl[i]->suspend_done); ev[n++] = ctl[i]->suspend_done; }
    ReleaseSRWLockShared(&lk);
    InterlockedExchange(&suspend_req, s);
    if (s && n) WaitForMultipleObjects(n, ev, TRUE, 3000);
}

// ---------------------------------------------------------------- pairing
static char pair_ip[32];   // pairing with this address only ("Add by address"), else search

static unsigned __stdcall pair_fn(void *p) {
    (void)p;
    char b[4096];
    InterlockedExchange(&pair_state, PAIR_SEARCH);
    int free_slot = 0;
    AcquireSRWLockShared(&lk);
    for (int i = NANO_MAX - 1; i >= 0; i--) if (!ctl[i]) free_slot = i + 1;
    ReleaseSRWLockShared(&lk);

    // every controller on the local /24 networks, except the ones already paired and answering to their token
    // (a paired controller that lost its token may be paired again: it keeps its slot)
    static char cand[32][32]; int nc = 0;
    // controllers that announce themselves (panels: _nanoleafapi; Essentials-class devices such as MAGRGB also _ltpdu),
    // then every host of the local /24 networks
    mdns_ctx mc = { cand, &nc, 32 };
    if (pair_ip[0]) { mdns_hit(pair_ip, &mc); logf_("nanoleaf: pairing with %s (added by address)", pair_ip); goto searched; }
    mdns_browse("_nanoleafapi._tcp.local", 1500, mdns_hit, &mc);
    mdns_browse("_ltpdu._tcp.local", 1500, mdns_hit, &mc);
    ULONG bc[8]; int nb = net_broadcasts(bc, 8);
    for (int i = 0; i < nb && nc < 32; i++) {
        char more[32][32]; int k = scan24(ntohl(bc[i]), accept_any, NULL, more, 32);
        for (int j = 0; j < k; j++) mdns_hit(more[j], &mc);
    }
searched:;
    int again[32] = { 0 };
    for (int i = 0; i < nc; i++) {
        addr_t a = { 0 }; int k = -1;
        AcquireSRWLockShared(&lk);
        for (int j = 0; j < NANO_MAX; j++) if (ctl[j] && !strcmp(ctl[j]->cur.ip, cand[i])) { a = ctl[j]->cur; k = j; }
        ReleaseSRWLockShared(&lk);
        if (k < 0) continue;
        if (accept_token(cand[i], a.token)) { memmove(cand[i], cand[i + 1], sizeof(cand[0]) * (nc - i - 1)); memmove(again + i, again + i + 1, sizeof(int) * (nc - i - 1)); nc--; i--; }
        else again[i] = k + 1;
    }
    if (!nc) { InterlockedExchange(&pair_state, PAIR_NOTFOUND); logf_("nanoleaf: pairing — no new controller found"); return 0; }
    if (!free_slot) {
        int any_again = 0; for (int i = 0; i < nc; i++) any_again |= again[i] != 0;
        if (!any_again) { InterlockedExchange(&pair_state, PAIR_FULL); logf_("nanoleaf: pairing — all %d slots in use", NANO_MAX); return 0; }
    }

    logf_("nanoleaf: pairing, %d controller(s) found, waiting for a power button / Connect to API", nc);
    InterlockedExchange(&pair_state, PAIR_PRESS);
    DWORD end = GetTickCount() + 90000;
    int last_st[32]; for (int i = 0; i < 32; i++) last_st[i] = -1;
    while (GetTickCount() < end) {
        for (int i = 0; i < nc; i++) {
            int st = http(cand[i], "POST", "/api/v1/new", NULL, b, sizeof(b));
            if (st != last_st[i]) { last_st[i] = st; logf_("nanoleaf: pairing %s answers %d", cand[i], st); }   // 403: window not open
            if (st != 200) continue;
            addr_t a = { 0 };
            strcpy_s(a.ip, sizeof(a.ip), cand[i]);
            jstr(b, "auth_token", a.token, sizeof(a.token));
            if (!a.token[0]) continue;
            char path[128]; snprintf(path, sizeof(path), "/api/v1/%s/", a.token);
            if (http(a.ip, "GET", path, NULL, b, sizeof(b)) == 200) jstr(b, "serialNo", a.serial, sizeof(a.serial));
            // the same controller as a paired one (same serial, or the address of one whose token stopped working)
            int k = again[i] - 1;
            AcquireSRWLockShared(&lk);
            for (int j = 0; j < NANO_MAX && k < 0; j++) if (ctl[j] && a.serial[0] && !strcmp(ctl[j]->cur.serial, a.serial)) k = j;
            ReleaseSRWLockShared(&lk);
            if (k >= 0) { a.slot = k + 1; drop_ctl(k, 0); }
            else if (free_slot) a.slot = free_slot;
            else { InterlockedExchange(&pair_state, PAIR_FULL); return 0; }
            add_ctl(&a);
            save_keys();
            InterlockedExchange(&pair_state, PAIR_OK);
            logf_("nanoleaf: paired with %s (slot %d)", a.ip, a.slot);
            return 0;
        }
        Sleep(1500);
    }
    InterlockedExchange(&pair_state, PAIR_TIMEOUT);
    return 0;
}

// ---------------------------------------------------------------- API
int nano_start(void) {
    addr_t a[NANO_MAX];
    int n = load_keys(a, NANO_MAX);
    for (int i = 0; i < n; i++) add_ctl(&a[i]);
    return n > 0;
}

// ip: pair with the controller at this address only (a strip on another subnet, or one the search misses);
// NULL / "": search the local networks
void nano_pair_start_ip(const char *ip) {
    if (pair_th) {
        if (WaitForSingleObject(pair_th, 0) != WAIT_OBJECT_0) return;   // still running
        CloseHandle(pair_th);
        pair_th = NULL;
    }
    struct in_addr a;
    snprintf(pair_ip, sizeof(pair_ip), "%s", ip && inet_pton(AF_INET, ip, &a) == 1 ? ip : "");
    net_ready();
    pair_th = (HANDLE)_beginthreadex(NULL, 0, pair_fn, NULL, 0, NULL);
}

void nano_pair_start(void) { nano_pair_start_ip(NULL); }

// Unpair a controller: its panels get their own scene back, the slot is freed.
void nano_forget(int slot) {
    if (slot < 1 || slot > NANO_MAX || !ctl[slot - 1]) return;
    logf_("nanoleaf: forgetting slot %d", slot);
    drop_ctl(slot - 1, 1);
    save_keys();
}

void nano_relayout(void) {
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < NANO_MAX; i++) if (ctl[i]) InterlockedExchange(&ctl[i]->want_relayout, 1);
    ReleaseSRWLockShared(&lk);
}

// the render / UI side: controllers can be forgotten meanwhile, so everything is read under the lock
static int getv(int k, int what) {
    AcquireSRWLockShared(&lk);
    const ctl_t *c = k >= 0 && k < NANO_MAX ? ctl[k] : NULL;
    int v = !c ? 0 : what == 0 ? 1 : what == 1 ? c->npanels : on_device(c);
    ReleaseSRWLockShared(&lk);
    return v;
}
int nano_configured(void) { for (int i = 0; i < NANO_MAX; i++) if (getv(i, 0)) return 1; return 0; }
int nano_present(int k) { return getv(k, 0); }
int nano_count(int k) { return getv(k, 1); }
int nano_on_device(int k) { return getv(k, 2); }
int nano_layout_changed(void) { return InterlockedExchange(&layout_new, 0); }
int nano_bake_wanted(int k) {
    AcquireSRWLockShared(&lk);
    int v = k >= 0 && k < NANO_MAX && ctl[k] && InterlockedExchange(&ctl[k]->bake_wanted, 0) != 0;
    ReleaseSRWLockShared(&lk);
    return v;
}
void nano_title(int k, char *out, int cap) {
    AcquireSRWLockShared(&lk);
    snprintf(out, cap, "%s", k >= 0 && k < NANO_MAX && ctl[k] && ctl[k]->name[0] ? ctl[k]->name : "Nanoleaf");
    ReleaseSRWLockShared(&lk);
}

void nano_panel(int k, int i, float *x, float *y, float *path) {
    AcquireSRWLockShared(&lk);
    if (k >= 0 && k < NANO_MAX && ctl[k] && i >= 0 && i < ctl[k]->npanels) { *x = ctl[k]->panels[i].x; *y = ctl[k]->panels[i].y; *path = ctl[k]->panels[i].path; }
    ReleaseSRWLockShared(&lk);
}

void nano_upload(int k, const rgbf *frames, int nf, int np, float step) {
    if (nf > NANO_MAX_FRAMES) nf = NANO_MAX_FRAMES;
    if (np > NANO_MAX_PANELS) np = NANO_MAX_PANELS;
    AcquireSRWLockExclusive(&lk);
    ctl_t *c = k >= 0 && k < NANO_MAX ? ctl[k] : NULL;
    if (c) {
        if (nf > 0) memcpy(c->anim, frames, sizeof(rgbf) * NANO_MAX_PANELS * nf);
        c->anim_frames = nf; c->anim_panels = np; c->anim_step = step;
        InterlockedExchange(&c->anim_new, 1);
    }
    ReleaseSRWLockExclusive(&lk);
}

void nano_submit(int k, const rgbf *col, int n, int enable) {
    AcquireSRWLockExclusive(&lk);
    ctl_t *c = k >= 0 && k < NANO_MAX ? ctl[k] : NULL;
    if (c) {
        if (n > MAX_PANELS) n = MAX_PANELS;
        memcpy(c->target, col, n * sizeof(rgbf));
        c->ntarget = n; c->have_target = 1; c->enabled = enable;
    }
    ReleaseSRWLockExclusive(&lk);
}

// {"pair":0,"ctls":[{"slot":1,"online":1,"ip":"..","name":"..","model":"NL81","side":0.3,"unit":0.002,"stream":0,
//   "panels":[[x,y,shape,angle],...]},...]}
int nano_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"pair\":%ld,\"max\":%d,\"ctls\":[", pair_state, NANO_MAX), first = 1;
    for (int k = 0; k < NANO_MAX && n < cap - 256; k++) {
        const ctl_t *c = ctl[k];
        if (!c) continue;
        char nm[140]; json_escape_to(nm, sizeof(nm), c->name);
        n += snprintf(out + n, cap - n, "%s{\"slot\":%d,\"online\":%d,\"ip\":\"%s\",\"name\":\"%s\",\"model\":\"%s\",\"side\":%.4f,\"unit\":%.6f,\"stream\":%d,\"panels\":[",
                      first ? "" : ",", c->slot, c->online, c->cur.ip, nm, c->model, c->side_ratio, c->unit_ratio, (int)c->no_custom);
        first = 0;
        for (int i = 0; i < c->npanels && n < cap - 64; i++)
            n += snprintf(out + n, cap - n, "%s[%.4f,%.4f,%d,%.1f]", i ? "," : "", c->panels[i].x, c->panels[i].y, c->panels[i].shape, c->panels[i].o);
        n += snprintf(out + n, cap - n, "]}");
    }
    ReleaseSRWLockShared(&lk);
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

void nano_stop(void) {
    if (pair_th) { WaitForSingleObject(pair_th, 100); CloseHandle(pair_th); pair_th = NULL; }
    for (int k = 0; k < NANO_MAX; k++) if (ctl[k]) ctl[k]->run = 0;   // all exit actions at once
    for (int k = 0; k < NANO_MAX; k++) drop_ctl(k, 0);
}
