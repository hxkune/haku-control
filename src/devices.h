// SPDX-License-Identifier: GPL-3.0-only
// LAN / bridge light devices ("ext devices"): WLED, OpenRGB, Govee, LIFX, Yeelight, Elgato, Wooting...
// Each configured device lives in a settings section [dev.<id>] (kind, host, sub, name, leds, enabled, reverse)
// and becomes one colour zone ("zone.dev<id>"). One worker thread (devices.c) opens, feeds and closes them;
// drivers (drv_*.c) only implement the protocol.
#pragma once
#include <winsock2.h>
#include "common.h"

#define EXT_MAX       EXT_SLOTS   // configured devices
#define EXT_MAX_LEDS  512     // LEDs per device
#define DISC_MAX      64      // discovery results

typedef struct ext_dev ext_dev;

// What the device does when we let go of it ([general] on_exit, or the device switched off in the app).
enum { LEAVE_OFF, LEAVE_KEEP, LEAVE_RESTORE };

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
    int   online;
    DWORD next_try;           // reconnect backoff
    int   fails;
    SOCKET sock;              // for drivers with one socket
    void *priv;               // driver state
};

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

extern const ext_driver drv_wled, drv_openrgb, drv_govee, drv_lifx, drv_yeelight, drv_hue, drv_wiz, drv_tuya, drv_nlusb, drv_goveecloud, drv_elgato, drv_wooting, drv_divoom;
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
