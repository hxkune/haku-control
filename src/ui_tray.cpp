// SPDX-License-Identifier: GPL-3.0-only
// The tray panel: a small WebView2 popup (ui\tray.html) by the tray icon, in place of the tray menu on a left click.
// It shares the settings window's WebView2 folder and virtual host but loads only its own light page.
// Shown: state from app_tray_json, commands through ui_dispatch (the phone's checks). Closed: hidden and suspended;
// 30 s later the browser part is let go of, so nothing of it runs while haku sits in the tray.
// Any failure leaves the right-click menu, and a left click falls back to it (app_tray_menu), once: so does a page
// that never says hello and its height (OPEN_TIMER). What a released browser part still answers is ignored (gen).
extern "C" {
#include "common.h"
}
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <wrl.h>
#include <string>
#include "../third_party/webview2/WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

#define RELEASE_TIMER   1
#define RELEASE_AFTER   30000
#define OPEN_TIMER      2
#define OPEN_FIRST      10000    // ms for a page to say its height: a cold start of the browser part, then the page
#define OPEN_AGAIN      3000     // ... the page already loaded (resumed)
#define WM_TRAY_REFRESH (WM_APP + 11)
#define PANEL_W         340      // logical px, as tray.css lays it out

static HWND tw, icon_wnd;
static HINSTANCE inst_;
static ComPtr<ICoreWebView2Environment> env;
static ComPtr<ICoreWebView2Controller> ctrl;
static ComPtr<ICoreWebView2> web;
static bool page_ready, opening, shown, first_open = true;
static DWORD open_t0, closed_at;
static unsigned gen;             // the browser part's: raised when it is let go of, so its late callbacks do nothing
static int content_h = 420;      // the page's own height in css px, as it last said
static COLORREF bg = RGB(18, 18, 18);

// the panel's ground in the theme in use (theme.css --menu), so the first frame does not flash another colour
static void theme_bg(void) {
    static const struct { const char *t; COLORREF c; } m[] = { { "grey", RGB(0x30, 0x31, 0x37) }, { "light", RGB(255, 255, 255) },
        { "bubblegum", RGB(0x2b, 0x13, 0x48) }, { "verity", RGB(0x16, 0x16, 0x12) }, { "akapzz", RGB(0x0c, 0x10, 0x18) }, { "kyoka", RGB(0x11, 0x0f, 0x17) } };
    const char *t = cfg_get("general", "theme", "dark");
    bg = RGB(18, 18, 18);
    for (auto &e : m) if (!strcmp(t, e.t)) bg = e.c;
    ComPtr<ICoreWebView2Controller2> c2;
    if (ctrl && SUCCEEDED(ctrl.As(&c2))) { COREWEBVIEW2_COLOR c = { 255, GetRValue(bg), GetGValue(bg), GetBValue(bg) }; c2->put_DefaultBackgroundColor(c); }
}

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
static void post(const char *json) { if (web && page_ready) web->PostWebMessageAsJson(widen(json).c_str()); }
static void send_state(void) { static char b[64 * 1024]; app_tray_json(b, sizeof(b)); post(b); }

// value of a flat JSON field (the page's own messages: short, plain)
static std::string field(const std::string &js, const char *key) {
    std::string k = std::string("\"") + key + "\":";
    size_t p = js.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    if (p < js.size() && js[p] == '"') { size_t e = js.find('"', p + 1); return e == std::string::npos ? "" : js.substr(p + 1, e - p - 1); }
    size_t e = js.find_first_of(",}", p);
    return js.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// everything browser-side let go of; the next click starts again
static void release(void) {
    page_ready = opening = shown = false;
    gen++;
    if (ctrl) ctrl->Close();
    web.Reset(); ctrl.Reset(); env.Reset();
    if (tw) { HWND h = tw; tw = NULL; DestroyWindow(h); }
}

// the panel failed before it could show: the classic menu instead, and a clean start next time
static void fail(const char *what, HRESULT hr) {
    bool was = opening;
    logf_("tray: %s 0x%08lx, the menu instead", what, hr);
    release();
    if (was) app_tray_menu();
}

// Beside the icon, on the taskbar's side, inside the work area (the icon's own edge when the taskbar hides itself).
// Coordinates are the process's (system DPI aware), so sizes scale with the system DPI.
static char place(void) {
    NOTIFYICONIDENTIFIER ni = { sizeof(ni) }; ni.hWnd = icon_wnd; ni.uID = 1;
    RECT ir;
    if (FAILED(Shell_NotifyIconGetRect(&ni, &ir))) { POINT p; GetCursorPos(&p); ir = { p.x, p.y, p.x + 1, p.y + 1 }; }
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&ir, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork, mr = mi.rcMonitor;
    UINT dpi = GetDpiForSystem();
    int gap = MulDiv(8, dpi, 96), cx = (ir.left + ir.right) / 2, cy = (ir.top + ir.bottom) / 2;
    // the side: where the work area gives way to the taskbar, else (auto-hide) the screen edge nearest the icon
    char side = wa.bottom < mr.bottom ? 'b' : wa.top > mr.top ? 't' : wa.left > mr.left ? 'l' : wa.right < mr.right ? 'r' : 0;
    if (!side) {
        int d[4] = { (int)mr.bottom - cy, cy - (int)mr.top, cx - (int)mr.left, (int)mr.right - cx }, m = 0;
        for (int i = 1; i < 4; i++) if (d[i] < d[m]) m = i;
        side = "btlr"[m];
    }
    // the area the panel may use: the work area, short of the icon on the taskbar's side
    RECT a = { wa.left + gap, wa.top + gap, wa.right - gap, wa.bottom - gap };
    if (side == 'b' && ir.top - gap < a.bottom) a.bottom = ir.top - gap;
    if (side == 't' && ir.bottom + gap > a.top) a.top = ir.bottom + gap;
    if (side == 'l' && ir.right + gap > a.left) a.left = ir.right + gap;
    if (side == 'r' && ir.left - gap < a.right) a.right = ir.left - gap;
    int w = MulDiv(PANEL_W, dpi, 96), h = MulDiv(content_h, dpi, 96), minh = MulDiv(160, dpi, 96);
    if (w > a.right - a.left) w = a.right - a.left;
    if (h > a.bottom - a.top) h = a.bottom - a.top;
    if (h < minh) h = minh;
    int x = side == 'l' ? a.left : side == 'r' ? a.right - w : cx - w / 2;
    int y = side == 't' ? a.top : side == 'b' ? a.bottom - h : cy - h / 2;
    if (x + w > a.right) x = a.right - w;
    if (x < a.left) x = a.left;
    if (y + h > a.bottom) y = a.bottom - h;
    if (y < a.top) y = a.top;
    SetWindowPos(tw, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (ctrl) { RECT r = { 0, 0, w, h }; ctrl->put_Bounds(r); }
    return side;
}

static void show(void) {
    if (!tw || !opening) return;
    char side = place(), m[64];
    ShowWindow(tw, SW_SHOW);
    KillTimer(tw, OPEN_TIMER);
    if (!SetForegroundWindow(tw)) logf_("tray: not brought to the front");
    if (ctrl) ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    opening = false; shown = true;
    snprintf(m, sizeof(m), "{\"type\":\"shown\",\"side\":\"%c\"}", side);
    post(m);
    logf_("tray: open in %lu ms (%s)", GetTickCount() - open_t0, first_open ? "first" : "again");
    first_open = false;
}

static void on_message(const std::string &js) {
    std::string cmd = field(js, "cmd");
    if (cmd == "hello") { page_ready = true; if (opening) { send_state(); post("{\"type\":\"open\"}"); } }
    else if (cmd == "height") {   // the page laid out: its height, then shown (or resized)
        int h = atoi(field(js, "h").c_str());
        if (h > 0 && h < 4000) content_h = h;
        if (opening) show(); else if (shown) place();
    }
    else if (cmd == "close") tray_close();
    else if (cmd == "menu") {     // the ⋯ menu: what the tray menu offered, the same calls
        std::string d = field(js, "do");
        tray_close();
        if (d == "window") ui_open(inst_);
        else if (d == "help") ui_open_help(inst_);
        else if (d == "settings") ui_open_at(inst_, "settings");
        else if (d == "diag") diag_save();
        else if (d == "log") app_open("log");
        else if (d == "ini") app_open("ini");
        else if (d == "quit") app_quit();
    }
    else if (cmd == "get") send_state();
    else { ui_dispatch(js.c_str()); tray_refresh(); }   // the window's commands, with the phone's checks
}

static void create_webview(void) {
    wchar_t data[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\" APP_ID L"\\WebView2", data, MAX_PATH);
    SHCreateDirectoryExW(NULL, data, NULL);
    unsigned g = gen;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, data, NULL,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([g](HRESULT res, ICoreWebView2Environment *e) -> HRESULT {
            if (g != gen || !tw) return S_OK;   // let go of meanwhile (timed out, crashed)
            if (FAILED(res) || !e) { fail("WebView2 environment failed", res); return S_OK; }
            env = e;
            HRESULT hc = env->CreateCoreWebView2Controller(tw, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [g](HRESULT res, ICoreWebView2Controller *c) -> HRESULT {
                    if (g != gen || !tw) { if (c) c->Close(); return S_OK; }
                    if (FAILED(res) || !c) { fail("WebView2 controller failed", res); return S_OK; }
                    ctrl = c;
                    if (FAILED(ctrl->get_CoreWebView2(&web)) || !web) { fail("WebView2 has no page", E_FAIL); return S_OK; }
                    theme_bg();
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
                        [g](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
                            LPWSTR m = NULL;
                            if (g != gen) return S_OK;
                            if (SUCCEEDED(a->TryGetWebMessageAsString(&m)) && m) { std::string js = narrow(m); CoTaskMemFree(m); on_message(js); }
                            return S_OK;
                        }).Get(), &tok);
                    web->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT { a->put_Handled(TRUE); return S_OK; }).Get(), &tok);
                    web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [g](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
                            BOOL ok = FALSE; a->get_IsSuccess(&ok);
                            if (!ok && g == gen) fail("page did not load", 0);
                            return S_OK;
                        }).Get(), &tok);
                    // a crashed browser part: let go of it all; the next click starts again (or gets the menu)
                    web->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                        [g](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *a) -> HRESULT {
                            COREWEBVIEW2_PROCESS_FAILED_KIND k; a->get_ProcessFailedKind(&k);
                            logf_("tray: WebView2 process failed (%d)", (int)k);
                            if (g == gen && tw) PostMessageW(tw, WM_CLOSE, 0, 0);
                            return S_OK;
                        }).Get(), &tok);
                    RECT r; GetClientRect(tw, &r); ctrl->put_Bounds(r);
                    HRESULT hn = web->Navigate(L"https://haku-control.ui/tray.html");
                    if (FAILED(hn)) fail("page not opened", hn);
                    return S_OK;
                }).Get());
            if (FAILED(hc)) fail("WebView2 controller not started", hc);
            return S_OK;
        }).Get());
    if (FAILED(hr)) fail("WebView2 runtime not available", hr);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATE:   // a click anywhere else: closed (and that click on the icon must not open it again)
        if (LOWORD(wp) == WA_INACTIVE && shown) { tray_close(); closed_at = GetTickCount(); }
        return 0;
    case WM_SETFOCUS: if (ctrl) ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); return 0;
    case WM_TIMER:
        if (wp == RELEASE_TIMER) { KillTimer(h, RELEASE_TIMER); if (!shown && !opening) { logf_("tray: let go of"); release(); } }
        // no hello or height in time (a hung page or browser part): the menu, and a fresh start next click
        if (wp == OPEN_TIMER) { KillTimer(h, OPEN_TIMER); if (opening) fail(page_ready ? "no height from the page" : "no hello from the page", 0); }
        return 0;
    case WM_TRAY_REFRESH: if (shown) send_state(); return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE: if (shown) place(); return 0;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(bg); FillRect((HDC)wp, &r, b); DeleteObject(b);
        return 1;
    }
    case WM_CLOSE: { bool was = opening; release(); if (was) app_tray_menu(); return 0; }   // (a crash while opening)
    }
    return DefWindowProcW(h, msg, wp, lp);
}

extern "C" int tray_visible(void) { return tw && (shown || opening); }

extern "C" void tray_close(void) {
    if (!tw || (!shown && !opening)) return;
    shown = opening = false;
    KillTimer(tw, OPEN_TIMER);   // closed while opening: no menu after all
    post("{\"type\":\"closed\"}");   // the page goes back to its first view for next time
    ShowWindow(tw, SW_HIDE);
    if (ctrl) {
        ctrl->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2_3> w3;
        if (web && SUCCEEDED(web.As(&w3))) w3->TrySuspend(Callback<ICoreWebView2TrySuspendCompletedHandler>([](HRESULT, BOOL) -> HRESULT { return S_OK; }).Get());
    }
    SetTimer(tw, RELEASE_TIMER, RELEASE_AFTER, NULL);   // one shot: the browser part goes if it is not opened again
}

// A left click on the icon. 0: the panel cannot be made (no WebView2), the caller shows the menu instead.
extern "C" int tray_toggle(HINSTANCE inst, HWND icon) {
    if (tray_visible()) { tray_close(); return 1; }
    if (GetTickCount() - closed_at < 300) return 1;   // the click that just closed it by taking the focus away
    inst_ = inst; icon_wnd = icon;
    open_t0 = GetTickCount();
    if (!tw) {
        static bool registered;
        if (!registered) {
            CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            WNDCLASSW wc = {};
            wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"haku_control_tray";
            wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
            wc.style = CS_DROPSHADOW;
            registered = RegisterClassW(&wc) != 0;
        }
        // hidden until the page says its height, but already its width: the page lays out at the size it will have
        UINT dpi = GetDpiForSystem();
        tw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"haku_control_tray", L"haku control", WS_POPUP,
                             0, 0, MulDiv(PANEL_W, dpi, 96), MulDiv(content_h, dpi, 96), NULL, NULL, inst, NULL);
        if (!tw) { logf_("tray: window not made (%lu), the menu instead", GetLastError()); return 0; }
        DWORD round = 2 /* DWMWCP_ROUND */;   // Windows 11: the system's rounded corners and border; older: square
        DwmSetWindowAttribute(tw, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        opening = true;
        SetTimer(tw, OPEN_TIMER, OPEN_FIRST, NULL);
        theme_bg();
        create_webview();
        return tw != NULL || !opening;   // a synchronous failure has already shown the menu
    }
    KillTimer(tw, RELEASE_TIMER);
    opening = true;
    SetTimer(tw, OPEN_TIMER, page_ready ? OPEN_AGAIN : OPEN_FIRST, NULL);
    theme_bg();
    if (ctrl) ctrl->put_IsVisible(TRUE);   // resumes a suspended page
    if (page_ready) { send_state(); post("{\"type\":\"open\"}"); }
    return 1;
}

extern "C" void tray_refresh(void) { if (tw && shown) PostMessageW(tw, WM_TRAY_REFRESH, 0, 0); }
