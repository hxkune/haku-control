// SPDX-License-Identifier: GPL-3.0-only
// Settings window: a WebView2 page (ui\index.html next to the exe, served as https://haku-control.ui/).
// Created on demand and fully torn down on close, so the browser processes exist only while it is open.
// Page -> core: JSON strings {"cmd":...}; core -> page: {"type":"state"|"status"|"frame"|...}.
extern "C" {
#include "common.h"
}
#include <dwmapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wincrypt.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include "../third_party/webview2/WebView2.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

#define FRAME_TIMER  1
#define STATUS_TIMER 2
#define WM_UI_REFRESH (WM_APP + 10)

static HWND wnd;
static ComPtr<ICoreWebView2Environment> env;
static ComPtr<ICoreWebView2Controller> ctrl;
static ComPtr<ICoreWebView2> web;
static bool go_help;                     // the tray's Help: the page opens it once it has said hello
static bool page_ready;                  // the page has said hello; messages can be posted
static const COLORREF BG = RGB(10, 10, 10);

static std::wstring widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

static std::string narrow(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, &r[0], n, NULL, NULL);
    return r;
}

static void post(const char *json) {
    if (web && page_ready) web->PostWebMessageAsJson(widen(json).c_str());
}

static char *big_buf(void) {
    static char *b = (char *)malloc(512 * 1024);
    return b;
}

static void send_state(void) { char *b = big_buf(); app_state_json(b, 512 * 1024); post(b); }
// the whole state changed (profile, preset, devices...): also tells a phone, which sees the new number in /api/status
static void post_state(void) { InterlockedIncrement(&g_state_rev); send_state(); }
// the window in the background: the page pauses its animations and the live view slows down (ui/app.js)
static void post_focus(bool on) { post(on ? "{\"type\":\"focus\",\"on\":1}" : "{\"type\":\"focus\",\"on\":0}"); }
static void post_status(void) { static char b[64 * 1024]; app_status_json(b, sizeof(b)); post(b); }

// Value of a string (or number) field in a flat JSON object.
static std::string field(const std::string &js, const char *key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = js.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < js.size() && js[p] == ' ') p++;
    std::string out;
    if (p < js.size() && js[p] == '"') {
        for (p++; p < js.size() && js[p] != '"'; p++) {
            if (js[p] == '\\' && p + 1 < js.size()) {
                char c = js[++p];
                if (c == 'n') out += '\n';
                else if (c == 'r') out += '\r';
                else if (c == 't') out += '\t';
                else if (c == 'u' && p + 4 < js.size()) {
                    wchar_t w[3] = { (wchar_t)strtol(js.substr(p + 1, 4).c_str(), NULL, 16), 0, 0 };
                    p += 4;
                    // a character beyond the BMP (an emoji) comes as two: 😀
                    if (w[0] >= 0xD800 && w[0] <= 0xDBFF && p + 6 < js.size() && js[p + 1] == '\\' && js[p + 2] == 'u') {
                        wchar_t lo = (wchar_t)strtol(js.substr(p + 3, 4).c_str(), NULL, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) { w[1] = lo; p += 6; }
                    }
                    out += narrow(w);
                } else out += c;
            } else out += js[p];
        }
    } else {
        while (p < js.size() && js[p] != ',' && js[p] != '}') out += js[p++];
    }
    return out;
}

// the title bar blends into the page: its theme's background ([general] theme = dark / grey / light, or a hidden one)
static void title_bar(const char *theme) {
    if (!wnd) return;
    int light = !_stricmp(theme, "light"), grey = !_stricmp(theme, "grey"), gum = !_stricmp(theme, "bubblegum"), verity = !_stricmp(theme, "verity"), sky = !_stricmp(theme, "akapzz");
    BOOL dark = !light;
    DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = light ? RGB(0xef, 0xef, 0xed) : grey ? RGB(0x25, 0x26, 0x2a) : gum ? RGB(0x1a, 0x0b, 0x2e) : verity ? RGB(0x0b, 0x0b, 0x09) : sky ? RGB(0x04, 0x06, 0x0b) : BG;
    COLORREF txt = light ? RGB(90, 90, 90) : gum ? RGB(0xff, 0x9b, 0xe6) : verity ? RGB(0xff, 0xe1, 0x4a) : sky ? RGB(0xcf, 0xda, 0xf0) : RGB(170, 170, 170);
    DwmSetWindowAttribute(wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(wnd, 34 /* DWMWA_BORDER_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(wnd, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
}

// a profile's programs: one picked from those open on screen, or any .exe through Browse; added to [profile.N] apps
static BOOL CALLBACK app_win(HWND w, LPARAM lp) {
    auto *names = (std::vector<std::wstring> *)lp;
    DWORD pid = 0;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || !GetWindowTextLengthW(w)) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *b = wcsrchr(path, L'\\'); b = b ? b + 1 : path;
        static const wchar_t *skip[] = { L"explorer.exe", L"ApplicationFrameHost.exe", L"TextInputHost.exe", L"SystemSettings.exe", L"ShellExperienceHost.exe" };
        bool dup = false;
        for (auto *s : skip) dup |= !_wcsicmp(b, s);
        for (auto &x : *names) dup |= !_wcsicmp(b, x.c_str());
        if (!dup) names->push_back(b);
    }
    CloseHandle(p);
    return TRUE;
}
static void profile_app_pick(int id) {
    char sec[24]; snprintf(sec, sizeof(sec), "profile.%d", id);
    if (id < 1 || id > PROFILE_MAX || !cfg_get(sec, "name", NULL)) return;
    std::vector<std::wstring> names;
    EnumWindows(app_win, (LPARAM)&names);
    std::sort(names.begin(), names.end(), [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < names.size() && i < 40; i++) AppendMenuW(m, MF_STRING, i + 1, names[i].c_str());
    if (!names.empty()) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1000, TR(L"Browse…", L"Обзор…", L"Parcourir…"));
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(wnd);
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, wnd, NULL);
    DestroyMenu(m);
    std::wstring pick;
    if (c == 1000) {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of = { sizeof(of) };
        of.hwndOwner = wnd; of.lpstrFilter = L"*.exe\0*.exe\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        const wchar_t *b;
        if (GetOpenFileNameW(&of) && (b = wcsrchr(file, L'\\'))) pick = b + 1;
    }
    else if (c > 0) pick = names[c - 1];
    if (pick.empty() || wcschr(pick.c_str(), L',')) return;
    std::string cur = cfg_get(sec, "apps", ""), add = narrow(pick.c_str()), low = "," + cur + ",", key = "," + add + ",";
    for (auto *s : { &low, &key }) for (auto &ch : *s) ch = (char)tolower((unsigned char)ch);
    for (size_t i; (i = low.find(", ")) != std::string::npos; ) low.erase(i + 1, 1);
    if (low.find(key) != std::string::npos) return;
    cfg_set_and_save(sec, "apps", (cur.empty() ? add : cur + ", " + add).c_str());
    app_auto_recheck();
}

static void on_message(const std::string &js) {
    std::string cmd = field(js, "cmd");
    if (cmd == "hello") { page_ready = true; send_state(); post_focus(GetForegroundWindow() == wnd); if (go_help) { go_help = false; post("{\"type\":\"go\",\"to\":\"help\"}"); } }
    else if (cmd == "get") send_state();
    else if (cmd == "effect") { app_set_effect(effect_index(field(js, "id").c_str())); post_status(); }
    else if (cmd == "brightness") { app_set_brightness(atoi(field(js, "v").c_str()) / 100.0f, 0); app_save_soon(); }
    else if (cmd == "set") app_set(field(js, "s").c_str(), field(js, "k").c_str(), field(js, "v").c_str());
    else if (cmd == "fans_set") {   // "key=value" lines, all written as one change; not in remote_allowed: the fans stay with this PC
        std::string kv = field(js, "kv"); std::vector<std::string> part; std::vector<const char *> p; bool ok = true;
        for (size_t a = 0, b; a < kv.size() && ok; a = b + 1) {
            b = kv.find('\n', a); if (b == std::string::npos) b = kv.size();
            std::string l = kv.substr(a, b - a); size_t eq = l.find('=');
            ok = eq != std::string::npos && eq > 0 && part.size() < 256;   // a group of every channel with every source's curve fits
            for (size_t i = 0; ok && i < eq; i++) ok = isalnum((unsigned char)l[i]) || l[i] == '_';
            if (ok) { part.push_back(l.substr(0, eq)); part.push_back(l.substr(eq + 1)); }
        }
        if (ok && !part.empty()) { for (const std::string &x : part) p.push_back(x.c_str()); app_set_fans(p.data(), (int)p.size() / 2); }
    }
    else if (cmd == "toggle") { app_toggle_device(field(js, "k").c_str()); post_status(); }
    else if (cmd == "power") { app_power(); post_status(); }
    else if (cmd == "timed_off") { app_timed_off(atoi(field(js, "min").c_str())); post_status(); }
    else if (cmd == "identify") app_identify(field(js, "zone").c_str());
    else if (cmd == "calib") app_calib(field(js, "zone").c_str(), atoi(field(js, "mode").c_str()), atoi(field(js, "from").c_str()), atoi(field(js, "n").c_str()));
    else if (cmd == "dev_retry") { ext_retry(); post_status(); }   // Retry now: every device not connected, the waits start over
    else if (cmd == "dev_check") { std::string id = field(js, "id"); ext_retry(); net_check_start(atoi(id.c_str()), cfg_get(("dev." + id).c_str(), "host", "")); post_status(); }
    else if (cmd == "pair") { std::string ip = field(js, "ip"); nano_pair_start_ip(ip.c_str()); post_status(); }
    else if (cmd == "preset") { app_preset_apply(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "preset_save") {
        // the preset window's values; "bri":"1" (older pages) keeps the current brightness
        std::string bri = field(js, "brightness");
        if (bri.empty() && field(js, "bri") == "1") bri = "cur";
        int id = app_preset_save(atoi(field(js, "id").c_str()), field(js, "name").c_str(), field(js, "effect").c_str(),
                                 field(js, "palette").c_str(), field(js, "speed").c_str(), bri.c_str(), field(js, "zones").c_str());
        if (id && field(js, "apply") == "1") app_preset_apply(id);   // shows what was just made
        post_state();
    }
    else if (cmd == "preset_delete") { app_preset_delete(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "profile") { app_profile_apply(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "profile_save") { app_profile_save(atoi(field(js, "id").c_str()), field(js, "name").c_str()); post_state(); }
    else if (cmd == "profile_app") { profile_app_pick(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "profile_delete") { app_profile_delete(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "profile_dup") { app_profile_dup(atoi(field(js, "id").c_str()), field(js, "name").c_str()); post_state(); }
    else if (cmd == "nano_forget") { nano_forget(atoi(field(js, "slot").c_str())); post_state(); }
    else if (cmd == "scan") { ext_scan(); orgb_check_start(); post_status(); }
    else if (cmd == "orgb_check") { orgb_check_start(); post_status(); }
    else if (cmd == "hw_scan") { hw_scan_start(); orgb_check_start(); post_status(); }
    else if (cmd == "cf_close") { conflicts_close(field(js, "id").c_str()); post_status(); }   // cf_close, cf_end: not in remote_allowed, they end programs on this PC
    else if (cmd == "cf_end") { conflicts_end(field(js, "id").c_str()); post_status(); }
    else if (cmd == "ai_setup") { ollama_setup(); post_status(); }
    else if (cmd == "theme") title_bar(field(js, "v").c_str());
    else if (cmd == "orgb_setup") { orgbapp_setup(); post_status(); }
    else if (cmd == "pc_own") { app_pc_own(field(js, "v") == "1"); post_state(); }
    else if (cmd == "orgb_auto") { cfg_set_and_save("openrgb", "auto", field(js, "v") == "1" ? "1" : "0"); if (field(js, "v") == "1") orgb_check_start(); post_state(); }
    else if (cmd == "orgb_locate") {   // OpenRGB.exe somewhere else: picked once
        wchar_t file[MAX_PATH] = L"OpenRGB.exe";
        OPENFILENAMEW of = { sizeof(of) };
        of.hwndOwner = wnd; of.lpstrFilter = L"OpenRGB.exe\0OpenRGB.exe\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        const wchar_t *base = NULL;
        if (GetOpenFileNameW(&of) && (base = wcsrchr(file, L'\\')) && !_wcsicmp(base + 1, L"OpenRGB.exe")) {
            cfg_set_and_save("openrgb", "path", narrow(file).c_str());
            orgb_check_start();
        }
        post_state();
    }
    else if (cmd == "tryx_media") {   // the TRYX screen's own picture / GIF / video: a file on this PC, picked here only
        std::string sec = "dev." + field(js, "id");
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of = { sizeof(of) };
        of.hwndOwner = wnd; of.lpstrFilter = L"*.png; *.jpg; *.gif; *.webp; *.bmp; *.mp4\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!_stricmp(cfg_get(sec.c_str(), "kind", ""), "tryx") && GetOpenFileNameW(&of)) {
            cfg_set(sec.c_str(), "media", narrow(file).c_str());
            cfg_set(sec.c_str(), "media_at", std::to_string(GetTickCount()).c_str());   // the same file picked again is sent again
            app_set(sec.c_str(), "theme", "-1");
        }
        post_state();
    }
    else if (cmd == "tryx_build") {   // the screen builder's result: a jpg or mp4 made on the page, kept as %APPDATA%\haku-control\tryx-<N>.<ext>
        std::string id = field(js, "id"), ext = field(js, "ext"), sec = "dev." + id, data = field(js, "data"), req = field(js, "req");
        DWORD n = 0; std::vector<BYTE> buf; int err = TS_OPEN; const char *why = "bad";
        req.erase(std::remove_if(req.begin(), req.end(), [](char c) { return !isalnum((unsigned char)c); }), req.end());
        if (req.size() > 16) req.resize(16);
        if (!id.empty() && id.size() < 6 && id.find_first_not_of("0123456789") == std::string::npos && (ext == "jpg" || ext == "mp4") &&
            !_stricmp(cfg_get(sec.c_str(), "kind", ""), "tryx") && data.size() <= (16u << 20) / 3 * 4 + 4 &&
            CryptStringToBinaryA(data.c_str(), (DWORD)data.size(), CRYPT_STRING_BASE64, NULL, &n, NULL, NULL) && n >= 12 && n <= 16u << 20) {
            buf.resize(n);
            wchar_t path[MAX_PATH];
            ExpandEnvironmentStringsW((L"%APPDATA%\\" APP_ID L"\\tryx-" + widen(id.c_str()) + L"." + widen(ext.c_str())).c_str(), path, MAX_PATH);
            // what it says it is: a JPEG starts FF D8 FF, an mp4 has "ftyp" at 4
            if (CryptStringToBinaryA(data.c_str(), (DWORD)data.size(), CRYPT_STRING_BASE64, buf.data(), &n, NULL, NULL) &&
                (ext == "jpg" ? buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF : !memcmp(buf.data() + 4, "ftyp", 4))) {
                err = tryx_save(path, buf.data(), n);   // a failed save keeps the old file, and the settings still point at it
                why = err == TS_OPEN ? "open" : err == TS_WRITE ? "write" : "replace";
            }
            if (!err) {
                cfg_set(sec.c_str(), "media", narrow(path).c_str());
                cfg_set(sec.c_str(), "media_at", std::to_string(GetTickCount()).c_str());
                app_set(sec.c_str(), "theme", "-1");
            }
        }
        if (err) logf_("tryx: the builder's file was not saved (%s, %s, %u bytes)", why, ext.c_str(), (unsigned)n);
        post_state();
        // the answer to this one request: saved on this PC (the screen's upload then shows on the device card), or why not
        post(("{\"type\":\"tryx_built\",\"req\":\"" + req + "\",\"ok\":" + (err ? "0" : "1") + ",\"err\":\"" + (err ? why : "") + "\"}").c_str());
    }
    else if (cmd == "ai_on_demand") { ollama_set_on_demand(field(js, "v") == "1"); post_state(); }
    else if (cmd == "dev_add") {
        ext_add(field(js, "kind").c_str(), field(js, "host").c_str(), atoi(field(js, "sub").c_str()),
                field(js, "name").c_str(), atoi(field(js, "leds").c_str()));
        post_state();
    }
    else if (cmd == "dev_remove") { ext_remove(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "remote_pin") { remote_new_pin(); post_status(); }
    else if (cmd == "remote_forget") { remote_forget(); post_status(); }
    else if (cmd == "autostart") { app_autostart(atoi(field(js, "v").c_str())); post_state(); }
    else if (cmd == "open") app_open(field(js, "what").c_str());
    else if (cmd == "update_check") update_check_now();
    else if (cmd == "backup_save") { backup_save(wnd); post_status(); }
    else if (cmd == "backup_load") { backup_load(wnd); post_state(); }
    else if (cmd == "pro_key") { pro_set_key(field(js, "key").c_str()); post_status(); }
    else if (cmd == "diag") { diag_save(); post_status(); }
    else if (cmd == "report_seen") diag_fixed_seen(field(js, "id").c_str());
    else if (cmd == "report") { diag_report(field(js, "text").c_str(), field(js, "contact").c_str(), field(js, "install").c_str(), field(js, "attach") != "0"); post_status(); }
    else if (cmd == "update_install") { update_install(); post_status(); }
    else if (cmd == "govee_login") {   // PC window only, like aidot_login
        std::string key = field(js, "key");
        accounts_govee_login(key.c_str());
        SecureZeroMemory(&key[0], key.size());
        post_status();
    }
    else if (cmd == "tuya_login") {   // PC window only, like aidot_login
        std::string sec = field(js, "secret");
        accounts_tuya_login(field(js, "region").c_str(), field(js, "id").c_str(), sec.c_str());
        SecureZeroMemory(&sec[0], sec.size());
        post_status();
    }
    else if (cmd == "aidot_login") {   // PC window only (not in the phone's list): the password stays on this PC
        std::string pw = field(js, "password");
        accounts_aidot_login(field(js, "country").c_str(), field(js, "email").c_str(), pw.c_str());
        SecureZeroMemory(&pw[0], pw.size());
        post_status();
    }
    else if (cmd == "mood") { mood_request(field(js, "text").c_str(), field(js, "again") == "1"); post_status(); }
    else if (cmd == "quit") app_quit();
    else if (cmd == "close") DestroyWindow(wnd);
}

static void fit(void) {
    if (!ctrl) return;
    RECT r; GetClientRect(wnd, &r);
    ctrl->put_Bounds(r);
}

static void create_webview(void) {
    wchar_t data[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\" APP_ID L"\\WebView2", data, MAX_PATH);
    SHCreateDirectoryExW(NULL, data, NULL);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, data, NULL,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT res, ICoreWebView2Environment *e) -> HRESULT {
            if (FAILED(res) || !e || !wnd) { logf_("ui: WebView2 environment failed 0x%08lx", res); return S_OK; }
            env = e;
            return env->CreateCoreWebView2Controller(wnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT res, ICoreWebView2Controller *c) -> HRESULT {
                    if (FAILED(res) || !c || !wnd) { logf_("ui: WebView2 controller failed 0x%08lx", res); return S_OK; }
                    ctrl = c;
                    ctrl->get_CoreWebView2(&web);

                    ComPtr<ICoreWebView2Controller2> c2;
                    if (SUCCEEDED(ctrl.As(&c2))) {
                        COREWEBVIEW2_COLOR bg = { 255, GetRValue(BG), GetGValue(BG), GetBValue(BG) };
                        c2->put_DefaultBackgroundColor(bg);
                    }
                    ComPtr<ICoreWebView2Settings> s;
                    web->get_Settings(&s);
                    int dev = cfg_geti("ui", "devtools", 0);
                    s->put_AreDevToolsEnabled(dev);
                    s->put_AreDefaultContextMenusEnabled(dev);
                    s->put_IsStatusBarEnabled(FALSE);
                    s->put_IsZoomControlEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings3> s3;
                    if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(dev);

                    wchar_t dir[MAX_PATH];
                    GetModuleFileNameW(NULL, dir, MAX_PATH);
                    wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0;
                    wcscat_s(dir, MAX_PATH, L"ui");
                    ComPtr<ICoreWebView2_3> w3;
                    if (SUCCEEDED(web.As(&w3)))
                        w3->SetVirtualHostNameToFolderMapping(L"haku-control.ui", dir, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);

                    EventRegistrationToken tok;
                    web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
                            LPWSTR m = NULL;
                            if (SUCCEEDED(a->TryGetWebMessageAsString(&m)) && m) { on_message(narrow(m)); CoTaskMemFree(m); }
                            return S_OK;
                        }).Get(), &tok);
                    // links open in the normal browser, never inside the window
                    web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
                            a->put_Handled(TRUE);
                            return S_OK;
                        }).Get(), &tok);

                    fit();
                    // the logo intro (ui/intro.js) plays the first time the window opens after haku starts, not from the tray again
                    static bool intro_done;
                    web->Navigate(intro_done ? L"https://haku-control.ui/index.html" : L"https://haku-control.ui/index.html#intro");
                    intro_done = true;
                    return S_OK;
                }).Get());
        }).Get());
    if (FAILED(hr)) {
        logf_("ui: WebView2 runtime not available 0x%08lx", hr);
        MessageBoxW(wnd, TR(L"Microsoft Edge WebView2 Runtime is missing, so the window cannot open.", L"Не найден Microsoft Edge WebView2 Runtime — окно не может открыться.", L"Microsoft Edge WebView2 Runtime est absent, la fenêtre ne peut pas s'ouvrir."), L"haku control", MB_ICONERROR);
        DestroyWindow(wnd);
    }
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        // minimised, the page is hidden: WebView2 stops drawing and throttles its timers
        if (ctrl) ctrl->put_IsVisible(wp != SIZE_MINIMIZED);
        fit(); return 0;
    case WM_ACTIVATE: post_focus(LOWORD(wp) != WA_INACTIVE); break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        UINT dpi = GetDpiForWindow(h);
        mm->ptMinTrackSize.x = MulDiv(980, dpi, 96);
        mm->ptMinTrackSize.y = MulDiv(660, dpi, 96);
        return 0;
    }
    case WM_SETFOCUS: if (ctrl) ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); return 0;
    case WM_TIMER:
        if (!page_ready) return 0;
        if (wp == FRAME_TIMER) {   // 20 a second in front, 5 behind other windows, none minimised
            static unsigned n; static char b[64 * 1024];
            if (IsIconic(h) || (GetForegroundWindow() != h && n++ % 4)) return 0;
            app_frame_json(b, sizeof(b)); post(b);
        }
        if (wp == STATUS_TIMER) post_status();
        return 0;
    case WM_UI_REFRESH: if (wp) send_state(); else post_status(); return 0;
    case WM_SETTINGCHANGE:
        // light / dark taskbar switched: the taskbar button gets the matching mark
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)app_icon(1));
        return 0;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(BG); FillRect((HDC)wp, &r, b); DeleteObject(b);
        return 1;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        KillTimer(h, FRAME_TIMER); KillTimer(h, STATUS_TIMER);
        cfg_save_if_dirty();
        page_ready = false;
        if (ctrl) ctrl->Close();
        web.Reset(); ctrl.Reset(); env.Reset();
        wnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void bring_to_front(void) {
    if (IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
    SetWindowPos(wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(wnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(wnd);
}

// Commands from a phone (remote.c) or Home Assistant (mqtt.c), marshalled to this thread: same handling as the
// page's, then the open window (if any) is brought up to date. Checked here, with on_message's own reader, so a
// body that reads one way to the sender's parser and another way here cannot slip past.
static bool remote_allowed(const std::string &js) {
    static const char *const ok[] = { "effect", "brightness", "set", "toggle", "power", "timed_off", "identify", "pair", "scan", "dev_add", "dev_remove", "mood", "preset",
                                      "preset_save", "preset_delete", "orgb_check", "hw_scan", "profile", "profile_save", "profile_delete", "profile_dup", "pc_own", "calib", "dev_check",
#ifdef HAKU_DEV   // test builds (loopback only by default): the rest can be driven from a browser for tests
                                      "aidot_login", "tuya_login", "govee_login", "diag", "update_check", "update_install", "ai_setup", "ai_on_demand",
                                      "orgb_setup", "orgb_auto", "pro_key", "backup_save", "backup_load", "report",
#endif
    };
    std::string cmd = field(js, "cmd");
    bool found = false;
    for (const char *c : ok) if (cmd == c) found = true;
    if (!found) return false;
    // calibration lights one known zone for a short while; the check reaches only a device already saved here
    if (cmd == "dev_check") { std::string id = field(js, "id"); return !id.empty() && id.size() < 6 && id.find_first_not_of("0123456789") == std::string::npos; }
    // a copy of a saved profile: a number in range and a short name of printable text (the page sends 40 letters at most)
    if (cmd == "profile_dup") {
        std::string id = field(js, "id"), nm = field(js, "name");
        if (id.empty() || id.size() > 2 || id.find_first_not_of("0123456789") != std::string::npos) return false;
        int n = atoi(id.c_str());
        if (n < 1 || n > PROFILE_MAX || nm.empty() || nm.size() > 160) return false;
        for (char c : nm) if ((unsigned char)c < 0x20 || c == 0x7f) return false;
        return true;
    }
    if (cmd == "calib") {
        std::string m = field(js, "mode"), z = field(js, "zone");
        if (m.size() != 1 || m[0] < '0' || m[0] > '3' || z.size() > 60) return false;
        for (const char *k : { "from", "n" }) { std::string v = field(js, k); if (v.size() > 3) return false; for (char c : v) if (!isdigit((unsigned char)c)) return false; }
        for (char c : z) if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
        return true;
    }
    if (cmd != "set") return true;
    // settings that start programs, hold this PC's own access or drive its fans stay with the PC window
    std::string s = field(js, "s"), k = field(js, "k");
    // plain names only: a space, "=" or "]" would write a different ini line than the one checked
    for (char c : s + k) if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    return !s.empty() && !k.empty() && _stricmp(s.c_str(), "remote") && _stricmp(s.c_str(), "mqtt") && _stricmp(s.c_str(), "fans") && _stricmp(s.c_str(), "link") && _stricmp(s.c_str(), "conflicts") &&
           !(!_stricmp(s.c_str(), "openrgb") && !_stricmp(k.c_str(), "path")) && !(!_stricmp(s.c_str(), "mood") && !_stricmp(k.c_str(), "ollama_path")) &&
           !(!_strnicmp(s.c_str(), "dev.", 4) && !_strnicmp(k.c_str(), "media", 5));   // a file on this PC for the TRYX screen
}

extern "C" int ui_dispatch(const char *json) {
    if (!remote_allowed(json)) { logf_("remote: command refused"); return 0; }
    on_message(json);
    send_state();
    return 1;
}

extern "C" void ui_open(HINSTANCE inst) {
    if (wnd) { bring_to_front(); return; }
    static bool registered;
    if (!registered) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"haku_control_ui";
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = app_icon(1);
        RegisterClassW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1200, dpi, 96), hgt = MulDiv(790, dpi, 96);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (hgt > wa.bottom - wa.top) hgt = wa.bottom - wa.top;
    wnd = CreateWindowExW(0, L"haku_control_ui", L"haku control", WS_OVERLAPPEDWINDOW,
                          wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt,
                          NULL, NULL, inst, NULL);
    title_bar(cfg_get("general", "theme", "dark"));
    SendMessageW(wnd, WM_SETICON, ICON_SMALL, (LPARAM)app_icon(0));
    SendMessageW(wnd, WM_SETICON, ICON_BIG, (LPARAM)app_icon(1));
    ShowWindow(wnd, SW_SHOW);
    bring_to_front();
    create_webview();
    SetTimer(wnd, FRAME_TIMER, 50, NULL);
    SetTimer(wnd, STATUS_TIMER, 1000, NULL);
}

extern "C" void ui_open_help(HINSTANCE inst) {
    go_help = true;
    ui_open(inst);
    if (page_ready) { go_help = false; post("{\"type\":\"go\",\"to\":\"help\"}"); }
}

extern "C" void ui_refresh(void) { if (wnd) PostMessageW(wnd, WM_UI_REFRESH, 0, 0); }
extern "C" void ui_refresh_state(void) { InterlockedIncrement(&g_state_rev); if (wnd) PostMessageW(wnd, WM_UI_REFRESH, 1, 0); }

extern "C" int ui_is_dialog_message(MSG *m) { (void)m; return 0; }
