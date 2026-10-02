// SPDX-License-Identifier: GPL-3.0-only
// A stand-in for G HUB's LED library, to try haku's Logitech driver without G HUB. Build (from a VS prompt):
//   cl /LD /O2 tools\sim\fake_logiled.c /Fe:bin-dev\fake_logiled.dll
// then in the dev settings: [logitech] dll=<full path of fake_logiled.dll> (test builds only). It writes what it gets
// to %TEMP%\fake_logiled.log: the calls, and once a second the frames per second and a sample colour.
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>

static FILE *out;
static int target, frames;
static DWORD t0;
static void say(const char *fmt, ...) {
    if (!out) { char p[MAX_PATH]; GetTempPathA(MAX_PATH, p); strcat_s(p, MAX_PATH, "fake_logiled.log"); out = fopen(p, "a"); }
    if (!out) return;
    va_list a; va_start(a, fmt);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(out, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    vfprintf(out, fmt, a); fputc('\n', out); fflush(out);
    va_end(a);
}
static void frame(const char *what) {
    frames++;
    if (GetTickCount() - t0 >= 1000) { say("%d frames/s, %s", frames, what); frames = 0; t0 = GetTickCount(); }
}

__declspec(dllexport) bool LogiLedInit(void) { say("LogiLedInit"); return true; }
__declspec(dllexport) bool LogiLedInitWithName(const char *name) { say("LogiLedInitWithName(%s)", name); return true; }
__declspec(dllexport) bool LogiLedSetTargetDevice(int t) { target = t; return true; }
__declspec(dllexport) bool LogiLedSetLighting(int r, int g, int b) {
    char s[64]; snprintf(s, sizeof(s), "target %d: rgb %d%% %d%% %d%%", target, r, g, b); frame(s); return true;
}
__declspec(dllexport) bool LogiLedSetLightingFromBitmap(unsigned char *bmp) {
    char s[96]; snprintf(s, sizeof(s), "target %d: bitmap, first key B%d G%d R%d A%d", target, bmp[0], bmp[1], bmp[2], bmp[3]); frame(s); return true;
}
__declspec(dllexport) void LogiLedShutdown(void) { say("LogiLedShutdown"); }
