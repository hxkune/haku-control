// SPDX-License-Identifier: GPL-3.0-only
// Nanoleaf (Blocks / Shapes / Canvas...) via the local OpenAPI:
//   HTTP :16021 for layout, state and effects,
//   usually: the effect is precomputed (effects_bake) and written once as a looping custom animation that the
//   controller plays itself, so nothing flows over Wi-Fi until the effect, colours, speed or brightness change;
//   live effects (temperature, audio) or [nanoleaf] mode=stream: UDP :60222 extControl v2, at most `rate` frames/s.
// IP and auth token live in %APPDATA%\haku-control\nanoleaf.json. Pairing (holding the power button)
// can be started from the settings window: nano_pair_start().
// The panels' own state (scene / colour / brightness) is captured on connect and restored on exit or when disabled.
#define FD_SETSIZE 256   // subnet scan waits on 254 sockets at once
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdlib.h>
#include <process.h>

#define MAX_PANELS NANO_MAX_PANELS

typedef struct { int id, shape; float x, y, path, o; } panel_t;   // o: drawing angle on screen, degrees clockwise
typedef struct { char ip[32], token[64]; } addr_t;

static addr_t  cur;                    // used by the streaming thread only
static addr_t  pending;                // new address from pairing
static volatile LONG have_pending, want_relayout;
static char    dev_name[64];
static panel_t panels[MAX_PANELS];
static int     npanels, online;
static float   side_ratio, unit_ratio;   // layout sideLength and one layout unit, in 0..1 map units
static volatile LONG layout_new;
static SRWLOCK lk = SRWLOCK_INIT;
static rgbf    target[MAX_PANELS];
static int     ntarget, enabled = 1, have_target;
// the baked animation (render thread -> streaming thread)
static rgbf    anim[NANO_MAX_FRAMES * NANO_MAX_PANELS];
static int     anim_frames, anim_panels;
static float   anim_step;
static volatile LONG anim_new, bake_wanted = 1;
static volatile LONG run_thread;
static HANDLE  th, pair_th;
static volatile LONG pair_state;       // PAIR_*
static volatile LONG suspend_req;      // PC is going to sleep
static HANDLE  suspend_done;
static int     wsa_ok;

enum { PAIR_IDLE, PAIR_SEARCH, PAIR_PRESS, PAIR_OK, PAIR_NOTFOUND, PAIR_TIMEOUT };

// saved panel state
static int  saved, s_on, s_bri, s_hue, s_sat, s_ct;
static char s_mode[16], s_effect[96];

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

// Authenticated call to the current controller (streaming thread).
static char http_buf[64 * 1024];
static int api(const char *method, const char *sub, const char *body) {
    char path[160]; snprintf(path, sizeof(path), "/api/v1/%s%s", cur.token, sub);
    return http(cur.ip, method, path, body, http_buf, sizeof(http_buf));
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

static void key_path(wchar_t *p) { app_data_path(L"nanoleaf.json", p); }

static int load_key(addr_t *a) {
    wchar_t p[MAX_PATH]; key_path(p);
    FILE *f = _wfopen(p, L"rb");
    if (!f) { if (errno != ENOENT) logf_("nanoleaf: cannot read %ls (errno %d)", p, errno); return 0; }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    jstr(buf, "ip", a->ip, sizeof(a->ip));
    jstr(buf, "token", a->token, sizeof(a->token));
    if (!a->ip[0] || !a->token[0]) { logf_("nanoleaf: key file has no ip/token"); return 0; }
    return 1;
}

static void save_key(const addr_t *a) {
    wchar_t p[MAX_PATH]; key_path(p);
    wchar_t d[MAX_PATH]; wcscpy_s(d, MAX_PATH, p);
    wchar_t *s = wcsrchr(d, L'\\'); if (s) { *s = 0; CreateDirectoryW(d, NULL); }
    FILE *f = _wfopen(p, L"wb");
    if (!f) { logf_("nanoleaf: cannot write key file"); return; }
    fprintf(f, "{\r\n  \"ip\": \"%s\",\r\n  \"token\": \"%s\"\r\n}\r\n", a->ip, a->token);
    fclose(f);
}

// ---------------------------------------------------------------- layout
// Panel centres, rotated by the controller's globalOrientation plus the user's rotation, normalised to 0..1 (y = 0 at top).
static void parse_layout(const char *js) {
    float rot = (float)jvalue(js, "globalOrientation", 0) + cfg_getf("nanoleaf", "rotate", 0);
    int flip = cfg_geti("nanoleaf", "flip", 0);
    float side = (float)jnum(strstr(js, "\"layout\":"), "sideLength", 0);
    float cs = cosf(rot * 3.14159265f / 180), sn = sinf(rot * 3.14159265f / 180);
    panel_t p[MAX_PANELS]; int n = 0;
    const char *arr = strstr(js, "\"positionData\"");
    for (const char *o = arr ? strchr(arr, '{') : NULL; o && n < MAX_PANELS; o = strchr(o + 1, '{')) {
        const char *oe = strchr(o, '}'); if (!oe) break;
        char one[256]; int l = (int)(oe - o); if (l > 255) l = 255;
        memcpy(one, o, l); one[l] = 0;
        int id = (int)jnum(one, "panelId", 0), shape = (int)jnum(one, "shapeType", 0);
        // panel 0, controllers, connectors and caps carry no light (Rhythm 1, Shapes controller 12, Lines connector 16,
        // controller cap 19, power connector 20, Blocks controller 35)
        if (id > 0 && shape != 1 && shape != 12 && shape != 16 && shape != 19 && shape != 20 && shape != 35) {
            float x = (float)jnum(one, "x", 0), y = (float)jnum(one, "y", 0);
            // the panel's own orientation goes through the same rotation / mirror as its position; the page draws
            // with y down, where angles turn the other way
            float o = (float)jnum(one, "o", 0) - rot;
            if (flip) o = 180 - o;
            o = fmodf(-o + 720, 360);
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
    if (!n) return;
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        x0 = min(x0, p[i].x); x1 = max(x1, p[i].x); y0 = min(y0, p[i].y); y1 = max(y1, p[i].y);
    }
    float w = x1 - x0, h = y1 - y0, span = max(w, h);
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
    memcpy(panels, p, sizeof(p)); npanels = n;
    side_ratio = side > 0 ? side / span : 0.2f;
    unit_ratio = 1 / span;
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&layout_new, 1);
    logf_("nanoleaf: %d panels, orientation %.0f", n, rot);
}

// ---------------------------------------------------------------- state save / restore
static void save_state(const char *js) {
    const char *st = strstr(js, "\"state\":");
    const char *fx = strstr(js, "\"effects\":");
    if (!st) return;
    s_on  = (int)jvalue(st, "on", 1);
    s_bri = (int)jvalue(st, "brightness", 100);
    s_hue = (int)jvalue(st, "hue", 0);
    s_sat = (int)jvalue(st, "sat", 0);
    s_ct  = (int)jvalue(st, "ct", 4000);
    jstr(st, "colorMode", s_mode, sizeof(s_mode));
    jstr(fx, "select", s_effect, sizeof(s_effect));
    // left in extControl by a previous run that did not exit cleanly: fall back to white
    if (!strcmp(s_effect, "*ExtControl*")) { strcpy_s(s_effect, sizeof(s_effect), "*Solid*"); strcpy_s(s_mode, sizeof(s_mode), "hs"); s_hue = s_sat = 0; }
    saved = 1;
    logf_("nanoleaf: saved state on=%d bri=%d mode=%s effect=%s", s_on, s_bri, s_mode, s_effect);
}

static void restore_state(void) {
    if (!saved) return;
    char b[160];
    if (s_effect[0] && s_effect[0] != '*') {
        snprintf(b, sizeof(b), "{\"select\":\"%s\"}", s_effect);
        api("PUT", "/effects", b);
    } else if (!strcmp(s_mode, "ct")) {
        snprintf(b, sizeof(b), "{\"ct\":{\"value\":%d}}", s_ct);
        api("PUT", "/state", b);
    } else {
        snprintf(b, sizeof(b), "{\"hue\":{\"value\":%d},\"sat\":{\"value\":%d}}", s_hue, s_sat);
        api("PUT", "/state", b);
    }
    snprintf(b, sizeof(b), "{\"brightness\":{\"value\":%d},\"on\":{\"value\":%s}}", s_bri, s_on ? "true" : "false");
    api("PUT", "/state", b);
    logf_("nanoleaf: restored own state");
}

static int power(int on) {
    return api("PUT", "/state", on ? "{\"on\":{\"value\":true}}" : "{\"on\":{\"value\":false}}") / 100 == 2;
}

static int start_stream(void) {
    int ok = api("PUT", "/effects", "{\"write\":{\"command\":\"display\",\"animType\":\"extControl\",\"extControlVersion\":\"v2\"}}") / 100 == 2;
    if (ok) power(1);   // they may have been switched off by us (device disabled, PC shut down)
    return ok;
}

// What the panels do when the app exits (PC shutdown): [general] on_exit = off | keep | restore
static void exit_action(void) {
    const char *m = cfg_get("general", "on_exit", "off");
    if (!_stricmp(m, "restore")) restore_state();
    else if (_stricmp(m, "keep")) power(0);
}

// Connect: read layout + state, switch to extControl.
static int connect_panels(void) {
    int st = api("GET", "/", NULL);
    if (st != 200) {
        if (st == 401 || st == 403) logf_("nanoleaf: token rejected (%d) — pair again", st);
        return 0;
    }
    char nm[64]; jstr(http_buf, "name", nm, sizeof(nm));
    AcquireSRWLockExclusive(&lk); strcpy_s(dev_name, sizeof(dev_name), nm); ReleaseSRWLockExclusive(&lk);
    parse_layout(http_buf);
    if (!saved) save_state(http_buf);
    if (!npanels) return 0;
    if (!nano_on_device()) return start_stream();
    power(1);
    InterlockedExchange(&bake_wanted, 1);   // the render thread bakes the current effect, then write_anim()
    return 1;
}

// ---------------------------------------------------------------- the effect on the panels themselves
int nano_on_device(void) { return _stricmp(cfg_get("nanoleaf", "mode", "device"), "stream") != 0; }
int nano_bake_wanted(void) { return InterlockedExchange(&bake_wanted, 0) != 0; }

void nano_upload(const rgbf *frames, int nf, int np, float step) {
    if (nf > NANO_MAX_FRAMES) nf = NANO_MAX_FRAMES;
    if (np > NANO_MAX_PANELS) np = NANO_MAX_PANELS;
    AcquireSRWLockExclusive(&lk);
    if (nf > 0) memcpy(anim, frames, sizeof(rgbf) * NANO_MAX_PANELS * nf);
    anim_frames = nf; anim_panels = np; anim_step = step;
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&anim_new, 1);
}

// The baked loop as a custom animation the controller keeps playing:
// animData = "numPanels  panelId numFrames  R G B W T  R G B W T ...", T = fade time into the frame, in 0.1 s.
static char anim_body[64 * 1024], anim_sel[96];
static int write_anim(void) {
    AcquireSRWLockShared(&lk);
    int nf = anim_frames, np = anim_panels < npanels ? anim_panels : npanels, T = (int)(anim_step * 10 + 0.5f), cap = sizeof(anim_body) - 64;
    if (T < 1) T = 1;
    int n = snprintf(anim_body, sizeof(anim_body), "{\"write\":{\"command\":\"display\",\"animType\":\"custom\",\"loop\":true,"
                     "\"palette\":[],\"animData\":\"%d", np);
    for (int p = 0; p < np && n < cap; p++) {
        n += snprintf(anim_body + n, sizeof(anim_body) - n, " %d %d", panels[p].id, nf);
        for (int f = 0; f < nf && n < cap; f++) {
            rgbf c = anim[f * NANO_MAX_PANELS + p];
            n += snprintf(anim_body + n, sizeof(anim_body) - n, " %d %d %d 0 %d", (int)(clampf(c.r, 0, 1) * 255 + .5f),
                          (int)(clampf(c.g, 0, 1) * 255 + .5f), (int)(clampf(c.b, 0, 1) * 255 + .5f), nf == 1 ? 5 : T);
        }
    }
    ReleaseSRWLockShared(&lk);
    if (n >= cap) { logf_("nanoleaf: animation too large"); return 0; }
    snprintf(anim_body + n, sizeof(anim_body) - n, "\"}}");
    int st = api("PUT", "/effects", anim_body);
    if (st / 100 != 2) { logf_("nanoleaf: animation refused (%d): %.160s", st, http_buf); return 0; }
    anim_sel[0] = 0;   // what the controller calls our loop is learnt at the next check
    logf_("nanoleaf: playing a %d-frame loop on the panels (%.1f s per frame)", nf, T / 10.0);
    return 1;
}

// ---------------------------------------------------------------- finding a controller on the local networks
// Tries every host of the /24 around `base` on port 16021; accept(ip) decides. Returns 1 and fills found.
static int scan24(ULONG base_host_order, int (*accept)(const char *ip, void *ctx), void *ctx, char *found) {
    unsigned net = base_host_order & 0xFFFFFF00u;
    SOCKET s[254]; int n = 0;
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
    found[0] = 0;
    DWORD end = GetTickCount() + 1500;
    int left = n;
    while (left && GetTickCount() < end && !found[0]) {
        fd_set w; FD_ZERO(&w);
        for (int i = 0; i < n; i++) if (s[i] != INVALID_SOCKET) FD_SET(s[i], &w);
        struct timeval tv = { 0, 200000 };
        if (select(0, NULL, &w, NULL, &tv) <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (s[i] == INVALID_SOCKET || !FD_ISSET(s[i], &w)) continue;
            struct sockaddr_in a; int al = sizeof(a);
            if (!found[0] && getpeername(s[i], (struct sockaddr *)&a, &al) == 0) {
                char cand[32]; inet_ntop(AF_INET, &a.sin_addr, cand, sizeof(cand));
                if (accept(cand, ctx)) strcpy_s(found, 32, cand);
            }
            closesocket(s[i]); s[i] = INVALID_SOCKET; left--;
        }
    }
    for (int i = 0; i < n; i++) if (s[i] != INVALID_SOCKET) closesocket(s[i]);
    return found[0] != 0;
}

// the controller that knows our token
static int accept_token(const char *ip, void *ctx) {
    (void)ctx;
    static char b[2048];
    char path[128]; snprintf(path, sizeof(path), "/api/v1/%s/state/on", cur.token);
    return http(ip, "GET", path, NULL, b, sizeof(b)) == 200;
}

// any Nanoleaf controller: unauthenticated API requests answer 401 / 403
static int accept_any(const char *ip, void *ctx) {
    (void)ctx;
    static char b[2048];
    int st = http(ip, "GET", "/api/v1/", NULL, b, sizeof(b));
    return st == 401 || st == 403;
}

static int rescan(void) {
    struct in_addr base; inet_pton(AF_INET, cur.ip, &base);
    char found[32];
    if (!scan24(ntohl(base.s_addr), accept_token, NULL, found)) return 0;
    strcpy_s(cur.ip, sizeof(cur.ip), found);
    save_key(&cur);
    logf_("nanoleaf: found at %s", found);
    return 1;
}

// ---------------------------------------------------------------- streaming thread
static unsigned __stdcall thread_fn(void *p) {
    (void)p;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    unsigned char last[2 + MAX_PANELS * 8]; int last_n = -1;
    DWORD retry_at = 0, last_send = 0, last_check = 0;
    int fails = 0, streaming = 0, off_done = 0, live = 0, write_due = 0;

    while (run_thread) {
        DWORD now = GetTickCount();
        // PC going to sleep: same as shutting down; streaming restarts after resume
        if (suspend_req) {
            if (streaming) { exit_action(); streaming = online = 0; last_n = -1; logf_("nanoleaf: sleep"); }
            SetEvent(suspend_done);
            Sleep(50);
            continue;
        }
        if (InterlockedExchange(&have_pending, 0)) {
            if (streaming) restore_state();
            AcquireSRWLockExclusive(&lk); cur = pending; ReleaseSRWLockExclusive(&lk);
            streaming = online = 0; saved = 0; retry_at = 0; fails = 0;
            logf_("nanoleaf: new controller %s", cur.ip);
        }
        rgbf tg[MAX_PANELS]; int nt, en, have;
        AcquireSRWLockShared(&lk); memcpy(tg, target, sizeof(tg)); nt = ntarget; en = enabled; have = have_target; ReleaseSRWLockShared(&lk);

        // switched off in the app: the panels go dark (once), like the other devices
        if (!en) {
            if (!off_done && cur.ip[0] && now >= retry_at) {
                if (power(0)) { off_done = 1; logf_("nanoleaf: switched off"); }
                else retry_at = now + 5000;
            }
            streaming = online = 0; last_n = -1;
            Sleep(100);
            continue;
        }
        if (off_done) { off_done = 0; retry_at = 0; }

        if (!streaming) {
            if (now < retry_at) { Sleep(100); continue; }
            if (connect_panels()) {
                streaming = online = 1; fails = 0; last_n = -1; last_check = now;
                live = !nano_on_device(); write_due = 0;
                logf_("nanoleaf: connected to %s (%s)", cur.ip, live ? "streaming" : "effects on the panels");
            }
            else {
                online = 0;
                if (++fails % 3 == 0 && rescan()) continue;
                retry_at = now + 10000;
            }
            continue;
        }

        if (InterlockedExchange(&want_relayout, 0) && api("GET", "/", NULL) == 200) { parse_layout(http_buf); InterlockedExchange(&bake_wanted, 1); }

        // a new baked loop (or none: the effect is live, or streaming was chosen)
        if (InterlockedExchange(&anim_new, 0)) {
            int nf; AcquireSRWLockShared(&lk); nf = anim_frames; ReleaseSRWLockShared(&lk);
            if (nf > 0 && nano_on_device()) { live = 0; write_due = 1; }
            else if (!live) { live = 1; start_stream(); last_n = -1; logf_("nanoleaf: streaming"); }
        }
        if (write_due && now >= retry_at) {
            if (write_anim()) write_due = 0; else retry_at = now + 5000;
        }

        // someone switched a scene in the Nanoleaf app / on the controller, or the panels vanished
        if (now - last_check > 15000) {
            last_check = now;
            int st = api("GET", "/state/on", NULL);
            if (st != 200) { streaming = online = 0; retry_at = now + 3000; logf_("nanoleaf: lost connection"); continue; }
            if (jnum(http_buf, "value", 1)) {
                api("GET", "/effects/select", NULL);
                if (live && !strstr(http_buf, "*ExtControl*")) { start_stream(); last_n = -1; }
                // our loop was replaced (a scene picked in the Nanoleaf app): play it again
                if (!live && !write_due) {
                    if (!anim_sel[0]) snprintf(anim_sel, sizeof(anim_sel), "%s", http_buf);
                    else if (strcmp(http_buf, anim_sel)) {
                        logf_("nanoleaf: scene changed to %.60s, playing our effect again", http_buf);
                        write_due = 1;
                    }
                }
            }
        }

        if (!live) { Sleep(50); continue; }   // the panels play the loop themselves

        float rate = cfg_getf("nanoleaf", "rate", 10);
        if (rate < 1) rate = 1; if (rate > 10) rate = 10;
        if (!have || now - last_send < (DWORD)(1000 / rate)) { Sleep(15); continue; }

        // extControl v2: nPanels u16, then per panel: id u16, R, G, B, W, transition u16 (x100 ms)
        unsigned char pk[2 + MAX_PANELS * 8]; int k = 2, np = 0;
        AcquireSRWLockShared(&lk);
        for (int i = 0; i < npanels && i < nt; i++, np++) {
            rgbf c = tg[i];
            pk[k++] = (unsigned char)(panels[i].id >> 8); pk[k++] = (unsigned char)panels[i].id;
            pk[k++] = (unsigned char)(clampf(c.r, 0, 1) * 255 + .5f);
            pk[k++] = (unsigned char)(clampf(c.g, 0, 1) * 255 + .5f);
            pk[k++] = (unsigned char)(clampf(c.b, 0, 1) * 255 + .5f);
            pk[k++] = 0;
            pk[k++] = 0; pk[k++] = 1;   // 100 ms fade = one frame at 10 Hz, keeps motion smooth
        }
        ReleaseSRWLockShared(&lk);
        pk[0] = (unsigned char)(np >> 8); pk[1] = (unsigned char)np;
        if (np && (k != last_n || memcmp(pk, last, k))) {
            struct sockaddr_in a = { AF_INET, htons(60222) };
            inet_pton(AF_INET, cur.ip, &a.sin_addr);
            sendto(u, (char *)pk, k, 0, (struct sockaddr *)&a, sizeof(a));
            memcpy(last, pk, k); last_n = k;
        }
        last_send = now;
    }
    if (streaming) exit_action();
    closesocket(u);
    return 0;
}

static void start_thread(void) {
    if (th) return;
    if (!wsa_ok) { WSADATA w; wsa_ok = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    run_thread = 1;
    if (!suspend_done) suspend_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, thread_fn, NULL, 0, NULL);
}

// 1: PC is about to sleep — apply the exit action and wait (briefly) until it went out; 0: resumed.
void nano_suspend(int s) {
    if (!th) return;
    ResetEvent(suspend_done);
    InterlockedExchange(&suspend_req, s);
    if (s) WaitForSingleObject(suspend_done, 3000);
}

// ---------------------------------------------------------------- pairing
typedef struct { ULONG addr[8]; int n; } subnets_t;

static unsigned __stdcall pair_fn(void *p) {
    (void)p;
    static char b[4096];
    char ip[32] = "";
    InterlockedExchange(&pair_state, PAIR_SEARCH);
    // the known controller first, then every local /24 network
    addr_t known; AcquireSRWLockShared(&lk); known = cur; ReleaseSRWLockShared(&lk);
    if (!known.ip[0]) load_key(&known);
    if (known.ip[0] && accept_any(known.ip, NULL)) strcpy_s(ip, sizeof(ip), known.ip);
    if (!ip[0]) {
        ULONG bc[8]; int nb = net_broadcasts(bc, 8);
        for (int i = 0; i < nb && !ip[0]; i++) scan24(ntohl(bc[i]), accept_any, NULL, ip);
    }
    if (!ip[0]) { InterlockedExchange(&pair_state, PAIR_NOTFOUND); logf_("nanoleaf: pairing — no controller found"); return 0; }

    logf_("nanoleaf: pairing with %s, waiting for the power button", ip);
    InterlockedExchange(&pair_state, PAIR_PRESS);
    DWORD end = GetTickCount() + 90000;
    while (GetTickCount() < end) {
        if (http(ip, "POST", "/api/v1/new", NULL, b, sizeof(b)) == 200) {
            addr_t a = { 0 };
            strcpy_s(a.ip, sizeof(a.ip), ip);
            jstr(b, "auth_token", a.token, sizeof(a.token));
            if (a.token[0]) {
                save_key(&a);
                AcquireSRWLockExclusive(&lk); pending = a; ReleaseSRWLockExclusive(&lk);
                InterlockedExchange(&have_pending, 1);
                start_thread();
                InterlockedExchange(&pair_state, PAIR_OK);
                logf_("nanoleaf: paired with %s", ip);
                return 0;
            }
        }
        Sleep(1500);
    }
    InterlockedExchange(&pair_state, PAIR_TIMEOUT);
    return 0;
}

// ---------------------------------------------------------------- API
int nano_start(void) {
    if (th) return 1;
    if (!load_key(&cur)) return 0;
    logf_("nanoleaf: controller %s", cur.ip);
    start_thread();
    return 1;
}

void nano_pair_start(void) {
    if (pair_th) {
        if (WaitForSingleObject(pair_th, 0) != WAIT_OBJECT_0) return;   // still running
        CloseHandle(pair_th);
    }
    if (!wsa_ok) { WSADATA w; wsa_ok = WSAStartup(MAKEWORD(2, 2), &w) == 0; }
    pair_th = (HANDLE)_beginthreadex(NULL, 0, pair_fn, NULL, 0, NULL);
}

void nano_relayout(void) { InterlockedExchange(&want_relayout, 1); }

int nano_configured(void) { return th != NULL; }
int nano_online(void) { return online; }
int nano_layout_changed(void) { return InterlockedExchange(&layout_new, 0); }
int nano_count(void) { return npanels; }

void nano_panel(int i, float *x, float *y, float *path) {
    AcquireSRWLockShared(&lk);
    if (i >= 0 && i < npanels) { *x = panels[i].x; *y = panels[i].y; *path = panels[i].path; }
    ReleaseSRWLockShared(&lk);
}

// {"configured":1,"online":1,"ip":"..","name":"..","side":0.3,"unit":0.002,"pair":0,"panels":[[x,y,shape,angle],...]}
int nano_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"configured\":%d,\"online\":%d,\"ip\":\"%s\",\"name\":\"%s\",\"side\":%.4f,\"unit\":%.6f,\"pair\":%ld,\"panels\":[",
                     th != NULL, online, cur.ip, dev_name, side_ratio, unit_ratio, pair_state);
    for (int i = 0; i < npanels && n < cap - 64; i++)
        n += snprintf(out + n, cap - n, "%s[%.4f,%.4f,%d,%.1f]", i ? "," : "", panels[i].x, panels[i].y, panels[i].shape, panels[i].o);
    ReleaseSRWLockShared(&lk);
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

void nano_submit(const rgbf *c, int n, int enable) {
    AcquireSRWLockExclusive(&lk);
    if (n > MAX_PANELS) n = MAX_PANELS;
    memcpy(target, c, n * sizeof(rgbf));
    ntarget = n;
    have_target = 1;
    enabled = enable;
    ReleaseSRWLockExclusive(&lk);
}

void nano_stop(void) {
    if (pair_th) { WaitForSingleObject(pair_th, 100); CloseHandle(pair_th); pair_th = NULL; }
    if (!th) return;
    run_thread = 0;
    WaitForSingleObject(th, 5000);
    CloseHandle(th); th = NULL;
}
