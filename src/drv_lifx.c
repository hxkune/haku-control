// LIFX bulbs and strips over the official LAN protocol (binary, UDP 56700), one colour per light.
// https://lan.developer.lifx.com  Messages used: GetService(2)/StateService(3), Get(101)/LightState(107),
// SetColor(102), SetLightPower(117).
#include "devices.h"
#include <stdlib.h>

#define LIFX_PORT 56700
#pragma pack(push, 1)
typedef struct {
    unsigned short size, proto;          // proto: 1024 | addressable | tagged
    unsigned int   source;
    unsigned char  target[8], res1[6], flags, seq;
    unsigned long long res2;
    unsigned short type, res3;
} lifx_hdr;
typedef struct { unsigned short h, s, b, k; } hsbk;
#pragma pack(pop)

typedef struct { char ip[64]; hsbk orig; int orig_power, have; unsigned char seq; } lifx_t;

static unsigned int source_id(void) { static unsigned int s; if (!s) s = 0x4841u << 16 | (GetCurrentProcessId() & 0xFFFF); return s; }

static int build(unsigned char *buf, unsigned short type, int tagged, int res, const void *payload, int plen, unsigned char seq) {
    lifx_hdr h = { 0 };
    h.size = (unsigned short)(sizeof(h) + plen);
    h.proto = 1024 | 0x1000 | (tagged ? 0x2000 : 0);
    h.source = source_id();
    h.flags = res ? 1 : 0;
    h.seq = seq; h.type = type;
    memcpy(buf, &h, sizeof(h));
    if (plen) memcpy(buf + sizeof(h), payload, plen);
    return (int)sizeof(h) + plen;
}

// Sends Get and waits for LightState: fills colour, power and label.
static int get_state(SOCKET s, const char *ip, hsbk *c, int *power, char *label, int lcap) {
    unsigned char buf[128];
    int n = build(buf, 101, 0, 1, NULL, 0, 1);
    if (!udp_send(s, ip, LIFX_PORT, buf, n)) return 0;
    DWORD end = GetTickCount() + 1000;
    unsigned char r[256]; char from[48];
    for (int left; (left = (int)(end - GetTickCount())) > 0;) {
        int k = udp_recv(s, r, sizeof(r), left, from, sizeof(from));
        if (k < (int)sizeof(lifx_hdr) + 52 || strcmp(from, ip)) continue;
        lifx_hdr h; memcpy(&h, r, sizeof(h));
        if (h.type != 107) continue;
        memcpy(c, r + sizeof(h), sizeof(hsbk));
        unsigned short pw; memcpy(&pw, r + sizeof(h) + 10, 2);
        *power = pw != 0;
        if (label) { int m = lcap - 1 < 32 ? lcap - 1 : 32; memcpy(label, r + sizeof(h) + 12, m); label[m] = 0; }
        return 1;
    }
    return 0;
}

static void set_power(ext_dev *d, int on, unsigned ms) {
    lifx_t *l = d->priv;
    unsigned char buf[64], p[6];
    unsigned short lv = on ? 65535 : 0; memcpy(p, &lv, 2); memcpy(p + 2, &ms, 4);
    int n = build(buf, 117, 0, 0, p, 6, ++l->seq);
    udp_send(d->sock, l->ip, LIFX_PORT, buf, n);
}

static void set_color(ext_dev *d, hsbk c, unsigned ms) {
    lifx_t *l = d->priv;
    unsigned char buf[64], p[13] = { 0 };
    memcpy(p + 1, &c, 8); memcpy(p + 9, &ms, 4);
    int n = build(buf, 102, 0, 0, p, 13, ++l->seq);
    udp_send(d->sock, l->ip, LIFX_PORT, buf, n);
}

static int lifx_open(ext_dev *d) {
    lifx_t *l = calloc(1, sizeof(lifx_t));
    if (!l) return 0;
    strcpy_s(l->ip, sizeof(l->ip), d->host);
    d->sock = udp_socket(0, 0);
    char label[40] = "";
    if (d->sock == INVALID_SOCKET || !get_state(d->sock, l->ip, &l->orig, &l->orig_power, label, sizeof(label))) {
        if (d->sock != INVALID_SOCKET) closesocket(d->sock);
        d->sock = INVALID_SOCKET; free(l); return 0;
    }
    l->have = 1;
    d->priv = l; d->nleds = 1;
    snprintf(d->info, sizeof(d->info), "LIFX · %s", label);
    if (!l->orig_power) set_power(d, 1, 0);
    return 1;
}

static int lifx_send(ext_dev *d, const rgbf *c, int n) {
    if (n < 1) return 1;
    float r = clampf(c[0].r, 0, 1), g = clampf(c[0].g, 0, 1), b = clampf(c[0].b, 0, 1);
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), dl = mx - mn, h = 0;
    if (dl > 0) h = mx == r ? fmodf((g - b) / dl, 6) : mx == g ? (b - r) / dl + 2 : (r - g) / dl + 4;
    h = h * 60; if (h < 0) h += 360;
    hsbk k = { (unsigned short)(h / 360 * 65535), (unsigned short)(mx > 0 ? dl / mx * 65535 : 0), (unsigned short)(mx * 65535), 3500 };
    set_color(d, k, 1000 / 20);
    return 1;
}

static void lifx_leave(ext_dev *d, int how) {
    lifx_t *l = d->priv;
    if (how == LEAVE_OFF) set_power(d, 0, 300);
    else if (how == LEAVE_RESTORE && l->have) { set_color(d, l->orig, 300); if (!l->orig_power) set_power(d, 0, 300); }
}

static void lifx_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

static void lifx_discover(int ms, void (*found)(const disc_t *)) {
    SOCKET s = udp_socket(0, 1);
    if (s == INVALID_SOCKET) return;
    unsigned char buf[64];
    int n = build(buf, 2, 1, 1, NULL, 0, 0);
    udp_send_all_ifaces(s, NULL, LIFX_PORT, buf, n);
    char hosts[32][48]; int nh = 0;
    DWORD end = GetTickCount() + ms / 2;
    unsigned char r[256]; char from[48];
    for (int left; (left = (int)(end - GetTickCount())) > 0 && nh < 32;) {
        int k = udp_recv(s, r, sizeof(r), left, from, sizeof(from));
        if (k < (int)sizeof(lifx_hdr)) continue;
        lifx_hdr h; memcpy(&h, r, sizeof(h));
        if (h.type != 3) continue;
        int dup = 0;
        for (int i = 0; i < nh; i++) if (!strcmp(hosts[i], from)) dup = 1;
        if (!dup) strcpy_s(hosts[nh++], 48, from);
    }
    for (int i = 0; i < nh; i++) {
        hsbk c; int pw; char label[40] = "";
        disc_t x = { "lifx" };
        strcpy_s(x.host, sizeof(x.host), hosts[i]);
        x.sub = -1; x.nleds = 1;
        if (get_state(s, hosts[i], &c, &pw, label, sizeof(label)) && label[0]) strcpy_s(x.name, sizeof(x.name), label);
        else strcpy_s(x.name, sizeof(x.name), "LIFX");
        strcpy_s(x.info, sizeof(x.info), "LIFX LAN");
        found(&x);
    }
    closesocket(s);
}

const ext_driver drv_lifx = { "lifx", "LIFX", 20, 0, lifx_open, lifx_send, lifx_leave, lifx_close, lifx_discover };
