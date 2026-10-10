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
    PKT_RESIZEZONE               = 1000,
    PKT_UPDATELEDS               = 1050,
    PKT_SETCUSTOMMODE            = 1100,
    PKT_UPDATEMODE               = 1101,
};

// orig / mode: the colours and the mode the controller had before haku took it, given back when it lets go;
// how / use: the way haku lights it (DRIVE_*) and, for its Static mode, that mode's description
enum { DRIVE_DIRECT, DRIVE_STATIC, DRIVE_ONE, DRIVE_NONE };
typedef struct {
    unsigned int orig[EXT_MAX_LEDS]; int norig; unsigned idx; int mode_idx, mode_len; unsigned char mode[512];
    int how, use_idx, use_len, use_cmin; unsigned char use[512];
} orgb_t;

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

// mode: the active mode as the protocol describes it (name, value, flags... colours), to send it back as it was;
// md / mraw: every mode (its name, colour mode, minimum colours and description), to choose the one haku lights it in
enum { COLORS_NONE, COLORS_PER_LED, COLORS_MODE_SPECIFIC, COLORS_RANDOM };   // a mode's colour mode (OpenRGB's MODE_COLORS_*)
typedef struct { char name[32]; int cm, cmin, off, len; } mode_t_;
// zn: the zones (a motherboard's ARGB headers can be resized: leds min < max, and OpenRGB starts them at 0)
typedef struct { char name[32]; int min, max, count; } zone_t_;
typedef struct {
    int type, nleds, ncolors, active, mode_len, nmodes, mraw_len, nzones; char name[64], vendor_desc[64], mode_name[32];
    unsigned colors[EXT_MAX_LEDS]; unsigned char mode[512]; mode_t_ md[64]; unsigned char mraw[8192]; zone_t_ zn[16];
} ctrl_t;

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
    c->active = (int)rd32(&r);         // active mode
    for (unsigned m = 0; m < nmodes && !r.bad; m++) {
        const unsigned char *m0 = r.p;
        char nm[32];
        rdstr(&r, nm, sizeof(nm));     // name
        rdskip(&r, 4 * 5);             // value, flags, speed min/max, colors min
        unsigned cmax = rd32(&r); (void)cmax;
        rdskip(&r, 4 * 2);             // speed, direction
        int cm = (int)rd32(&r);        // colour mode
        unsigned nc = rd16(&r);
        rdskip(&r, nc * 4);
        if (r.bad) break;
        int len = (int)(r.p - m0);
        if ((int)m == c->active) {
            strcpy_s(c->mode_name, sizeof(c->mode_name), nm);
            if (len <= (int)sizeof(c->mode)) { c->mode_len = len; memcpy(c->mode, m0, len); }
        }
        if (c->nmodes < 64 && c->mraw_len + len <= (int)sizeof(c->mraw)) {
            mode_t_ *x = &c->md[c->nmodes++];
            strcpy_s(x->name, sizeof(x->name), nm); x->cm = cm; x->off = c->mraw_len; x->len = len;
            unsigned cmin; memcpy(&cmin, m0 + 2 + (m0[0] | m0[1] << 8) + 16, 4); x->cmin = (int)cmin;
            memcpy(c->mraw + c->mraw_len, m0, len); c->mraw_len += len;
        }
    }
    unsigned nzones = rd16(&r);
    for (unsigned z = 0; z < nzones && !r.bad; z++) {
        char zn[32];
        rdstr(&r, zn, sizeof(zn));
        rd32(&r);                      // type
        int lmin = (int)rd32(&r), lmax = (int)rd32(&r), lc = (int)rd32(&r);
        if (c->nzones < 16) { zone_t_ *x = &c->zn[c->nzones++]; strcpy_s(x->name, sizeof(x->name), zn); x->min = lmin; x->max = lmax; x->count = lc; }
        unsigned ml = rd16(&r);
        rdskip(&r, ml);                // matrix map (height, width, cells)
    }
    unsigned nl = rd16(&r);
    for (unsigned i = 0; i < nl && !r.bad; i++) { rdstr(&r, NULL, 0); rdskip(&r, 4); }
    unsigned ncol = rd16(&r);
    c->nleds = (int)nl;
    c->ncolors = 0;
    for (unsigned i = 0; i < ncol && !r.bad; i++) { unsigned v = rd32(&r); if (i < EXT_MAX_LEDS) c->colors[c->ncolors++] = v; }
    // no LEDs yet is fine when a zone can be given some (an ARGB header at 0): the window offers to set them
    int resizable = 0;
    for (int z = 0; z < c->nzones; z++) if (c->zn[z].min < c->zn[z].max) resizable = 1;
    return !r.bad && (nl > 0 || resizable);
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

// The mode to give back when haku lets go (the controller's own effect, as its maker's app or OpenRGB set it). One
// found still in Direct / Custom was left so by haku (a crash) or another app: then the last other mode seen,
// kept in [dev.N] orig_mode as "<index>:<hex of its description>" (when it fits a settings line).
static int direct_mode(const char *name) { return !_stricmp(name, "Direct") || !_stricmp(name, "Custom"); }

static void keep_mode(const char *sec, const ctrl_t *c, orgb_t *o) {
    char v[256];
    if (c->mode_len && !direct_mode(c->mode_name)) {
        o->mode_idx = c->active; o->mode_len = c->mode_len; memcpy(o->mode, c->mode, c->mode_len);
        int n = snprintf(v, sizeof(v), "%d:", c->active);
        if (n + c->mode_len * 2 < (int)sizeof(v)) {
            for (int i = 0; i < c->mode_len; i++) n += snprintf(v + n, sizeof(v) - n, "%02x", c->mode[i]);
            if (strcmp(cfg_get(sec, "orig_mode", ""), v)) { cfg_set(sec, "orig_mode", v); cfg_save_if_dirty(); }
        }
        return;
    }
    const char *s = cfg_get(sec, "orig_mode", ""), *h = strchr(s, ':');
    if (!h) return;
    int len = (int)strlen(h + 1) / 2;
    if (len < 1 || len > (int)sizeof(o->mode)) return;
    for (int i = 0; i < len; i++) { unsigned b; if (sscanf_s(h + 1 + i * 2, "%2x", &b) != 1) return; o->mode[i] = (unsigned char)b; }
    o->mode_idx = atoi(s); o->mode_len = len;
}

// How haku lights a controller. OpenRGB's own choice (SETCUSTOMMODE: a mode named Direct, Custom or Static) leaves a
// controller with none of them in its own effect (a flashing one, say) and says nothing: haku's colours then only
// flash over it. So: Direct / Custom with colours per LED, frame by frame; else its Static mode, slower (many such
// controllers fade or blink at every colour), per LED or one colour for all; else not at all.
static int pick_mode(const ctrl_t *c, orgb_t *o, float *rate, const char **how) {
    static const char *const NAMES[] = { "Direct", "Custom" };
    for (int k = 0; k < 2; k++)
        for (int m = 0; m < c->nmodes; m++)
            if (!_stricmp(c->md[m].name, NAMES[k]) && c->md[m].cm == COLORS_PER_LED) { *how = c->md[m].name; return DRIVE_DIRECT; }
    for (int pass = 0; pass < 2; pass++)
        for (int m = 0; m < c->nmodes; m++) {
            const mode_t_ *x = &c->md[m];
            if (_stricmp(x->name, "Static") || x->len > (int)sizeof(o->use)) continue;
            if (pass == 0 ? x->cm != COLORS_PER_LED : x->cm != COLORS_MODE_SPECIFIC) continue;
            o->use_idx = m; o->use_len = x->len; o->use_cmin = x->cmin; memcpy(o->use, c->mraw + x->off, x->len);
            if (pass == 0) { *rate = 5; *how = "Static, slower"; return DRIVE_STATIC; }
            *rate = 4; *how = "Static, one colour"; return DRIVE_ONE;
        }
    *how = "no mode haku can set";
    return DRIVE_NONE;
}

// The resizable zones (ARGB headers): the sizes chosen in the window ([dev.N] zones "name=count|...") given to
// OpenRGB (it starts them at 0 and keeps a size only until it restarts, so this is done at every connect), and the
// zones as they are written to [dev.N] zones_found "name:count:min:max|..." for the window. 1: a size was changed.
static int apply_zones(SOCKET s, unsigned idx, const char *sec, const ctrl_t *c) {
    const char *want = cfg_get(sec, "zones", "");
    char found[256] = ""; int n = 0, changed = 0;
    for (int z = 0; z < c->nzones; z++) {
        const zone_t_ *x = &c->zn[z];
        if (x->min >= x->max) continue;
        int size = x->count;
        char key[40]; snprintf(key, sizeof(key), "%s=", x->name);
        for (const char *p = want; (p = strstr(p, key)) != NULL; p += strlen(key))
            if (p == want || p[-1] == '|') { size = atoi(p + strlen(key)); break; }
        if (size < x->min) size = x->min;
        if (size > x->max) size = x->max;
        if (size > EXT_MAX_LEDS) size = EXT_MAX_LEDS;
        if (size != x->count) {
            unsigned char b[8]; int zi = z;
            memcpy(b, &zi, 4); memcpy(b + 4, &size, 4);
            if (send_pkt(s, idx, PKT_RESIZEZONE, b, 8)) changed = 1;
        }
        n += snprintf(found + n, sizeof(found) - n, "%s%s:%d:%d:%d", n ? "|" : "", x->name, size, x->min, x->max);
        if (n >= (int)sizeof(found)) break;
    }
    if (strcmp(cfg_get(sec, "zones_found", ""), found)) { cfg_set(sec, "zones_found", found); cfg_save_if_dirty(); }
    return changed;
}

// a mode as it is described (index, then the description), made the active one
static int send_mode(ext_dev *d, int idx, const unsigned char *desc, int len) {
    unsigned char buf[8 + 1024];
    if (len > 1024) return 0;
    unsigned size = 8 + len;
    memcpy(buf, &size, 4); memcpy(buf + 4, &idx, 4); memcpy(buf + 8, desc, len);
    return send_pkt(d->sock, ((orgb_t *)d->priv)->idx, PKT_UPDATEMODE, buf, size);
}

static int server_answers(void);

static int orgb_open(ext_dev *d) {
    unsigned count = 0;
    SOCKET s = orgb_connect(d->host, &count);
    // OpenRGB on this PC not running (e.g. closed here when all its devices were given back): started for this one
    if (s == INVALID_SOCKET && (!_strnicmp(d->host, "127.0.0.1", 9) || !_strnicmp(d->host, "localhost", 9)) && orgbapp_ensure(server_answers))
        s = orgb_connect(d->host, &count);
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
    // ARGB headers sized as chosen in the window; then read again, with their LEDs
    if (apply_zones(s, (unsigned)idx, sec, c)) { Sleep(150); if (!read_ctrl(s, (unsigned)idx, c)) { free(c); closesocket(s); return 0; } }
    if (c->nleds < 1) {   // only zones still at 0 LEDs: nothing to light until the window gives them some
        snprintf(d->info, sizeof(d->info), "OpenRGB · %s · set the LEDs on its connectors", c->name);
        free(c); closesocket(s); return 0;
    }
    d->nleds = c->nleds > EXT_MAX_LEDS ? EXT_MAX_LEDS : c->nleds;
    snprintf(d->info, sizeof(d->info), "OpenRGB · %s · %s", c->name, type_name(c->type));
    orgb_t *o = calloc(1, sizeof(orgb_t));
    if (!o) { free(c); closesocket(s); return 0; }
    memcpy(o->orig, c->colors, sizeof(unsigned) * c->ncolors); o->norig = c->ncolors; o->idx = (unsigned)idx;
    keep_mode(sec, c, o);
    const char *how = ""; float rate = 0;
    o->how = pick_mode(c, o, &rate, &how);
    d->rate = rate;
    snprintf(d->info, sizeof(d->info), "OpenRGB · %s · %s · %s", c->name, type_name(c->type), how);
    if (o->how != DRIVE_DIRECT) {   // which modes it has, as names and colour modes, so a report shows why
        char ml[400]; int k = 0;
        for (int m = 0; m < c->nmodes && k < (int)sizeof(ml) - 40; m++) k += snprintf(ml + k, sizeof(ml) - k, "%s%s/%d", m ? ", " : "", c->md[m].name, c->md[m].cm);
        logf_("dev.%d (openrgb): '%s' has no Direct mode: %s (modes: %s)", d->id, c->name, how, ml);
    }
    free(c);
    d->priv = o; d->sock = s;
    if (o->how == DRIVE_DIRECT) send_pkt(s, o->idx, PKT_SETCUSTOMMODE, NULL, 0);   // Direct / Custom
    else if (o->how == DRIVE_STATIC) send_mode(d, o->use_idx, o->use, o->use_len);
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
    orgb_t *o = d->priv;
    if (o->how == DRIVE_NONE) return 1;   // nothing it takes: left as it is
    if (o->how == DRIVE_ONE) {   // one colour for the whole controller: its Static mode, with the frame's average
        float r = 0, g = 0, b = 0;
        for (int i = 0; i < n; i++) { r += c[i].r; g += c[i].g; b += c[i].b; }
        if (n) { r /= n; g /= n; b /= n; }
        unsigned v = to8(r) | to8(g) << 8 | to8(b) << 16;
        // the description up to its colour count, then that many colours (at least its minimum, at least one)
        int head = 2 + (o->use[0] | o->use[1] << 8) + 36, nc = o->use_cmin > 0 ? o->use_cmin : 1;
        unsigned char m[512];
        if (head + 2 + nc * 4 > (int)sizeof(m) || head > o->use_len) return 1;
        memcpy(m, o->use, head);
        m[head] = (unsigned char)nc; m[head + 1] = (unsigned char)(nc >> 8);
        for (int i = 0; i < nc; i++) memcpy(m + head + 2 + i * 4, &v, 4);
        return send_mode(d, o->use_idx, m, head + 2 + nc * 4);
    }
    unsigned col[EXT_MAX_LEDS];
    for (int i = 0; i < n; i++) col[i] = to8(c[i].r) | to8(c[i].g) << 8 | to8(c[i].b) << 16;
    return send_colors(d, col, n);
}

static void orgb_leave(ext_dev *d, int how) {
    orgb_t *o = d->priv;
    unsigned col[EXT_MAX_LEDS] = { 0 };
    if (how == LEAVE_OFF) send_colors(d, col, d->nleds);
    else if (how == LEAVE_RESTORE) {
        if (o->norig) send_colors(d, o->orig, o->norig < d->nleds ? o->norig : d->nleds);
        if (o->mode_len) {   // its own effect again: the mode it had, as it was described
            unsigned char buf[8 + sizeof(o->mode)];
            unsigned size = 8 + o->mode_len;
            memcpy(buf, &size, 4); memcpy(buf + 4, &o->mode_idx, 4); memcpy(buf + 8, o->mode, o->mode_len);
            send_pkt(d->sock, o->idx, PKT_UPDATEMODE, buf, size);
        }
    }
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
    // its devices all given back to their own lighting: not started for them, only asked if it runs anyway
    if (ext_orgb_given_back() ? server_answers() : orgbapp_ensure(server_answers)) {
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
