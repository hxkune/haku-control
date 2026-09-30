// SPDX-License-Identifier: GPL-3.0-only
// Diagnostics: one text file with what is needed to look into a problem on someone else's PC, saved to Downloads
// and shown in Explorer, ready to send. It holds the version and Windows build, the live status (devices, what is
// online), the settings, the network adapters, the firewall rule and the end of the log (this run and the one
// before). Nothing secret goes in: settings values named like keys / tokens / secrets / passwords / PINs are
// replaced, the phone PIN is cut from the status, and the key files (aidot.json, tuya.json, govee.json,
// nanoleaf.json, remote.json) are only listed with their size, never read into the report.
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <process.h>
#include <share.h>
#include <stdlib.h>

static volatile LONG busy;
static SRWLOCK lk = SRWLOCK_INIT;
static char last_file[MAX_PATH * 3];   // UTF-8 name of the last report, for the page
static int  last_ok = -1;

typedef struct { char *p; int n, cap; } buf_t;

static void put(buf_t *b, const char *fmt, ...) {
    if (b->n >= b->cap - 1) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0) b->n += min(k, b->cap - 1 - b->n);
}

static void section(buf_t *b, const char *title) { put(b, "\r\n==================== %s\r\n", title); }

// a settings key whose value must not leave the PC
static int secret_key(const char *k) {
    char l[64]; snprintf(l, sizeof(l), "%s", k); _strlwr_s(l, sizeof(l));
    return strstr(l, "key") || strstr(l, "token") || strstr(l, "secret") || strstr(l, "pass") || strstr(l, "pin") || strstr(l, "mail");
}

static void add_settings(buf_t *b) {
    FILE *f = _wfopen(cfg_path(), L"rb");
    if (!f) { put(b, "(no settings file)\r\n"); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        char *eq = strchr(line, '=');
        if (eq && line[0] != '[' && line[0] != ';' && line[0] != '#') {
            char k[64]; int kl = (int)(eq - line); if (kl > 63) kl = 63;
            memcpy(k, line, kl); k[kl] = 0;
            while (kl > 0 && k[kl - 1] == ' ') k[--kl] = 0;
            // hotkeys are named like keys but are harmless; everything else key-like is hidden
            if (secret_key(k) && _strnicmp(k, "hotkey", 6) && eq[1]) { put(b, "%s=(hidden)\r\n", k); continue; }
        }
        put(b, "%s\r\n", line);
    }
    fclose(f);
}

// the last max_bytes of a text file (from a line start)
static void add_tail(buf_t *b, const wchar_t *file, long max_bytes) {
    FILE *f = _wfsopen(file, L"rb", _SH_DENYNO);
    if (!f) { put(b, "(none)\r\n"); return; }
    fseek(f, 0, SEEK_END); long size = ftell(f);
    long from = size > max_bytes ? size - max_bytes : 0;
    fseek(f, from, SEEK_SET);
    char line[1024]; int first = from > 0;
    if (first) put(b, "(… the first %ld KB left out)\r\n", from / 1024);
    while (fgets(line, sizeof(line), f)) {
        if (first) { first = 0; continue; }   // a cut line
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        put(b, "%s\r\n", line);
    }
    fclose(f);
}

static void add_key_files(buf_t *b) {
    static const wchar_t *const F[] = { L"nanoleaf.json", L"aidot.json", L"tuya.json", L"govee.json", L"remote.json" };
    for (int i = 0; i < (int)(sizeof(F) / sizeof(F[0])); i++) {
        wchar_t p[MAX_PATH]; app_data_path(F[i], p);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &a)) put(b, "%ls: %lu bytes (contents not included)\r\n", F[i], a.nFileSizeLow);
        else put(b, "%ls: -\r\n", F[i]);
    }
}

static void add_adapters(buf_t *b) {
    ULONG len = 64 * 1024;
    IP_ADAPTER_ADDRESSES *all = malloc(len);
    if (!all || GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                     NULL, all, &len) != NO_ERROR) { free(all); put(b, "(could not read)\r\n"); return; }
    for (IP_ADAPTER_ADDRESSES *a = all; a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char nm[256], ds[256];
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName ? a->FriendlyName : L"", -1, nm, sizeof(nm), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, a->Description ? a->Description : L"", -1, ds, sizeof(ds), NULL, NULL);
        put(b, "%s (%s): %s, type %lu%s\r\n", nm, ds, a->OperStatus == IfOperStatusUp ? "up" : "down", a->IfType, a->FirstGatewayAddress ? ", has gateway" : "");
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[32]; inet_ntop(AF_INET, &((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            put(b, "    %s/%u\r\n", ip, u->OnLinkPrefixLength);
        }
    }
    free(all);
}

// runs a console program and adds its output (OEM code page -> UTF-8)
static void add_command(buf_t *b, const wchar_t *cmdline) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    wchar_t cl[2048]; wcscpy_s(cl, 2048, cmdline);
    if (!CreateProcessW(NULL, cl, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); put(b, "(could not run)\r\n"); return; }
    CloseHandle(wr);
    static char raw[32 * 1024]; DWORD got, n = 0;
    while (n < sizeof(raw) - 1 && ReadFile(rd, raw + n, (DWORD)sizeof(raw) - 1 - n, &got, NULL) && got) n += got;
    raw[n] = 0;
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    static wchar_t w[32 * 1024]; static char u8[64 * 1024];
    MultiByteToWideChar(CP_OEMCP, 0, raw, -1, w, 32 * 1024);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8), NULL, NULL);
    put(b, "%s\r\n", u8);
}

static void windows_version(char *out, int cap) {
    typedef LONG(WINAPI * rtl_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = { sizeof(v) };
    rtl_t f = (rtl_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (f && f(&v) == 0) snprintf(out, cap, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    else snprintf(out, cap, "Windows (unknown)");
}

static int elevated(void) {
    HANDLE t; TOKEN_ELEVATION e = { 0 }; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}

static unsigned __stdcall run(void *arg) {
    (void)arg;
    buf_t b = { malloc(4 * 1024 * 1024), 0, 4 * 1024 * 1024 };
    int ok = 0;
    wchar_t path[MAX_PATH] = L"";
    if (!b.p) goto done;
    SYSTEMTIME t; GetLocalTime(&t);
    char win[64]; windows_version(win, sizeof(win));
    put(&b, "haku control diagnostics\r\n");
    put(&b, "version %s%s, %s, running as %s\r\n", HAKU_VER_STR,
#ifdef HAKU_DEV
        " (test build)",
#else
        "",
#endif
        win, elevated() ? "administrator" : "normal user");
    put(&b, "made %04d-%02d-%02d %02d:%02d:%02d\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    put(&b, "(keys, tokens, passwords and PINs are left out)\r\n");

    section(&b, "status");
    {
        static char st[512 * 1024];
        app_status_json(st, sizeof(st));
        char *p = strstr(st, "\"pin\":\"");   // the phone PIN
        if (p) { p += 7; while (*p && *p != '"') *p++ = '*'; }
        put(&b, "%s\r\n", st);
    }
    section(&b, "hardware");
    { char *hw = malloc(64 * 1024); if (hw) { hw_inventory(hw, 64 * 1024); put(&b, "%s", hw); free(hw); } }
    section(&b, "settings.ini");
    add_settings(&b);
    section(&b, "key files");
    add_key_files(&b);
    section(&b, "network adapters");
    add_adapters(&b);
    section(&b, "firewall rule \"haku control\"");
    add_command(&b, L"netsh.exe advfirewall firewall show rule name=\"haku control\" verbose");
    section(&b, "every firewall rule for this exe (a Block one keeps phones out)");
    {
        wchar_t exe[MAX_PATH], cl[MAX_PATH + 300]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cl, MAX_PATH + 300, L"powershell.exe -NoProfile -NonInteractive -Command \"Get-NetFirewallApplicationFilter -Program '%s' | "
                 L"Get-NetFirewallRule | Format-Table DisplayName,Direction,Action,Enabled,Profile -AutoSize | Out-String -Width 200\"", exe);
        add_command(&b, cl);
    }
    section(&b, "log (this run)");
    { wchar_t p[MAX_PATH]; app_data_path(L"haku-control.log", p); add_tail(&b, p, 1500 * 1024); }
    section(&b, "log (the run before)");
    { wchar_t p[MAX_PATH]; app_data_path(L"haku-control.prev.log", p); add_tail(&b, p, 500 * 1024); }

    // Downloads (or the data folder when there is none)
    PWSTR dl = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &dl)) && dl)
        swprintf(path, MAX_PATH, L"%s\\haku-diagnostics-%04d%02d%02d-%02d%02d.txt", dl, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    else { wchar_t nm[64]; swprintf(nm, 64, L"haku-diagnostics-%04d%02d%02d-%02d%02d.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute); app_data_path(nm, path); }
    CoTaskMemFree(dl);
    FILE *f = _wfopen(path, L"wb");
    if (f) {
        fwrite("\xEF\xBB\xBF", 1, 3, f);   // UTF-8 mark: Notepad shows Cyrillic names right
        ok = fwrite(b.p, 1, b.n, f) == (size_t)b.n;
        fclose(f);
    }
done:
    free(b.p);
    AcquireSRWLockExclusive(&lk);
    last_ok = ok;
    WideCharToMultiByte(CP_UTF8, 0, ok ? wcsrchr(path, L'\\') + 1 : L"", -1, last_file, sizeof(last_file), NULL, NULL);
    ReleaseSRWLockExclusive(&lk);
    logf_("diagnostics %s", ok ? "saved" : "could not be saved");
    if (ok) {   // shown in Explorer, selected, ready to drag into a chat
        wchar_t args[MAX_PATH + 16]; swprintf(args, MAX_PATH + 16, L"/select,\"%s\"", path);
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    }
    ui_refresh();
    InterlockedExchange(&busy, 0);
    return 0;
}

void diag_save(void) {
    if (InterlockedCompareExchange(&busy, 1, 0)) return;
    AcquireSRWLockExclusive(&lk); last_ok = -1; last_file[0] = 0; ReleaseSRWLockExclusive(&lk);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, run, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&busy, 0);
}

// {"busy":0,"ok":1,"file":"haku-diagnostics-20260930-1412.txt"}; ok -1 before the first report
int diag_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"busy\":%d,\"ok\":%d,\"file\":\"%s\"}", (int)busy, last_ok, last_file);
    ReleaseSRWLockShared(&lk);
    return n;
}
