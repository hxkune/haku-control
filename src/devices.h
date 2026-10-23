// SPDX-License-Identifier: GPL-3.0-only
// LAN / bridge light devices ("ext devices"): WLED, OpenRGB, Govee, LIFX, Yeelight, Elgato, Wooting...
// Each configured device lives in a settings section [dev.<id>] (kind, host, sub, name, leds, enabled, reverse, own)
// and becomes one colour zone ("zone.dev<id>"). One worker thread (devices.c) opens, feeds and closes them;
// drivers (drv_*.c) only implement the protocol.
#pragma once
#include <winsock2.h>
#include "common.h"
#include <time.h>

#define EXT_MAX       EXT_SLOTS   // configured devices
#define EXT_MAX_LEDS  512     // LEDs per device
#define DISC_MAX      64      // discovery results

typedef struct ext_dev ext_dev;

// A device's short history, shown on its card (devices.c dev_event): what happened, when, how many times in a row
enum { EV_ONLINE = 1, EV_BACK, EV_FAIL, EV_LOST, EV_CAP, EV_LET };
#define EXT_EV 8
typedef struct { unsigned t; unsigned char code, a, n; } ext_ev;   // t: time(); a: EV_CAP the CAP_*, EV_LET the LEAVE_*

// What the device does when we let go of it ([general] on_exit, or the device switched off in the app).
enum { LEAVE_OFF, LEAVE_KEEP, LEAVE_RESTORE };

// What a device can show, set by its driver on open (shown on its card): not stated, effects frame by frame, its
// effects through a mode that is not made for it (may fade at each colour), one steady colour only, no mode haku
// can set, a mode that did not take, several controllers of its name with none to tell them apart
enum { CAP_NONE, CAP_EFFECTS, CAP_EXPERIMENTAL, CAP_STEADY, CAP_UNSUPPORTED, CAP_FAILED, CAP_AMBIGUOUS };

// OpenRGB: a board's ARGB connector as a device of its own, sub = controller + ORGB_PART_STEP x (zone + 1)
#define ORGB_PART_STEP 1000

typedef struct {
    char kind[16];            // driver id
    char host[64];
    int  sub;                 // controller / light index inside the host, -1 if none
    char name[64];
    int  nleds;
    char info[96];            // model, firmware... shown in the list
} disc_t;

typedef struct {
    const char *kind;         // settings value, e.g. "wled"
    const char *title;        // shown in the UI, e.g. "WLED"
    float       rate;         // max frames per second the device handles well
    int         per_led;      // 1: addressable strip (LED count can be set), 0: separate lights (bulbs, lamps of a bridge)
    // Connect and read the device info. Fills d->nleds (when the device reports it), d->info, maybe d->name.
    // Returns 1 when the device is ready for frames.
    int  (*open)(ext_dev *d);
    // One frame, n = d->nleds colours (0..1 sRGB, brightness already applied). Returns 0 if the connection is lost.
    int  (*send)(ext_dev *d, const rgbf *c, int n);
    // Let go of the device: LEAVE_OFF / LEAVE_KEEP / LEAVE_RESTORE (the state it had before open, if the driver can).
    void (*leave)(ext_dev *d, int how);
    void (*close)(ext_dev *d);
    // Look for devices on the LAN for about `ms` milliseconds; calls found() for each.
    void (*discover)(int ms, void (*found)(const disc_t *));
} ext_driver;

struct ext_dev {
    int   id;                 // N of [dev.N]
    const ext_driver *drv;
    char  host[64], name[64], info[96];
    char  key[128];           // pairing token (Hue); saved with ext_save_key()
    int   sub, cfg_leds, nleds, reverse, enabled;
    int   own;                // [dev.N] own=1: given back to its own lighting (its maker's app, the mode it had)
    int   same;               // the [dev.N] of the same hardware on the other path (an OpenRGB device's [dev.M] same=N: dev.N drives it directly)
    int   held;               // 1: that other path drives it ([dev.M] path=direct|openrgb), so this one is not opened
    int   covered;            // an OpenRGB connector whose whole controller is a device too: that one's id (it lights it, this one is not opened)
    int   cap;                // CAP_*, from the driver's last open
    float rate;               // frames/s this device takes (set by the driver on open; 0: the driver's rate)
    float gain[3];            // [dev.N] warmth as r/g/b gains
    float cfg_rate;           // [dev.N] rate: chosen in the window (0: auto)
    int   cfg_static;         // [dev.N] static=1: OpenRGB through its Static mode even if it has Direct (a board that flickers in Direct)
    char  zones[160];         // [dev.N] zones: LEDs on each resizable zone, "name=count|..." (a | as ; starts a comment in settings.ini) (OpenRGB's ARGB headers)
    unsigned gen;             // its settings' generation: a connect started under other settings is not taken over
    int   online;
    DWORD next_try;           // reconnect backoff
    int   fails;              // attempts that failed since it last stayed connected
    DWORD up_at;              // when it came online (a loss soon after counts as a failed attempt)
    ext_ev ev[EXT_EV];        // the last events, oldest first (kept across a settings change)
    int   nev;
    SOCKET sock;              // for drivers with one socket
    void *priv;               // driver state
};

// The send pace (devices.c). due: when the next frame may go at the chosen rate, moved on by one interval from where
// it was (the jitter of ~33 ms frames does not cost a frame) or from now after a pause (no catch-up burst); hard: the
// device's own limit in ms since the last send, never cut short. Ticks compared as differences, so they may wrap.
static inline int ext_pace_due(DWORD now, DWORD due, DWORD last, int hard) { return (int)(now - due) >= 0 && (!hard || (int)(now - last) >= hard); }
static inline DWORD ext_pace_next(DWORD now, DWORD due, int iv) { return (int)(now - due) < iv ? due + iv : now + iv; }

// One event in a device's history (devices.c): the same as the last one again only counts (a device that keeps
// dropping is one line), the oldest goes when it is full
static inline void ext_event(ext_dev *d, int code, int a) {
    ext_ev *l = d->nev ? &d->ev[d->nev - 1] : NULL;
    if (l && l->code == code && l->a == a) { if (l->n < 255) l->n++; l->t = (unsigned)time(NULL); return; }
    if (d->nev == EXT_EV) { memmove(d->ev, d->ev + 1, sizeof(d->ev) - sizeof(d->ev[0])); d->nev--; }
    d->ev[d->nev++] = (ext_ev){ (unsigned)time(NULL), (unsigned char)code, (unsigned char)a, 1 };
}

// A finished connect (devices.c), the device copy it ran on (res) against the devices now: the index that takes it
// over, or -1 and *how, what is done with what it opened: LEAVE_KEEP closed as it is (its settings changed, the new
// connect sets it up; on the other path or its whole board), LEAVE_OFF (switched off), LEAVE_RESTORE (removed, given back).
// Failed: the index is still that of its device under the same settings (for the retry wait), switched off or not.
static inline int ext_open_fate(const ext_dev *devs, int n, const ext_dev *res, int *how) {
    *how = LEAVE_RESTORE;
    for (int k = 0; k < n; k++) {
        const ext_dev *d = &devs[k];
        if (d->id != res->id) continue;
        if (d->drv != res->drv || strcmp(d->host, res->host) || d->sub != res->sub) return -1;   // other hardware under its number
        if (d->gen != res->gen || d->online) { *how = LEAVE_KEEP; return -1; }
        *how = !d->enabled ? LEAVE_OFF : d->own ? LEAVE_RESTORE : d->held || d->covered ? LEAVE_KEEP : -1;
        return k;
    }
    return -1;
}

// OpenRGB on this PC (drv_openrgb.c)
typedef struct { int idx, type, leds; char name[64], kind[24]; } orgb_ctl;
int  orgb_list(orgb_ctl *out, int max);   // -1: no OpenRGB SDK server answers on this PC
// OpenRGB looked after (openrgb_app.c): found, started for its SDK server, what it finds added, set up
int  orgbapp_exe(wchar_t *out, int running);
int  orgbapp_auto(void);
int  orgbapp_ensure(int (*answers)(void));   // 1: the SDK server answers (started if needed)
void orgbapp_add_new(const orgb_ctl *c, int n);
void orgbapp_removed(const char *name);
int  orgbapp_json(char *out, int cap);
void orgbapp_stop(void);                      // OpenRGB started here closed (all its devices went back to their own lighting)

extern const ext_driver drv_wled, drv_openrgb, drv_govee, drv_lifx, drv_yeelight, drv_hue, drv_wiz, drv_tuya, drv_nlusb, drv_goveecloud, drv_elgato, drv_wooting, drv_divoom,
                        drv_razer, drv_steelseries, drv_logitech, drv_tryx, drv_aura, drv_lianli;
const ext_driver *ext_driver_by_kind(const char *kind);
void ext_save_key(ext_dev *d, const char *key);   // stores a pairing token in [dev.N] key=
// What the device can do, for the window ([dev.N] caps, kmin, kmax): "" everything, "white" white light only (its
// colour temperature range in K). The window then shows only the settings that apply.
void ext_set_caps(ext_dev *d, const char *caps, int kmin, int kmax);

// ---- netutil.c
void   net_init(void);
int    net_addr(const char *host, int port, struct sockaddr_in *a);
SOCKET tcp_connect(const char *host, int port, int timeout_ms);
int    tcp_send_all(SOCKET s, const void *data, int len);
int    tcp_recv_all(SOCKET s, void *data, int len);
int    http_request(const char *host, int port, const char *method, const char *path, const char *body, char *buf, int cap);
int    http_call(const char *host, int port, const char *method, const char *path, const char *body, char *buf, int cap,
                 char *why, int whycap);   // HTTP/1.1, one packet: for devices' own small web servers
int    https_download(const wchar_t *url, const wchar_t *to, long long max, volatile LONG *pct);
int    sha256_hex(const wchar_t *file, char *hex);   // 64 hex digits
int    file_signer(const wchar_t *file, wchar_t *name, int cap);   // 1: validly signed, by `name`
int    web_call(const char *url, const char *body, char *out, int cap);   // http(s); POST json, GET if body NULL
int    web_post(const char *url, const char *ctype, const char *body, char *out, int cap);   // the body's type given
SOCKET udp_socket(int bind_port, int broadcast);   // bind_port < 0: unbound
int    udp_send(SOCKET s, const char *host, int port, const void *data, int len);
int    udp_recv(SOCKET s, void *buf, int cap, int ms, char *from, int from_cap);
void   udp_send_all_ifaces(SOCKET s, const char *group, int port, const void *data, int len);   // group NULL: broadcast
void   mdns_browse(const char *service, int ms, void (*hit)(const char *ip, void *ctx), void *ctx);
int    host_port(const char *host, int def_port, char *h, int cap);   // "ip[:port]" -> ip, port
int    json_get_str(const char *js, const char *key, char *out, int cap);
double json_get_num(const char *js, const char *key, double def);
const char *json_get_obj(const char *js, const char *key);
int    json_escape_to(char *out, int cap, const char *s);

static inline unsigned char to8(float v) { return (unsigned char)(clampf(v, 0, 1) * 255.0f + 0.5f); }
