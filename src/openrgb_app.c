// SPDX-License-Identifier: GPL-3.0-only
// OpenRGB on this PC, looked after, so its hardware (boards, graphics cards, RAM of other brands...) just works:
//  - found: [openrgb] path, where haku's setup put it, the usual install folders, or a copy that runs (kept);
//  - started when its SDK server does not answer: "OpenRGB.exe --server" (no window), in a job object, so it
//    closes with this app. A copy that is starting by itself (at sign-in) is waited for instead;
//  - what it finds is added by itself: not what this app drives itself, not a device removed by hand
//    ([openrgb] removed), not twice. [openrgb] auto=0 turns starting and adding off;
//  - set up from the PC page: OpenRGB's portable zip from its official release on Codeberg, checked against the
//    size and SHA-256 below (OpenRGB.exe is not signed), unpacked with Windows' own tar.exe into
//    %LOCALAPPDATA%\haku-control\OpenRGB.
#include "devices.h"
#include <process.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <stdlib.h>

#define ORGB_ZIP_URL  L"https://codeberg.org/OpenRGB/OpenRGB/releases/download/release_1.0/OpenRGB_1.0_Windows_64_81bbe18.zip"
#define ORGB_ZIP_SIZE 22012179LL
#define ORGB_ZIP_SHA  "182a52a3c97c4c4ae52c80286b4260c9666c51c3447dbc416ee8945f66192e90"

enum { SU_IDLE, SU_DOWNLOAD, SU_VERIFY, SU_UNPACK, SU_DONE, SU_ERROR };
static SRWLOCK lk = SRWLOCK_INIT;
static HANDLE job;                 // OpenRGB started here
static volatile LONG su_stage, su_pct;
static char su_err[16];

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void own_dir(wchar_t *out) { ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\" APP_ID L"\\OpenRGB", out, MAX_PATH); }

// a running OpenRGB.exe (not one started here): its path
static int running_copy(wchar_t *out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) }; int found = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"OpenRGB.exe")) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        BOOL mine = FALSE; if (job) IsProcessInJob(p, job, &mine);
        DWORD n = MAX_PATH;
        if (!mine && QueryFullProcessImageNameW(p, 0, out, &n)) found = 1;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}

// OpenRGB.exe on this PC; running: also a copy that runs (its path is kept)
int orgbapp_exe(wchar_t *out, int running) {
    const char *p = cfg_get("openrgb", "path", "");
    if (*p) { MultiByteToWideChar(CP_UTF8, 0, p, -1, out, MAX_PATH); if (exists(out)) return 1; }
    wchar_t d[MAX_PATH]; own_dir(d);
    swprintf(out, MAX_PATH, L"%s\\OpenRGB Windows 64-bit\\OpenRGB.exe", d);
    if (exists(out)) return 1;
    static const wchar_t *const USUAL[] = { L"%ProgramFiles%\\OpenRGB\\OpenRGB.exe", L"%LOCALAPPDATA%\\Programs\\OpenRGB\\OpenRGB.exe",
                                             L"%ProgramFiles(x86)%\\OpenRGB\\OpenRGB.exe" };
    for (int i = 0; i < 3; i++) { ExpandEnvironmentStringsW(USUAL[i], out, MAX_PATH); if (exists(out)) return 1; }
    if (running && running_copy(out)) {   // kept, so it can be started next time too
        char u[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, out, -1, u, sizeof(u), NULL, NULL);
        cfg_set_and_save("openrgb", "path", u);
        return 1;
    }
    return 0;
}

int orgbapp_auto(void) { return cfg_geti("openrgb", "auto", 1); }
int orgbapp_ours(void) { AcquireSRWLockShared(&lk); int o = job != NULL; ReleaseSRWLockShared(&lk); return o; }

// All of OpenRGB's devices given back to their own lighting: the copy started here is closed (the job ends it),
// so it holds nothing their makers' apps want. A copy the user runs is left alone.
void orgbapp_stop(void) {
    AcquireSRWLockExclusive(&lk);
    if (job) { CloseHandle(job); job = NULL; logf_("openrgb: closed, its devices have their own lighting"); }
    ReleaseSRWLockExclusive(&lk);
}

static int ensure_run(int (*answers)(void));

// Makes sure the SDK server answers: waits for a copy that is starting, or starts one. 1: it answers.
int orgbapp_ensure(int (*answers)(void)) {
    if (answers()) return 1;
    if (!orgbapp_auto()) return 0;
    static volatile LONG busy;   // one start at a time (the PC page's check, devices connecting): the others wait for it
    if (InterlockedCompareExchange(&busy, 1, 0)) {
        for (int i = 0; i < 40 && busy; i++) Sleep(1000);
        return answers();
    }
    int r = ensure_run(answers);
    InterlockedExchange(&busy, 0);
    return r;
}

static int ensure_run(int (*answers)(void)) {
    wchar_t exe[MAX_PATH], run[MAX_PATH];
    if (running_copy(run)) {   // e.g. OpenRGB's own autostart, still finding the hardware
        for (int i = 0; i < 20; i++) { Sleep(1000); if (answers()) return 1; }
        logf_("openrgb: runs but its SDK server is off (Settings → SDK Server in OpenRGB)");
        return 0;
    }
    if (!orgbapp_exe(exe, 1)) return 0;
    AcquireSRWLockExclusive(&lk);
    if (!job) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // closes with this app
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    wchar_t cmd[MAX_PATH + 32], dir[MAX_PATH];
    swprintf(cmd, MAX_PATH + 32, L"\"%s\" --server", exe);
    wcscpy_s(dir, MAX_PATH, exe); wchar_t *s = wcsrchr(dir, L'\\'); if (s) *s = 0;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    int ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi);
    if (ok) {
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        logf_("openrgb: started %ls --server", exe);
    } else logf_("openrgb: could not start %ls (%lu)", exe, GetLastError());
    ReleaseSRWLockExclusive(&lk);
    if (!ok) return 0;
    for (int i = 0; i < 30; i++) { Sleep(1000); if (answers()) return 1; }
    logf_("openrgb: started, but its SDK server does not answer");
    return 0;
}

// ---- adding what it finds
static int removed_by_hand(const char *name) {
    char q[80]; snprintf(q, sizeof(q), "|%s|", name);
    char all[1100]; snprintf(all, sizeof(all), "|%s|", cfg_get("openrgb", "removed", ""));
    return strstr(all, q) != NULL;
}

// a device removed by hand is remembered, so it is not added by itself again
void orgbapp_removed(const char *name) {
    if (!name || !*name || removed_by_hand(name)) return;
    char v[1024]; const char *old = cfg_get("openrgb", "removed", "");
    snprintf(v, sizeof(v), "%s%s%s", old, *old ? "|" : "", name);
    for (char *p = v; *p; p++) if (*p == ';' || *p == '[' || *p == ']' || *p == '=') *p = ' ';
    cfg_set_and_save("openrgb", "removed", v);
}

static int added_already(const orgb_ctl *c) {
    for (int id = 1; id <= 64; id++) {
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
        const char *k = cfg_get(sec, "kind", NULL);
        if (!k || _stricmp(k, "openrgb")) continue;
        const char *h = cfg_get(sec, "host", "");
        if (strncmp(h, "127.0.0.1", 9) && strncmp(h, "localhost", 9)) continue;
        const char *m = cfg_get(sec, "match", "");
        if (*m ? !strcmp(m, c->name) : cfg_geti(sec, "sub", -1) == c->idx) return 1;
    }
    return 0;
}

// a controller of a device this app already drives directly (a Wooting, an Elgato light, a Nanoleaf...): its brand
// in the controller's name
static int driven_directly(const char *name) {
    char n[96]; snprintf(n, sizeof(n), "%s", name); _strlwr_s(n, sizeof(n));
    for (int id = 1; id <= 64; id++) {
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
        const char *k = cfg_get(sec, "kind", NULL);
        const ext_driver *d = k && _stricmp(k, "openrgb") ? ext_driver_by_kind(k) : NULL;
        if (!d) continue;
        char brand[32]; snprintf(brand, sizeof(brand), "%s", d->title); _strlwr_s(brand, sizeof(brand));
        char *sp = strchr(brand, ' '); if (sp) *sp = 0;   // "Philips Hue" -> philips, "Nanoleaf USB" -> nanoleaf
        if (strlen(brand) >= 3 && strstr(n, brand)) return 1;
    }
    for (int k = 0; k < NANO_MAX; k++) if (nano_present(k) && strstr(n, "nanoleaf")) return 1;
    return 0;
}

void orgbapp_add_new(const orgb_ctl *c, int n) {
    if (!orgbapp_auto()) return;
    for (int i = 0; i < n; i++) {
        if (app_hw_own(c[i].type, c[i].name) || driven_directly(c[i].name) || added_already(&c[i]) || removed_by_hand(c[i].name)) continue;
        int id = ext_add("openrgb", "127.0.0.1", c[i].idx, c[i].name, 0);
        if (!id) continue;
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
        cfg_set_and_save(sec, "match", c[i].name);
        logf_("openrgb: added %s (%s) by itself", c[i].name, c[i].kind);
    }
}

// ---- setup
static void su_fail(const char *why) {
    AcquireSRWLockExclusive(&lk); snprintf(su_err, sizeof(su_err), "%s", why); ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&su_stage, SU_ERROR);
    logf_("openrgb setup: %s", why);
    ui_refresh();
}
static void su_set(int s) { InterlockedExchange(&su_stage, s); InterlockedExchange(&su_pct, 0); ui_refresh(); }

static unsigned __stdcall setup_run(void *arg) {
    (void)arg;
    wchar_t dir[MAX_PATH], zip[MAX_PATH], exe[MAX_PATH];
    own_dir(dir);
    SHCreateDirectoryExW(NULL, dir, NULL);
    swprintf(zip, MAX_PATH, L"%s\\OpenRGB.zip", dir);
    su_set(SU_DOWNLOAD);
    logf_("openrgb setup: downloading OpenRGB 1.0");
    if (!https_download(ORGB_ZIP_URL, zip, ORGB_ZIP_SIZE, &su_pct)) { DeleteFileW(zip); su_fail("download"); return 0; }
    su_set(SU_VERIFY);
    WIN32_FILE_ATTRIBUTE_DATA fa; char hex[65] = "";
    long long size = GetFileAttributesExW(zip, GetFileExInfoStandard, &fa) ? ((long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) : -1;
    if (size != ORGB_ZIP_SIZE || !sha256_hex(zip, hex) || strcmp(hex, ORGB_ZIP_SHA)) {
        logf_("openrgb setup: the download is not OpenRGB 1.0's zip (%lld bytes, sha256 %s)", size, hex);
        DeleteFileW(zip); su_fail("verify"); return 0;
    }
    su_set(SU_UNPACK);
    wchar_t tar[MAX_PATH], cmd[MAX_PATH * 3];
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\tar.exe", tar, MAX_PATH);
    swprintf(cmd, MAX_PATH * 3, L"\"%s\" -xf \"%s\" -C \"%s\"", tar, zip, dir);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi; DWORD code = 1;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    DeleteFileW(zip);
    swprintf(exe, MAX_PATH, L"%s\\OpenRGB Windows 64-bit\\OpenRGB.exe", dir);
    if (code || !exists(exe)) { logf_("openrgb setup: unpacking ended with %lu", code); su_fail("unpack"); return 0; }
    char u[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, exe, -1, u, sizeof(u), NULL, NULL);
    cfg_set_and_save("openrgb", "path", u);
    if (!cfg_get("openrgb", "auto", NULL)) cfg_set_and_save("openrgb", "auto", "1");
    logf_("openrgb setup: OpenRGB 1.0 is in %ls", dir);
    su_set(SU_DONE);
    orgb_check_start();   // start it, see what it finds, add it
    return 0;
}

void orgbapp_setup(void) {
    LONG s = su_stage;
    if (s == SU_DOWNLOAD || s == SU_VERIFY || s == SU_UNPACK) return;
    su_set(SU_DOWNLOAD);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, setup_run, NULL, 0, NULL);
    if (h) CloseHandle(h); else su_fail("thread");
}

// "exe":1,"auto":1,"ours":0,"setup":0,"pct":0,"err":""
int orgbapp_json(char *out, int cap) {
    wchar_t exe[MAX_PATH];
    AcquireSRWLockShared(&lk); char e[16]; snprintf(e, sizeof(e), "%s", su_err); int ours = job != NULL; ReleaseSRWLockShared(&lk);
    return snprintf(out, cap, "\"exe\":%d,\"auto\":%d,\"ours\":%d,\"setup\":%ld,\"pct\":%ld,\"err\":\"%s\"",
                    orgbapp_exe(exe, 0), orgbapp_auto(), ours, su_stage, su_pct, e);
}
