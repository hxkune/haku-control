// SPDX-License-Identifier: GPL-3.0-only
// Updates: once a day asks GitHub for the latest release of the repo the build came from and tells the settings
// window when it is newer (off with [general] update_check=0). "Update" downloads that release's
// haku-control-setup.exe, checks it (the size and SHA-256 GitHub lists for it; once releases are signed, and this
// exe is, the signature too) and runs it with /update: it installs without questions, keeps autostart as it was,
// closes this app and starts the new one.
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <shellapi.h>
#include <winhttp.h>
#include <softpub.h>
#include <wintrust.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef HAKU_REPO
#define HAKU_REPO "hxkune/haku-control"   // the official repo; build.cmd / CI can point a fork at its own (HAKU_REPO)
#endif

static char   latest[32], page[256];
static char   asset_url[512], asset_sha[80];   // haku-control-setup.exe of the latest release
static long long asset_size;
static volatile LONG inst_state, inst_pct;     // INST_*
static char   inst_err[24];
enum { INST_IDLE, INST_DOWNLOAD, INST_VERIFY, INST_RUN, INST_ERROR };
static SRWLOCK lk = SRWLOCK_INIT;
static HANDLE th, stop_ev, now_ev;

static const char *repo(void) {
    const char *r = cfg_get("general", "update_repo", "");
    return *r ? r : HAKU_REPO;
}

// "v1.2.3" / "1.2" -> comparable number; -1 when it is not a version
static long long ver_num(const char *s) {
    if (*s == 'v' || *s == 'V') s++;
    long long v = 0; int parts = 0;
    while (parts < 4) {
        if (*s < '0' || *s > '9') break;
        long p = strtol(s, (char **)&s, 10);
        v = v * 100000 + p; parts++;
        if (*s != '.') break;
        s++;
    }
    if (!parts) return -1;
    while (parts++ < 4) v *= 100000;
    return v;
}

static int https_get_status(const wchar_t *host, const wchar_t *path, char *out, int cap, int *status) {
    int n = 0, ok = 0;
    if (status) *status = 0;
    HINTERNET ses = WinHttpOpen(L"haku-control/" HAKU_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return -1;
    WinHttpSetTimeouts(ses, 5000, 5000, 10000, 10000);
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, NULL, 0, 0, 0)
            && WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        DWORD got;
        while (n < cap - 1 && WinHttpReadData(req, out + n, cap - 1 - n, &got) && got) n += got;
        ok = code == 200;
        if (status) *status = (int)code;
    }
    out[n] = 0;
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok ? n : -1;
}

static int https_get(const wchar_t *host, const wchar_t *path, char *out, int cap) { return https_get_status(host, path, out, cap, NULL); }

// ---------------------------------------------------------------- versions the author has stopped
// policy/policy.txt in the official repository, signed with the author's key (tools/policy/sign.ps1): a version
// below "min", or one named in "blocked", lets go of the lights, shows the author's message with a download link
// and quits. Read at start from the copy kept at the last check (so it holds offline too) and online a minute after
// start and then daily. Unlike the update check it is not switched off by update_check and does not follow
// update_repo. A missing file (404) lifts an earlier block; an unsigned or wrongly signed one changes nothing.
#define POLICY_HOST L"raw.githubusercontent.com"
#define POLICY_PATH L"/hxkune/haku-control/main/policy/policy.txt"
// ECDSA P-256 public key, X then Y (tools/policy/new-key.ps1)
static const char POLICY_KEY[] = "26bcaa7201ce992cc851f0a20ad1a6fb882d7d2b8a8e8bcdc1f7676c9b5e368cce635985a2c333138ffc4d31507449cbe39ea36ff52385d9ce5dc11894d3a2d2";

// line 1: base64 signature (r||s) of everything after it. Returns the signed JSON, or NULL.
static const char *policy_verify(const char *txt) {
    const char *nl = strchr(txt, '\n');
    if (!nl || nl - txt > 200) return NULL;
    char b64[208]; int bl = (int)(nl - txt); memcpy(b64, txt, bl); b64[bl] = 0;
    if (bl && b64[bl - 1] == '\r') b64[--bl] = 0;
    BYTE sig[80]; DWORD sl = sizeof(sig);
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, sig, &sl, NULL, NULL) || sl != 64) return NULL;
    const char *json = nl + 1;
    BYTE hash[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)json, (ULONG)strlen(json), hash, 32)) return NULL;
    struct { BCRYPT_ECCKEY_BLOB h; BYTE xy[64]; } blob = { { BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32 } };
    for (int i = 0; i < 64; i++) { unsigned v; sscanf_s(POLICY_KEY + 2 * i, "%2x", &v); blob.xy[i] = (BYTE)v; }
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_KEY_HANDLE key = NULL; int ok = 0;
    if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) &&
        !BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob, sizeof(blob), 0))
        ok = BCryptVerifySignature(key, NULL, hash, 32, sig, 64, 0) == 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? json : NULL;
}

// 1: the signed policy stops this version; msg (in the app's language) and url filled
static int policy_stops(const char *json, char *msg, int mcap, char *url, int ucap) {
    const char *mine = HAKU_VER_STR;
#ifdef HAKU_DEV
    mine = cfg_get("general", "policy_fake_version", mine);   // test builds: pretend to be another version
#endif
    char min_v[32] = "";
    json_get_str(json, "min", min_v, sizeof(min_v));
    int stop = ver_num(min_v) > 0 && ver_num(mine) < ver_num(min_v);
    const char *b = strstr(json, "\"blocked\""), *e = b ? strchr(b, ']') : NULL;
    char q[40]; snprintf(q, sizeof(q), "\"%s\"", mine);
    if (b && e) { const char *f = strstr(b, q); if (f && f < e) stop = 1; }
    if (!stop) return 0;
    if (!json_get_str(json, TR("msg_en", "msg_ru", "msg_fr"), msg, mcap)) json_get_str(json, "msg_en", msg, mcap);
    if (!json_get_str(json, "url", url, ucap) || strncmp(url, "https://", 8)) snprintf(url, ucap, "https://github.com/%s/releases/latest", HAKU_REPO);
    if (!msg[0]) snprintf(msg, mcap, "%s", TR("This version of haku control is no longer supported.", "Эта версия haku control больше не поддерживается.", "Cette version de haku control n'est plus prise en charge."));
    return 1;
}

static void policy_cache(wchar_t *p) { app_data_path(L"policy.txt", p); }

static int read_file(const wchar_t *p, char *out, int cap) {
    FILE *f = _wfopen(p, L"rb"); if (!f) return 0;
    int n = (int)fread(out, 1, cap - 1, f); fclose(f);
    out[n > 0 ? n : 0] = 0;
    return n > 0;
}

// The published policy: fetched, its signature checked, kept. 1: it stops this version (msg / url filled),
// 0: it does not (or none is published any more), -1: offline or not signed right (the kept copy stands).
static int policy_fetch(char *msg, int mcap, char *url, int ucap) {
    static char txt[16 * 1024];
    int status = 0, n = -1;
#ifdef HAKU_DEV
    const char *pf = cfg_get("general", "policy_file", "");   // test builds: a local file instead
    if (*pf) { wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, pf, -1, w, MAX_PATH); status = read_file(w, txt, sizeof(txt)) ? 200 : 404; n = status == 200 ? (int)strlen(txt) : -1; }
    else
#endif
    n = https_get_status(POLICY_HOST, POLICY_PATH, txt, sizeof(txt), &status);
    wchar_t p[MAX_PATH]; policy_cache(p);
    if (status == 404) { if (DeleteFileW(p)) logf_("policy: none published any more"); return 0; }
    if (n < 0) return -1;
    const char *json = policy_verify(txt);
    if (!json) { logf_("policy: the published file is not signed right; ignored"); return -1; }
    FILE *f = _wfopen(p, L"wb"); if (f) { fwrite(txt, 1, strlen(txt), f); fclose(f); }
    return policy_stops(json, msg, mcap, url, ucap);
}

// at start: the policy kept from the last check; when that stops this version, the published one is asked first
// (the author may have lifted it)
int policy_blocked(char *msg, int mcap, char *url, int ucap) {
    static char txt[16 * 1024]; wchar_t p[MAX_PATH]; policy_cache(p);
    const char *json;
    if (!read_file(p, txt, sizeof(txt)) || !(json = policy_verify(txt)) || !policy_stops(json, msg, mcap, url, ucap)) return 0;
    char m2[1024] = "", u2[256] = "";
    int now = policy_fetch(m2, sizeof(m2), u2, sizeof(u2));
    if (now == 0) { logf_("policy: lifted"); msg[0] = url[0] = 0; return 0; }
    if (now == 1) { snprintf(msg, mcap, "%s", m2); snprintf(url, ucap, "%s", u2); }
    return 1;
}

// online, daily: tells the app when this version is stopped
static void policy_check(void) {
    char msg[1024] = "", url[256] = "";
    if (policy_fetch(msg, sizeof(msg), url, sizeof(url)) == 1) { logf_("policy: version %s is stopped by its author", HAKU_VER_STR); app_blocked(msg, url); }
}

static void check(void) {
    char r[128]; snprintf(r, sizeof(r), "%s", repo());
    if (!*r || !strchr(r, '/') || strpbrk(r, "?#% ")) return;
    wchar_t path[200]; swprintf(path, 200, L"/repos/%hs/releases/latest", r);
    static char body[64 * 1024];
    if (https_get(L"api.github.com", path, body, sizeof(body)) < 0) { logf_("update: check failed"); return; }
    char tag[32] = "", url[256] = "";
    json_get_str(body, "tag_name", tag, sizeof(tag));
    json_get_str(body, "html_url", url, sizeof(url));
    if (ver_num(tag) < 0 || strncmp(url, "https://github.com/", 19)) return;
    const char *mine = HAKU_VER_STR;
#ifdef HAKU_DEV
    mine = cfg_get("general", "update_fake_version", mine);   // test builds: pretend to be older
#endif
    int newer = ver_num(tag) > ver_num(mine);
    // the installer among the release's files: its download address, size and SHA-256
    char au[512] = "", sha[80] = ""; long long asz = 0;
    const char *a = strstr(body, "\"name\":\"haku-control-setup.exe\"");
    if (a) {
        json_get_str(a, "browser_download_url", au, sizeof(au));
        asz = (long long)json_get_num(a, "size", 0);
        char dg[96] = ""; json_get_str(a, "digest", dg, sizeof(dg));
        if (!strncmp(dg, "sha256:", 7) && strlen(dg + 7) == 64) snprintf(sha, sizeof(sha), "%s", dg + 7);
    }
    char want[300]; snprintf(want, sizeof(want), "https://github.com/%s/releases/download/", r);
    if (_strnicmp(au, want, strlen(want))) au[0] = 0;   // only from this repo's releases
    AcquireSRWLockExclusive(&lk);
    if (newer) { snprintf(latest, sizeof(latest), "%s", *tag == 'v' || *tag == 'V' ? tag + 1 : tag); snprintf(page, sizeof(page), "%s", url); }
    else latest[0] = page[0] = 0;
    snprintf(asset_url, sizeof(asset_url), "%s", newer ? au : ""); snprintf(asset_sha, sizeof(asset_sha), "%s", newer ? sha : ""); asset_size = newer ? asz : 0;
    ReleaseSRWLockExclusive(&lk);
    logf_("update: latest release %s%s", tag, newer ? " (newer)" : "");
}

static unsigned __stdcall run(void *arg) {
    (void)arg;
    HANDLE ev[2] = { stop_ev, now_ev };
    DWORD wait = 60 * 1000;   // not while the PC is still starting
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, ev, FALSE, wait);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_TIMEOUT) policy_check();   // a minute after start, then daily, whatever update_check says
        if (r == WAIT_OBJECT_0 + 1 || atoi(cfg_get("general", "update_check", "1"))) check();   // asked for, or daily
        if (r == WAIT_TIMEOUT) wait = 24 * 3600 * 1000;
    }
    return 0;
}

// installers downloaded by an earlier update (the one that ran is no longer in use)
static void clean_downloads(void) {
    wchar_t dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-control-update");
    swprintf(pat, MAX_PATH, L"%s\\*.exe", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do { swprintf(f, MAX_PATH, L"%s\\%s", dir, fd.cFileName); DeleteFileW(f); } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

void update_start(void) {
    clean_downloads();
    if (th || !*repo()) return;
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    now_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, run, NULL, 0, NULL);
}

void update_stop(void) {
    if (!th) return;
    SetEvent(stop_ev);
    WaitForSingleObject(th, 3000);
    CloseHandle(th); CloseHandle(stop_ev); CloseHandle(now_ev);
    th = NULL;
}

void update_check_now(void) {
    if (!th) update_start();
    if (th) SetEvent(now_ev);
}

// ---------------------------------------------------------------- one-click update
static void inst_fail(const char *why) {
    AcquireSRWLockExclusive(&lk); snprintf(inst_err, sizeof(inst_err), "%s", why); ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&inst_state, INST_ERROR);
    logf_("update: %s", why);
    ui_refresh();
}

// HTTPS download to a file, following GitHub's redirect to its file host; progress in inst_pct
static int download(const char *url, const wchar_t *to, long long *got_bytes) {
    wchar_t wurl[512]; MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 512);
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return 0;
    int ok = 0; *got_bytes = 0;
    HINTERNET ses = WinHttpOpen(L"haku-control/" HAKU_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 10000, 10000, 20000, 30000);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    FILE *f = NULL;
    if (req && WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        wchar_t cl[32] = L""; DWORD cls = sizeof(cl);
        long long total = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &cls, NULL) ? _wtoi64(cl) : asset_size;
        if (code == 200 && (f = _wfopen(to, L"wb")) != NULL) {
            static char chunk[64 * 1024]; DWORD n;
            ok = 1;
            while (WinHttpReadData(req, chunk, sizeof(chunk), &n) && n) {
                if (fwrite(chunk, 1, n, f) != n) { ok = 0; break; }
                *got_bytes += n;
                LONG pct = total > 0 ? (LONG)(*got_bytes * 100 / total) : 0;
                if (pct != inst_pct) { InterlockedExchange(&inst_pct, pct); ui_refresh(); }   // the window shows the progress
                if (*got_bytes > 64LL * 1024 * 1024) { ok = 0; break; }   // an installer is ~2 MB; this is not one
            }
            fclose(f);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static int sha256_file(const wchar_t *file, char *hex) {
    FILE *f = _wfopen(file, L"rb");
    if (!f) return 0;
    BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE h; BYTE d[32];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0);
    static BYTE buf[64 * 1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) BCryptHashData(h, buf, (ULONG)n, 0);
    fclose(f);
    BCryptFinishHash(h, d, 32, 0);
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a, 0);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
    return 1;
}

// Authenticode check (the file's own signature, no revocation lookups over the network)
static int signed_ok(const wchar_t *file) {
    WINTRUST_FILE_INFO fi = { sizeof(fi) }; fi.pcwszFilePath = file;
    WINTRUST_DATA wd = { sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE; wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY; wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(NULL, &act, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(NULL, &act, &wd);
    return r == 0;
}

static unsigned __stdcall install_run(void *arg) {
    (void)arg;
    char url[512], sha[80], ver[32]; long long size;
    AcquireSRWLockShared(&lk);
    snprintf(url, sizeof(url), "%s", asset_url); snprintf(sha, sizeof(sha), "%s", asset_sha); snprintf(ver, sizeof(ver), "%s", latest); size = asset_size;
    ReleaseSRWLockShared(&lk);
    if (!url[0]) { inst_fail("nofile"); return 0; }
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-control-update");
    CreateDirectoryW(dir, NULL);
    swprintf(file, MAX_PATH, L"%s\\haku-control-setup-%hs.exe", dir, ver);
    logf_("update: downloading %s", url);
    long long got = 0;
    InterlockedExchange(&inst_pct, 0);
    if (!download(url, file, &got)) { DeleteFileW(file); inst_fail("download"); return 0; }

    InterlockedExchange(&inst_state, INST_VERIFY); ui_refresh();
    char hex[65] = "";
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    int ok = got > 100 * 1024 && (!size || got == size) && sha256_file(file, hex) && (!sha[0] || !_stricmp(hex, sha));
    FILE *f = ok ? _wfopen(file, L"rb") : NULL; char mz[2] = "";
    if (f) { fread(mz, 1, 2, f); fclose(f); }
    ok = ok && mz[0] == 'M' && mz[1] == 'Z';
    if (ok && signed_ok(self) && !signed_ok(file)) ok = 0;   // a signed app only takes a signed update
    if (!ok) { logf_("update: check failed (%lld bytes, sha256 %s, expected %s)", got, hex, sha[0] ? sha : "-"); DeleteFileW(file); inst_fail("verify"); return 0; }
    logf_("update: %s checked (%lld bytes%s)", ver, got, sha[0] ? ", sha256 matches" : "");

    InterlockedExchange(&inst_state, INST_RUN); ui_refresh();
#ifdef HAKU_DEV
    logf_("update: test build, not running the installer (%ls)", file);
    Sleep(1500);
    inst_fail("test");
    return 0;
#else
    wchar_t args[64]; swprintf(args, 64, L"/update /autostart=%d", app_autostart(-1) == 0 ? 0 : 1);
    SHELLEXECUTEINFOW si = { sizeof(si) };
    si.lpVerb = L"open"; si.lpFile = file; si.lpParameters = args; si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si)) { inst_fail("run"); return 0; }
    logf_("update: installer started, it closes this app");
    return 0;   // the installer stops this app, installs and starts the new version
#endif
}

void update_install(void) {
    LONG s = inst_state;
    if (s == INST_DOWNLOAD || s == INST_VERIFY || s == INST_RUN) return;
    InterlockedExchange(&inst_state, INST_DOWNLOAD);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, install_run, NULL, 0, NULL);
    if (h) CloseHandle(h); else inst_fail("thread");
}

// {"version":"0.2.0","repo":1,"latest":"0.3.0","url":"https://github.com/...","can":1,"inst":0,"pct":0,"err":""}
// latest/url empty when up to date; can: the release has an installer to take; inst: INST_*
int update_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"version\":\"%s\",\"repo\":%d,\"latest\":\"%s\",\"url\":\"%s\",\"can\":%d,\"inst\":%ld,\"pct\":%ld,\"err\":\"%s\"}",
                     HAKU_VER_STR, *repo() != 0, latest, page, asset_url[0] != 0, inst_state, inst_pct, inst_err);
    ReleaseSRWLockShared(&lk);
    return n;
}

void update_open_page(void) {
    wchar_t w[256] = L"";
    AcquireSRWLockShared(&lk);
    if (!strncmp(page, "https://github.com/", 19)) MultiByteToWideChar(CP_UTF8, 0, page, -1, w, 256);
    ReleaseSRWLockShared(&lk);
    if (*w) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}
