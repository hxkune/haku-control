// SPDX-License-Identifier: GPL-3.0-only
// Ollama for the mood picker (mood.c): only in memory while it works.
//  - The model is dropped from memory right after each answer (keep_alive 0, in mood.c).
//  - When Ollama is not running, a request starts "ollama serve" hidden (in a job object, so it never outlives this
//    app) and stops it again IDLE_MS after the last request. An Ollama that runs on its own is only used.
//  - [mood] ollama_on_demand=1 keeps Ollama's own app out of Windows startup (its Startup shortcut is moved into
//    %APPDATA%\haku-control, shared by the test build, and back, or made anew) and closes it, so it runs only for a
//    request. Ollama's updates put the shortcut back: it is moved again when this app starts.
//  - Setup from the window: Ollama's installer from ollama.com (checked: an Authenticode signature by Ollama),
//    run without questions, then the model (POST /api/pull, with progress).
#define COBJMACROS
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <softpub.h>
#include <wintrust.h>
#include <tlhelp32.h>
#include <process.h>
#include <stdlib.h>

#define OLLAMA_PORT   11434
#define IDLE_MS       30000
#define SETUP_URL     L"https://ollama.com/download/OllamaSetup.exe"
#define DEFAULT_MODEL "gemma3:4b"

enum { ST_IDLE, ST_DOWNLOAD, ST_VERIFY, ST_INSTALL, ST_START, ST_PULL, ST_DONE, ST_ERROR };

static SRWLOCK lk = SRWLOCK_INIT;
static HANDLE  job, srv;                    // "ollama serve" started here
static int     users;                       // requests using it now
static DWORD   last_use;
static volatile LONG stage, pct;
static char    st_err[24];

// ---------------------------------------------------------------- where it is
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// ollama.exe: [mood] ollama_path, the per-user install folder, or PATH
static int ollama_exe(wchar_t *out) {
    const char *p = cfg_get("mood", "ollama_path", "");
    if (*p) { MultiByteToWideChar(CP_UTF8, 0, p, -1, out, MAX_PATH); return exists(out); }
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Programs\\Ollama\\ollama.exe", out, MAX_PATH);
    if (exists(out)) return 1;
    return SearchPathW(NULL, L"ollama.exe", NULL, MAX_PATH, out, NULL) > 0;
}

// any model pulled (manifests under %OLLAMA_MODELS% or ~\.ollama\models), without asking the server
static int have_model(void) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    if (!GetEnvironmentVariableW(L"OLLAMA_MODELS", dir, MAX_PATH)) ExpandEnvironmentStringsW(L"%USERPROFILE%\\.ollama\\models", dir, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%s\\manifests\\registry.ollama.ai\\library\\*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int any = 0;
    do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') any = 1; while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

static int alive(void) {
    char buf[512];
    return http_request("127.0.0.1", OLLAMA_PORT, "GET", "/api/version", NULL, buf, sizeof(buf)) == 200;
}

static void startup_lnk(wchar_t *out) {
    out[0] = 0;
    if (SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, out) == S_OK) wcscat_s(out, MAX_PATH, L"\\Ollama.lnk");
}

// where the Startup shortcut waits while Ollama runs only when needed (the installed app's folder, also for the
// test build, so either can put it back)
static void kept_lnk(wchar_t *out) { ExpandEnvironmentStringsW(L"%APPDATA%\\haku-control\\ollama-startup.lnk", out, MAX_PATH); }

// a new Startup shortcut to Ollama's own app (when the one moved away is gone)
static int make_startup_lnk(const wchar_t *lnk) {
    wchar_t exe[MAX_PATH], app[MAX_PATH];
    if (!ollama_exe(exe)) return 0;
    wcscpy_s(app, MAX_PATH, exe);
    wchar_t *s = wcsrchr(app, L'\\'); if (!s) return 0;
    wcscpy_s(s + 1, MAX_PATH - (s + 1 - app), L"ollama app.exe");
    if (!exists(app)) return 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = NULL; int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
        IShellLinkW_SetPath(sl, app);
        *s = 0; IShellLinkW_SetWorkingDirectory(sl, app);
        IPersistFile *pf = NULL;
        if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) { ok = SUCCEEDED(IPersistFile_Save(pf, lnk, TRUE)); IPersistFile_Release(pf); }
        IShellLinkW_Release(sl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

// ---------------------------------------------------------------- the server, started for requests
static void stop_server(void) {   // (lk held)
    if (job) { TerminateJobObject(job, 0); CloseHandle(job); job = NULL; }
    if (srv) { CloseHandle(srv); srv = NULL; }
}

static unsigned __stdcall idle_watch(void *arg) {
    (void)arg;
    for (;;) {
        Sleep(5000);
        AcquireSRWLockExclusive(&lk);
        int gone = !srv;
        if (!gone && !users && GetTickCount() - last_use > IDLE_MS) { stop_server(); gone = 1; logf_("ollama: stopped (not needed)"); }
        ReleaseSRWLockExclusive(&lk);
        if (gone) return 0;
    }
}

static int start_server(void) {   // (lk held)
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    if (!ollama_exe(exe)) return 0;
    job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = { 0 };
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;   // gone with this app, whatever happens
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    swprintf(cmd, MAX_PATH + 16, L"\"%s\" serve", exe);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        logf_("ollama: could not start %ls (%lu)", exe, GetLastError());
        if (job) { CloseHandle(job); job = NULL; }
        return 0;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    srv = pi.hProcess;
    for (int i = 0; i < 120; i++) {   // up to 30 s
        if (alive()) {
            logf_("ollama: started for a request");
            HANDLE t = (HANDLE)_beginthreadex(NULL, 0, idle_watch, NULL, 0, NULL);
            if (t) CloseHandle(t);
            return 1;
        }
        if (WaitForSingleObject(srv, 250) == WAIT_OBJECT_0) break;
    }
    logf_("ollama: started but does not answer");
    stop_server();
    return 0;
}

// 1: Ollama answers (started here if needed); pair with ollama_release()
int ollama_acquire(void) {
    AcquireSRWLockExclusive(&lk);
    users++; last_use = GetTickCount();
    int ok = alive() || (!srv && start_server()) || (srv && alive());
    if (!ok) users--;
    ReleaseSRWLockExclusive(&lk);
    return ok;
}

void ollama_release(void) {
    AcquireSRWLockExclusive(&lk);
    if (users > 0) users--;
    last_use = GetTickCount();
    ReleaseSRWLockExclusive(&lk);
}

int ollama_installed(void) { wchar_t exe[MAX_PATH]; return ollama_exe(exe); }

// ---------------------------------------------------------------- only when needed
// Closes Ollama's own app and its server (not one started here).
static void close_ollama_app(void) {
    wchar_t exe[MAX_PATH];
    if (!ollama_exe(exe)) return;
    wchar_t *s = wcsrchr(exe, L'\\'); if (s) s[1] = 0;   // its folder
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"ollama app.exe") && _wcsicmp(pe.szExeFile, L"ollama.exe")) continue;
        HANDLE p = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t img[MAX_PATH]; DWORD n = MAX_PATH; BOOL mine = FALSE;
        if (job) IsProcessInJob(p, job, &mine);
        if (!mine && QueryFullProcessImageNameW(p, 0, img, &n) && !_wcsnicmp(img, exe, wcslen(exe))) {
            TerminateProcess(p, 0); WaitForSingleObject(p, 2000);
            logf_("ollama: closed %ls (runs only when needed)", pe.szExeFile);
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
}

void ollama_set_on_demand(int on) {
    cfg_set_and_save("mood", "ollama_on_demand", on ? "1" : "0");
    wchar_t lnk[MAX_PATH], kept[MAX_PATH], dir[MAX_PATH];
    startup_lnk(lnk); kept_lnk(kept);
    wcscpy_s(dir, MAX_PATH, kept); wchar_t *s = wcsrchr(dir, L'\\'); if (s) { *s = 0; CreateDirectoryW(dir, NULL); }
    if (on) {
        if (lnk[0] && exists(lnk) && MoveFileExW(lnk, kept, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) logf_("ollama: left out of Windows startup");
        close_ollama_app();
    } else if (lnk[0] && !exists(lnk) && ((exists(kept) && MoveFileExW(kept, lnk, MOVEFILE_COPY_ALLOWED)) || make_startup_lnk(lnk))) {
        logf_("ollama: back in Windows startup (from the next sign-in)");
    }
    ui_refresh();
}

// at start: Ollama's updates put its Startup shortcut back
void ollama_init(void) {
    if (!cfg_geti("mood", "ollama_on_demand", 0)) return;
    wchar_t lnk[MAX_PATH]; startup_lnk(lnk);
    if (lnk[0] && exists(lnk)) ollama_set_on_demand(1);
}

// ---------------------------------------------------------------- setup
static void fail(const char *why) {
    AcquireSRWLockExclusive(&lk); snprintf(st_err, sizeof(st_err), "%s", why); ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&stage, ST_ERROR);
    logf_("ollama setup: %s", why);
    ui_refresh();
}
static void set_stage(int s, int p) { InterlockedExchange(&stage, s); InterlockedExchange(&pct, p); ui_refresh(); }

// HTTPS download (WinHTTP follows ollama.com's redirect to its file host), progress in pct
static int download(const wchar_t *url, const wchar_t *to) {
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
                if (p != pct) { InterlockedExchange(&pct, p); ui_refresh(); }
                if (got > 6LL * 1024 * 1024 * 1024) { ok = 0; break; }   // far more than Ollama's installer
            }
            if (total > 0 && got != total) ok = 0;
            fclose(f);
        } else logf_("ollama setup: download answered %lu", code);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// A valid Authenticode signature whose signer's name has `who` in it (no revocation lookups over the network)
static int signed_by(const wchar_t *file, const wchar_t *who) {
    WINTRUST_FILE_INFO fi = { sizeof(fi) }; fi.pcwszFilePath = file;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE; wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    int ok = 0;
    if (WinVerifyTrust(NULL, &act, &wd) == 0) {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
        if (sg && sg->csCertChain && sg->pasCertChain[0].pCert) {
            wchar_t nm[256] = L"";
            CertGetNameStringW(sg->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, nm, 256);
            logf_("ollama setup: installer signed by %ls", nm);
            ok = wcsstr(nm, who) != NULL;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(NULL, &act, &wd);
    return ok;
}

static int install_ollama(void) {
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-control-ollama");
    CreateDirectoryW(dir, NULL);
    swprintf(file, MAX_PATH, L"%s\\OllamaSetup.exe", dir);
    set_stage(ST_DOWNLOAD, 0);
    logf_("ollama setup: downloading the installer");
    if (!download(SETUP_URL, file)) { DeleteFileW(file); fail("download"); return 0; }
    set_stage(ST_VERIFY, 0);
    if (!signed_by(file, L"Ollama")) { DeleteFileW(file); fail("verify"); return 0; }
#ifdef HAKU_DEV
    if (cfg_geti("mood", "setup_test", 0)) { logf_("ollama setup: test build, installer checked, not run"); DeleteFileW(file); fail("test"); return 0; }
#endif
    set_stage(ST_INSTALL, 0);
    // Inno Setup: no windows, no questions; it installs for this user (no admin rights needed)
    wchar_t cmd[MAX_PATH + 64]; swprintf(cmd, MAX_PATH + 64, L"\"%s\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART", file);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { fail("run"); return 0; }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 15 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(file);
    if (code != 0 || !ollama_installed()) { logf_("ollama setup: installer ended with %lu", code); fail("install"); return 0; }
    logf_("ollama setup: Ollama installed");
    // installed from here: it runs only when needed (unless that was switched off before)
    if (!cfg_get("mood", "ollama_on_demand", NULL) || cfg_geti("mood", "ollama_on_demand", 0)) { Sleep(3000); ollama_set_on_demand(1); }
    return 1;
}

// POST /api/pull, streamed: one JSON line per step, with total / completed while the layers download
static int pull_model(const char *name) {
    SOCKET s = tcp_connect("127.0.0.1", OLLAMA_PORT, 3000);
    if (s == INVALID_SOCKET) return 0;
    DWORD to = 300000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    char body[160], head[256];
    int bl = snprintf(body, sizeof(body), "{\"model\":\"%s\"}", name);
    int hl = snprintf(head, sizeof(head), "POST /api/pull HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", bl);
    int ok = tcp_send_all(s, head, hl) && tcp_send_all(s, body, bl), done = 0, in_body = 0;
    static char buf[64 * 1024]; int n = 0;
    while (ok && !done) {
        int r = recv(s, buf + n, sizeof(buf) - 1 - n, 0);
        if (r <= 0) break;
        n += r; buf[n] = 0;
        char *p = buf;
        if (!in_body) { char *e = strstr(buf, "\r\n\r\n"); if (!e) continue; p = e + 4; in_body = 1; }
        for (char *nl; (nl = strchr(p, '\n')) != NULL; p = nl + 1) {
            *nl = 0;
            char err[200] = "", st[64] = "";
            if (json_get_str(p, "error", err, sizeof(err))) { logf_("ollama setup: pull: %s", err); ok = 0; break; }
            json_get_str(p, "status", st, sizeof(st));
            if (!strcmp(st, "success")) { done = 1; break; }
            double total = json_get_num(p, "total", 0), got = json_get_num(p, "completed", 0);
            if (total > 1e6) { LONG q = (LONG)(got * 100 / total); if (q != pct) { InterlockedExchange(&pct, q); ui_refresh(); } }
        }
        n = (int)strlen(p); memmove(buf, p, n + 1);
    }
    closesocket(s);
    return ok && done;
}

static unsigned __stdcall setup_run(void *arg) {
    (void)arg;
    int force = 0;
#ifdef HAKU_DEV
    force = cfg_geti("mood", "setup_test", 0);   // test build: the installer's download and check, with Ollama installed
#endif
    if ((force || !ollama_installed()) && !install_ollama()) return 0;
    set_stage(ST_START, 0);
    if (!ollama_acquire()) { fail("start"); return 0; }
    char model[96]; snprintf(model, sizeof(model), "%s", cfg_get("mood", "model", DEFAULT_MODEL));
    set_stage(ST_PULL, 0);
    logf_("ollama setup: pulling %s", model);
    int ok = pull_model(model);
    ollama_release();
    if (!ok) { fail("pull"); return 0; }
    logf_("ollama setup: %s ready", model);
    set_stage(ST_DONE, 100);
    return 0;
}

void ollama_setup(void) {
    LONG s = stage;
    if (s != ST_IDLE && s != ST_DONE && s != ST_ERROR) return;
    set_stage(ST_START, 0);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, setup_run, NULL, 0, NULL);
    if (h) CloseHandle(h); else fail("thread");
}

// {"exe":1,"model":1,"up":0,"ours":0,"on_demand":1,"startup":0,"stage":0,"pct":0,"err":""}
// stage: ST_* (the setup); startup: Ollama's own app starts with Windows
int ollama_json(char *out, int cap) {
    wchar_t lnk[MAX_PATH]; startup_lnk(lnk);
    AcquireSRWLockShared(&lk);
    int ours = srv != NULL;
    char e[24]; snprintf(e, sizeof(e), "%s", st_err);
    ReleaseSRWLockShared(&lk);
    return snprintf(out, cap, "{\"exe\":%d,\"model\":%d,\"ours\":%d,\"on_demand\":%d,\"startup\":%d,\"stage\":%ld,\"pct\":%ld,\"err\":\"%s\"}",
                    ollama_installed(), have_model(), ours, cfg_geti("mood", "ollama_on_demand", 0), lnk[0] && exists(lnk),
                    stage, pct, e);
}
