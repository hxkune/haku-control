// SPDX-License-Identifier: GPL-3.0-only
// Yeelight bulbs / strips with "LAN Control" on (Yeelight app). Control is JSON lines over TCP 55443; normal
// commands are limited to ~60/min, so after connecting we ask the bulb for "music mode": it connects back to us
// and then takes any number of commands. Discovery: SSDP-like M-SEARCH to 239.255.255.250:1982.
#include "devices.h"
#include <ws2tcpip.h>
#include <stdlib.h>

typedef struct { SOCKET ctl, music; char ip[64]; int power, bright, rgb, ct, mode, have, last_bri, id; DWORD last_slow; } yee_t;

static int yee_cmd(SOCKET s, yee_t *y, const char *method, const char *params) {
    char js[256];
    int n = snprintf(js, sizeof(js), "{\"id\":%d,\"method\":\"%s\",\"params\":[%s]}\r\n", ++y->id, method, params);
    return tcp_send_all(s, js, n);
}

// Reads one reply line with "result" (skips "props" notifications).
static int yee_reply(SOCKET s, char *buf, int cap) {
    int got = 0;
    DWORD end = GetTickCount() + 1500;
    while ((int)(end - GetTickCount()) > 0 && got < cap - 1) {
        int r = recv(s, buf + got, cap - 1 - got, 0);
        if (r <= 0) return 0;
        got += r; buf[got] = 0;
        char *line = buf;
        for (char *nl; (nl = strstr(line, "\r\n"));) {
            *nl = 0;
            if (strstr(line, "\"result\"")) { memmove(buf, line, strlen(line) + 1); return 1; }
            line = nl + 2;
        }
        memmove(buf, line, strlen(line) + 1); got = (int)strlen(buf);
    }
    return 0;
}

static int yee_open(ext_dev *d) {
    yee_t *y = calloc(1, sizeof(yee_t));
    if (!y) return 0;
    y->ctl = y->music = INVALID_SOCKET;
    char ip[64]; int port = host_port(d->host, 55443, ip, sizeof(ip));
    strcpy_s(y->ip, sizeof(y->ip), ip);
    y->ctl = tcp_connect(ip, port, 1500);
    if (y->ctl == INVALID_SOCKET) { free(y); return 0; }
    char buf[1024];
    if (yee_cmd(y->ctl, y, "get_prop", "\"power\",\"bright\",\"rgb\",\"ct\",\"color_mode\"") && yee_reply(y->ctl, buf, sizeof(buf))) {
        // {"id":1,"result":["on","100","16711680","4000","1"]}
        char *p = strchr(buf, '[');
        char v[5][24] = { { 0 } };
        for (int i = 0; p && i < 5; i++) {
            p = strchr(p, '"'); if (!p) break;
            char *e = strchr(p + 1, '"'); if (!e) break;
            int l = (int)(e - p - 1); if (l > 23) l = 23;
            memcpy(v[i], p + 1, l); v[i][l] = 0; p = e + 1;
        }
        y->power = !strcmp(v[0], "on"); y->bright = atoi(v[1]); y->rgb = atoi(v[2]); y->ct = atoi(v[3]); y->mode = atoi(v[4]);
        y->have = 1;
    } else { closesocket(y->ctl); free(y); return 0; }

    // music mode: listen on a free port of the interface that reaches the bulb
    struct sockaddr_in me; int ml = sizeof(me);
    getsockname(y->ctl, (struct sockaddr *)&me, &ml);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    me.sin_port = 0;
    if (ls != INVALID_SOCKET && bind(ls, (struct sockaddr *)&me, sizeof(me)) == 0 && listen(ls, 1) == 0) {
        getsockname(ls, (struct sockaddr *)&me, &ml);
        char myip[48], params[96];
        inet_ntop(AF_INET, &me.sin_addr, myip, sizeof(myip));
        snprintf(params, sizeof(params), "1,\"%s\",%d", myip, ntohs(me.sin_port));
        yee_cmd(y->ctl, y, "set_music", params);
        fd_set r; FD_ZERO(&r); FD_SET(ls, &r);
        struct timeval tv = { 3, 0 };
        if (select(0, &r, NULL, NULL, &tv) == 1) y->music = accept(ls, NULL, NULL);
    }
    if (ls != INVALID_SOCKET) closesocket(ls);
    if (!y->power) yee_cmd(y->music != INVALID_SOCKET ? y->music : y->ctl, y, "set_power", "\"on\",\"sudden\",0");
    y->last_bri = -1;
    d->priv = y; d->nleds = 1;
    snprintf(d->info, sizeof(d->info), "Yeelight%s", y->music != INVALID_SOCKET ? " · music mode" : " · slow mode (1 cmd/s)");
    return 1;
}

static int yee_send(ext_dev *d, const rgbf *c, int n) {
    yee_t *y = d->priv;
    if (n < 1) return 1;
    // replies / notifications on the control connection: read and drop them
    u_long avail = 0; char junk[512];
    while (ioctlsocket(y->ctl, FIONREAD, &avail) == 0 && avail > 0) if (recv(y->ctl, junk, sizeof(junk), 0) <= 0) return 0;
    SOCKET s = y->music;
    if (s == INVALID_SOCKET) {   // without music mode the bulb takes about one command a second
        if (GetTickCount() - y->last_slow < 1100) return 1;
        y->last_slow = GetTickCount(); s = y->ctl;
    }
    float mx = max(c[0].r, max(c[0].g, c[0].b));
    int bri = (int)(mx * 100 + 0.5f);
    char p[64];
    if (bri < 1) bri = 1;
    int rgb = mx > 0 ? (to8(c[0].r / mx) << 16 | to8(c[0].g / mx) << 8 | to8(c[0].b / mx)) : 0;
    snprintf(p, sizeof(p), "%d,\"smooth\",100", rgb ? rgb : 1);
    if (!yee_cmd(s, y, "set_rgb", p)) return 0;
    if (bri != y->last_bri) {
        snprintf(p, sizeof(p), "%d,\"smooth\",100", bri);
        if (!yee_cmd(s, y, "set_bright", p)) return 0;
        y->last_bri = bri;
    }
    return 1;
}

static void yee_leave(ext_dev *d, int how) {
    yee_t *y = d->priv;
    SOCKET s = y->music != INVALID_SOCKET ? y->music : y->ctl;
    char p[64];
    if (how == LEAVE_OFF) yee_cmd(s, y, "set_power", "\"off\",\"smooth\",300");
    else if (how == LEAVE_RESTORE && y->have) {
        if (y->mode == 2) { snprintf(p, sizeof(p), "%d,\"smooth\",300", y->ct); yee_cmd(s, y, "set_ct_abx", p); }
        else { snprintf(p, sizeof(p), "%d,\"smooth\",300", y->rgb); yee_cmd(s, y, "set_rgb", p); }
        snprintf(p, sizeof(p), "%d,\"smooth\",300", y->bright); yee_cmd(s, y, "set_bright", p);
        if (!y->power) yee_cmd(s, y, "set_power", "\"off\",\"smooth\",300");
    }
    if (y->music != INVALID_SOCKET) yee_cmd(y->ctl, y, "set_music", "0");
}

static void yee_close(ext_dev *d) {
    yee_t *y = d->priv;
    if (y) {
        if (y->music != INVALID_SOCKET) closesocket(y->music);
        if (y->ctl != INVALID_SOCKET) closesocket(y->ctl);
        free(y);
    }
    d->priv = NULL;
}

static void yee_discover(int ms, void (*found)(const disc_t *)) {
    SOCKET s = udp_socket(0, 0);
    if (s == INVALID_SOCKET) return;
    static const char q[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n";
    udp_send_all_ifaces(s, "239.255.255.250", 1982, q, (int)sizeof(q) - 1);
    DWORD end = GetTickCount() + ms;
    char buf[1500], from[48], seen[32][48]; int nseen = 0;
    for (int left; (left = (int)(end - GetTickCount())) > 0;) {
        int n = udp_recv(s, buf, sizeof(buf) - 1, left, from, sizeof(from));
        if (n <= 0) continue;
        buf[n] = 0;
        if (!strstr(buf, "yeelight://")) continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], from)) dup = 1;
        if (dup || nseen >= 32) continue;
        strcpy_s(seen[nseen++], 48, from);
        disc_t x = { "yeelight" };
        strcpy_s(x.host, sizeof(x.host), from);
        x.sub = -1; x.nleds = 1;
        char *m = strstr(buf, "\nmodel: "), *nm = strstr(buf, "\nname: ");
        char model[32] = "";
        if (m) sscanf_s(m + 8, "%31[^\r\n]", model, (unsigned)sizeof(model));
        if (nm) sscanf_s(nm + 7, "%63[^\r\n]", x.name, (unsigned)sizeof(x.name));
        if (!x.name[0]) snprintf(x.name, sizeof(x.name), "Yeelight %s", model);
        snprintf(x.info, sizeof(x.info), "Yeelight %s", model);
        found(&x);
    }
    closesocket(s);
}

const ext_driver drv_yeelight = { "yeelight", "Yeelight", 10, 0, yee_open, yee_send, yee_leave, yee_close, yee_discover };
