// SPDX-License-Identifier: GPL-3.0-only
// Phone / remote control: a tiny HTTP server on the local network ([remote] enabled=1, port=8723).
//   GET  /, /<file>          the settings page from .\ui (the page talks to /api when it is not inside WebView2)
//   POST /api/pair {"pin"}   PIN shown in the desktop window -> token (kept in %APPDATA%\haku-control\remote.json)
//   GET  /api/state|status|frame, POST /api/cmd {"cmd":...}   need the token (header X-Haku-Token or cookie)
// Only private / link-local / Tailscale addresses are served. Commands run on the UI thread, like the window's.
#include "common.h"
#include "devices.h"
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <process.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_TOKENS 16
static char   tokens[MAX_TOKENS][33];
static int    ntokens;
static char   pin[8];
static SRWLOCK lk = SRWLOCK_INIT;
static HANDLE th;
static SOCKET lsock = INVALID_SOCKET;
static volatile LONG run, fails, port_now;
static DWORD  locked_until;
static wchar_t ui_dir[MAX_PATH];

static void random_hex(char *out, int bytes) {
    unsigned char b[32];
    BCryptGenRandom(NULL, b, bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (int i = 0; i < bytes; i++) sprintf_s(out + i * 2, 3, "%02x", b[i]);
}

static void new_pin(void) {
    unsigned int r; BCryptGenRandom(NULL, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    snprintf(pin, sizeof(pin), "%06u", r % 1000000);
}

static void save_tokens(void) {
    wchar_t p[MAX_PATH]; app_data_path(L"remote.json", p);
    FILE *f = _wfopen(p, L"wb");
    if (!f) return;
    fprintf(f, "{\"tokens\":[");
    for (int i = 0; i < ntokens; i++) fprintf(f, "%s\"%s\"", i ? "," : "", tokens[i]);
    fprintf(f, "]}\n");
    fclose(f);
}

static void load_tokens(void) {
    wchar_t p[MAX_PATH]; app_data_path(L"remote.json", p);
    FILE *f = _wfopen(p, L"rb");
    ntokens = 0;
    if (!f) return;
    char buf[2048]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    for (char *q = strchr(buf, '['); q && ntokens < MAX_TOKENS && (q = strchr(q + 1, '"'));) {
        char *e = strchr(q + 1, '"'); if (!e) break;
        if (e - q - 1 == 32) { memcpy(tokens[ntokens], q + 1, 32); tokens[ntokens][32] = 0; ntokens++; }
        q = e;
    }
}

static int token_ok(const char *t) {
    if (!t || strlen(t) != 32) return 0;
    int ok = 0;
    AcquireSRWLockShared(&lk);
    for (int i = 0; i < ntokens; i++) {
        int d = 0; for (int k = 0; k < 32; k++) d |= tokens[i][k] ^ t[k];   // constant time
        ok |= d == 0;
    }
    ReleaseSRWLockShared(&lk);
    return ok;
}

// 10/8, 172.16/12, 192.168/16, 169.254/16, 100.64/10 (Tailscale / CGNAT), loopback
static int private_addr(ULONG a) {
    a = ntohl(a);
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE ||
           (a >> 22) == (100u << 2 | 1) || (a >> 24) == 127;
}

static void reply(SOCKET c, int status, const char *type, const char *body, int len, const char *extra) {
    char h[512];
    const char *st = status == 200 ? "OK" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 429 ? "Too Many Requests" : "Bad Request";
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n", status, st, type, len, extra ? extra : "");
    tcp_send_all(c, h, n);
    if (len) tcp_send_all(c, body, len);
}
static void reply_json(SOCKET c, int status, const char *js) { reply(c, status, "application/json; charset=utf-8", js, (int)strlen(js), NULL); }

static const char *mime(const char *name) {
    const char *e = strrchr(name, '.');
    if (!e) return "application/octet-stream";
    if (!_stricmp(e, ".html")) return "text/html; charset=utf-8";
    if (!_stricmp(e, ".js")) return "text/javascript; charset=utf-8";
    if (!_stricmp(e, ".css")) return "text/css; charset=utf-8";
    if (!_stricmp(e, ".svg")) return "image/svg+xml";
    if (!_stricmp(e, ".png")) return "image/png";
    if (!_stricmp(e, ".json") || !_stricmp(e, ".webmanifest")) return "application/manifest+json";
    return "application/octet-stream";
}

static void serve_file(SOCKET c, const char *name) {
    for (const char *p = name; *p; p++)   // flat folder, plain names only
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_') || (p[0] == '.' && p[1] == '.')) { reply_json(c, 404, "{}"); return; }
    if (!_strnicmp(name, "mock", 4)) { reply_json(c, 404, "{}"); return; }
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s%S", ui_dir, name);
    FILE *f = _wfopen(p, L"rb");
    if (!f) { reply_json(c, 404, "{}"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = n > 0 && n < 8 * 1024 * 1024 ? malloc(n) : NULL;
    if (buf && fread(buf, 1, n, f) == (size_t)n) reply(c, 200, mime(name), buf, (int)n, NULL);
    else reply_json(c, 404, "{}");
    free(buf); fclose(f);
}

static void header(const char *req, const char *name, char *out, int cap) {
    out[0] = 0;
    char pat[64]; snprintf(pat, sizeof(pat), "\r\n%s:", name);
    const char *p = NULL;
    for (const char *q = req; (q = strchr(q, '\r')); q++) if (!_strnicmp(q, pat, strlen(pat))) { p = q + strlen(pat); break; }
    if (!p) return;
    while (*p == ' ') p++;
    int i = 0; while (*p && *p != '\r' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
}

static void handle(SOCKET c) {
    static char req[70 * 1024];   // one connection at a time
    int got = 0, hl = -1;
    while (got < (int)sizeof(req) - 1) {
        int r = recv(c, req + got, (int)sizeof(req) - 1 - got, 0);
        if (r <= 0) break;
        got += r; req[got] = 0;
        char *e = strstr(req, "\r\n\r\n");
        if (e) {
            hl = (int)(e - req) + 4;
            char cl[16]; header(req, "Content-Length", cl, sizeof(cl));
            int need = hl + atoi(cl);
            if (need > (int)sizeof(req) - 1) { reply_json(c, 400, "{}"); return; }
            while (got < need) { r = recv(c, req + got, need - got, 0); if (r <= 0) break; got += r; }
            req[got] = 0;
            break;
        }
    }
    if (hl < 0) return;
    char method[8] = "", path[256] = "";
    sscanf_s(req, "%7s %255s", method, (unsigned)sizeof(method), path, (unsigned)sizeof(path));
    char *qs = strchr(path, '?'); if (qs) *qs = 0;
    const char *body = req + hl;

    if (!strcmp(method, "GET") && strncmp(path, "/api/", 5)) {
        serve_file(c, !strcmp(path, "/") ? "index.html" : path + 1);
        return;
    }
    if (!strcmp(path, "/api/pair") && !strcmp(method, "POST")) {
        if ((int)(GetTickCount() - locked_until) < 0) { reply_json(c, 429, "{\"error\":\"wait\"}"); return; }
        char p[16] = ""; json_get_str(body, "pin", p, sizeof(p));
        AcquireSRWLockExclusive(&lk);
        int ok = p[0] && !strcmp(p, pin);
        char tok[33] = "";
        if (ok) {
            random_hex(tok, 16);
            if (ntokens == MAX_TOKENS) { memmove(tokens[0], tokens[1], sizeof(tokens[0]) * (MAX_TOKENS - 1)); ntokens--; }
            strcpy_s(tokens[ntokens++], 33, tok);
            save_tokens();
            new_pin();   // one pairing per PIN
        }
        ReleaseSRWLockExclusive(&lk);
        if (!ok) {
            if (InterlockedIncrement(&fails) >= 5) { locked_until = GetTickCount() + 30000; InterlockedExchange(&fails, 0); }
            reply_json(c, 401, "{\"error\":\"pin\"}");
            return;
        }
        InterlockedExchange(&fails, 0);
        logf_("remote: a device was paired");
        char js[96], ck[128];
        snprintf(js, sizeof(js), "{\"token\":\"%s\"}", tok);
        snprintf(ck, sizeof(ck), "Set-Cookie: haku_token=%s; Max-Age=31536000; Path=/; SameSite=Strict\r\n", tok);
        reply(c, 200, "application/json", js, (int)strlen(js), ck);
        return;
    }
    // everything else needs a token
    char tok[64] = "", cookie[512] = "";
    header(req, "X-Haku-Token", tok, sizeof(tok));
    if (!tok[0]) {
        header(req, "Cookie", cookie, sizeof(cookie));
        char *k = strstr(cookie, "haku_token=");
        if (k) { k += 11; int i = 0; while (k[i] && k[i] != ';' && i < 63) { tok[i] = k[i]; i++; } tok[i] = 0; }
    }
    if (!token_ok(tok)) { reply_json(c, 401, "{\"error\":\"pair\"}"); return; }

    if (!strcmp(method, "GET") && (!strcmp(path, "/api/state") || !strcmp(path, "/api/status") || !strcmp(path, "/api/frame"))) {
        static char *big; if (!big) big = malloc(512 * 1024);
        if (!big) { reply_json(c, 400, "{}"); return; }
        if (!strcmp(path, "/api/state")) app_state_json(big, 512 * 1024);
        else if (!strcmp(path, "/api/status")) app_status_json(big, 512 * 1024);
        else app_frame_json(big, 512 * 1024);
        reply_json(c, 200, big);
        return;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/cmd")) {
        char cmd[32] = ""; json_get_str(body, "cmd", cmd, sizeof(cmd));
        static const char *allowed[] = { "effect", "brightness", "set", "toggle", "power", "pair", "scan", "dev_add", "dev_remove", "mood", "preset", "preset_save", "preset_delete" };
        int ok = 0;
        for (int i = 0; i < (int)(sizeof(allowed) / sizeof(allowed[0])); i++) if (!strcmp(cmd, allowed[i])) ok = 1;
#ifdef HAKU_DEV
        // test builds (loopback only by default): the account sign-in can be driven from a browser for tests
        if (!strcmp(cmd, "aidot_login") || !strcmp(cmd, "tuya_login") || !strcmp(cmd, "govee_login") || !strcmp(cmd, "diag") || !strcmp(cmd, "update_check") || !strcmp(cmd, "update_install")) ok = 1;
#endif
        if (!ok) { reply_json(c, 400, "{\"error\":\"cmd\"}"); return; }
        app_remote_cmd(body);
        reply_json(c, 200, "{\"ok\":1}");
        return;
    }
    reply_json(c, 404, "{}");
}

static unsigned __stdcall server(void *p) {
    (void)p;
    while (run) {
        fd_set r; FD_ZERO(&r); FD_SET(lsock, &r);
        struct timeval tv = { 0, 300000 };
        if (select(0, &r, NULL, NULL, &tv) != 1) continue;
        struct sockaddr_in a; int al = sizeof(a);
        SOCKET c = accept(lsock, (struct sockaddr *)&a, &al);
        if (c == INVALID_SOCKET) continue;
        if (!private_addr(a.sin_addr.s_addr)) { closesocket(c); continue; }
        DWORD to = 4000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char *)&to, sizeof(to));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (char *)&to, sizeof(to));
        handle(c);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
    return 0;
}

// The firewall, repaired once per start while the phone page is on (the app runs as administrator): Windows asks
// "allow access?" the first time the port opens, and if nobody answered (the app starts hidden at sign-in) it adds
// rules that *block* haku-control.exe, which win over any allow rule. So: drop every inbound rule for this exe and
// add the one allow rule again, for the program where it is now, from private addresses only (the app refuses the
// rest anyway). [remote] firewall=0 leaves the firewall alone.
static unsigned __stdcall fix_firewall(void *arg) {
    (void)arg;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t cmd[2][MAX_PATH + 400];
    swprintf(cmd[0], MAX_PATH + 400, L"netsh.exe advfirewall firewall delete rule name=all dir=in program=\"%s\"", exe);
    swprintf(cmd[1], MAX_PATH + 400, L"netsh.exe advfirewall firewall add rule name=\"haku control\" dir=in action=allow enable=yes profile=any "
             L"program=\"%s\" remoteip=LocalSubnet,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,100.64.0.0/10", exe);
    DWORD code[2] = { 1, 1 };
    for (int i = 0; i < 2; i++) {
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (!CreateProcessW(NULL, cmd[i], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) continue;
        if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code[i]);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    logf_("remote: firewall rule %s", code[1] == 0 ? "set" : "could not be set (not running as administrator?)");
    return 0;
}

static int elevated(void) {
    HANDLE t; TOKEN_ELEVATION e = { 0 }; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}

void remote_apply(void) {
    int want = cfg_geti("remote", "enabled", 0), port = cfg_geti("remote", "port", 8723);
    if (th && (!want || port != port_now)) {
        run = 0; WaitForSingleObject(th, 2000); CloseHandle(th); th = NULL;
        closesocket(lsock); lsock = INVALID_SOCKET;
        logf_("remote: off");
    }
    if (!want || th) return;
    net_init();
    if (!ui_dir[0]) {
        GetModuleFileNameW(NULL, ui_dir, MAX_PATH);
        wchar_t *s = wcsrchr(ui_dir, L'\\'); if (s) s[1] = 0;
        wcscat_s(ui_dir, MAX_PATH, L"ui\\");
    }
    AcquireSRWLockExclusive(&lk); load_tokens(); if (!pin[0]) new_pin(); ReleaseSRWLockExclusive(&lk);
    lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { AF_INET, htons((u_short)port) };
    inet_pton(AF_INET, cfg_get("remote", "bind", "0.0.0.0"), &a.sin_addr);   // 127.0.0.1: this PC only (testing)
    if (lsock == INVALID_SOCKET || bind(lsock, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lsock, 8) != 0) {
        logf_("remote: cannot listen on port %d (%d)", port, WSAGetLastError());
        if (lsock != INVALID_SOCKET) closesocket(lsock);
        lsock = INVALID_SOCKET;
        return;
    }
    port_now = port; run = 1;
    th = (HANDLE)_beginthreadex(NULL, 0, server, NULL, 0, NULL);
    logf_("remote: listening on port %d", port);
    static int fw_done;
    if (!fw_done && elevated() && cfg_geti("remote", "firewall", 1)) {
        fw_done = 1;
        HANDLE f = (HANDLE)_beginthreadex(NULL, 0, fix_firewall, NULL, 0, NULL);
        if (f) CloseHandle(f);
    }
#ifdef HAKU_DEV
    logf_("remote: PIN %s (shown in the log by test builds only)", pin);
#endif
}

void remote_stop(void) { if (th) { run = 0; WaitForSingleObject(th, 2000); CloseHandle(th); th = NULL; closesocket(lsock); lsock = INVALID_SOCKET; } }

void remote_forget(void) {
    AcquireSRWLockExclusive(&lk); ntokens = 0; save_tokens(); new_pin(); ReleaseSRWLockExclusive(&lk);
    logf_("remote: all paired devices forgotten");
}

void remote_new_pin(void) { AcquireSRWLockExclusive(&lk); new_pin(); ReleaseSRWLockExclusive(&lk); }

// {"enabled":1,"on":1,"port":8723,"pin":"123456","paired":2,"urls":["http://192.168.1.10:8723"]}
int remote_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"enabled\":%d,\"on\":%d,\"port\":%d,\"pin\":\"%s\",\"paired\":%d,\"urls\":[",
                     cfg_geti("remote", "enabled", 0), th != NULL, (int)port_now, th ? pin : "", ntokens);
    ReleaseSRWLockShared(&lk);
    ULONG addrs[16]; int na = th ? net_phone_addresses(addrs, 16) : 0, first = 1;
    for (int i = 0; i < na && n < cap - 64; i++) {
        if (!private_addr(addrs[i]) || (ntohl(addrs[i]) >> 24) == 127) continue;
        char ip[32]; struct in_addr ia; ia.s_addr = addrs[i]; inet_ntop(AF_INET, &ia, ip, sizeof(ip));
        n += snprintf(out + n, cap - n, "%s\"http://%s:%d\"", first ? "" : ",", ip, (int)port_now);
        first = 0;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}
