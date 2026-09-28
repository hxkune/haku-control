// Govee lights with the "LAN Control" switch on in the Govee Home app (official LAN API):
//   scan: multicast 239.255.255.250:4001, answers come to UDP 4002; commands go to the light's UDP 4003.
// One colour per light (colorwc + brightness). Replies (scan, devStatus) all arrive on port 4002, so one
// shared socket bound to it is used under a lock.
#include "devices.h"
#include <stdlib.h>

typedef struct { int on, bri, r, g, b, kelvin, have; int last_bri; char ip[64]; } govee_t;

static CRITICAL_SECTION rx_cs;
static INIT_ONCE rx_once = INIT_ONCE_STATIC_INIT;
static SOCKET rx = INVALID_SOCKET;

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&rx_cs); return TRUE; }

static SOCKET rx_sock(void) {   // call with rx_cs held
    if (rx == INVALID_SOCKET) rx = udp_socket(4002, 0);
    return rx;
}

static void cmd(SOCKET s, const char *ip, const char *json) { udp_send(s, ip, 4003, json, (int)strlen(json)); }

// Asks a light for its state; 1 if it answered.
static int dev_status(const char *ip, govee_t *g) {
    InitOnceExecuteOnce(&rx_once, init_cs, NULL, NULL);
    EnterCriticalSection(&rx_cs);
    SOCKET s = rx_sock();
    int ok = 0;
    if (s != INVALID_SOCKET) {
        cmd(s, ip, "{\"msg\":{\"cmd\":\"devStatus\",\"data\":{}}}");
        DWORD end = GetTickCount() + 1200;
        char buf[1024], from[48];
        for (int left; !ok && (left = (int)(end - GetTickCount())) > 0;) {
            int n = udp_recv(s, buf, sizeof(buf) - 1, left, from, sizeof(from));
            if (n <= 0) continue;
            buf[n] = 0;
            if (strcmp(from, ip) || !strstr(buf, "devStatus")) continue;
            g->on = (int)json_get_num(buf, "onOff", 1);
            g->bri = (int)json_get_num(buf, "brightness", 100);
            const char *c = json_get_obj(buf, "color");
            g->r = (int)json_get_num(c, "r", 255); g->g = (int)json_get_num(c, "g", 255); g->b = (int)json_get_num(c, "b", 255);
            g->kelvin = (int)json_get_num(buf, "colorTemInKelvin", 0);
            g->have = ok = 1;
        }
    }
    LeaveCriticalSection(&rx_cs);
    return ok;
}

static int govee_open(ext_dev *d) {
    govee_t *g = calloc(1, sizeof(govee_t));
    if (!g) return 0;
    strcpy_s(g->ip, sizeof(g->ip), d->host);
    if (!dev_status(d->host, g)) { free(g); return 0; }
    d->sock = udp_socket(-1, 0);
    if (d->sock == INVALID_SOCKET) { free(g); return 0; }
    d->nleds = 1;
    d->priv = g;
    g->last_bri = -1;
    if (!g->on) cmd(d->sock, g->ip, "{\"msg\":{\"cmd\":\"turn\",\"data\":{\"value\":1}}}");
    strcpy_s(d->info, sizeof(d->info), "Govee LAN");
    return 1;
}

static int govee_send(ext_dev *d, const rgbf *c, int n) {
    govee_t *g = d->priv;
    if (n < 1) return 1;
    float mx = max(c[0].r, max(c[0].g, c[0].b));
    int bri = (int)(mx * 100 + 0.5f);
    char js[160];
    if (bri < 1) {
        if (g->last_bri != 0) cmd(d->sock, g->ip, "{\"msg\":{\"cmd\":\"brightness\",\"data\":{\"value\":1}}}");
        snprintf(js, sizeof(js), "{\"msg\":{\"cmd\":\"colorwc\",\"data\":{\"color\":{\"r\":0,\"g\":0,\"b\":0},\"colorTemInKelvin\":0}}}");
        cmd(d->sock, g->ip, js);
        g->last_bri = 0;
        return 1;
    }
    if (bri != g->last_bri) {
        snprintf(js, sizeof(js), "{\"msg\":{\"cmd\":\"brightness\",\"data\":{\"value\":%d}}}", bri);
        cmd(d->sock, g->ip, js);
        g->last_bri = bri;
    }
    snprintf(js, sizeof(js), "{\"msg\":{\"cmd\":\"colorwc\",\"data\":{\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"colorTemInKelvin\":0}}}",
             to8(c[0].r / mx), to8(c[0].g / mx), to8(c[0].b / mx));
    cmd(d->sock, g->ip, js);
    return 1;   // UDP: loss is noticed on the next reconnect only
}

static void govee_leave(ext_dev *d, int how) {
    govee_t *g = d->priv;
    char js[200];
    if (how == LEAVE_OFF) cmd(d->sock, g->ip, "{\"msg\":{\"cmd\":\"turn\",\"data\":{\"value\":0}}}");
    else if (how == LEAVE_RESTORE && g->have) {
        snprintf(js, sizeof(js), "{\"msg\":{\"cmd\":\"brightness\",\"data\":{\"value\":%d}}}", g->bri < 1 ? 1 : g->bri);
        cmd(d->sock, g->ip, js);
        snprintf(js, sizeof(js), "{\"msg\":{\"cmd\":\"colorwc\",\"data\":{\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"colorTemInKelvin\":%d}}}",
                 g->r, g->g, g->b, g->kelvin);
        cmd(d->sock, g->ip, js);
        if (!g->on) cmd(d->sock, g->ip, "{\"msg\":{\"cmd\":\"turn\",\"data\":{\"value\":0}}}");
    }
}

static void govee_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

static void govee_discover(int ms, void (*found)(const disc_t *)) {
    InitOnceExecuteOnce(&rx_once, init_cs, NULL, NULL);
    EnterCriticalSection(&rx_cs);
    SOCKET s = rx_sock();
    if (s != INVALID_SOCKET) {
        SOCKET tx = udp_socket(-1, 0);
        static const char scan[] = "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
        if (tx != INVALID_SOCKET) { udp_send_all_ifaces(tx, "239.255.255.250", 4001, scan, (int)sizeof(scan) - 1); closesocket(tx); }
        DWORD end = GetTickCount() + ms;
        char buf[1024], from[48];
        for (int left; (left = (int)(end - GetTickCount())) > 0;) {
            int n = udp_recv(s, buf, sizeof(buf) - 1, left, from, sizeof(from));
            if (n <= 0) continue;
            buf[n] = 0;
            if (!strstr(buf, "\"scan\"")) continue;
            disc_t x = { "govee" };
            x.sub = -1; x.nleds = 1;
            if (!json_get_str(buf, "ip", x.host, sizeof(x.host))) strcpy_s(x.host, sizeof(x.host), from);
            char sku[32] = ""; json_get_str(buf, "sku", sku, sizeof(sku));
            snprintf(x.name, sizeof(x.name), "Govee %s", sku);
            json_get_str(buf, "device", x.info, sizeof(x.info));
            found(&x);
        }
    }
    LeaveCriticalSection(&rx_cs);
}

const ext_driver drv_govee = { "govee", "Govee", 8, 0, govee_open, govee_send, govee_leave, govee_close, govee_discover };
