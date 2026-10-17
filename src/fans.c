// SPDX-License-Identifier: GPL-3.0-only
// Aqua Computer QUADRO (USB 0C70:F00D): its four fan headers, temperature and flow sensors, over HID.
// Status: input report 1, about once a second (big-endian). Control: feature report 3 (CRC-16/USB at 0x3BF),
// read fresh before every change; only the 3 control bytes of a channel haku drives are patched, and the bytes
// found there first go back when the channel is set to "as is", on exit and before sleep.
// [fans] fanN = "" (as is) | fix | curve, fanN_pct = 20..100, fanN_src = gpu | t1..t4, fanN_curve = "30:25,50:40,85:100"
#include "common.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <process.h>
#include <stdlib.h>

#define FANS 4
#define MIN_PCT 20
static const int st_fan[FANS] = { 0x70, 0x7D, 0x8A, 0x97 }, ctl_fan[FANS] = { 0x36, 0x8B, 0xE0, 0x135 };

static CRITICAL_SECTION cs, io;   // cs: the readings below; io: the control handle and what was taken
static struct { int on, ser1, ser2, fw, temp[4], flow; struct { int duty, cv, ma, cw, rpm; } f[FANS]; } st;
static HANDLE th, stop_ev, hc = INVALID_HANDLE_VALUE;   // hc: control (feature reports), synchronous
static int flen, paused, bad_crc;
static BYTE orig[FANS][3]; static int taken[FANS];
static volatile LONG need_gpu, gpu_c = -1000;            // gpu_c: GPU temperature in whole degrees, fed by the render loop

static int be16(const BYTE *p) { return p[0] << 8 | p[1]; }
static int crc16usb(const BYTE *p, int n) {   // poly 0xA001 reflected, init 0xFFFF, xorout 0xFFFF
    unsigned c = 0xFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0xA001 : c >> 1; }
    return c ^ 0xFFFF;
}

// want[f]: -2 leave, -1 put the original back (if taken), else centi-percent in direct mode. Caller holds io.
static int ctl_write(const int *want) {
    static BYTE r[2048];
    if (hc == INVALID_HANDLE_VALUE) return 0;
    memset(r, 0, flen); r[0] = 3;
    if (!HidD_GetFeature(hc, r, flen)) return 0;
    if (crc16usb(r + 1, 0x3C1 - 3) != be16(r + 0x3BF)) {   // a layout this code does not know: hands off
        if (!bad_crc++) logf_("fans: QUADRO control report checksum mismatch, not writing");
        return 0;
    }
    int ch = 0, tk[FANS]; BYTE og[FANS][3];
    memcpy(tk, taken, sizeof(tk)); memcpy(og, orig, sizeof(og));
    for (int f = 0; f < FANS; f++) {
        BYTE *p = r + ctl_fan[f];
        if (want[f] >= 0) {
            if (!tk[f]) { memcpy(og[f], p, 3); tk[f] = 1; }
            BYTE n[3] = { 0, (BYTE)(want[f] >> 8), (BYTE)want[f] };
            if (memcmp(p, n, 3)) { memcpy(p, n, 3); ch = 1; }
        } else if (want[f] == -1 && tk[f]) {
            if (memcmp(p, og[f], 3)) { memcpy(p, og[f], 3); ch = 1; }
            tk[f] = 0;
        }
    }
    if (ch) {
        int c = crc16usb(r + 1, 0x3C1 - 3); r[0x3BF] = (BYTE)(c >> 8); r[0x3C0] = (BYTE)c;
        Sleep(200);   // the controller wants a pause between reports
        if (!HidD_SetFeature(hc, r, flen)) { logf_("fans: QUADRO write failed (%lu)", GetLastError()); return 0; }
    }
    memcpy(taken, tk, sizeof(tk)); memcpy(orig, og, sizeof(og));
    return 1;
}

static void restore_all(void) {   // caller holds io
    int want[FANS] = { -1, -1, -1, -1 }, any = 0;
    for (int f = 0; f < FANS; f++) any |= taken[f];
    if (any && ctl_write(want)) logf_("fans: QUADRO channels given back");
}

// "30:25,50:40,85:100" at temperature t -> percent; points in any order, flat outside them. 0 when unusable.
static int curve_at(const char *s, float t, int hot) {
    int x[8], y[8], n = 0;
    for (const char *p = s; *p && n < 8;) {
        char *e; long a = strtol(p, &e, 10);
        if (*e != ':') break;
        long b = strtol(e + 1, &e, 10);
        x[n] = (int)a; y[n] = (int)b; n++;
        p = *e == ',' ? e + 1 : e;
    }
    if (!n) return 0;
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && x[j] < x[j - 1]; j--) {   // sort by temperature
        int tx = x[j]; x[j] = x[j - 1]; x[j - 1] = tx; int ty = y[j]; y[j] = y[j - 1]; y[j - 1] = ty;
    }
    if (hot || t >= x[n - 1]) return y[n - 1];   // hot: the source is unknown, the curve's top is the safe side
    if (t <= x[0]) return y[0];
    for (int i = 1; i < n; i++) if (t <= x[i]) return x[i] == x[i - 1] ? y[i] : (int)(y[i - 1] + (y[i] - y[i - 1]) * (t - x[i - 1]) / (x[i] - x[i - 1]) + .5f);
    return y[n - 1];
}

// the configured target of each channel: -1 as is, else centi-percent
static void targets(int *want) {
    int ng = 0;
    for (int f = 0; f < FANS; f++) {
        char k[24]; want[f] = -1;
        snprintf(k, sizeof(k), "fan%d", f + 1);
        const char *m = cfg_get("fans", k, "");
        int pct = 0;
        if (!_stricmp(m, "fix")) { snprintf(k, sizeof(k), "fan%d_pct", f + 1); pct = cfg_geti("fans", k, 50); }
        else if (!_stricmp(m, "curve")) {
            snprintf(k, sizeof(k), "fan%d_src", f + 1);
            const char *src = cfg_get("fans", k, "gpu");
            float t = NAN;
            if (!_stricmp(src, "gpu")) { ng = 1; LONG g = gpu_c; if (g > -1000) t = (float)g; }
            else if ((src[0] == 't' || src[0] == 'T') && src[1] >= '1' && src[1] <= '4' && !src[2]) {
                EnterCriticalSection(&cs); int v = st.temp[src[1] - '1']; LeaveCriticalSection(&cs);
                if (v != 0x7FFF) t = (short)v / 100.0f;
            }
            snprintf(k, sizeof(k), "fan%d_curve", f + 1);
            pct = curve_at(cfg_get("fans", k, "30:30,50:45,70:75,85:100"), t, isnan(t));
            if (!pct) pct = 100;   // a curve that does not read: full speed rather than a guess
        } else continue;
        pct = pct < MIN_PCT ? MIN_PCT : pct > 100 ? 100 : pct;
        want[f] = pct * 100;
    }
    InterlockedExchange(&need_gpu, ng);
}

static HANDLE open_quadro(HANDLE *rd, int *in_len) {
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    HANDLE h = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (void *)buf; d->cbSize = sizeof(*d);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, sizeof(buf), NULL, NULL)) continue;
        if (!wcsstr(_wcslwr(d->DevicePath), L"vid_0c70&pid_f00d")) continue;
        h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp; HIDP_CAPS caps = { 0 };
        if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        *rd = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (caps.InputReportByteLength < 0xA4 || caps.FeatureReportByteLength < 0x3C1 || caps.FeatureReportByteLength > 2048 || *rd == INVALID_HANDLE_VALUE) {
            logf_("fans: QUADRO with an unknown layout (in %d, feature %d)", caps.InputReportByteLength, caps.FeatureReportByteLength);
            if (*rd != INVALID_HANDLE_VALUE) CloseHandle(*rd);
            CloseHandle(h); h = INVALID_HANDLE_VALUE; break;
        }
        *in_len = caps.InputReportByteLength; flen = caps.FeatureReportByteLength;
    }
    SetupDiDestroyDeviceInfoList(di);
    return h;
}

static void parse(const BYTE *r) {
    EnterCriticalSection(&cs);
    st.on = 1; st.ser1 = be16(r + 3); st.ser2 = be16(r + 5); st.fw = be16(r + 0xD); st.flow = be16(r + 0x6E);
    for (int t = 0; t < 4; t++) st.temp[t] = be16(r + 0x34 + 2 * t);
    for (int f = 0; f < FANS; f++) {
        const BYTE *p = r + st_fan[f];
        st.f[f].duty = be16(p); st.f[f].cv = be16(p + 2); st.f[f].ma = be16(p + 4); st.f[f].cw = be16(p + 6); st.f[f].rpm = be16(p + 8);
    }
    LeaveCriticalSection(&cs);
}

static unsigned __stdcall fans_thread(void *arg) {
    (void)arg;
    HANDLE rd = INVALID_HANDLE_VALUE, ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    static BYTE r[2048]; int in_len = 0, misses = 0;
    DWORD last_ctl = 0;
    for (;;) {
        if (rd == INVALID_HANDLE_VALUE) {
            HANDLE h = open_quadro(&rd, &in_len);
            if (h == INVALID_HANDLE_VALUE) { rd = INVALID_HANDLE_VALUE; if (WaitForSingleObject(stop_ev, 10000) == WAIT_OBJECT_0) break; continue; }
            EnterCriticalSection(&io); hc = h; bad_crc = 0; LeaveCriticalSection(&io);
            misses = 0;
            logf_("fans: QUADRO found");
        }
        OVERLAPPED ov = { 0 }; ov.hEvent = ev; ResetEvent(ev);
        DWORD got = 0; int ok = 0;
        if (in_len <= (int)sizeof(r) && (ReadFile(rd, r, in_len, &got, &ov) || GetLastError() == ERROR_IO_PENDING)) {
            HANDLE w[2] = { stop_ev, ev };
            DWORD wr = WaitForMultipleObjects(2, w, FALSE, 2000);
            if (wr != WAIT_OBJECT_0 + 1) CancelIo(rd);
            ok = GetOverlappedResult(rd, &ov, &got, TRUE) && got > 0xA4;
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_TIMEOUT) ok = -1;
        }
        if (ok == 1) { misses = 0; if (r[0] == 1) parse(r); }
        else if (ok == 0 || ++misses >= 5) {   // unplugged (or silent for 10 s): let go, look again later
            EnterCriticalSection(&io); CloseHandle(hc); hc = INVALID_HANDLE_VALUE; memset(taken, 0, sizeof(taken)); LeaveCriticalSection(&io);
            CloseHandle(rd); rd = INVALID_HANDLE_VALUE;
            EnterCriticalSection(&cs); st.on = 0; LeaveCriticalSection(&cs);
            logf_("fans: QUADRO lost");
            if (WaitForSingleObject(stop_ev, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        if (GetTickCount() - last_ctl >= 2000) {
            last_ctl = GetTickCount();
            int want[FANS]; targets(want);
            EnterCriticalSection(&io);
            if (!paused) ctl_write(want);
            LeaveCriticalSection(&io);
        }
    }
    EnterCriticalSection(&io);
    restore_all();
    if (hc != INVALID_HANDLE_VALUE) CloseHandle(hc);
    hc = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&io);
    if (rd != INVALID_HANDLE_VALUE) CloseHandle(rd);
    CloseHandle(ev);
    return 0;
}

void fans_start(void) {
    InitializeCriticalSection(&cs); InitializeCriticalSection(&io);
    stop_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = (HANDLE)_beginthreadex(NULL, 0, fans_thread, NULL, 0, NULL);
}

void fans_stop(void) {
    if (!th) return;
    SetEvent(stop_ev);
    WaitForSingleObject(th, 3000);
    CloseHandle(th); th = NULL;
}

// sleep: the QUADRO's own settings back now (the machine may not wake with haku); after waking, taken again
void fans_suspend(int on) {
    if (!th) return;
    EnterCriticalSection(&io);
    paused = on;
    if (on) restore_all();
    LeaveCriticalSection(&io);
}

void fans_feed(float gpu_temp) { InterlockedExchange(&gpu_c, isnan(gpu_temp) ? -1000 : (LONG)gpu_temp); }
int  fans_need_gpu(void) { return need_gpu; }

float fans_water(void) {
    if (!th) return NAN;
    float t = NAN;
    EnterCriticalSection(&cs);
    for (int i = 0; st.on && i < 4 && isnan(t); i++) if (st.temp[i] != 0x7FFF) t = (short)st.temp[i] / 100.0f;
    LeaveCriticalSection(&cs);
    return t;
}

int fans_json(char *out, int cap) {
    if (!th) return snprintf(out, cap, "{\"on\":0}");
    EnterCriticalSection(&cs);
    int n = snprintf(out, cap, "{\"on\":%d", st.on);
    if (st.on) {
        n += snprintf(out + n, cap - n, ",\"serial\":\"%05d-%05d\",\"fw\":%d,\"flow\":%.1f,\"t\":[", st.ser1, st.ser2, st.fw, st.flow / 10.0);
        for (int t = 0; t < 4; t++) {
            if (st.temp[t] == 0x7FFF) n += snprintf(out + n, cap - n, "%snull", t ? "," : "");
            else n += snprintf(out + n, cap - n, "%s%.1f", t ? "," : "", (short)st.temp[t] / 100.0);
        }
        n += snprintf(out + n, cap - n, "],\"f\":[");
        for (int f = 0; f < FANS; f++)
            n += snprintf(out + n, cap - n, "%s{\"rpm\":%d,\"pct\":%.1f,\"v\":%.2f,\"w\":%.2f,\"own\":%d}", f ? "," : "",
                          st.f[f].rpm, st.f[f].duty / 100.0, st.f[f].cv / 100.0, st.f[f].cw / 100.0, taken[f]);
        n += snprintf(out + n, cap - n, "]");
    }
    n += snprintf(out + n, cap - n, "}");
    LeaveCriticalSection(&cs);
    return n;
}

#ifdef FANS_TEST   // cl /DFANS_TEST /Isrc src\fans.c ... : the curve math
#include <assert.h>
void logf_(const char *fmt, ...) { (void)fmt; }
const char *cfg_get(const char *s, const char *k, const char *d) { (void)s; (void)k; return d; }
int cfg_geti(const char *s, const char *k, int d) { (void)s; (void)k; return d; }
int main(void) {
    const char *c = "50:40,30:25,85:100,70:80";
    assert(curve_at(c, 20, 0) == 25 && curve_at(c, 30, 0) == 25 && curve_at(c, 40, 0) == 33);
    assert(curve_at(c, 60, 0) == 60 && curve_at(c, 90, 0) == 100 && curve_at(c, 40, 1) == 100);
    assert(curve_at("", 50, 0) == 0 && curve_at("x", 50, 0) == 0 && curve_at("40:50", 10, 0) == 50);
    puts("fans: ok");
    return 0;
}
#endif
