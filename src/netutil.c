// SPDX-License-Identifier: GPL-3.0-only
// Small network helpers shared by the LAN light drivers: blocking TCP with timeouts, tiny HTTP/1.0,
// UDP sockets, and a forgiving JSON field reader (flat lookups by key, good enough for device APIs).
#include "common.h"
#include "devices.h"
#include <ctype.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "../res/version.h"
#include <ws2tcpip.h>
#include <stdlib.h>

static volatile LONG wsa_ready;

void net_init(void) {
    if (InterlockedCompareExchange(&wsa_ready, 1, 0) == 0) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
}

int net_addr(const char *host, int port, struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET; a->sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &a->sin_addr) == 1) return 1;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    a->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 1;
}

int host_port(const char *host, int def_port, char *h, int cap) {
    strcpy_s(h, cap, host);
    char *c = strrchr(h, ':');
    if (c && strchr(h, ':') == c) { *c = 0; int p = atoi(c + 1); return p > 0 && p < 65536 ? p : def_port; }
    return def_port;
}

SOCKET tcp_connect(const char *host, int port, int timeout_ms) {
    net_init();
    struct sockaddr_in a;
    if (!net_addr(host, port, &a)) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&a, sizeof(a));
    fd_set w, e; FD_ZERO(&w); FD_SET(s, &w); FD_ZERO(&e); FD_SET(s, &e);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (select(0, NULL, &w, &e, &tv) != 1 || FD_ISSET(s, &e)) { closesocket(s); return INVALID_SOCKET; }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof(to));
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
    return s;
}

int tcp_send_all(SOCKET s, const void *data, int len) {
    const char *p = (const char *)data;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return 0;
        p += n; len -= n;
    }
    return 1;
}

int tcp_recv_all(SOCKET s, void *data, int len) {
    char *p = (char *)data;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return 0;
        p += n; len -= n;
    }
    return 1;
}

int http_request(const char *host, int port, const char *method, const char *path, const char *body, char *buf, int cap) {
    buf[0] = 0;
    SOCKET s = tcp_connect(host, port, 1500);
    if (s == INVALID_SOCKET) return 0;
    char req[1024];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n", method, path, host, bl);
    int ok = tcp_send_all(s, req, n) && (!bl || tcp_send_all(s, body, bl));
    int got = 0;
    while (ok && got < cap - 1) {
        int r = recv(s, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += r;
    }
    closesocket(s);
    buf[got] = 0;
    int status = 0;
    if (sscanf_s(buf, "HTTP/%*d.%*d %d", &status) != 1) return 0;
    char *b = strstr(buf, "\r\n\r\n");
    if (b) memmove(buf, b + 4, strlen(b + 4) + 1); else buf[0] = 0;
    return status;
}

// HTTP/1.1 for small devices' own web servers: the request and its body in one packet, and the answer read up to
// the Content-Length it names (or to the end of a chunked one), since such servers may keep the connection open.
// Sent over HTTP/1.0 in two packets (http_request), PUTs to Elgato Key Lights MK.2 (firmware 1.0.4) went
// unanswered. 2 s for an answer. Returns the status (0: no answer); why (may be NULL) says what went wrong.
int http_call(const char *ip, int port, const char *method, const char *path, const char *body,
                    char *buf, int cap, char *why, int whycap) {
    buf[0] = 0; if (why) why[0] = 0;
    DWORD t0 = GetTickCount();
    SOCKET s = tcp_connect(ip, port, 1500);
    if (s == INVALID_SOCKET) { if (why) snprintf(why, whycap, "no connection"); return 0; }
    DWORD to = 2000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
    char req[4608];   // a Divoom Times Frame screen layout is some 2 KB
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:%d\r\nAccept: application/json\r\n%s"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s", method, path, ip, port,
                     bl ? "Content-Type: application/json\r\n" : "", bl, body ? body : "");
    if (n >= (int)sizeof(req) || !tcp_send_all(s, req, n)) { closesocket(s); if (why) snprintf(why, whycap, "send failed"); return 0; }
    int got = 0, head = -1, clen = -1, chunked = 0;
    while (got < cap - 1) {
        int r = recv(s, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += r; buf[got] = 0;
        if (head < 0) {
            char *e = strstr(buf, "\r\n\r\n");
            if (e) {
                head = (int)(e - buf) + 4;
                char low[1024]; int hl = min(head, (int)sizeof(low) - 1);
                for (int i = 0; i < hl; i++) low[i] = (char)tolower((unsigned char)buf[i]);
                low[hl] = 0;
                char *cl = strstr(low, "content-length:");
                if (cl) clen = atoi(cl + 15);
                chunked = strstr(low, "transfer-encoding: chunked") != NULL;
            }
        }
        if (head >= 0 && clen >= 0 && got >= head + clen) break;   // the whole answer
        if (head >= 0 && chunked && strstr(buf + head, "\r\n0\r\n\r\n") != NULL) break;
        if (head >= 0 && chunked && !strncmp(buf + head, "0\r\n\r\n", 5)) break;
    }
    closesocket(s);
    buf[got] = 0;
    int status = 0;
    if (sscanf_s(buf, "HTTP/%*d.%*d %d", &status) != 1) {
        if (why) snprintf(why, whycap, got ? "odd answer after %lu ms: %.60s" : "no answer in %lu ms", GetTickCount() - t0, buf);
        return 0;
    }
    if (head >= 0) memmove(buf, buf + head, strlen(buf + head) + 1); else buf[0] = 0;
    if (chunked) {   // size CRLF data CRLF ... 0 CRLF CRLF -> data
        char *r = buf, *w = buf;
        for (;;) {
            long n = strtol(r, &r, 16);
            char *nl = strstr(r, "\r\n");
            if (n <= 0 || !nl) break;
            r = nl + 2;
            if ((long)strlen(r) < n) n = (long)strlen(r);
            memmove(w, r, n); w += n; r += n;
            if (r[0] == '\r' && r[1] == '\n') r += 2;
        }
        *w = 0;
    }
    if (why && (status < 200 || status >= 300)) snprintf(why, whycap, "answered %d: %.80s", status, buf);
    return status;
}

// HTTPS download to a file (WinHTTP follows redirects, e.g. to a file host); gives up past max bytes or when the
// download is shorter than announced. pct (may be NULL) follows the progress, and the window is told.
int https_download(const wchar_t *url, const wchar_t *to, long long max, volatile LONG *pct) {
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;
    int ok = 0; long long got = 0;
    HINTERNET ses = WinHttpOpen(L"haku-control/" HAKU_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 10000, 10000, 30000, 60000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    FILE *f = NULL;
    if (req && WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        wchar_t cl[32] = L""; DWORD cls = sizeof(cl);
        long long total = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &cls, NULL) ? _wtoi64(cl) : 0;
        if (code == 200 && (f = _wfopen(to, L"wb")) != NULL) {
            static char chunk[256 * 1024]; DWORD n;
            ok = 1;
            while (WinHttpReadData(req, chunk, sizeof(chunk), &n) && n) {
                if (fwrite(chunk, 1, n, f) != n) { ok = 0; break; }
                got += n;
                LONG p = total > 0 ? (LONG)(got * 100 / total) : 0;
                if (pct && p != *pct) { InterlockedExchange(pct, p); ui_refresh(); }
                if (got > max) { ok = 0; break; }
            }
            if (total > 0 && got != total) ok = 0;
            fclose(f);
        } else logf_("download: %ls answered %lu", host, code);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// One request to an http:// or https:// URL (WinHTTP, so proxies and TLS are Windows'): POST with a JSON body,
// or GET when body is NULL. The answer goes to out; returns the HTTP status, 0 when there was no answer.
int web_call(const char *url, const char *body, char *out, int cap) {
    wchar_t wurl[512], host[256], path[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    out[0] = 0;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return 0;
    int n = 0; DWORD code = 0;
    HINTERNET ses = WinHttpOpen(L"haku-control/" HAKU_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 6000, 6000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, body ? L"POST" : L"GET", path, NULL, NULL, NULL,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    DWORD bl = body ? (DWORD)strlen(body) : 0;
    if (req && WinHttpSendRequest(req, body ? L"Content-Type: application/json\r\n" : NULL, (DWORD)-1, (LPVOID)body, bl, bl, 0)
            && WinHttpReceiveResponse(req, NULL)) {
        DWORD sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        DWORD got;
        while (n < cap - 1 && WinHttpReadData(req, out + n, cap - 1 - n, &got) && got) n += got;
    }
    out[n] = 0;
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return (int)code;
}

// SHA-256 of a file as lower-case hex (65 chars with the 0)
int sha256_hex(const wchar_t *file, char *hex) {
    FILE *f = _wfopen(file, L"rb");
    if (!f) return 0;
    BCRYPT_HASH_HANDLE h = NULL; BYTE d[32]; int ok = 0;
    if (!BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &h, NULL, 0, NULL, 0, 0)) {
        static BYTE buf[256 * 1024]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) BCryptHashData(h, buf, (ULONG)n, 0);
        ok = !BCryptFinishHash(h, d, 32, 0);
        BCryptDestroyHash(h);
    }
    fclose(f);
    for (int i = 0; ok && i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
    return ok;
}

SOCKET udp_socket(int bind_port, int broadcast) {
    net_init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    if (broadcast) { int one = 1; setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof(one)); }
    if (bind_port >= 0) {
        int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
        struct sockaddr_in a = { AF_INET, htons((u_short)bind_port) };
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { closesocket(s); return INVALID_SOCKET; }
    }
    return s;
}

int udp_send(SOCKET s, const char *host, int port, const void *data, int len) {
    struct sockaddr_in a;
    if (!net_addr(host, port, &a)) return 0;
    return sendto(s, (const char *)data, len, 0, (struct sockaddr *)&a, sizeof(a)) == len;
}

// Waits up to ms for a datagram. Returns its length (0 on timeout); from gets the sender's address.
int udp_recv(SOCKET s, void *buf, int cap, int ms, char *from, int from_cap) {
    fd_set r; FD_ZERO(&r); FD_SET(s, &r);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    if (select(0, &r, NULL, NULL, &tv) != 1) return 0;
    struct sockaddr_in a; int al = sizeof(a);
    int n = recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&a, &al);
    if (n <= 0) return 0;
    if (from) inet_ntop(AF_INET, &a.sin_addr, from, from_cap);
    return n;
}

// Sends one datagram to a multicast group out of every interface (and to every directed broadcast
// address when group is NULL). Replies arrive on s.
void udp_send_all_ifaces(SOCKET s, const char *group, int port, const void *data, int len) {
    ULONG addrs[16];
    if (group) {
        int n = net_addresses(addrs, 16);
        for (int i = 0; i < n; i++) {
            struct in_addr ifa; ifa.s_addr = addrs[i];
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (char *)&ifa, sizeof(ifa));
            udp_send(s, group, port, data, len);
        }
        if (!n) udp_send(s, group, port, data, len);
    } else {
        int n = net_broadcasts(addrs, 16);
        for (int i = 0; i < n; i++) {
            struct sockaddr_in a = { AF_INET, htons((u_short)port) };
            a.sin_addr.s_addr = addrs[i];
            sendto(s, (const char *)data, len, 0, (struct sockaddr *)&a, sizeof(a));
        }
        udp_send(s, "255.255.255.255", port, data, len);
    }
}

// mDNS browse (RFC 6762 "legacy unicast": the query comes from an ephemeral port, so responders answer
// straight to us). hit() gets the address of every host that answered for `service` (e.g. "_wled._tcp.local").
void mdns_browse(const char *service, int ms, void (*hit)(const char *ip, void *ctx), void *ctx) {
    SOCKET s = udp_socket(0, 0);
    if (s == INVALID_SOCKET) return;
    unsigned char q[256] = { 0 };
    int n = 12;
    q[5] = 1;   // one question
    char tmp[128]; strcpy_s(tmp, sizeof(tmp), service);
    for (char *ctx2 = NULL, *lab = strtok_s(tmp, ".", &ctx2); lab; lab = strtok_s(NULL, ".", &ctx2)) {
        int l = (int)strlen(lab);
        q[n++] = (unsigned char)l; memcpy(q + n, lab, l); n += l;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 12;       // PTR
    q[n++] = 0x80; q[n++] = 1;     // class IN, unicast response wanted
    udp_send_all_ifaces(s, "224.0.0.251", 5353, q, n);
    // the service name in wire format, to spot it in answers
    char label[64]; const char *dot = strchr(service, '.');
    int ll = dot ? (int)(dot - service) : (int)strlen(service);
    if (ll > 60) ll = 60;
    memcpy(label, service, ll); label[ll] = 0;
    char seen[32][48]; int nseen = 0;
    DWORD end = GetTickCount() + ms;
    for (;;) {
        int left = (int)(end - GetTickCount());
        if (left <= 0) break;
        unsigned char buf[1500]; char from[48];
        int r = udp_recv(s, buf, sizeof(buf), left, from, sizeof(from));
        if (r <= 12 || !(buf[2] & 0x80)) continue;   // responses only
        int found = 0;
        for (int i = 0; i + ll <= r && !found; i++) if (buf[i] == ll && !memcmp(buf + i + 1, label, ll)) found = 1;
        if (!found) continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], from)) dup = 1;
        if (dup || nseen >= 32) continue;
        strcpy_s(seen[nseen++], 48, from);
        hit(from, ctx);
    }
    closesocket(s);
}

// ---- JSON: finds "key": and returns a pointer to the value (after spaces), or NULL.
static const char *json_find(const char *js, const char *key) {
    char pat[80]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    for (const char *p = strstr(js, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
    }
    return NULL;
}

int json_get_str(const char *js, const char *key, char *out, int cap) {
    out[0] = 0;
    const char *v = js ? json_find(js, key) : NULL;
    if (!v || *v != '"') return 0;
    int i = 0;
    for (v++; *v && *v != '"' && i < cap - 1; v++) {
        if (*v == '\\' && v[1]) v++;
        out[i++] = *v;
    }
    out[i] = 0;
    return 1;
}

double json_get_num(const char *js, const char *key, double def) {
    const char *v = js ? json_find(js, key) : NULL;
    if (!v) return def;
    if (*v == '"') v++;   // some devices quote numbers
    if (!strncmp(v, "true", 4)) return 1;
    if (!strncmp(v, "false", 5)) return 0;
    char *e; double d = strtod(v, &e);
    return e == v ? def : d;
}

const char *json_get_obj(const char *js, const char *key) {
    const char *v = js ? json_find(js, key) : NULL;
    return v && (*v == '{' || *v == '[') ? v : NULL;
}

// Copies s into out as a JSON string body (no quotes), escaping quotes, backslashes and control chars.
int json_escape_to(char *out, int cap, const char *s) {
    int n = 0;
    for (; *s && n < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}
