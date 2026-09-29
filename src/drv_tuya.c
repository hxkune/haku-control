// SPDX-License-Identifier: GPL-3.0-only
// Tuya / Smart Life lights over the local protocol (TCP 6668), versions 3.3, 3.4 and 3.5, one colour per light.
// Keys and DP numbers come from %APPDATA%\haku-control\tuya.json (Devices -> Sign-ins: Tuya cloud project);
// addresses and protocol versions from the devices' own UDP broadcasts (6666 / 6667 / 7000). A [dev.N] entry's
// host is the device id. Framing, session keys and encryption follow the tinytuya project:
//   3.3  000055AA frames, AES-128-ECB(local key), "3.3"+12 zero bytes in front of the ciphertext, CRC32
//   3.4  session key = AES-ECB(local key, nonceA ^ nonceB); "3.4"+12 zeros inside the ciphertext; HMAC-SHA256
//   3.5  00006699 frames, AES-128-GCM (12-byte IV, 16-byte tag, header as AAD); session key via GCM
#include "devices.h"
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <process.h>
#include <stdlib.h>
#include <time.h>

#define TUYA_PORT 6668
#define MAX_TUYA 32

enum { CMD_SESS_START = 3, CMD_SESS_RESP = 4, CMD_SESS_FINISH = 5, CMD_CONTROL = 7, CMD_STATUS = 8, CMD_HEART_BEAT = 9,
       CMD_DP_QUERY = 10, CMD_CONTROL_NEW = 13, CMD_DP_QUERY_NEW = 16 };

typedef struct {
    char id[40], name[64], key[20], product[64];
    int  dp_sw, dp_mode, dp_bright, dp_col, fmt_b;   // fmt_b: colour as hhhhssssvvvv 0..1000, else rrggbb0hhhssvv
    char ip[48], ver[8];                               // from the broadcasts
    DWORD seen;
} tkey_t;

static tkey_t  keys[MAX_TUYA];
static int     nkeys;
static FILETIME key_time;
static SRWLOCK klk = SRWLOCK_INIT;
static volatile LONG listening;

// ---------------------------------------------------------------- crypto (CNG)
static BCRYPT_ALG_HANDLE alg_ecb, alg_gcm, alg_hmac, alg_md5;
static INIT_ONCE crypto_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK crypto_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    BCryptOpenAlgorithmProvider(&alg_ecb, BCRYPT_AES_ALGORITHM, NULL, 0);
    BCryptSetProperty(alg_ecb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    BCryptOpenAlgorithmProvider(&alg_gcm, BCRYPT_AES_ALGORITHM, NULL, 0);
    BCryptSetProperty(alg_gcm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    BCryptOpenAlgorithmProvider(&alg_hmac, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    BCryptOpenAlgorithmProvider(&alg_md5, BCRYPT_MD5_ALGORITHM, NULL, 0);
    return TRUE;
}
static void crypto(void) { InitOnceExecuteOnce(&crypto_once, crypto_init, NULL, NULL); }

// AES-128-ECB; pad: PKCS7 on encrypt / strip it on decrypt. Returns the output length or -1.
static int ecb(const BYTE *key, const BYTE *in, int len, BYTE *out, int cap, int enc, int pad) {
    BCRYPT_KEY_HANDLE k; ULONG n = 0;
    if (BCryptGenerateSymmetricKey(alg_ecb, &k, NULL, 0, (PUCHAR)key, 16, 0)) return -1;
    NTSTATUS st = enc ? BCryptEncrypt(k, (PUCHAR)in, len, NULL, NULL, 0, out, cap, &n, pad ? BCRYPT_BLOCK_PADDING : 0)
                      : BCryptDecrypt(k, (PUCHAR)in, len, NULL, NULL, 0, out, cap, &n, pad ? BCRYPT_BLOCK_PADDING : 0);
    BCryptDestroyKey(k);
    return st ? -1 : (int)n;
}
// AES-128-GCM with a 12-byte IV and 16-byte tag. Returns 1 on success (decrypt: tag verified).
static int gcm(const BYTE *key, const BYTE *iv, const BYTE *aad, int aadlen, const BYTE *in, int len, BYTE *out, BYTE *tag, int enc) {
    BCRYPT_KEY_HANDLE k; ULONG n = 0;
    if (BCryptGenerateSymmetricKey(alg_gcm, &k, NULL, 0, (PUCHAR)key, 16, 0)) return 0;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai; BCRYPT_INIT_AUTH_MODE_INFO(ai);
    ai.pbNonce = (PUCHAR)iv; ai.cbNonce = 12;
    ai.pbAuthData = (PUCHAR)aad; ai.cbAuthData = aadlen;
    ai.pbTag = tag; ai.cbTag = 16;
    NTSTATUS st = enc ? BCryptEncrypt(k, (PUCHAR)in, len, &ai, NULL, 0, out, len, &n, 0)
                      : BCryptDecrypt(k, (PUCHAR)in, len, &ai, NULL, 0, out, len, &n, 0);
    BCryptDestroyKey(k);
    return st == 0;
}
static void hmac256(const BYTE *key, int klen, const BYTE *d, int len, BYTE *out) {
    BCryptHash(alg_hmac, (PUCHAR)key, klen, (PUCHAR)d, len, out, 32);
}
static unsigned crc32(const BYTE *d, int n) {
    static unsigned t[256];
    if (!t[1]) for (unsigned i = 0; i < 256; i++) { unsigned c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    unsigned c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = t[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void put32(BYTE *p, unsigned v) { p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16); p[2] = (BYTE)(v >> 8); p[3] = (BYTE)v; }
static unsigned get32(const BYTE *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

// ---------------------------------------------------------------- frames
// 55AA: prefix seq cmd len | payload | crc32 or hmac | suffix. len counts payload + end.
static int frame_55aa(BYTE *out, unsigned seq, unsigned cmd, const BYTE *pl, int plen, const BYTE *hkey) {
    put32(out, 0x000055AA); put32(out + 4, seq); put32(out + 8, cmd);
    int end = hkey ? 36 : 8;
    put32(out + 12, plen + end);
    memcpy(out + 16, pl, plen);
    int n = 16 + plen;
    if (hkey) { hmac256(hkey, 16, out, n, out + n); n += 32; }
    else { put32(out + n, crc32(out, n)); n += 4; }
    put32(out + n, 0x0000AA55);
    return n + 4;
}
// 6699: prefix 0000 seq cmd len | iv | GCM(payload) | tag | suffix; AAD = the 14 header bytes after the prefix.
static int frame_6699(BYTE *out, unsigned seq, unsigned cmd, const BYTE *pl, int plen, const BYTE *key) {
    put32(out, 0x00006699); out[4] = out[5] = 0;
    put32(out + 6, seq); put32(out + 10, cmd); put32(out + 14, 12 + plen + 16);
    BYTE *iv = out + 18;
    BCryptGenRandom(NULL, iv, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!gcm(key, iv, out + 4, 14, pl, plen, out + 30, out + 30 + plen, 1)) return -1;
    int n = 30 + plen + 16;
    put32(out + n, 0x00009966);
    return n + 4;
}

// ---------------------------------------------------------------- tuya.json
static void key_file(wchar_t *p) { app_data_path(L"tuya.json", p); }

static void load_keys(void) {
    wchar_t p[MAX_PATH]; key_file(p);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fa) || !CompareFileTime(&fa.ftLastWriteTime, &key_time)) return;
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    static char js[256 * 1024]; size_t n = fread(js, 1, sizeof(js) - 1, f); js[n] = 0; fclose(f);
    tkey_t nk[MAX_TUYA]; int nn = 0;
    const char *a = strstr(js, "\"devices\"");
    for (const char *o = a ? strchr(a, '{') : NULL; o && nn < MAX_TUYA; o = strchr(o + 1, '{')) {
        const char *e = strchr(o, '}'); if (!e) break;
        char one[1024]; int l = (int)(e - o + 1); if (l > (int)sizeof(one) - 1) l = sizeof(one) - 1;
        memcpy(one, o, l); one[l] = 0;
        tkey_t *k = &nk[nn]; memset(k, 0, sizeof(*k));
        json_get_str(one, "id", k->id, sizeof(k->id));
        json_get_str(one, "key", k->key, sizeof(k->key));
        json_get_str(one, "name", k->name, sizeof(k->name));
        json_get_str(one, "product", k->product, sizeof(k->product));
        json_get_str(one, "version", k->ver, sizeof(k->ver));
        k->dp_sw = (int)json_get_num(one, "dp_switch", 20); k->dp_mode = (int)json_get_num(one, "dp_mode", 21);
        k->dp_bright = (int)json_get_num(one, "dp_bright", 22); k->dp_col = (int)json_get_num(one, "dp_colour", 24);
        k->fmt_b = (int)json_get_num(one, "colour_v2", k->dp_col != 5);
        if (k->id[0] && strlen(k->key) == 16) nn++;
        o = e;
    }
    AcquireSRWLockExclusive(&klk);
    for (int i = 0; i < nn; i++)   // keep addresses already learnt
        for (int j = 0; j < nkeys; j++)
            if (!strcmp(nk[i].id, keys[j].id) && keys[j].ip[0]) { strcpy_s(nk[i].ip, sizeof(nk[i].ip), keys[j].ip); if (keys[j].ver[0]) strcpy_s(nk[i].ver, sizeof(nk[i].ver), keys[j].ver); nk[i].seen = keys[j].seen; }
    memcpy(keys, nk, sizeof(nk)); nkeys = nn;
    key_time = fa.ftLastWriteTime;
    ReleaseSRWLockExclusive(&klk);
    logf_("tuya: %d light(s) in the key file", nn);
}

static int find_key(const char *id, tkey_t *out) {
    load_keys();
    AcquireSRWLockShared(&klk);
    int ok = 0;
    for (int i = 0; i < nkeys; i++) if (!strcmp(keys[i].id, id)) { *out = keys[i]; ok = 1; }
    ReleaseSRWLockShared(&klk);
    return ok;
}

// ---------------------------------------------------------------- broadcasts: where the devices are
// 6666: plain JSON; 6667: 55AA + AES-ECB(md5("yGAdlopoPVldABfn")); 7000: 6699 + GCM with the same key.
static void heard(const char *js, const char *from) {
    char id[40] = "", ip[48] = "", ver[8] = "";
    json_get_str(js, "gwId", id, sizeof(id));
    json_get_str(js, "ip", ip, sizeof(ip));
    json_get_str(js, "version", ver, sizeof(ver));
    if (!id[0]) return;
    if (!ip[0]) strcpy_s(ip, sizeof(ip), from);
    AcquireSRWLockExclusive(&klk);
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i].id, id)) {
            strcpy_s(keys[i].ip, sizeof(keys[i].ip), ip);
            if (ver[0]) strcpy_s(keys[i].ver, sizeof(keys[i].ver), ver);
            keys[i].seen = GetTickCount();
        }
    ReleaseSRWLockExclusive(&klk);
}

static void decode_broadcast(BYTE *d, int n, const char *from) {
    static BYTE udpkey[16]; static int have;
    if (!have) { BCryptHash(alg_md5, NULL, 0, (PUCHAR)"yGAdlopoPVldABfn", 16, udpkey, 16); have = 1; }
    BYTE out[1024]; char js[1024];
    if (n > 0 && d[0] == '{') { d[n < 1023 ? n : 1023] = 0; heard((char *)d, from); return; }
    if (n >= 28 && get32(d) == 0x000055AA) {
        int len = (int)get32(d + 12);
        if (16 + len > n || len < 12) return;
        BYTE *pl = d + 20; int pn = len - 4 - 8;   // after the return code, before crc + suffix
        if (pn > 0 && pl[0] == '{') { memcpy(js, pl, pn < 1023 ? pn : 1023); js[pn < 1023 ? pn : 1023] = 0; heard(js, from); return; }
        int m = pn > 0 && pn % 16 == 0 ? ecb(udpkey, pl, pn, out, sizeof(out) - 1, 0, 1) : -1;
        if (m > 0) { out[m] = 0; heard((char *)out, from); }
        return;
    }
    if (n >= 50 && get32(d) == 0x00006699) {
        int len = (int)get32(d + 14);
        if (18 + len + 4 > n || len < 28 || len - 28 > (int)sizeof(out) - 1) return;
        int cn = len - 28;
        if (!gcm(udpkey, d + 18, d + 4, 14, d + 30, cn, out, d + 30 + cn, 0)) return;
        out[cn] = 0;
        char *j = (char *)out;
        if (j[0] != '{' && cn > 4 && j[4] == '{') j += 4;   // return code
        heard(j, from);
    }
}

static unsigned __stdcall listen_fn(void *p) {
    (void)p;
    net_init(); crypto();
    SOCKET s[3]; int ports[3] = { 6666, 6667, 7000 }, ns = 0;
    for (int i = 0; i < 3; i++) { SOCKET k = udp_socket(ports[i], 1); if (k != INVALID_SOCKET) s[ns++] = k; }
    logf_("tuya: listening for devices (%d ports)", ns);
    for (;;) {
        fd_set r; FD_ZERO(&r);
        for (int i = 0; i < ns; i++) FD_SET(s[i], &r);
        struct timeval tv = { 5, 0 };
        if (select(0, &r, NULL, NULL, &tv) <= 0) { load_keys(); continue; }
        for (int i = 0; i < ns; i++) {
            if (!FD_ISSET(s[i], &r)) continue;
            BYTE buf[2048]; struct sockaddr_in a; int al = sizeof(a);
            int n = recvfrom(s[i], (char *)buf, sizeof(buf) - 1, 0, (struct sockaddr *)&a, &al);
            char from[48]; inet_ntop(AF_INET, &a.sin_addr, from, sizeof(from));
            if (n > 0) decode_broadcast(buf, n, from);
        }
    }
}

static void listen_start(void) {
    if (InterlockedCompareExchange(&listening, 1, 0)) return;
    crypto(); load_keys();
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, listen_fn, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

// ---------------------------------------------------------------- one connection
typedef struct {
    tkey_t k;
    int    v;                 // 33, 34, 35
    BYTE   real[16], sess[16];
    unsigned seq;
    DWORD  last_hb;
    char   saved[256];        // the device's own DPs, for LEAVE_RESTORE
    int    was_on;
} tconn_t;

static int send_msg(SOCKET s, tconn_t *c, unsigned cmd, const char *json, int plainlen) {
    static BYTE pt[4096], enc[4096], fr[4200];
    int plen = plainlen >= 0 ? plainlen : (int)strlen(json), n;
    int header = cmd != CMD_DP_QUERY && cmd != CMD_DP_QUERY_NEW && cmd != CMD_HEART_BEAT &&
                 cmd != CMD_SESS_START && cmd != CMD_SESS_FINISH;
    const char *vh = c->v == 35 ? "3.5" : c->v == 34 ? "3.4" : "3.3";
    if (plen > 3000) return 0;
    if (c->v == 35) {
        int m = 0;
        if (header) { memcpy(pt, vh, 3); memset(pt + 3, 0, 12); m = 15; }
        memcpy(pt + m, json, plen);
        n = frame_6699(fr, ++c->seq, cmd, pt, m + plen, cmd == CMD_SESS_START || cmd == CMD_SESS_FINISH ? c->real : c->sess);
    } else if (c->v == 34) {
        int m = 0;
        if (header) { memcpy(pt, vh, 3); memset(pt + 3, 0, 12); m = 15; }
        memcpy(pt + m, json, plen);
        const BYTE *key = cmd == CMD_SESS_START || cmd == CMD_SESS_FINISH ? c->real : c->sess;
        int e = ecb(key, pt, m + plen, enc, sizeof(enc), 1, 1);
        if (e < 0) return 0;
        n = frame_55aa(fr, ++c->seq, cmd, enc, e, key);
    } else {
        int e = ecb(c->real, (const BYTE *)json, plen, enc + 15, sizeof(enc) - 15, 1, 1);
        if (e < 0) return 0;
        int m = 0;
        if (header) { memcpy(enc, vh, 3); memset(enc + 3, 0, 12); m = 15; }
        else memmove(enc, enc + 15, e);
        n = frame_55aa(fr, ++c->seq, cmd, enc, m + e, NULL);
    }
    return n > 0 && tcp_send_all(s, fr, n);
}

// Reads one message (waits up to ms); decrypts it into out (NUL-terminated). Returns its command, -1 on failure.
static int recv_msg(SOCKET s, tconn_t *c, BYTE *out, int cap, int *olen, int ms) {
    DWORD to = ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    BYTE h[18];
    if (!tcp_recv_all(s, h, 4)) return -1;
    static BYTE body[8192];
    int sess = c->v >= 34 && !(c->sess[0] == 0 && c->sess[15] == 0 && !memcmp(c->sess, c->sess + 1, 15));
    const BYTE *key = sess ? c->sess : c->real;
    *olen = 0;
    if (get32(h) == 0x00006699) {
        if (!tcp_recv_all(s, h + 4, 14)) return -1;
        int len = (int)get32(h + 14), cmd = (int)get32(h + 10);
        if (len < 28 || len + 4 > (int)sizeof(body)) return -1;
        if (!tcp_recv_all(s, body, len + 4)) return -1;
        int cn = len - 28;
        if (cn > cap - 1 || !gcm(key, body, h + 4, 14, body + 12, cn, out, body + 12 + cn, 0)) return -1;
        int off = cn >= 4 ? 4 : 0;   // return code
        memmove(out, out + off, cn - off); cn -= off;
        if (cn >= 15 && !memcmp(out, "3.5", 3)) { memmove(out, out + 15, cn - 15); cn -= 15; }
        out[cn] = 0; *olen = cn;
        return cmd;
    }
    if (get32(h) != 0x000055AA || !tcp_recv_all(s, h + 4, 12)) return -1;
    int len = (int)get32(h + 12), cmd = (int)get32(h + 8), end = c->v >= 34 ? 36 : 8;
    if (len < end || len > (int)sizeof(body)) return -1;
    if (!tcp_recv_all(s, body, len)) return -1;
    BYTE *pl = body; int pn = len - end;
    if (pn > 4 && pl[0] == 0 && pl[1] == 0 && pl[2] == 0 && pl[3] < 16) { pl += 4; pn -= 4; }   // return code
    if (pn >= 15 && pl[0] == '3' && pl[1] == '.' && c->v < 34) { pl += 15; pn -= 15; }
    if (pn <= 0) { out[0] = 0; return cmd; }
    if (pl[0] == '{') { int m = pn < cap - 1 ? pn : cap - 1; memcpy(out, pl, m); out[m] = 0; *olen = m; return cmd; }
    int m = pn % 16 == 0 && pn < cap ? ecb(c->v >= 34 ? key : c->real, pl, pn, out, cap - 1, 0, 1) : -1;
    if (m < 0) return -1;
    if (m >= 15 && out[0] == '3' && out[1] == '.') { memmove(out, out + 15, m - 15); m -= 15; }
    out[m] = 0; *olen = m;
    return cmd;
}

// 3.4 / 3.5: agree on a session key.
static int negotiate(SOCKET s, tconn_t *c) {
    BYTE ln[16], rn[16], mac[32], buf[256]; int n;
    memset(c->sess, 0, 16);
    BCryptGenRandom(NULL, ln, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!send_msg(s, c, CMD_SESS_START, (const char *)ln, 16)) return 0;
    int cmd = recv_msg(s, c, buf, sizeof(buf), &n, 3000);
    if (cmd != CMD_SESS_RESP || n < 48) return 0;
    hmac256(c->real, 16, ln, 16, mac);
    if (memcmp(mac, buf + 16, 32)) { logf_("tuya: %s answered the handshake with a wrong key", c->k.id); return 0; }
    memcpy(rn, buf, 16);
    hmac256(c->real, 16, rn, 16, mac);
    if (!send_msg(s, c, CMD_SESS_FINISH, (const char *)mac, 32)) return 0;
    BYTE x[16]; for (int i = 0; i < 16; i++) x[i] = ln[i] ^ rn[i];
    if (c->v == 34) return ecb(c->real, x, 16, c->sess, 16, 1, 0) == 16;
    BYTE tag[16];
    return gcm(c->real, ln, NULL, 0, x, 16, c->sess, tag, 1);
}

static void control_json(tconn_t *c, const char *dps, char *out, int cap) {
    long long t = (long long)time(NULL);
    if (c->v >= 34) snprintf(out, cap, "{\"protocol\":5,\"t\":%lld,\"data\":{\"dps\":%s}}", t, dps);
    else snprintf(out, cap, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}", c->k.id, c->k.id, t, dps);
}

static int set_dps(ext_dev *d, const char *dps) {
    tconn_t *c = d->priv; char js[512];
    control_json(c, dps, js, sizeof(js));
    return send_msg(d->sock, c, c->v >= 34 ? CMD_CONTROL_NEW : CMD_CONTROL, js, -1);
}

static void drain(ext_dev *d) {   // acknowledgements and status pushes: read and drop
    u_long avail = 0; BYTE b[1024];
    while (ioctlsocket(d->sock, FIONREAD, &avail) == 0 && avail > 0 && recv(d->sock, (char *)b, avail < sizeof(b) ? (int)avail : sizeof(b), 0) > 0) {}
}

// ---------------------------------------------------------------- driver
static int tuya_open(ext_dev *d) {
    crypto(); listen_start();
    tkey_t k;
    if (!find_key(d->host, &k)) return 0;
    for (int w = 0; !k.ip[0] && w < 60; w++) { Sleep(100); find_key(d->host, &k); }   // first broadcast (every ~5 s)
    if (!k.ip[0]) return 0;
    tconn_t *c = calloc(1, sizeof(tconn_t));
    if (!c) return 0;
    c->k = k; memcpy(c->real, k.key, 16);
    c->v = !strcmp(k.ver, "3.5") ? 35 : !strcmp(k.ver, "3.4") ? 34 : 33;
    SOCKET s = tcp_connect(k.ip, TUYA_PORT, 1500);
    if (s == INVALID_SOCKET) { free(c); return 0; }
    if (c->v >= 34 && !negotiate(s, c)) { closesocket(s); free(c); return 0; }
    // the device's state, to give it back on exit
    BYTE st[2048]; int n;
    if (c->v >= 34) send_msg(s, c, CMD_DP_QUERY_NEW, "{}", -1);
    else { char q[256]; snprintf(q, sizeof(q), "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\"}", k.id, k.id, k.id, (long long)time(NULL)); send_msg(s, c, CMD_DP_QUERY, q, -1); }
    for (int i = 0; i < 3; i++) {
        int cmd = recv_msg(s, c, st, sizeof(st), &n, 1500);
        if (cmd < 0) break;
        const char *dps = n ? json_get_obj((char *)st, "dps") : NULL;
        if (dps) {
            const char *e = strchr(dps, '}');
            if (e && e - dps < (int)sizeof(c->saved) - 1) { memcpy(c->saved, dps, e - dps + 1); c->saved[e - dps + 1] = 0; }
            char sw[8]; snprintf(sw, sizeof(sw), "%d", k.dp_sw);
            c->was_on = (int)json_get_num(c->saved, sw, 1);
            break;
        }
    }
    DWORD to = 0; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    d->sock = s; d->priv = c; d->nleds = 1; c->last_hb = GetTickCount();
    snprintf(d->info, sizeof(d->info), "Tuya · %s · v%s · %s", k.product[0] ? k.product : k.name, k.ver[0] ? k.ver : "3.3", k.ip);
    return 1;
}

static int tuya_send(ext_dev *d, const rgbf *col, int n) {
    if (n < 1) return 1;
    tconn_t *c = d->priv;
    drain(d);
    float r = clampf(col[0].r, 0, 1), g = clampf(col[0].g, 0, 1), b = clampf(col[0].b, 0, 1);
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), dl = mx - mn, h = 0;
    char dps[160];
    if (mx < 0.01f) snprintf(dps, sizeof(dps), "{\"%d\":false}", c->k.dp_sw);
    else {
        if (dl > 0) h = mx == r ? fmodf((g - b) / dl, 6) : mx == g ? (b - r) / dl + 2 : (r - g) / dl + 4;
        h *= 60; if (h < 0) h += 360;
        float sat = dl / mx;
        char colour[20];
        if (c->k.fmt_b) snprintf(colour, sizeof(colour), "%04x%04x%04x", (int)(h + .5f) % 360, (int)(sat * 1000 + .5f), max(10, (int)(mx * 1000 + .5f)));
        else snprintf(colour, sizeof(colour), "%02x%02x%02x%04x%02x%02x", (int)(r * 255 + .5f), (int)(g * 255 + .5f), (int)(b * 255 + .5f),
                      (int)(h + .5f) % 360, (int)(sat * 255 + .5f), max(1, (int)(mx * 255 + .5f)));
        snprintf(dps, sizeof(dps), "{\"%d\":true,\"%d\":\"colour\",\"%d\":\"%s\"}", c->k.dp_sw, c->k.dp_mode, c->k.dp_col, colour);
    }
    if (!set_dps(d, dps)) return 0;
    if (GetTickCount() - c->last_hb > 10000) { send_msg(d->sock, c, CMD_HEART_BEAT, "{}", -1); c->last_hb = GetTickCount(); }
    return 1;
}

static void tuya_leave(ext_dev *d, int how) {
    tconn_t *c = d->priv;
    char dps[300];
    if (how == LEAVE_OFF) snprintf(dps, sizeof(dps), "{\"%d\":false}", c->k.dp_sw);
    else if (how == LEAVE_RESTORE && c->saved[0]) snprintf(dps, sizeof(dps), "%s", c->saved);
    else return;
    set_dps(d, dps);
    Sleep(50);
}

static void tuya_close(ext_dev *d) {
    if (d->sock != INVALID_SOCKET) closesocket(d->sock);
    d->sock = INVALID_SOCKET;
    free(d->priv); d->priv = NULL;
}

// Lights from the key file; "found" once their broadcast was heard.
static void tuya_discover(int ms, void (*found)(const disc_t *)) {
    crypto(); load_keys();
    if (!nkeys) return;   // not signed in: nothing to look for
    listen_start();
    Sleep(ms < 6000 ? ms : 6000);
    load_keys();
    AcquireSRWLockShared(&klk);
    tkey_t k[MAX_TUYA]; int nk = nkeys; memcpy(k, keys, sizeof(k));
    ReleaseSRWLockShared(&klk);
    for (int i = 0; i < nk; i++) {
        if (!k[i].ip[0]) continue;
        disc_t x = { "tuya" };
        strcpy_s(x.host, sizeof(x.host), k[i].id);
        x.sub = -1; x.nleds = 1;
        strcpy_s(x.name, sizeof(x.name), k[i].name[0] ? k[i].name : "Tuya light");
        snprintf(x.info, sizeof(x.info), "Tuya · %s · v%s · %s", k[i].product[0] ? k[i].product : "light", k[i].ver[0] ? k[i].ver : "3.3", k[i].ip);
        found(&x);
    }
}

const ext_driver drv_tuya = { "tuya", "Tuya / Smart Life", 6, 0, tuya_open, tuya_send, tuya_leave, tuya_close, tuya_discover };
