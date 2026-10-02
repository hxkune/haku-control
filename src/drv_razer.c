// SPDX-License-Identifier: GPL-3.0-only
// Razer devices through Razer Synapse, over the Chroma SDK's local REST API (assets.razerzone.com/dev_portal/REST):
// POST http://localhost:54235/razer/chromasdk with a description of the app opens a session and answers its own
// address ({"sessionid":..,"uri":"http://localhost:<port>/chromasdk"}); the session lives while PUT <uri>/heartbeat
// comes at least every 15 s, and DELETE <uri> ends it (Synapse shows its own lighting again).
// Frames: PUT <uri>/<device> {"effect":"CHROMA_CUSTOM","param":...}, colours as numbers 0xBBGGRR.
// The API does not say which Razer devices are plugged in, so while Synapse answers a scan offers each kind of
// device, and each one added is a device of its own (sub = its kind): keyboard (6 x 22 grid: its columns), mouse
// (9 x 7: its rows, front to back), mousepad (15 LEDs round it), headset (5), keypad (4 x 5: its columns), Chroma
// Link (5). They all share one session. Written after Razer's documentation, not yet tried on a Razer device.
#include "devices.h"
#include <stdlib.h>

#define RZ_HOST "127.0.0.1"
#define RZ_PORT 54235

static const struct { const char *path, *name, *what, *effect; int rows, cols, n; } CAT[] = {
    { "keyboard",   "Razer keyboard",    "keyboard",   "CHROMA_CUSTOM",  6, 22, 22 },   // n: the columns
    { "mouse",      "Razer mouse",       "mouse",      "CHROMA_CUSTOM2", 9, 7,  9 },    // n: the rows
    { "mousepad",   "Razer mousepad",    "mousepad",   "CHROMA_CUSTOM",  0, 15, 15 },
    { "headset",    "Razer headset",     "headset",    "CHROMA_CUSTOM",  0, 5,  5 },
    { "keypad",     "Razer keypad",      "keyboard",   "CHROMA_CUSTOM",  4, 5,  5 },
    { "chromalink", "Razer Chroma Link", "Chroma Link", "CHROMA_CUSTOM", 0, 5,  5 },
};
#define NCAT (int)(sizeof(CAT) / sizeof(CAT[0]))

// the one session, shared by every Razer device (opened on connecting threads, used by the device thread)
static SRWLOCK slk = SRWLOCK_INIT;
static struct { char host[64], path[64]; int port, gen, refs, dead; DWORD beat; } ses;

typedef struct { int cat, gen, fails; } rz_t;

static int cat_of(const ext_dev *d) { return d->sub >= 0 && d->sub < NCAT ? d->sub : 0; }

// Under slk. Opens a session if there is none that works; returns its number (0: Synapse does not answer).
static int session(void) {
    if (ses.port && !ses.dead) return ses.gen;
    const char *body = "{\"title\":\"haku control\",\"description\":\"Lighting for the whole room\","
                       "\"author\":{\"name\":\"haku\",\"contact\":\"github.com/hxkune/haku-control\"},"
                       "\"device_supported\":[\"keyboard\",\"mouse\",\"headset\",\"mousepad\",\"keypad\",\"chromalink\"],"
                       "\"category\":\"application\"}";
    char buf[512], why[160], uri[128] = "";
    int st = http_call(RZ_HOST, RZ_PORT, "POST", "/razer/chromasdk", body, buf, sizeof(buf), why, sizeof(why));
    if (st != 200 || !json_get_str(buf, "uri", uri, sizeof(uri))) {
        static DWORD said;   // every device retries: in the log once a minute
        if (!said || GetTickCount() - said > 60000) { logf_("razer: Synapse did not open a session: %s", st ? buf : why); said = GetTickCount(); }
        return 0;
    }
    // "http://localhost:55555/chromasdk" -> 127.0.0.1, 55555, /chromasdk
    const char *p = strstr(uri, "://"); p = p ? p + 3 : uri;
    const char *colon = strchr(p, ':'), *slash = strchr(p, '/');
    if (!slash) { logf_("razer: odd session address %s", uri); return 0; }
    int hl = (int)((colon && colon < slash ? colon : slash) - p);
    snprintf(ses.host, sizeof(ses.host), "%.*s", hl, p);
    if (!_stricmp(ses.host, "localhost")) strcpy_s(ses.host, sizeof(ses.host), RZ_HOST);
    ses.port = colon && colon < slash ? atoi(colon + 1) : 80;
    snprintf(ses.path, sizeof(ses.path), "%s", slash);
    ses.gen++; ses.refs = 0; ses.dead = 0; ses.beat = GetTickCount();
    logf_("razer: session %s:%d%s", ses.host, ses.port, ses.path);
    return ses.gen;
}

// Under slk: PUT <session>/<what>. 0: Synapse is gone (the session is then dead).
static int put(const char *what, const char *body, char *why, int whycap) {
    char path[96], buf[256];
    snprintf(path, sizeof(path), "%s/%s", ses.path, what);
    int st = http_call(ses.host, ses.port, "PUT", path, body, buf, sizeof(buf), why, whycap);
    if (!st) ses.dead = 1;
    else if (st == 200 && json_get_num(buf, "result", 0) != 0 && why) snprintf(why, whycap, "result %s", buf);
    return st;
}

static int rz_open(ext_dev *d) {
    rz_t *r = calloc(1, sizeof(rz_t));
    if (!r) return 0;
    r->cat = cat_of(d);
    AcquireSRWLockExclusive(&slk);
    r->gen = session();
    if (r->gen) ses.refs++;
    ReleaseSRWLockExclusive(&slk);
    if (!r->gen) { free(r); snprintf(d->info, sizeof(d->info), "Razer Synapse is not running"); return 0; }
    d->priv = r; d->nleds = CAT[r->cat].n;
    snprintf(d->info, sizeof(d->info), "Razer Synapse · %s", CAT[r->cat].what);
    return 1;
}

static int rz_send(ext_dev *d, const rgbf *c, int n) {
    rz_t *r = d->priv;
    const int K = CAT[r->cat].n;
    int v[22];
    for (int i = 0; i < K; i++) {
        rgbf x = i < n ? c[i] : (rgbf){ 0 };
        v[i] = to8(x.b) << 16 | to8(x.g) << 8 | to8(x.r);
    }
    char body[2400], why[200] = "";
    int o = snprintf(body, sizeof(body), "{\"effect\":\"%s\",\"param\":[", CAT[r->cat].effect);
    int rows = CAT[r->cat].rows, cols = CAT[r->cat].cols;
    if (!rows) for (int i = 0; i < cols; i++) o += snprintf(body + o, sizeof(body) - o, "%s%d", i ? "," : "", v[i]);
    else for (int y = 0; y < rows; y++) {
        o += snprintf(body + o, sizeof(body) - o, "%s[", y ? "," : "");
        for (int x = 0; x < cols; x++)   // the mouse takes its rows from the strip, the others their columns
            o += snprintf(body + o, sizeof(body) - o, "%s%d", x ? "," : "", r->cat == 1 ? v[y] : v[x]);
        o += snprintf(body + o, sizeof(body) - o, "]");
    }
    snprintf(body + o, sizeof(body) - o, "]}");
    AcquireSRWLockExclusive(&slk);
    int ok = r->gen == ses.gen && !ses.dead;
    if (ok && GetTickCount() - ses.beat >= 1000) {   // the session's keep-alive, once a second for all devices
        char hw[160];
        if (put("heartbeat", "", hw, sizeof(hw))) ses.beat = GetTickCount();
        else logf_("razer: heartbeat failed: %s", hw);
    }
    int st = ok && !ses.dead ? put(CAT[r->cat].path, body, why, sizeof(why)) : 0;
    ReleaseSRWLockExclusive(&slk);
    if (!st) return 0;   // Synapse closed or the session was replaced: connect again
    if (st != 200 || why[0]) {   // refused: in the log (the first time, then every 50th)
        if (r->fails++ % 50 == 0) logf_("dev.%d (razer %s): %s refused: %s", d->id, CAT[r->cat].path, CAT[r->cat].effect, why);
    } else r->fails = 0;
    return 1;
}

static void rz_leave(ext_dev *d, int how) {
    // Synapse lights the device itself again once the session ends; LEAVE_OFF darkens it until then
    if (how == LEAVE_OFF) { rgbf z[22] = { 0 }; rz_send(d, z, CAT[((rz_t *)d->priv)->cat].n); }
}

static void rz_close(ext_dev *d) {
    rz_t *r = d->priv;
    if (!r) return;
    AcquireSRWLockExclusive(&slk);
    if (r->gen == ses.gen && --ses.refs <= 0) {   // the last one: end the session
        char buf[128];
        if (!ses.dead) http_call(ses.host, ses.port, "DELETE", ses.path, NULL, buf, sizeof(buf), NULL, 0);
        ses.port = 0;
        logf_("razer: session closed");
    }
    ReleaseSRWLockExclusive(&slk);
    free(r); d->priv = NULL;
}

// Synapse answers GET /razer/chromasdk with its SDK version: then each kind of device is offered
static void rz_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    char buf[512];
    if (http_call(RZ_HOST, RZ_PORT, "GET", "/razer/chromasdk", NULL, buf, sizeof(buf), NULL, 0) != 200 || !strstr(buf, "version")) return;
    char ver[32] = ""; json_get_str(buf, "version", ver, sizeof(ver));
    for (int i = 0; i < NCAT; i++) {
        disc_t x = { "razer", "synapse" };
        x.sub = i; x.nleds = CAT[i].n;
        snprintf(x.name, sizeof(x.name), "%s", CAT[i].name);
        snprintf(x.info, sizeof(x.info), "Razer Synapse · %s%s%s", CAT[i].what, ver[0] ? " · Chroma SDK " : "", ver);
        found(&x);
    }
}

const ext_driver drv_razer = { "razer", "Razer Chroma", 15, 1, rz_open, rz_send, rz_leave, rz_close, rz_discover };
