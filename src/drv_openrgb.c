// SPDX-License-Identifier: GPL-3.0-only
// OpenRGB SDK client (https://openrgb.org, "SDK Server", TCP 6742). Every OpenRGB controller (a keyboard,
// a GPU, a fan hub...) becomes one haku device: host = the OpenRGB server, sub = controller index.
// The protocol is used at version 0 (the client never announces a newer one), so the controller data
// always comes in the simplest, stable layout.
#include "devices.h"
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

typedef struct { unsigned int orig[EXT_MAX_LEDS]; int norig; } orgb_t;

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
    if (!c || d->sub < 0 || (unsigned)d->sub >= count || !read_ctrl(s, (unsigned)d->sub, c)) { free(c); closesocket(s); return 0; }
    d->nleds = c->nleds > EXT_MAX_LEDS ? EXT_MAX_LEDS : c->nleds;
    snprintf(d->info, sizeof(d->info), "OpenRGB · %s · %s", c->name, type_name(c->type));
    orgb_t *o = calloc(1, sizeof(orgb_t));
    memcpy(o->orig, c->colors, sizeof(unsigned) * c->ncolors); o->norig = c->ncolors;
    free(c);
    d->priv = o; d->sock = s;
    send_pkt(s, (unsigned)d->sub, PKT_SETCUSTOMMODE, NULL, 0);   // direct / custom mode
    return 1;
}

static int send_colors(ext_dev *d, const unsigned *col, int n) {
    unsigned char buf[6 + EXT_MAX_LEDS * 4];
    unsigned size = 6 + n * 4;
    unsigned short nn = (unsigned short)n;
    memcpy(buf, &size, 4); memcpy(buf + 4, &nn, 2); memcpy(buf + 6, col, n * 4);
    return send_pkt(d->sock, (unsigned)d->sub, PKT_UPDATELEDS, buf, size);
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

const ext_driver drv_openrgb = { "openrgb", "OpenRGB", 30, 1, orgb_open, orgb_send, orgb_leave, orgb_close, orgb_discover };
