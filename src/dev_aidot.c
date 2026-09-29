// AiDot / Linkind Wi-Fi bulbs, local LAN protocol (same as the official python-AiDot library):
//   discovery: UDP broadcast to :6666, AES-256-ECB with a fixed key
//   control:   TCP :10000, frames "1E ED | type u16 | len u32 | AES-128-ECB(json)" with the per-bulb key
// Keys come from %APPDATA%\haku-control\aidot.json (written once by scripts\aidot-setup.ps1).
// Everything runs on one thread; colours are smoothed and sent at most `rate` times per second.
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <stdlib.h>
#include <ctype.h>
#include <process.h>

#define MAX_BULBS 8

typedef struct {
    char   id[64], name[96], mac[32], key[40], pass[64];
    int    simple;                 // "simpleVersion" present: full payload with credentials
    char   ip[32];
    SOCKET sock;
    int    online, warned;
    long long asc;
    int    seq;
    DWORD  retry_at, last_send, last_ping;
    unsigned char rx[32768]; int rxn;
    unsigned fx_hash;              // research: last logged effect attributes
    char   sent_probe[64];
    float  cur[3];                 // smoothed colour
    int    sent_rgb[3], sent_dim, sent_on, sent_k;
    BCRYPT_KEY_HANDLE bkey;
} bulb_t;

static bulb_t  bulbs[MAX_BULBS];
static int     nbulbs;
static char    user_id[64];
static SRWLOCK lk = SRWLOCK_INIT;
static rgbf    target[MAX_BULBS];
static int     target_k[MAX_BULBS];
static int     have_target, enabled;
static volatile LONG run_thread;
static HANDLE  th;
static volatile LONG keys_changed;
static FILETIME key_time;
static BCRYPT_ALG_HANDLE alg;
static BCRYPT_KEY_HANDLE disc_key;

// ---------------------------------------------------------------- AES-ECB (PKCS7)
static BCRYPT_KEY_HANDLE make_key(const char *k, int len) {
    BCRYPT_KEY_HANDLE h = NULL;
    UCHAR buf[32] = { 0 };
    int n = (int)strlen(k); if (n > len) n = len;
    memcpy(buf, k, n);
    BCryptGenerateSymmetricKey(alg, &h, NULL, 0, buf, len, 0);
    return h;
}

static int aes_enc(BCRYPT_KEY_HANDLE key, const char *in, int n, unsigned char *out, int cap) {
    int pad = 16 - n % 16, total = n + pad;
    if (total > cap) return -1;
    memcpy(out, in, n); memset(out + n, pad, pad);
    ULONG r = 0;
    return BCryptEncrypt(key, out, total, NULL, NULL, 0, out, total, &r, 0) ? -1 : (int)r;
}

static int aes_dec(BCRYPT_KEY_HANDLE key, unsigned char *buf, int n) {
    if (n <= 0 || n % 16) return -1;
    ULONG r = 0;
    if (BCryptDecrypt(key, buf, n, NULL, NULL, 0, buf, n, &r, 0)) return -1;
    int pad = buf[n - 1];
    if (pad >= 1 && pad <= 16) n -= pad;
    buf[n] = 0;
    return n;
}

// ---------------------------------------------------------------- tiny JSON helpers
// Copies the string value of "key" found between p and end. Returns 1 if found.
static int json_str(const char *p, const char *end, const char *key, char *out, int cap) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    for (const char *s = p; s && s < end; s++) {
        s = strstr(s, pat);
        if (!s || s >= end) break;
        const char *v = s + strlen(pat);
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        if (*v != ':') continue;
        v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        if (*v != '"') continue;
        v++;
        int i = 0;
        while (v < end && *v != '"' && i < cap - 1) { if (*v == '\\' && v[1]) v++; out[i++] = *v++; }
        out[i] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}

static long long json_int(const char *s, const char *key) {
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *v = strstr(s, pat);
    return v ? _atoi64(v + strlen(pat)) : -1;
}

static void json_escape(const char *in, char *out, int cap) {
    int j = 0;
    for (; *in && j < cap - 2; in++) { if (*in == '"' || *in == '\\') out[j++] = '\\'; out[j++] = *in; }
    out[j] = 0;
}

static void key_file(wchar_t *p) { app_data_path(L"aidot.json", p); }

static FILETIME key_file_time(void) {
    wchar_t p[MAX_PATH]; key_file(p);
    WIN32_FILE_ATTRIBUTE_DATA a; FILETIME z = { 0 };
    return GetFileAttributesExW(p, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

static int load_keys(void) {
    wchar_t p[MAX_PATH];
    key_file(p);
    key_time = key_file_time();
    nbulbs = 0;
    FILE *f = _wfopen(p, L"rb");
    if (!f) { logf_("aidot: no key file %ls (errno %d) - sign in under Devices, Sign-ins and pairing", p, errno); return 0; }
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    char *s = buf; if ((unsigned char)s[0] == 0xEF) s += 3;   // BOM
    char *end = s + strlen(s);
    json_str(s, end, "userId", user_id, sizeof(user_id));
    char *arr = strstr(s, "\"lights\"");
    nbulbs = 0;
    for (char *o = arr ? strchr(arr, '{') : NULL; o && nbulbs < MAX_BULBS; o = strchr(o + 1, '{')) {
        char *oe = strchr(o, '}'); if (!oe) break;
        bulb_t *b = &bulbs[nbulbs];
        memset(b, 0, sizeof(*b));
        json_str(o, oe, "id", b->id, sizeof(b->id));
        json_str(o, oe, "name", b->name, sizeof(b->name));
        json_str(o, oe, "mac", b->mac, sizeof(b->mac));
        json_str(o, oe, "aesKey", b->key, sizeof(b->key));
        json_str(o, oe, "password", b->pass, sizeof(b->pass));
        char sv[32]; json_str(o, oe, "simpleVersion", sv, sizeof(sv)); b->simple = sv[0] != 0;
        for (char *c = b->mac; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!b->id[0] || !b->key[0]) continue;
        b->sock = INVALID_SOCKET;
        b->sent_on = -1;
        nbulbs++;
        o = oe;
    }
    logf_("aidot: %d bulb(s) in key file", nbulbs);
    return nbulbs;
}

// ---------------------------------------------------------------- framing
static int send_frame(bulb_t *b, int type, const char *json) {
    static unsigned char out[24576];
    int n = aes_enc(b->bkey, json, (int)strlen(json), out + 8, sizeof(out) - 8);
    if (n < 0) return -1;
    out[0] = 0x1E; out[1] = 0xED; out[2] = 0; out[3] = (unsigned char)type;
    out[4] = (unsigned char)(n >> 24); out[5] = (unsigned char)(n >> 16); out[6] = (unsigned char)(n >> 8); out[7] = (unsigned char)n;
    return send(b->sock, (char *)out, n + 8, 0) == n + 8 ? 0 : -1;
}

static void close_bulb(bulb_t *b, const char *why) {
    if (b->sock != INVALID_SOCKET) closesocket(b->sock);
    if (b->online) logf_("aidot: %s offline (%s)", b->mac, why);
    b->sock = INVALID_SOCKET; b->online = 0; b->rxn = 0;
    b->retry_at = GetTickCount() + 5000;
    b->sent_on = -1;
}

static void now_str(char *out, int cap) {
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d.%06d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds * 1000);
}

static long long now_ms(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return (long long)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000 - 11644473600000ULL);
}

static void connect_bulb(bulb_t *b) {
    b->retry_at = GetTickCount() + 5000;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a = { AF_INET, htons(10000) };
    inet_pton(AF_INET, b->ip, &a.sin_addr);
    connect(s, (struct sockaddr *)&a, sizeof(a));
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    struct timeval tv = { 2, 0 };
    if (select(0, NULL, &w, NULL, &tv) != 1) { closesocket(s); return; }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&nd, sizeof(nd));
    b->sock = s; b->rxn = 0;

    char ts[40], pw[128], msg[768];
    now_str(ts, sizeof(ts)); json_escape(b->pass, pw, sizeof(pw));
    long long ms = now_ms();
    snprintf(msg, sizeof(msg),
             "{\"service\":\"device\",\"method\":\"loginReq\",\"seq\":\"%09lld\",\"srcAddr\":\"%s\",\"deviceId\":\"%s\","
             "\"payload\":{\"userId\":\"%s\",\"password\":\"%s\",\"timestamp\":\"%s\",\"ascNumber\":1}}",
             ms % 1000000000LL, user_id, b->id, user_id, pw, ts);
    if (send_frame(b, 1, msg) < 0) close_bulb(b, "login send");
    b->last_ping = GetTickCount();
}

// Research: what the bulb says about effects (set from the AiDot app, which runs them through the cloud). The
// "attr" object of any message mentioning an effect is logged once per change, to learn how to start effects
// locally. [lights] probe=1 also asks the bulbs for all attributes every 5 s while they are switched off in the app.
static void log_effect_attrs(bulb_t *b, const char *js) {
    if (!cfg_geti("lights", "probe", 0)) return;
    if (!strstr(js, "Effect") && !strstr(js, "effect") && !strstr(js, "Script") && !strstr(js, "Mode")) return;
    const char *a = strstr(js, "\"attr\"");
    a = a ? strchr(a, '{') : NULL;
    if (!a) a = js;
    int depth = 0, in_str = 0, n = 0;
    for (const char *p = a; *p; p++, n++) {   // the attr object, braces balanced (strings respected)
        if (in_str) { if (*p == '\\' && p[1]) { p++; n++; } else if (*p == '"') in_str = 0; continue; }
        if (*p == '"') in_str = 1; else if (*p == '{') depth++; else if (*p == '}' && --depth == 0) { n++; break; }
    }
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)a[i]) * 16777619u;
    if (h == b->fx_hash) return;
    b->fx_hash = h;
    logf_("aidot: %s effect attributes (%d chars): %.*s", b->mac, n, n, a);
}

// Handles complete frames in b->rx.
static void process_rx(bulb_t *b) {
    while (b->rxn >= 8) {
        unsigned char *h = b->rx;
        if (h[0] != 0x1E || h[1] != 0xED) { close_bulb(b, "bad frame"); return; }
        int len = (h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7];
        if (len < 0 || len > (int)sizeof(b->rx) - 9) { close_bulb(b, "frame too large"); return; }
        if (b->rxn < 8 + len) return;
        static unsigned char body[32768];
        memcpy(body, h + 8, len);
        int n = aes_dec(b->bkey, body, len);
        memmove(b->rx, b->rx + 8 + len, b->rxn - 8 - len);
        b->rxn -= 8 + len;
        if (n < 0) {
            if (!b->warned) { b->warned = 1; logf_("aidot: %s reply cannot be decrypted — keys changed? sign in to AiDot again (Devices)", b->mac); }
            continue;
        }
        const char *js = (const char *)body;
        long long asc = json_int(js, "ascNumber");
        if (asc > 0) b->asc = asc + 1;
        log_effect_attrs(b, js);
        if (strstr(js, "\"loginResp\"")) {
            long long code = json_int(js, "code");
            if (code == 200) { b->online = 1; b->sent_on = -1; logf_("aidot: %s online at %s", b->mac, b->ip); }
            else { logf_("aidot: %s login refused (%lld)", b->mac, code); close_bulb(b, "login refused"); b->retry_at = GetTickCount() + 60000; return; }
        }
    }
}

static void send_attr(bulb_t *b, const char *method, const char *attr) {
    char pw[128];
    static char msg[20000];
    json_escape(b->pass, pw, sizeof(pw));
    b->seq++;
    if (b->simple)
        snprintf(msg, sizeof(msg),
                 "{\"method\":\"%s\",\"service\":\"device\",\"clientId\":\"ha-%s\",\"srcAddr\":\"0.%s\",\"seq\":\"ha93%05d\","
                 "\"payload\":{\"devId\":\"%s\",\"parentId\":\"%s\",\"userId\":\"%s\",\"password\":\"%s\",\"attr\":%s,\"channel\":\"tcp\",\"ascNumber\":%lld},"
                 "\"tst\":%lld,\"deviceId\":\"%s\"}",
                 method, user_id, user_id, b->seq % 100000, b->id, b->id, user_id, pw, attr, b->asc, now_ms(), b->id);
    else
        snprintf(msg, sizeof(msg),
                 "{\"method\":\"%s\",\"service\":\"device\",\"clientId\":\"\",\"srcAddr\":\"0.%s\",\"seq\":\"ha93%05d\","
                 "\"payload\":{\"devId\":\"\",\"parentId\":\"\",\"userId\":\"\",\"password\":\"\",\"attr\":%s,\"channel\":\"tcp\",\"ascNumber\":%lld},"
                 "\"tst\":%lld,\"deviceId\":\"%s\"}",
                 method, user_id, b->seq % 100000, attr, b->asc, now_ms(), b->id);
    if (send_frame(b, 1, msg) < 0) close_bulb(b, "send");
}

static void send_color(bulb_t *b, int on, int r, int g, int bl, int dim, int kelvin) {
    char attr[160];
    if (!on) snprintf(attr, sizeof(attr), "{\"OnOff\":0}");
    else if (kelvin) snprintf(attr, sizeof(attr), "{\"OnOff\":1,\"CCT\":%d,\"Dimming\":%d}", kelvin, dim);
    else {
        int rgbw = (int)(((unsigned)r << 24) | ((unsigned)g << 16) | ((unsigned)bl << 8));
        snprintf(attr, sizeof(attr), "{\"OnOff\":1,\"RGBW\":%d,\"Dimming\":%d}", rgbw, dim);
    }
    send_attr(b, "setDevAttrReq", attr);
}

// ---------------------------------------------------------------- discovery
static SOCKET disc;

static void discover_send(void) {
    char msg[512];
    long long ms = now_ms();
    snprintf(msg, sizeof(msg),
             "{\"protocolVer\":\"2.0.0\",\"service\":\"device\",\"method\":\"devDiscoveryReq\",\"seq\":\"%09lld\",\"srcAddr\":\"0.%s\","
             "\"tst\":%lld,\"payload\":{\"extends\":{},\"localCtrFlag\":1,\"timestamp\":\"%lld\"}}",
             (ms + 1) % 1000000000LL, user_id, ms, ms);
    unsigned char out[1024];
    int n = aes_enc(disc_key, msg, (int)strlen(msg), out, sizeof(out));
    // 255.255.255.255 leaves through one interface only, so also broadcast on each network
    // (bulbs may sit on the phone's network or on the PC's own hotspot)
    ULONG bc[8]; int nb = net_broadcasts(bc, 7);
    bc[nb++] = INADDR_BROADCAST;
    for (int i = 0; i < nb; i++) {
        struct sockaddr_in a = { AF_INET, htons(6666) };
        a.sin_addr.s_addr = bc[i];
        sendto(disc, (char *)out, n, 0, (struct sockaddr *)&a, sizeof(a));
    }
}

static void discover_recv(void) {
    unsigned char buf[2048];
    struct sockaddr_in from; int fl = sizeof(from);
    int n = recvfrom(disc, (char *)buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
    if (n <= 0 || (n = aes_dec(disc_key, buf, n)) < 0) return;
    char dev[64], mac[32], ip[32];
    const char *js = (const char *)buf, *end = js + n;
    json_str(js, end, "devId", dev, sizeof(dev));
    json_str(js, end, "mac", mac, sizeof(mac));
    for (char *c = mac; *c; c++) *c = (char)tolower((unsigned char)*c);
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    for (int i = 0; i < nbulbs; i++) {
        bulb_t *b = &bulbs[i];
        if (strcmp(b->id, dev) && strcmp(b->mac, mac)) continue;
        if (strcmp(b->ip, ip)) {
            logf_("aidot: %s found at %s", b->mac, ip);
            strcpy_s(b->ip, sizeof(b->ip), ip);
            if (b->sock != INVALID_SOCKET) close_bulb(b, "ip changed");
            b->retry_at = 0;
        }
    }
}

// ---------------------------------------------------------------- thread
static volatile LONG suspend_req;   // PC is going to sleep
static HANDLE suspend_done;

// [general] on_exit = off (default) | keep | restore — for bulbs only "off" does something
static int exit_turns_off(void) {
    const char *m = cfg_get("general", "on_exit", "off");
    return _stricmp(m, "keep") && _stricmp(m, "restore");
}

void lights_suspend(int s) {
    if (!th) return;
    ResetEvent(suspend_done);
    InterlockedExchange(&suspend_req, s);
    if (s) WaitForSingleObject(suspend_done, 3000);
}

// New key file (bulbs re-added in the AiDot app and aidot-setup.ps1 run again): start over with it.
static void reload_keys(void) {
    for (int i = 0; i < nbulbs; i++) {
        if (bulbs[i].sock != INVALID_SOCKET) close_bulb(&bulbs[i], "keys reloaded");
        if (bulbs[i].bkey) BCryptDestroyKey(bulbs[i].bkey);
    }
    AcquireSRWLockExclusive(&lk);
    load_keys();
    for (int i = 0; i < nbulbs; i++) bulbs[i].bkey = make_key(bulbs[i].key, 16);
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&keys_changed, 1);
}

static unsigned __stdcall thread_fn(void *p) {
    (void)p;
    DWORD last_disc = 0, last_tick = GetTickCount(), last_key_check = GetTickCount();
    while (run_thread) {
        DWORD now = GetTickCount();
        if (now - last_key_check > 5000) {
            last_key_check = now;
            FILETIME t = key_file_time();
            if (CompareFileTime(&t, &key_time) != 0) { logf_("aidot: key file changed, reloading"); reload_keys(); last_disc = 0; continue; }
        }
        float dt = (now - last_tick) / 1000.0f; last_tick = now;

        int missing = 0;
        for (int i = 0; i < nbulbs; i++) if (!bulbs[i].online) missing = 1;
        if (nbulbs && (now - last_disc > (DWORD)(missing ? 10000 : 120000) || !last_disc)) { discover_send(); last_disc = now; }

        float rate = cfg_getf("lights", "rate", 4);
        float smooth = cfg_getf("lights", "smooth", 0.35f);
        if (rate < 0.5f) rate = 0.5f; if (rate > 10) rate = 10;

        rgbf tg[MAX_BULBS]; int tk[MAX_BULBS], have, en;
        AcquireSRWLockShared(&lk); memcpy(tg, target, sizeof(tg)); memcpy(tk, target_k, sizeof(tk)); have = have_target; en = enabled; ReleaseSRWLockShared(&lk);

        // PC going to sleep: bulbs get the exit action (by default off) and wait for the resume
        if (suspend_req) {
            if (exit_turns_off())
                for (int i = 0; i < nbulbs; i++) {
                    bulb_t *b = &bulbs[i];
                    if (b->online && b->sent_on != 0) { send_color(b, 0, 0, 0, 0, 0, 0); b->sent_on = 0; }
                }
            SetEvent(suspend_done);
        }

        for (int i = 0; i < nbulbs && !suspend_req; i++) {
            bulb_t *b = &bulbs[i];
            if (b->sock == INVALID_SOCKET) { if (b->ip[0] && now >= b->retry_at) connect_bulb(b); continue; }
            if (!b->online || !have) continue;
            // switched off in the app: the bulb goes dark (once) and stays connected
            if (!en) {
                if (b->sent_on != 0 && !cfg_geti("lights", "probe", 0)) { send_color(b, 0, 0, 0, 0, 0, 0); b->sent_on = 0; b->last_send = now; }
                // research ([lights] probe=1): leave the bulb to the AiDot app, read its attributes, replay attr_send
                if (cfg_geti("lights", "probe", 0)) {
                    b->sent_on = -1;
                    if (now - b->last_send > 5000) { send_attr(b, "getDevAttrReq", "{}"); b->last_send = now; }
                    // %APPDATA%\haku-control\aidot-send.json: an attr object, sent once per change of the file
                    static char as[16384]; static FILETIME as_time; static DWORD as_check;
                    if (now - as_check > 2000) {
                        as_check = now;
                        wchar_t fp[MAX_PATH]; app_data_path(L"aidot-send.json", fp);
                        WIN32_FILE_ATTRIBUTE_DATA fa;
                        if (GetFileAttributesExW(fp, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &as_time)) {
                            as_time = fa.ftLastWriteTime; as[0] = 0;
                            FILE *f = _wfopen(fp, L"rb");
                            if (f) { size_t n = fread(as, 1, sizeof(as) - 1, f); as[n] = 0; fclose(f); }
                            for (int k = 0; k < nbulbs; k++) bulbs[k].sent_probe[0] = 0;
                        }
                    }
                    if (as[0] == '{' && !b->sent_probe[0]) {
                        strcpy_s(b->sent_probe, sizeof(b->sent_probe), "sent");
                        logf_("aidot: %s research: sending %.300s", b->mac, as);
                        send_attr(b, "setDevAttrReq", as);
                    }
                }
                continue;
            }

            // smoothing toward the effect colour
            float k = smooth > 0.01f ? 1 - expf(-dt / smooth) : 1;
            float t3[3] = { tg[i].r, tg[i].g, tg[i].b };
            for (int c = 0; c < 3; c++) b->cur[c] += (t3[c] - b->cur[c]) * k;

            if (now - b->last_send < (DWORD)(1000 / rate)) continue;
            // brightness goes to Dimming, hue/saturation to RGB, so dark colours don't wash out
            float mx = max(b->cur[0], max(b->cur[1], b->cur[2]));
            int on = mx > 0.02f, dim = (int)(mx * 100 + 0.5f), rgb[3] = { 0, 0, 0 };
            if (dim < 1) dim = 1;
            if (on) for (int c = 0; c < 3; c++) rgb[c] = (int)(b->cur[c] / mx * 255 + 0.5f);
            int kel = tk[i];
            int diff = on != b->sent_on || abs(dim - b->sent_dim) > 1 || kel != b->sent_k;
            for (int c = 0; c < 3 && on && !kel; c++) diff |= abs(rgb[c] - b->sent_rgb[c]) > 3;
            if (!diff) continue;
            send_color(b, on, rgb[0], rgb[1], rgb[2], dim, kel);
            b->sent_on = on; b->sent_dim = dim; b->sent_k = kel; memcpy(b->sent_rgb, rgb, sizeof(rgb));
            b->last_send = now;
        }

        // wait for network input (max 50 ms)
        fd_set r; FD_ZERO(&r); FD_SET(disc, &r);
        for (int i = 0; i < nbulbs; i++) if (bulbs[i].sock != INVALID_SOCKET) FD_SET(bulbs[i].sock, &r);
        struct timeval tv = { 0, 50000 };
        if (select(0, &r, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(disc, &r)) discover_recv();
            for (int i = 0; i < nbulbs; i++) {
                bulb_t *b = &bulbs[i];
                if (b->sock == INVALID_SOCKET || !FD_ISSET(b->sock, &r)) continue;
                int n = recv(b->sock, (char *)b->rx + b->rxn, (int)sizeof(b->rx) - b->rxn, 0);
                if (n <= 0) { close_bulb(b, "connection closed"); continue; }
                b->rxn += n;
                process_rx(b);
            }
        }
        // keep-alive
        for (int i = 0; i < nbulbs; i++) {
            bulb_t *b = &bulbs[i];
            if (b->online && GetTickCount() - b->last_ping > 30000) {
                b->last_ping = GetTickCount();
                if (send_frame(b, 2, "{\"service\":\"test\",\"method\":\"pingreq\",\"seq\":\"123456\",\"srcAddr\":\"123456\",\"payload\":{}}") < 0)
                    close_bulb(b, "ping");
            }
        }
    }
    // the app exits / PC shuts down: without the PC the bulbs cannot be controlled, so by default they go dark
    if (exit_turns_off())
        for (int i = 0; i < nbulbs; i++) if (bulbs[i].online && bulbs[i].sent_on != 0) send_color(&bulbs[i], 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < nbulbs; i++) if (bulbs[i].sock != INVALID_SOCKET) close_bulb(&bulbs[i], "exit");
    return 0;
}

// ---------------------------------------------------------------- API
int lights_start(void) {
    if (th) return nbulbs;
    load_keys();   // no file yet is fine: the thread picks it up when it appears
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    disc_key = make_key("T54uednca587", 32);
    for (int i = 0; i < nbulbs; i++) bulbs[i].bkey = make_key(bulbs[i].key, 16);
    disc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL bc = TRUE; setsockopt(disc, SOL_SOCKET, SO_BROADCAST, (char *)&bc, sizeof(bc));
    struct sockaddr_in a = { AF_INET, 0 }; bind(disc, (struct sockaddr *)&a, sizeof(a));
    run_thread = 1;
    suspend_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, thread_fn, NULL, 0, NULL);
    return nbulbs;
}

int lights_count(void) { return nbulbs; }
int lights_changed(void) { return InterlockedExchange(&keys_changed, 0); }
int lights_is_online(int i) { return i >= 0 && i < nbulbs && bulbs[i].online; }
const char *lights_ip(int i) { return i >= 0 && i < nbulbs ? bulbs[i].ip : ""; }

const char *lights_name(int i) { return i >= 0 && i < nbulbs ? bulbs[i].name : ""; }

int lights_online(void) {
    int n = 0;
    for (int i = 0; i < nbulbs; i++) n += bulbs[i].online;
    return n;
}

void lights_submit(const rgbf *c, const int *kelvin, int n, int enable) {
    AcquireSRWLockExclusive(&lk);
    if (n > MAX_BULBS) n = MAX_BULBS;
    memcpy(target, c, n * sizeof(rgbf));
    memcpy(target_k, kelvin, n * sizeof(int));
    have_target = 1;
    enabled = enable;
    ReleaseSRWLockExclusive(&lk);
}

void lights_stop(void) {
    if (!th) return;
    run_thread = 0;
    WaitForSingleObject(th, 3000);
    CloseHandle(th); th = NULL;
    closesocket(disc);
    WSACleanup();
}
