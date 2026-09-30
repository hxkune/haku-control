// SPDX-License-Identifier: GPL-3.0-only
// OpenRGB SDK client (https://openrgb.org, "SDK Server", TCP 6742). Every OpenRGB controller (a keyboard,
// a GPU, a fan hub...) becomes one haku device: host = the OpenRGB server, sub = controller index.
// The protocol is used at version 0 (the client never announces a newer one), so the controller data
// always comes in the simplest, stable layout.
// OpenRGB is also the way in for PC hardware haku has no driver of its own for (graphics cards, other boards, RAM,
// coolers of any brand): orgb_check_start() looks for the SDK server on this PC and the PC page lists what it has.
// OpenRGB numbers its controllers in the order it finds them, and that order can change (a device unplugged, a
// new one): [dev.N] match keeps the controller's name, and a device is looked up by it when the number no longer fits.
#include "devices.h"
#include <process.h>
#include <stdlib.h>

#define ORGB_PORT 6742
enum {
    PKT_REQUEST_CONTROLLER_COUNT = 0,
    PKT_REQUEST_CONTROLLER_DATA  = 1,
    PKT_SET_CLIENT_NAME          = 50,
    PKT_DEVICE_LIST_UPDATED      = 100,
    PKT_UPDATELEDS               = 1050,
    PKT_SETCUSTOMMODE            = 1100,
};

typedef struct { unsigned int orig[EXT_MAX_LEDS]; int norig; unsigned idx; } orgb_t;

static int send_pkt(SOCKET s, unsigned dev, unsigned id, const void *data, unsigned len) {
    unsigned char h[16] = { 'O', 'R', 'G', 'B' };
    memcpy(h + 4, &dev, 4); memcpy(h + 8, &id, 4); memcpy(h + 12, &len, 4);
    return tcp_send_all(s, h, 16) && (!len || tcp_send_all(s, data, (int)len));
}

// Reads packets until one with the wanted id arrives (others, like "device list updated", are skipped).
// Returns the data length or -1. *changed is set when the server said its device list changed.
static int recv_pkt(SOCKET s, unsigned want, unsigned char *buf, int cap, int *changed) {
    for (int guard = 0; guard < 16; guard++) {
        unsigned char h[16];
        if (!tcp_recv_all(s, h, 16) || memcmp(h, "ORGB", 4)) return -1;
        unsigned id, len; memcpy(&id, h + 8, 4); memcpy(&len, h + 12, 4);
        if (len > (unsigned)cap) {   // too big for us: drain and fail
            unsigned char tmp[512];
            while (len) { int k = len > sizeof(tmp) ? (int)sizeof(tmp) : (int)len; if (!tcp_recv_all(s, tmp, k)) return -1; len -= k; }
            return -1;
        }
        if (len && !tcp_recv_all(s, buf, (int)len)) return -1;
        if (id == PKT_DEVICE_LIST_UPDATED && changed) *changed = 1;
        if (id == want) return (int)len;
    }
    return -1;
}

// ---- controller description, protocol version 0
typedef struct { const unsigned char *p, *e; int bad; } rd_t;
static unsigned rd32(rd_t *r) { if (r->e - r->p < 4) { r->bad = 1; return 0; } unsigned v; memcpy(&v, r->p, 4); r->p += 4; return v; }
static unsigned rd16(rd_t *r) { if (r->e - r->p < 2) { r->bad = 1; return 0; } unsigned v = r->p[0] | r->p[1] << 8; r->p += 2; return v; }
static void rdskip(rd_t *r, unsigned n) { if ((unsigned)(r->e - r->p) < n) { r->bad = 1; r->p = r->e; } else r->p += n; }
static void rdstr(rd_t *r, char *out, int cap) {
    unsigned n = rd16(r);
    if (out) { int k = (int)n < cap ? (int)n : cap - 1; if ((unsigned)(r->e - r->p) >= n) { memcpy(out, r->p, k); out[k] = 0; } else out[0] = 0; }
    rdskip(r, n);
}

typedef struct { int type, nleds, ncolors; char name[64], vendor_desc[64]; unsigned colors[EXT_MAX_LEDS]; } ctrl_t;

static int parse_ctrl(const unsigned char *buf, int len, ctrl_t *c) {
    rd_t r = { buf, buf + len, 0 };
    memset(c, 0, sizeof(*c));
    rd32(&r);                          // data size
    c->type = (int)rd32(&r);
    rdstr(&r, c->name, sizeof(c->name));
    rdstr(&r, c->vendor_desc, sizeof(c->vendor_desc));   // description
    rdstr(&r, NULL, 0);                // version
    rdstr(&r, NULL, 0);                // serial
    rdstr(&r, NULL, 0);                // location
    unsigned nmodes = rd16(&r);
    rd32(&r);                          // active mode
    for (unsigned m = 0; m < nmodes && !r.bad; m++) {
        rdstr(&r, NULL, 0);            // name
        rdskip(&r, 4 * 9);             // value, flags, speed min/max, colors min/max, speed, direction, colour mode
        unsigned nc = rd16(&r);
        rdskip(&r, nc * 4);
    }
    unsigned nzones = rd16(&r);
    for (unsigned z = 0; z < nzones && !r.bad; z++) {
        rdstr(&r, NULL, 0);
        rdskip(&r, 4 * 4);             // type, leds min, leds max, leds count
        unsigned ml = rd16(&r);
        rdskip(&r, ml);                // matrix map (height, width, cells)
    }
    unsigned nl = rd16(&r);
    for (unsigned i = 0; i < nl && !r.bad; i++) { rdstr(&r, NULL, 0); rdskip(&r, 4); }
    unsigned ncol = rd16(&r);
    c->nleds = (int)nl;
    c->ncolors = 0;
    for (unsigned i = 0; i < ncol && !r.bad; i++) { unsigned v = rd32(&r); if (i < EXT_MAX_LEDS) c->colors[c->ncolors++] = v; }
    return !r.bad && nl > 0;
}

static const char *type_name(int t) {
    static const char *n[] = { "motherboard", "memory", "graphics card", "cooler", "LED strip", "keyboard", "mouse", "mousemat",
                               "headset", "headset stand", "gamepad", "light", "speaker", "virtual", "storage", "case", "microphone",
                               "accessory", "keypad" };
    return t >= 0 && t < (int)(sizeof(n) / sizeof(n[0])) ? n[t] : "device";
}

static SOCKET orgb_connect(const char *host, unsigned *count) {
    char ip[64]; int port = host_port(host, ORGB_PORT, ip, sizeof(ip));
    SOCKET s = tcp_connect(ip, port, 1000);
    if (s == INVALID_SOCKET) return s;
    static const char nm[] = "haku control";
    unsigned char buf[16];
    if (!send_pkt(s, 0, PKT_SET_CLIENT_NAME, nm, sizeof(nm)) || !send_pkt(s, 0, PKT_REQUEST_CONTROLLER_COUNT, NULL, 0) ||
        recv_pkt(s, PKT_REQUEST_CONTROLLER_COUNT, buf, sizeof(buf), NULL) < 4) { closesocket(s); return INVALID_SOCKET; }
    memcpy(count, buf, 4);
    return s;
}

static int read_ctrl(SOCKET s, unsigned idx, ctrl_t *c) {
    static __declspec(thread) unsigned char *buf;
    if (!buf) buf = malloc(1 << 20);
    if (!buf || !send_pkt(s, idx, PKT_REQUEST_CONTROLLER_DATA, NULL, 0)) return 0;
    int n = recv_pkt(s, PKT_REQUEST_CONTROLLER_DATA, buf, 1 << 20, NULL);
    return n > 0 && parse_ctrl(buf, n, c);
}

static int orgb_open(ext_dev *d) {
    unsigned count = 0;
    SOCKET s = orgb_connect(d->host, &count);
    if (s == INVALID_SOCKET) return 0;
    ctrl_t *c = malloc(sizeof(ctrl_t));
    char sec[16], want[64]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    snprintf(want, sizeof(want), "%s", cfg_get(sec, "match", ""));
    // the controller: number sub, unless it has another name than the one it was added with
    int idx = d->sub >= 0 && (unsigned)d->sub < count && c && read_ctrl(s, (unsigned)d->sub, c) ? d->sub : -1;
    if (c && want[0] && (idx < 0 || strcmp(c->name, want))) {
        idx = -1;
        for (unsigned i = 0; i < count && i < 64 && idx < 0; i++) if (read_ctrl(s, i, c) && !strcmp(c->name, want)) idx = (int)i;
        if (idx >= 0) logf_("dev.%d (openrgb): '%s' is now number %d", d->id, want, idx);
    }
    if (idx < 0) { free(c); closesocket(s); return 0; }
    if (!want[0]) { cfg_set(sec, "match", c->name); cfg_save_if_dirty(); }
    d->nleds = c->nleds > EXT_MAX_LEDS ? EXT_MAX_LEDS : c->nleds;
    snprintf(d->info, sizeof(d->info), "OpenRGB · %s · %s", c->name, type_name(c->type));
    orgb_t *o = calloc(1, sizeof(orgb_t));
    if (!o) { free(c); closesocket(s); return 0; }
    memcpy(o->orig, c->colors, sizeof(unsigned) * c->ncolors); o->norig = c->ncolors; o->idx = (unsigned)idx;
    free(c);
    d->priv = o; d->sock = s;
    send_pkt(s, o->idx, PKT_SETCUSTOMMODE, NULL, 0);   // direct / custom mode
    return 1;
}

static int send_colors(ext_dev *d, const unsigned *col, int n) {
    unsigned char buf[6 + EXT_MAX_LEDS * 4];
    unsigned size = 6 + n * 4;
    unsigned short nn = (unsigned short)n;
    memcpy(buf, &size, 4); memcpy(buf + 4, &nn, 2); memcpy(buf + 6, col, n * 4);
    return send_pkt(d->sock, ((orgb_t *)d->priv)->idx, PKT_UPDATELEDS, buf, size);
}

static int orgb_send(ext_dev *d, const rgbf *c, int n) {
    // the server may tell us its device list changed: indices can shift, so reconnect
    u_long avail = 0;
    while (ioctlsocket(d->sock, FIONREAD, &avail) == 0 && avail >= 16) {
        unsigned char h[16];
        if (!tcp_recv_all(d->sock, h, 16) || memcmp(h, "ORGB", 4)) return 0;
        unsigned id, len; memcpy(&id, h + 8, 4); memcpy(&len, h + 12, 4);
        if (id == PKT_DEVICE_LIST_UPDATED) return 0;
        unsigned char tmp[512];
        while (len) { int k = len > sizeof(tmp) ? (int)sizeof(tmp) : (int)len; if (!tcp_recv_all(d->sock, tmp, k)) return 0; len -= k; }
    }
    unsigned col[EXT_MAX_LEDS];
    for (int i = 0; i < n; i++) col[i] = to8(c[i].r) | to8(c[i].g) << 8 | to8(c[i].b) << 16;
    return send_colors(d, col, n);
}

static void orgb_leave(ext_dev *d, int how) {
    orgb_t *o = d->priv;
    unsigned col[EXT_MAX_LEDS] = { 0 };
    if (how == LEAVE_OFF) send_colors(d, col, d->nleds);
    else if (how == LEAVE_RESTORE && o->norig) send_colors(d, o->orig, o->norig < d->nleds ? o->norig : d->nleds);
}

static void orgb_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

// Lists the controllers of an OpenRGB server on this PC.
static void orgb_discover(int ms, void (*found)(const disc_t *)) {
    (void)ms;
    unsigned count = 0;
    SOCKET s = orgb_connect("127.0.0.1", &count);
    if (s == INVALID_SOCKET) return;
    ctrl_t *c = malloc(sizeof(ctrl_t));
    for (unsigned i = 0; c && i < count && i < 64; i++) {
        if (!read_ctrl(s, i, c)) continue;
        disc_t x = { "openrgb", "127.0.0.1" };
        x.sub = (int)i; x.nleds = c->nleds;
        strcpy_s(x.name, sizeof(x.name), c->name);
        snprintf(x.info, sizeof(x.info), "OpenRGB · %s", type_name(c->type));
        found(&x);
    }
    free(c);
    closesocket(s);
}

// ---- what OpenRGB on this PC has, for the PC page and the diagnostics
int orgb_list(orgb_ctl *out, int max) {
    unsigned count = 0;
    SOCKET s = orgb_connect("127.0.0.1", &count);
    if (s == INVALID_SOCKET) return -1;
    ctrl_t *c = malloc(sizeof(ctrl_t));
    int n = 0;
    for (unsigned i = 0; c && i < count && n < max; i++) {
        if (!read_ctrl(s, i, c)) continue;
        orgb_ctl *o = &out[n++];
        o->idx = (int)i; o->type = c->type; o->leds = c->nleds;
        snprintf(o->name, sizeof(o->name), "%s", c->name);
        snprintf(o->kind, sizeof(o->kind), "%s", type_name(c->type));
    }
    free(c);
    closesocket(s);
    return n;
}

static SRWLOCK chk_lk = SRWLOCK_INIT;
static orgb_ctl chk[48];
static int chk_n, chk_state;   // 0 not looked yet, 1 looking, 2 OpenRGB answers, 3 not running, 4 not installed
static volatile LONG chk_busy;

static int server_answers(void) {
    SOCKET s = tcp_connect("127.0.0.1", ORGB_PORT, 500);
    if (s == INVALID_SOCKET) return 0;
    closesocket(s);
    return 1;
}

// OpenRGB started if needed (openrgb_app.c), its list read once it stops growing (it finds hardware for a few
// seconds after it starts), and what is new added
static unsigned __stdcall check_fn(void *p) {
    (void)p;
    net_init();
    static orgb_ctl tmp[48];
    int n = -1;
    if (orgbapp_ensure(server_answers)) {
        for (int i = 0, same = 0, last = -1; i < 20 && same < 2; i++) {
            if (i) Sleep(1500);
            n = orgb_list(tmp, 48);
            same = n > 0 && n == last ? same + 1 : 0;
            last = n;
        }
    }
    wchar_t exe[MAX_PATH];
    AcquireSRWLockExclusive(&chk_lk);
    chk_state = n >= 0 ? 2 : orgbapp_exe(exe, 0) ? 3 : 4; chk_n = n < 0 ? 0 : n;
    memcpy(chk, tmp, sizeof(orgb_ctl) * chk_n);
    ReleaseSRWLockExclusive(&chk_lk);
    logf_("openrgb: %s", n >= 0 ? "SDK server answers" : chk_state == 3 ? "installed, no SDK server" : "not on this PC");
    if (n > 0) orgbapp_add_new(tmp, n);
    InterlockedExchange(&chk_busy, 0);
    ui_refresh();
    return 0;
}

void orgb_check_start(void) {
    if (InterlockedCompareExchange(&chk_busy, 1, 0)) return;
    AcquireSRWLockExclusive(&chk_lk); if (chk_state != 2) chk_state = 1; ReleaseSRWLockExclusive(&chk_lk);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, check_fn, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&chk_busy, 0);
}

// "orgb":{"state":2,"ctls":[{"i":0,"type":2,"kind":"graphics card","name":"...","leds":8}]}
int orgb_json(char *out, int cap) {
    AcquireSRWLockShared(&chk_lk);
    int n = snprintf(out, cap, "\"orgb\":{\"state\":%d,", chk_state);
    n += orgbapp_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"ctls\":[");
    for (int i = 0; i < chk_n && n < cap - 300; i++) {
        n += snprintf(out + n, cap - n, "%s{\"i\":%d,\"type\":%d,\"kind\":\"%s\",\"leds\":%d,\"name\":\"", i ? "," : "", chk[i].idx, chk[i].type, chk[i].kind, chk[i].leds);
        n += json_escape_to(out + n, cap - n, chk[i].name);
        n += snprintf(out + n, cap - n, "\"}");
    }
    n += snprintf(out + n, cap - n, "]}");
    ReleaseSRWLockShared(&chk_lk);
    return n;
}

const ext_driver drv_openrgb = { "openrgb", "OpenRGB", 30, 1, orgb_open, orgb_send, orgb_leave, orgb_close, orgb_discover };
