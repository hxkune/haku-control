// SPDX-License-Identifier: GPL-3.0-only
// Other lighting programs that hold a controller haku then cannot reach: Armoury Crate's LightingService an ASUS board,
// iCUE a Corsair hub, KANALI the TRYX screen, L-Connect the Lian Li receiver... The PC page lists the ones installed or
// running with what they hold. Close (a click, from the PC window only) ends one until it starts again: its services
// are stopped through Windows first (a service ended from outside counts as a crash and may be started again), then
// what is left of it is ended. The ones named in [conflicts] stop are closed whenever they come up while haku runs.
// Removing one is left to Windows' Installed apps. Not here: Razer Synapse, G HUB and SteelSeries GG, which haku
// lights those devices through.
#include "common.h"
#include <tlhelp32.h>
#include <process.h>
#include <stdlib.h>

// w: what it holds, in the page's words (cf.w.<w>); exe: how its process names start; reg: parts of its name in Installed apps
typedef struct { const char *id, *name, *w; const wchar_t *exe[3], *reg[2]; } cf_app;
static const cf_app APPS[] = {
    { "armoury", "ASUS Armoury Crate / Aura", "board", { L"armoury", L"lightingservice", L"aura" }, { L"armoury crate", L"aura" } },
    { "icue", "Corsair iCUE", "corsair", { L"icue", L"corsair" }, { L"icue" } },
    { "signalrgb", "SignalRGB", "all", { L"signalrgb" }, { L"signalrgb" } },
    { "msi", "MSI Center / Mystic Light", "board", { L"mysticlight", L"msi.centralserver", L"msi.center" }, { L"msi center", L"mystic light" } },
    { "gigabyte", "Gigabyte RGB Fusion / Control Center", "board", { L"rgbfusion", L"gigabyte" }, { L"rgb fusion", L"gigabyte control center" } },
    { "asrock", "ASRock Polychrome", "board", { L"polychrome" }, { L"polychrome" } },
    { "nzxt", "NZXT CAM", "cooler", { L"nzxt" }, { L"nzxt cam" } },
    { "tryx", "TRYX KANALI", "cooler", { L"kanali", L"tryx" }, { L"kanali" } },
    { "lianli", "Lian Li L-Connect", "fans", { L"l-connect", L"lconnect", L"lianli" }, { L"l-connect" } },
    { "openlinkhub", "OpenLinkHub", "corsair", { L"openlinkhub" }, { L"openlinkhub" } },
};
#define CF_N (int)(sizeof(APPS) / sizeof(APPS[0]))

static volatile LONG run_m, inst_m, fail_m, busy = -1;   // bit i: APPS[i] runs / is installed / did not close; busy: the one closing

static int cf_match(const wchar_t *exe) {
    wchar_t low[MAX_PATH]; wcsncpy_s(low, MAX_PATH, exe, _TRUNCATE); _wcslwr_s(low, MAX_PATH);
    for (int a = 0; a < CF_N; a++) for (int i = 0; i < 3; i++) if (APPS[a].exe[i] && wcsstr(low, APPS[a].exe[i]) == low) return a;   // the start only: "aura" is not Laura.exe
    return -1;
}

// the processes running now: a mask of the apps, and with pid the ids of one app's (up to n)
static LONG cf_running(int app, DWORD *pid, int *n) {
    LONG m = 0; int k = 0, cap = n ? *n : 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        int a = pe.th32ProcessID == GetCurrentProcessId() ? -1 : cf_match(pe.szExeFile);
        if (a < 0) continue;
        m |= 1 << a;
        if (a == app && k < cap) pid[k++] = pe.th32ProcessID;
    }
    CloseHandle(snap);
    if (n) *n = k;
    return m;
}

static LONG cf_installed(void) {
    static const struct { HKEY root; const wchar_t *path; } U[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" } };
    LONG m = 0;
    for (int u = 0; u < 3; u++) {
        HKEY h;
        if (RegOpenKeyExW(U[u].root, U[u].path, 0, KEY_READ, &h)) continue;
        wchar_t sub[256], dn[256];
        for (DWORD i = 0, n = 256; !RegEnumKeyExW(h, i, sub, &n, NULL, NULL, NULL, NULL); i++, n = 256) {
            DWORD sz = sizeof(dn);
            if (RegGetValueW(h, sub, L"DisplayName", RRF_RT_REG_SZ, NULL, dn, &sz)) continue;
            _wcslwr_s(dn, 256);
            for (int a = 0; a < CF_N; a++) for (int r = 0; r < 2; r++) if (APPS[a].reg[r] && wcsstr(dn, APPS[a].reg[r])) m |= 1 << a;
        }
        RegCloseKey(h);
    }
    return m;
}

static unsigned __stdcall cf_close_run(void *arg) {
    int a = (int)(INT_PTR)arg, n = 64, stopped = 0;
    DWORD pid[64];
    cf_running(a, pid, &n);
    // its services first, by the process they run in
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (scm) {
        DWORD need = 0, cnt = 0, resume = 0;
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, NULL, 0, &need, &cnt, &resume, NULL);
        BYTE *buf = need ? malloc(need) : NULL;
        resume = 0;
        if (buf && EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buf, need, &need, &cnt, &resume, NULL)) {
            ENUM_SERVICE_STATUS_PROCESSW *s = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
            for (DWORD i = 0; i < cnt; i++) for (int k = 0; k < n; k++) if (s[i].ServiceStatusProcess.dwProcessId == pid[k]) {
                SC_HANDLE sv = OpenServiceW(scm, s[i].lpServiceName, SERVICE_STOP);
                SERVICE_STATUS st;
                BOOL ok = sv && ControlService(sv, SERVICE_CONTROL_STOP, &st);
                char nm[256]; WideCharToMultiByte(CP_UTF8, 0, s[i].lpServiceName, -1, nm, sizeof(nm), NULL, NULL);
                logf_("conflicts: %s: service %s %s (%lu)", APPS[a].id, nm, ok ? "stopped" : "not stopped", ok ? 0 : GetLastError());
                if (sv) CloseServiceHandle(sv);
                stopped += ok;
                break;
            }
        }
        free(buf);
        CloseServiceHandle(scm);
    }
    // then what is left, once the services had a few seconds to end
    for (int t = 0; t < 10 && stopped && (n = 64, cf_running(a, pid, &n) >> a & 1); t++) Sleep(500);
    n = 64; cf_running(a, pid, &n);
    for (int k = 0; k < n; k++) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid[k]);
        if (p && TerminateProcess(p, 0)) WaitForSingleObject(p, 2000);
        if (p) CloseHandle(p);
    }
    LONG left = cf_running(-1, NULL, NULL);
    InterlockedExchange(&run_m, left);
    if (left >> a & 1) { InterlockedOr(&fail_m, 1 << a); logf_("conflicts: %s still runs (not elevated, or it protects itself)", APPS[a].id); }
    else { InterlockedAnd(&fail_m, ~(1 << a)); logf_("conflicts: %s closed", APPS[a].id); app_reopen_devices(); }
    InterlockedExchange(&busy, -1);
    ui_refresh();
    return 0;
}

void conflicts_close(const char *id) {
    for (int a = 0; a < CF_N; a++) if (!strcmp(APPS[a].id, id)) {
        if (InterlockedCompareExchange(&busy, a, -1) != -1) return;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, cf_close_run, (void *)(INT_PTR)a, 0, NULL);
        if (h) CloseHandle(h); else InterlockedExchange(&busy, -1);
        ui_refresh();
        return;
    }
}

static int cf_listed(const char *list, const char *id) {
    size_t l = strlen(id);
    for (const char *p = list; (p = strstr(p, id)) != NULL; p += l)
        if ((p == list || p[-1] == ',' || p[-1] == ' ') && (!p[l] || p[l] == ',' || p[l] == ' ')) return 1;
    return 0;
}

// every 3 s on the window thread: what runs now (and once a minute what is installed); the listed ones are closed
// once each time they come up, so one that will not close is not tried again and again
void conflicts_tick(void) {
    static DWORD inst_at; static int first = 1; static LONG tried;
    if (first || GetTickCount() - inst_at > 60000) { first = 0; inst_at = GetTickCount(); InterlockedExchange(&inst_m, cf_installed()); }
    if (busy != -1) return;
    LONG r = cf_running(-1, NULL, NULL), old = InterlockedExchange(&run_m, r);
    tried &= r; InterlockedAnd(&fail_m, r);
    const char *stop = cfg_get("conflicts", "stop", "");
    for (int a = 0; a < CF_N && *stop; a++)
        if ((r >> a & 1) && !(tried >> a & 1) && cf_listed(stop, APPS[a].id)) { tried |= 1 << a; conflicts_close(APPS[a].id); break; }
    if (r != old) ui_refresh();
}

// "cf":{"busy":"",[{"id","name","w","run","inst","fail"}]}: the ones installed or running
int conflicts_json(char *out, int cap) {
    LONG r = run_m, in = inst_m, f = fail_m; int b = busy;
    int n = snprintf(out, cap, "{\"busy\":\"%s\",\"a\":[", b >= 0 && b < CF_N ? APPS[b].id : ""), first = 1;
    for (int a = 0; a < CF_N && n < cap - 160; a++) if ((r | in) >> a & 1) {
        n += snprintf(out + n, cap - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"w\":\"%s\",\"run\":%d,\"inst\":%d,\"fail\":%d}", first ? "" : ",",
                      APPS[a].id, APPS[a].name, APPS[a].w, (int)(r >> a & 1), (int)(in >> a & 1), (int)(f >> a & 1));
        first = 0;
    }
    return n + snprintf(out + n, cap - n, "]}");
}

#ifdef CF_TEST   // cl /DCF_TEST src\conflicts.c advapi32.lib: the name matching, and the processes and apps of this PC
#include <assert.h>
void logf_(const char *fmt, ...) { (void)fmt; }
void app_reopen_devices(void) {}
void ui_refresh(void) {}
const char *cfg_get(const char *s, const char *k, const char *def) { (void)s; (void)k; return def; }
int main(void) {
    assert(cf_match(L"ArmouryCrate.Service.exe") == 0 && cf_match(L"LightingService.exe") == 0 && cf_match(L"iCUE.exe") == 1);
    assert(cf_match(L"L-Connect 3.exe") == 8 && cf_match(L"haku-control.exe") < 0 && cf_match(L"RazerAppEngine.exe") < 0 && cf_match(L"Laura.exe") < 0);
    assert(cf_listed("icue,armoury", "armoury") && cf_listed("icue, armoury", "icue") && cf_listed("icue", "icue"));
    assert(!cf_listed("xicue", "icue") && !cf_listed("icuex", "icue") && !cf_listed("", "icue"));
    char js[4096];
    run_m = cf_running(-1, NULL, NULL); inst_m = cf_installed();
    conflicts_json(js, sizeof(js));
    printf("conflicts: ok %s\n", js);
    return 0;
}
#endif
