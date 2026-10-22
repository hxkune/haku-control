// SPDX-License-Identifier: GPL-3.0-only
// Other lighting programs that may hold a controller haku then cannot reach: Armoury Crate's LightingService an ASUS
// board, iCUE a Corsair hub, KANALI the TRYX screen, L-Connect the Lian Li receiver... The PC page lists the ones
// installed or running. Running is only a possible conflict: whether one really holds a device is that device's own
// status. They are known by their exact process names, and where the maker signs its programs by that signature too;
// a process of the same name signed by someone else (or not at all) is shown but never ended.
// Close (a click, from the PC window only) asks it to end: its services are stopped through Windows, its windows are
// sent WM_CLOSE, and what still runs is reported. End (a click, after a warning) ends the whole program and its
// services the hard way. The ones named in [conflicts] stop are closed whenever they come up while haku runs, except
// the ones that also drive fans or pumps: haku has no tested handover of those, so for them the rule waits and the page
// says why. [conflicts] leave: the ones the user leaves their devices to, never closed by a rule. Removing one is left
// to Windows' Installed apps. Not here: Razer Synapse, G HUB and SteelSeries GG, which haku lights those devices through.
#include "devices.h"
#include <tlhelp32.h>
#include <process.h>
#include <stdlib.h>

// w: what it may hold, in the page's words (cf.w.<w>); cool: it drives fans or pumps too; pub: a part of its maker's
// signature, lower case (NULL: not known, the exact name is all); exe: its process names; reg: parts of its name in
// Installed apps
#define CF_EXE 8
typedef struct { const char *id, *name, *w; int cool; const wchar_t *pub, *exe[CF_EXE], *reg[2]; } cf_app;
// shortcut: the names and signers are the makers' known ones, not seen on a PC with each program; add one when a
// diagnostics report shows another
static const cf_app APPS[] = {
    { "armoury", "ASUS Armoury Crate / Aura", "board", 1, L"asustek", { L"ArmouryCrate.exe", L"ArmouryCrate.Service.exe", L"ArmouryCrate.UserSessionHelper.exe",
      L"ArmourySocketServer.exe", L"LightingService.exe", L"AuraWallpaperService.exe" }, { L"armoury crate", L"aura" } },
    { "icue", "Corsair iCUE", "corsair", 1, L"corsair", { L"iCUE.exe", L"Corsair.Service.exe", L"iCUEDevicePluginHost.exe", L"CorsairDeviceControlService.exe",
      L"CorsairLLAService.exe", L"Corsair.Service.CpuIdRemote64.exe" }, { L"icue" } },
    { "signalrgb", "SignalRGB", "all", 1, NULL, { L"SignalRgb.exe", L"SignalRgbLauncher.exe" }, { L"signalrgb" } },
    { "msi", "MSI Center / Mystic Light", "board", 1, L"micro-star", { L"MSI.CentralServer.exe", L"MSI.TerminalServer.exe", L"MSI Center.exe",
      L"MysticLight.exe", L"LEDKeeper2.exe" }, { L"msi center", L"mystic light" } },
    { "gigabyte", "Gigabyte RGB Fusion / Control Center", "board", 1, L"giga-byte", { L"RGBFusion.exe", L"RGBFusion2.exe", L"GCC.exe", L"GigabyteControlCenter.exe" },
      { L"rgb fusion", L"gigabyte control center" } },
    { "asrock", "ASRock Polychrome", "board", 0, NULL, { L"PolychromeRGB.exe", L"ASRPolychromeRGB.exe", L"Polychrome Sync.exe" }, { L"polychrome" } },
    { "nzxt", "NZXT CAM", "cooler", 1, L"nzxt", { L"NZXT CAM.exe", L"CAM.exe", L"NZXT.CAM.Service.exe" }, { L"nzxt cam" } },
    { "tryx", "TRYX KANALI", "cooler", 1, NULL, { L"KANALI.exe", L"Kanali.Service.exe" }, { L"kanali" } },
    { "lianli", "Lian Li L-Connect", "fans", 1, NULL, { L"L-Connect 3.exe", L"L-Connect-Service.exe", L"L-Connect.exe", L"LConnect3.exe" }, { L"l-connect" } },
    { "openlinkhub", "OpenLinkHub", "corsair", 1, NULL, { L"OpenLinkHub.exe" }, { L"openlinkhub" } },
};
#define CF_N (int)(sizeof(APPS) / sizeof(APPS[0]))

// bit i: APPS[i] runs / is installed / has a process of its name without its maker's signature (not ended) / still
// ran after Close / still ran after End; busy: the one closing
static volatile LONG run_m, inst_m, bad_m, soft_m, hard_m, busy = -1;

static int cf_match(const wchar_t *exe) {
    for (int a = 0; a < CF_N; a++) for (int i = 0; i < CF_EXE; i++) if (APPS[a].exe[i] && !_wcsicmp(exe, APPS[a].exe[i])) return a;
    return -1;
}

// 1: the process's file is signed by the app's maker, or its maker is not known (then the exact name is all there is)
static int cf_signed(int a, DWORD pid) {
    static struct { DWORD pid; int a, ok; } memo[32]; static int next;   // a process is checked once: the check reads its file
    if (!APPS[a].pub) return 1;
    for (int i = 0; i < 32; i++) if (memo[i].pid == pid && memo[i].a == a) return memo[i].ok;
    wchar_t path[MAX_PATH], who[256]; DWORD len = MAX_PATH; int ok = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p && QueryFullProcessImageNameW(p, 0, path, &len) && file_signer(path, who, 256)) { _wcslwr_s(who, 256); ok = wcsstr(who, APPS[a].pub) != NULL; }
    if (p) CloseHandle(p);
    memo[next].pid = pid; memo[next].a = a; memo[next].ok = ok; next = (next + 1) % 32;
    if (!ok) logf_("conflicts: %s: process %lu has its name but not its maker's signature: left alone", APPS[a].id, pid);
    return ok;
}

// the processes running now: a mask of the apps, and with pid the ids of one app's that may be ended (up to n);
// bad: the apps with a process of the name that may not
static LONG cf_running(int app, DWORD *pid, int *n, LONG *bad) {
    LONG m = 0, b = 0; int k = 0, cap = n ? *n : 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        int a = pe.th32ProcessID == GetCurrentProcessId() ? -1 : cf_match(pe.szExeFile);
        if (a < 0) continue;
        m |= 1 << a;
        if (!cf_signed(a, pe.th32ProcessID)) { b |= 1 << a; continue; }
        if (a == app && k < cap) pid[k++] = pe.th32ProcessID;
    }
    CloseHandle(snap);
    if (n) *n = k;
    if (bad) *bad = b;
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

static BOOL CALLBACK cf_wm_close(HWND w, LPARAM l) {
    const DWORD *pid = (const DWORD *)l; DWORD p = 0;
    GetWindowThreadProcessId(w, &p);
    for (DWORD k = 1; k <= pid[0]; k++) if (p == pid[k]) { PostMessageW(w, WM_CLOSE, 0, 0); break; }
    return TRUE;
}

// arg: the app | hard << 8. Close: its services stopped, its windows asked to close, what still runs reported.
// End: its services stopped, then what is left ended.
static unsigned __stdcall cf_close_run(void *arg) {
    int a = (int)(INT_PTR)arg & 0xff, hard = (int)(INT_PTR)arg >> 8, n = 64, stopped = 0;
    DWORD pid[64];
    cf_running(a, pid, &n, NULL);
    logf_("conflicts: %s: %s, %d of its processes", APPS[a].id, hard ? "end" : "close", n);
    // its services first, by the process they run in
    SC_HANDLE scm = n ? OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE) : NULL;
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
    // Close also asks its windows (a tray program may only hide); then a few seconds for it to end
    if (n && !hard) { DWORD w[65] = { (DWORD)n }; memcpy(w + 1, pid, n * sizeof(DWORD)); EnumWindows(cf_wm_close, (LPARAM)w); }
    for (int t = 0; n && (stopped || !hard) && t < (hard ? 10 : 16); t++) { Sleep(500); int m = 64; cf_running(a, pid, &m, NULL); if (!m) break; }
    if (hard) {
        n = 64; cf_running(a, pid, &n, NULL);
        for (int k = 0; k < n; k++) {
            HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid[k]);
            if (p && TerminateProcess(p, 0)) WaitForSingleObject(p, 2000);
            if (p) CloseHandle(p);
        }
    }
    LONG bad, left = cf_running(-1, NULL, NULL, &bad);
    n = 64; cf_running(a, pid, &n, NULL);   // what of it could have been ended and still runs
    InterlockedExchange(&run_m, left); InterlockedExchange(&bad_m, bad);
    if (n) {
        InterlockedOr(hard ? &hard_m : &soft_m, 1 << a);
        logf_("conflicts: %s: %d of its processes still run (%s)", APPS[a].id, n, hard ? "not elevated, or it protects itself" : "it only hid, or a service did not stop");
    } else {
        InterlockedAnd(&soft_m, ~(1 << a)); InterlockedAnd(&hard_m, ~(1 << a));
        logf_("conflicts: %s %s", APPS[a].id, left >> a & 1 ? "ended, but a process of its name not signed by its maker is left" : "closed");
        app_reopen_devices();
    }
    InterlockedExchange(&busy, -1);
    ui_refresh();
    return 0;
}

static void cf_start(const char *id, int hard) {
    for (int a = 0; a < CF_N; a++) if (!strcmp(APPS[a].id, id)) {
        if (InterlockedCompareExchange(&busy, a, -1) != -1) return;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, cf_close_run, (void *)(INT_PTR)(a | hard << 8), 0, NULL);
        if (h) CloseHandle(h); else InterlockedExchange(&busy, -1);
        ui_refresh();
        return;
    }
}
void conflicts_close(const char *id) { cf_start(id, 0); }
void conflicts_end(const char *id) { cf_start(id, 1); }

static int cf_listed(const char *list, const char *id) {
    size_t l = strlen(id);
    for (const char *p = list; (p = strstr(p, id)) != NULL; p += l)
        if ((p == list || p[-1] == ',' || p[-1] == ' ') && (!p[l] || p[l] == ',' || p[l] == ' ')) return 1;
    return 0;
}

// 1: [conflicts] stop closes it when it comes up: listed, not left to, and not one that drives fans or pumps
static int cf_auto(int a, const char *stop, const char *leave) { return !APPS[a].cool && cf_listed(stop, APPS[a].id) && !cf_listed(leave, APPS[a].id); }

// every 3 s on the window thread: what runs now (and once a minute what is installed); the listed ones are closed
// once each time they come up, so one that will not close is not tried again and again
void conflicts_tick(void) {
    static DWORD inst_at; static int first = 1; static LONG tried;
    if (first || GetTickCount() - inst_at > 60000) { first = 0; inst_at = GetTickCount(); InterlockedExchange(&inst_m, cf_installed()); }
    if (busy != -1) return;
    LONG bad, r = cf_running(-1, NULL, NULL, &bad), old = InterlockedExchange(&run_m, r);
    InterlockedExchange(&bad_m, bad);
    tried &= r; InterlockedAnd(&soft_m, r); InterlockedAnd(&hard_m, r);
    const char *stop = cfg_get("conflicts", "stop", ""), *leave = cfg_get("conflicts", "leave", "");
    for (int a = 0; a < CF_N && *stop; a++)
        if ((r >> a & 1) && !(tried >> a & 1) && cf_auto(a, stop, leave)) { tried |= 1 << a; logf_("conflicts: %s came up, closed by the rule", APPS[a].id); conflicts_close(APPS[a].id); break; }
    if (r != old) ui_refresh();
}

// "cf":{"busy":"",[{"id","name","w","cool","run","inst","bad","fail"}]}: the ones installed or running; bad: a process
// of its name is not signed by its maker (not ended); fail: still ran after Close (1) or End (2)
int conflicts_json(char *out, int cap) {
    LONG r = run_m, in = inst_m, bd = bad_m, sf = soft_m, hf = hard_m; int b = busy;
    int n = snprintf(out, cap, "{\"busy\":\"%s\",\"a\":[", b >= 0 && b < CF_N ? APPS[b].id : ""), first = 1;
    for (int a = 0; a < CF_N && n < cap - 200; a++) if ((r | in) >> a & 1) {
        n += snprintf(out + n, cap - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"w\":\"%s\",\"cool\":%d,\"run\":%d,\"inst\":%d,\"bad\":%d,\"fail\":%d}", first ? "" : ",",
                      APPS[a].id, APPS[a].name, APPS[a].w, APPS[a].cool, (int)(r >> a & 1), (int)(in >> a & 1), (int)(bd >> a & 1),
                      hf >> a & 1 ? 2 : (int)(sf >> a & 1));
        first = 0;
    }
    return n + snprintf(out + n, cap - n, "]}");
}

#ifdef CF_TEST   // cl /DCF_TEST src\conflicts.c advapi32.lib user32.lib: the matching and the rule, and the processes and apps of this PC
#include <assert.h>
void logf_(const char *fmt, ...) { (void)fmt; }
void app_reopen_devices(void) {}
void ui_refresh(void) {}
const char *cfg_get(const char *s, const char *k, const char *def) { (void)s; (void)k; return def; }
int file_signer(const wchar_t *file, wchar_t *name, int cap) { (void)file; wcscpy_s(name, cap, L"Someone Else Ltd"); return 1; }
int main(void) {
    assert(cf_match(L"ArmouryCrate.Service.exe") == 0 && cf_match(L"LightingService.exe") == 0 && cf_match(L"iCUE.exe") == 1 && cf_match(L"icue.EXE") == 1);
    assert(cf_match(L"L-Connect 3.exe") == 8 && cf_match(L"haku-control.exe") < 0 && cf_match(L"RazerAppEngine.exe") < 0 && cf_match(L"Laura.exe") < 0);
    // the old prefixes caught these: a name that only starts like one is a stranger now
    assert(cf_match(L"iCUEUpdateService.exe") < 0 && cf_match(L"icuesomething.exe") < 0 && cf_match(L"AuraSync.exe") < 0 && cf_match(L"GigabyteUpdate.exe") < 0);
    assert(cf_match(L"nzxtcam-helper.exe") < 0 && cf_match(L"tryx-installer.exe") < 0 && cf_match(L"lianli_tool.exe") < 0 && cf_match(L"OpenRGB.exe") < 0);
    assert(cf_match(L"lghub.exe") < 0 && cf_match(L"SteelSeriesGG.exe") < 0 && cf_match(L"RazerCentralService.exe") < 0);
    // a known maker's name signed by someone else is not to be ended; with the maker not known the name is enough
    assert(!cf_signed(1, 4242) && !cf_signed(1, 4242) && cf_signed(7, 4243));
    assert(cf_listed("icue,armoury", "armoury") && cf_listed("icue, armoury", "icue") && cf_listed("icue", "icue"));
    assert(!cf_listed("xicue", "icue") && !cf_listed("icuex", "icue") && !cf_listed("", "icue"));
    // the rule closes a lighting-only one; never one that drives fans or pumps, nor one left to
    assert(cf_auto(5, "asrock", "") && !cf_auto(5, "asrock", "asrock") && !cf_auto(1, "icue", "") && !cf_auto(8, "lianli,asrock", "") && !cf_auto(5, "", ""));
    char js[4096];
    LONG bad; run_m = cf_running(-1, NULL, NULL, &bad); bad_m = bad; inst_m = cf_installed();
    conflicts_json(js, sizeof(js));
    printf("conflicts: ok %s\n", js);
    return 0;
}
#endif
