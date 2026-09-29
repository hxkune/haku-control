// Update check: once a day asks GitHub for the latest release of the repo the build came from and tells the
// settings window when it is newer. Nothing is downloaded or installed; the window offers a link to the release
// page. Off with [general] update_check=0. Builds without a repo (HAKU_REPO empty, local builds) never check.
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <shellapi.h>
#include <winhttp.h>
#include <process.h>
#include <stdio.h>

#ifndef HAKU_REPO
#define HAKU_REPO ""   // "owner/name", set by build.cmd from the HAKU_REPO environment variable (CI: the GitHub repo)
#endif

static char   latest[32], page[256];
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

static int https_get(const wchar_t *host, const wchar_t *path, char *out, int cap) {
    int n = 0, ok = 0;
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
    }
    out[n] = 0;
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok ? n : -1;
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
    int newer = ver_num(tag) > ver_num(HAKU_VER_STR);
    AcquireSRWLockExclusive(&lk);
    if (newer) { snprintf(latest, sizeof(latest), "%s", *tag == 'v' || *tag == 'V' ? tag + 1 : tag); snprintf(page, sizeof(page), "%s", url); }
    else latest[0] = page[0] = 0;
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
        if (r == WAIT_OBJECT_0 + 1 || atoi(cfg_get("general", "update_check", "1"))) check();   // asked for, or daily
        if (r == WAIT_TIMEOUT) wait = 24 * 3600 * 1000;
    }
    return 0;
}

void update_start(void) {
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

// {"version":"0.2.0","repo":1,"latest":"0.3.0","url":"https://github.com/..."}; latest/url empty when up to date
int update_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"version\":\"%s\",\"repo\":%d,\"latest\":\"%s\",\"url\":\"%s\"}",
                     HAKU_VER_STR, *repo() != 0, latest, page);
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
