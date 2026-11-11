// SPDX-License-Identifier: GPL-3.0-only
// What a tray icon event does (main.c WM_TRAY; checked in tools\sim\tray_test.c). The icon keeps the shell's version 0
// messages: a left or right click, and the keyboard (Enter, Space, Shift+F10 come as a right click), all toggle the
// quick panel; a double click opens the window, and the second click of it is not taken for another toggle.
#pragma once
#include <windows.h>
#include <shellapi.h>

enum { TRAY_NONE, TRAY_PANEL, TRAY_WINDOW };

static int tray_event(UINT ev, int *dbl) {
    switch (ev) {
    case NIN_BALLOONUSERCLICK: return TRAY_WINDOW;   // a notice clicked
    case WM_LBUTTONDBLCLK: *dbl = 1; return TRAY_WINDOW;
    case WM_LBUTTONUP: if (*dbl) { *dbl = 0; return TRAY_NONE; } return TRAY_PANEL;
    case WM_RBUTTONUP: *dbl = 0; return TRAY_PANEL;
    }
    return TRAY_NONE;
}
