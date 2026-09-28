// WLED (ESP8266 / ESP32 LED controller firmware, https://kno.wled.ge):
//   HTTP JSON API for info / on-off, UDP realtime "DNRGB" (port 21324) for frames, mDNS "_wled._tcp" to find it.
// While frames arrive WLED shows them; when they stop (timeout byte) it returns to its own effect by itself.
#include "devices.h"
#include <stdlib.h>

#define WLED_UDP     21324
#define RT_TIMEOUT   2        // seconds without frames before WLED goes back to its own effect
#define CHUNK        489      // LEDs per DNRGB packet

typedef struct { int was_on; char ip[64]; int http_port; rgbf last[EXT_MAX_LEDS]; int nlast; } wled_t;

static int wled_open(ext_dev *d) {
    char ip[64]; int port = host_port(d->host, 80, ip, sizeof(ip));
    static char buf[16384];
    if (http_request(ip, port, "GET", "/json/info", NULL, buf, sizeof(buf)) != 200) return 0;
    int n = (int)json_get_num(json_get_obj(buf, "leds"), "count", 0);
    if (n <= 0) return 0;
    d->nleds = n;
    char ver[32] = "", arch[32] = "";
    json_get_str(buf, "ver", ver, sizeof(ver));
    json_get_str(buf, "arch", arch, sizeof(arch));
    snprintf(d->info, sizeof(d->info), "WLED %s · %s", ver, arch);
    wled_t *w = calloc(1, sizeof(wled_t));
    strcpy_s(w->ip, sizeof(w->ip), ip); w->http_port = port;
    w->was_on = 1;
    if (http_request(ip, port, "GET", "/json/state", NULL, buf, sizeof(buf)) == 200) w->was_on = (int)json_get_num(buf, "on", 1);
    if (!w->was_on) http_request(ip, port, "POST", "/json/state", "{\"on\":true}", buf, sizeof(buf));
    d->priv = w;
    d->sock = udp_socket(-1, 0);
    if (d->sock == INVALID_SOCKET) { free(w); d->priv = NULL; return 0; }
    return 1;
}

static int wled_send_rt(ext_dev *d, const rgbf *c, int n, int timeout) {
    wled_t *w = d->priv;
    unsigned char pkt[4 + CHUNK * 3];
    for (int start = 0; start < n; start += CHUNK) {
        int m = n - start < CHUNK ? n - start : CHUNK;
        pkt[0] = 4; pkt[1] = (unsigned char)timeout; pkt[2] = (unsigned char)(start >> 8); pkt[3] = (unsigned char)start;
        for (int i = 0; i < m; i++) {
            pkt[4 + i * 3] = to8(c[start + i].r); pkt[5 + i * 3] = to8(c[start + i].g); pkt[6 + i * 3] = to8(c[start + i].b);
        }
        if (!udp_send(d->sock, w->ip, WLED_UDP, pkt, 4 + m * 3)) return 0;
    }
    return 1;
}

static int wled_send(ext_dev *d, const rgbf *c, int n) {
    wled_t *w = d->priv;
    memcpy(w->last, c, n * sizeof(rgbf)); w->nlast = n;
    return wled_send_rt(d, c, n, RT_TIMEOUT);
}

static void wled_leave(ext_dev *d, int how) {
    wled_t *w = d->priv;
    char buf[1024];
    if (how == LEAVE_OFF) http_request(w->ip, w->http_port, "POST", "/json/state", "{\"on\":false,\"live\":false}", buf, sizeof(buf));
    else if (how == LEAVE_RESTORE)
        http_request(w->ip, w->http_port, "POST", "/json/state", w->was_on ? "{\"live\":false}" : "{\"on\":false,\"live\":false}", buf, sizeof(buf));
    else if (w->nlast) wled_send_rt(d, w->last, w->nlast, 255);   // LEAVE_KEEP: hold the last frame
}

static void wled_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

typedef struct { void (*found)(const disc_t *); } wctx;
static void wled_hit(const char *ip, void *p) {
    wctx *c = p;
    static char buf[16384];   // one discovery thread per driver
    if (http_request(ip, 80, "GET", "/json/info", NULL, buf, sizeof(buf)) != 200) return;
    disc_t x = { "wled" };
    strcpy_s(x.host, sizeof(x.host), ip);
    x.sub = -1;
    json_get_str(buf, "name", x.name, sizeof(x.name));
    x.nleds = (int)json_get_num(json_get_obj(buf, "leds"), "count", 0);
    char ver[32] = ""; json_get_str(buf, "ver", ver, sizeof(ver));
    snprintf(x.info, sizeof(x.info), "WLED %s", ver);
    c->found(&x);
}

static void wled_discover(int ms, void (*found)(const disc_t *)) {
    wctx c = { found };
    mdns_browse("_wled._tcp.local", ms, wled_hit, &c);
}

const ext_driver drv_wled = { "wled", "WLED", 40, 1, wled_open, wled_send, wled_leave, wled_close, wled_discover };
