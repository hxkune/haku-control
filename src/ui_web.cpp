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
#include <wrl.h>
#include <string>
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

static void post_state(void) { char *b = big_buf(); app_state_json(b, 512 * 1024); post(b); }
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
                else if (c == 't') out += '\t';
                else if (c == 'u' && p + 4 < js.size()) {
                    wchar_t w[2] = { (wchar_t)strtol(js.substr(p + 1, 4).c_str(), NULL, 16), 0 };
                    out += narrow(w); p += 4;
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
    int light = !_stricmp(theme, "light"), grey = !_stricmp(theme, "grey"), gum = !_stricmp(theme, "bubblegum"), verity = !_stricmp(theme, "verity");
    BOOL dark = !light;
    DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = light ? RGB(0xef, 0xef, 0xed) : grey ? RGB(0x25, 0x26, 0x2a) : gum ? RGB(0x1a, 0x0b, 0x2e) : verity ? RGB(0x0b, 0x0b, 0x09) : BG;
    COLORREF txt = light ? RGB(90, 90, 90) : gum ? RGB(0xff, 0x9b, 0xe6) : verity ? RGB(0xff, 0xe1, 0x4a) : RGB(170, 170, 170);
    DwmSetWindowAttribute(wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(wnd, 34 /* DWMWA_BORDER_COLOR */, &cap, sizeof(cap));
    DwmSetWindowAttribute(wnd, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
}

static void on_message(const std::string &js) {
    std::string cmd = field(js, "cmd");
    if (cmd == "hello") { page_ready = true; post_state(); }
    else if (cmd == "get") post_state();
    else if (cmd == "effect") { app_set_effect(effect_index(field(js, "id").c_str())); post_status(); }
    else if (cmd == "brightness") { app_set_brightness(atoi(field(js, "v").c_str()) / 100.0f, 0); app_save_soon(); }
    else if (cmd == "set") app_set(field(js, "s").c_str(), field(js, "k").c_str(), field(js, "v").c_str());
    else if (cmd == "toggle") { app_toggle_device(field(js, "k").c_str()); post_status(); }
    else if (cmd == "power") { app_power(); post_status(); }
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
    else if (cmd == "profile_delete") { app_profile_delete(atoi(field(js, "id").c_str())); post_state(); }
    else if (cmd == "nano_forget") { nano_forget(atoi(field(js, "slot").c_str())); post_state(); }
    else if (cmd == "scan") { ext_scan(); orgb_check_start(); post_status(); }
    else if (cmd == "orgb_check") { orgb_check_start(); post_status(); }
    else if (cmd == "ai_setup") { ollama_setup(); post_status(); }
    else if (cmd == "theme") title_bar(field(js, "v").c_str());
    else if (cmd == "orgb_setup") { orgbapp_setup(); post_status(); }
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
    else if (cmd == "diag") { diag_save(); post_status(); }
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
                    web->Navigate(L"https://haku-control.ui/index.html");
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
    case WM_SIZE: fit(); return 0;
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
        if (wp == FRAME_TIMER) { static char b[64 * 1024]; app_frame_json(b, sizeof(b)); post(b); }
        if (wp == STATUS_TIMER) post_status();
        return 0;
    case WM_UI_REFRESH: if (wp) post_state(); else post_status(); return 0;
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

// Commands from a phone (remote.c, marshalled to this thread): same handling as the page's, then the open
// window (if any) is brought up to date.
extern "C" void ui_dispatch(const char *json) {
    on_message(json);
    post_state();
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

extern "C" void ui_refresh(void) { if (wnd) PostMessageW(wnd, WM_UI_REFRESH, 0, 0); }
extern "C" void ui_refresh_state(void) { if (wnd) PostMessageW(wnd, WM_UI_REFRESH, 1, 0); }

extern "C" int ui_is_dialog_message(MSG *m) { (void)m; return 0; }
