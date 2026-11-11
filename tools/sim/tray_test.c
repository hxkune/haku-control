// SPDX-License-Identifier: GPL-3.0-only
// The tray icon's events (src\trayev.h): what each click does. The shell, the panel window and WebView2 are not here;
// the panel's own open / close / fall-back paths are in ui_tray.cpp and are checked by hand (docs\hardware-check.md).
#include <stdio.h>
#include "trayev.h"

static int bad;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) bad = 1; }

int main(void) {
    int dbl = 0;
    check(tray_event(WM_LBUTTONUP, &dbl) == TRAY_PANEL, "left click: the panel");
    check(tray_event(WM_RBUTTONUP, &dbl) == TRAY_PANEL, "right click (and the keyboard, as version 0 sends it): the panel");
    check(tray_event(WM_LBUTTONDOWN, &dbl) == TRAY_NONE && tray_event(WM_RBUTTONDOWN, &dbl) == TRAY_NONE
          && tray_event(WM_MOUSEMOVE, &dbl) == TRAY_NONE, "press and move: nothing (only the release acts)");
    // a double click comes as up, dblclk, up: the panel opened by the first is closed by the window, the last up is no toggle
    int a = tray_event(WM_LBUTTONUP, &dbl), b = tray_event(WM_LBUTTONDBLCLK, &dbl), c = tray_event(WM_LBUTTONUP, &dbl);
    check(a == TRAY_PANEL && b == TRAY_WINDOW && c == TRAY_NONE && !dbl, "double click: the window, the panel not opened again");
    check(tray_event(WM_LBUTTONUP, &dbl) == TRAY_PANEL, "a click after it: the panel again");
    tray_event(WM_LBUTTONDBLCLK, &dbl);
    check(tray_event(WM_RBUTTONUP, &dbl) == TRAY_PANEL && !dbl, "a right click after a double click: the panel, the left is not swallowed later");
    check(tray_event(WM_LBUTTONUP, &dbl) == TRAY_PANEL, "... and the next left click works");
    check(tray_event(NIN_BALLOONUSERCLICK, &dbl) == TRAY_WINDOW, "a notice clicked: the window");
    puts(bad ? "tray: FAILED" : "tray: ok");
    return bad;
}
