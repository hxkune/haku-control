// SPDX-License-Identifier: GPL-3.0-only
// LAN / bridge light devices ("ext devices"): WLED, OpenRGB, Govee, LIFX, Yeelight...
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

extern const ext_driver drv_wled, drv_openrgb, drv_govee, drv_lifx, drv_yeelight, drv_hue, drv_wiz, drv_tuya, drv_nlusb, drv_goveecloud;
const ext_driver *ext_driver_by_kind(const char *kind);
void ext_save_key(ext_dev *d, const char *key);   // stores a pairing token in [dev.N] key=

// ---- netutil.c
void   net_init(void);
int    net_addr(const char *host, int port, struct sockaddr_in *a);
SOCKET tcp_connect(const char *host, int port, int timeout_ms);
int    tcp_send_all(SOCKET s, const void *data, int len);
int    tcp_recv_all(SOCKET s, void *data, int len);
int    http_request(const char *host, int port, const char *method, const char *path, const char *body, char *buf, int cap);
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
