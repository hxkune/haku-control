// SPDX-License-Identifier: GPL-3.0-only
// Aqua Computer QUADRO (USB 0C70:F00D): its four fan headers, temperature and flow sensors, over HID.
// Status: input report 1, about once a second (big-endian). Control: feature report 3 (CRC-16/USB at 0x3BF),
// read fresh before every change; only the 3 control bytes of a channel haku drives are patched, read back after
// the write, and the bytes found there first go back when the channel is set to "as is", on exit and before sleep.
// Those first bytes are kept per QUADRO (serial) in fans-state.ini until they are back, so a lost USB link or a
// crash does not turn haku's own setting into "the original": after a crash they go back when haku next starts.
// [fans] fanN = "" (as is) | fix | curve, fanN_pct = 20..100, fanN_src = gpu | t1..t4, fanN_curve = "30:25,50:40,85:100"
//        fanN_min = the channel's floor (never under 20), fanN_fail = its speed while the curve's temperature is
//        missing or stale or the curve does not read (100 by default)
//        fanN_down = how many % a curve may slow per 2 s (1..100, 3 by default); it speeds up and goes to emergency at once
// The board's Super I/O fan controls (sio.c) are channels 5..11 (fan5..fan11), on a thread of their own with the same
// rules: the mode and duty found first are kept in fans-state.ini [board-<chip>] until they are back.
#include "common.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <process.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

#define FANS 4
#define BF 7                // the board's fan controls at most (sio.c), channels FANS+1..FANS+BF
#define NCH (FANS + BF)
#define MIN_PCT 20        // haku's floor for every channel: a channel may be set higher (fanN_min), never lower
#define STALE_MS 5000     // a temperature older than this is not used
#define GRACE_MS 5000     // after the GPU is first asked for, before its absence counts
#define DOWN_PCT 3        // fanN_down: a curve slows by at most this many % per control round (2 s); it speeds up at once
static const int st_fan[FANS] = { 0x70, 0x7D, 0x8A, 0x97 }, ctl_fan[FANS] = { 0x36, 0x8B, 0xE0, 0x135 };
enum { EM_NONE, EM_NOSENSOR, EM_STALE, EM_CURVE };
static const char *const em_name[] = { "", "nosensor", "stale", "curve" };
// why the last write of a channel did not land, for the page
enum { ER_NONE, ER_NOLINK, ER_READ, ER_CRC, ER_SAVE, ER_WRITE, ER_BACK, ER_KEPT };
static const char *const er_name[] = { "", "nolink", "read", "crc", "save", "write", "back", "kept" };

#ifdef FANS_TEST   // the self-test below stands in for the QUADRO: no device is opened
static int sim_get(BYTE *r, int n); static int sim_set(const BYTE *r, int n);
#define GET_FEATURE(r, n) sim_get(r, n)
#define SET_FEATURE(r, n) sim_set(r, n)
#define PAUSE(ms) ((void)0)
#elif defined(HAKU_DEV)   // the test build only looks
#define GET_FEATURE(r, n) HidD_GetFeature(hc, r, n)
#define SET_FEATURE(r, n) (SetLastError(ERROR_ACCESS_DENIED), FALSE)
#define PAUSE(ms) Sleep(ms)
#else
#define GET_FEATURE(r, n) HidD_GetFeature(hc, r, n)
#define SET_FEATURE(r, n) HidD_SetFeature(hc, r, n)
#define PAUSE(ms) Sleep(ms)
#endif

// cs: the readings, the channels' state for the page and the problem log; io: the control handle and what was taken
static CRITICAL_SECTION cs, io;
static struct { int on, link, ser1, ser2, fw, temp[4], flow; DWORD seen; struct { int duty, cv, ma, cw, rpm; } f[FANS]; } st;
static struct { int want, out, ok, em, own, er; } ch[NCH];   // asked (centi-%, -1 as is, -2 waiting), sent, read back (1/0/-1)
static int rfail;                                           // channels whose own setting could not be given back
static struct { char t[9], m[100]; int n; } lg[8]; static int lgn;   // recent problems, oldest first
static HANDLE th, stop_ev, hc = INVALID_HANDLE_VALUE;      // hc: control (feature reports), synchronous
static int flen, paused, bound, bad_crc;
static BYTE orig[FANS][3]; static int taken[FANS], chk[FANS], sent[FANS] = { -1, -1, -1, -1 }, cer[FANS];
static wchar_t sec[16];                                     // the serial taken/orig belong to
// the board (io): its mode and duty found first while haku holds a channel, the section they are saved under
static int bn, btaken[BF], borig[BF][2], bchk[BF], bsent[BF] = { -1, -1, -1, -1, -1, -1, -1 }, ber[BF], brfail;
static wchar_t bsec[24];
static HANDLE bth;
static struct { int n, rpm[BF], pct[BF]; char chip[16]; } bd;   // cs: the board's readings
static volatile LONG need_gpu, gpu_c = -1000, gpu_at, need_since;   // gpu_c: whole degrees, fed by the render loop

static int be16(const BYTE *p) { return p[0] << 8 | p[1]; }
static int crc16usb(const BYTE *p, int n) {   // poly 0xA001 reflected, init 0xFFFF, xorout 0xFFFF
    unsigned c = 0xFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = c & 1 ? c >> 1 ^ 0xA001 : c >> 1; }
    return c ^ 0xFFFF;
}
static int crc_ok(const BYTE *r) { return crc16usb(r + 1, 0x3C1 - 3) == be16(r + 0x3BF); }

// to the log, and to the page's short list (a repeat of the last one is counted, not added)
static void flog(const char *fmt, ...) {
    char m[100]; va_list a; va_start(a, fmt); vsnprintf(m, sizeof(m), fmt, a); va_end(a);
    for (char *p = m; *p; p++) if (*p == '"' || *p == '\\' || (BYTE)*p < 0x20) *p = '\'';
    SYSTEMTIME t; GetLocalTime(&t);
    EnterCriticalSection(&cs);
    int rep = lgn && !strcmp(lg[lgn - 1].m, m);
    if (!rep) {
        if (lgn == 8) { memmove(lg, lg + 1, sizeof(lg[0]) * 7); lgn--; }
        strcpy_s(lg[lgn].m, sizeof(lg[lgn].m), m); lg[lgn].n = 0; lgn++;
    }
    snprintf(lg[lgn - 1].t, sizeof(lg[0].t), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond); lg[lgn - 1].n++;
    LeaveCriticalSection(&cs);
    if (!rep) logf_("fans: %s", m);
}

// fans-state.ini [serial] fanN = the channel's own 3 bytes while haku holds it. Caller holds io.
static void state_path(wchar_t *p) { app_data_path(L"fans-state.ini", p); }
static int state_put(int f, const BYTE *o) {
    wchar_t p[MAX_PATH], k[8], v[8]; state_path(p); swprintf(k, 8, L"fan%d", f + 1);
    if (o) swprintf(v, 8, L"%02X%02X%02X", o[0], o[1], o[2]);
    if (WritePrivateProfileStringW(sec, k, o ? v : NULL, p)) return 1;
    if (o) flog("could not save the QUADRO's own setting of channel %d to disk (error %lu); not taking it", f + 1, GetLastError());
    return 0;
}
static void state_load(void) {
    wchar_t p[MAX_PATH], k[8], v[8]; state_path(p);
    for (int f = 0; f < FANS; f++) {
        unsigned a, b, c; swprintf(k, 8, L"fan%d", f + 1);
        GetPrivateProfileStringW(sec, k, L"", v, 8, p);
        taken[f] = wcslen(v) == 6 && swscanf_s(v, L"%2x%2x%2x", &a, &b, &c) == 3;
        if (taken[f]) { orig[f][0] = (BYTE)a; orig[f][1] = (BYTE)b; orig[f][2] = (BYTE)c; }
        sent[f] = -1;
    }
}

// want[f]: -2 leave, -1 put the original back (if taken), else centi-percent in direct mode. Caller holds io.
// res[f]: 1 read back as sent (or already so), 0 sent but not read back so, -1 not sent, 2 not touched; cer[f]: why not.
static int not_sent(const int *res, int e) { for (int f = 0; f < FANS; f++) if (res[f] != 2 && !cer[f]) cer[f] = e; return 0; }
static int ctl_write(const int *want, int *res) {
    static BYTE r[2048], b[2048];
    int tk[FANS]; BYTE og[FANS][3];
    memcpy(tk, taken, sizeof(tk)); memcpy(og, orig, sizeof(og));
    for (int f = 0; f < FANS; f++) { res[f] = want[f] >= 0 || (want[f] == -1 && tk[f]) ? -1 : 2; cer[f] = ER_NONE; }
    if (hc == INVALID_HANDLE_VALUE) return not_sent(res, ER_NOLINK);
    memset(r, 0, flen); r[0] = 3;
    if (!GET_FEATURE(r, flen)) { flog("could not read the QUADRO's settings (error %lu)", GetLastError()); return not_sent(res, ER_READ); }
    if (!crc_ok(r)) {   // a layout this code does not know: hands off
        if (!bad_crc++) flog("the QUADRO's settings fail their checksum; not writing");
        return not_sent(res, ER_CRC);
    }
    for (int f = 0; f < FANS; f++) {   // first look after (re)connecting: is what was kept still what to give back?
        BYTE *p = r + ctl_fan[f];
        if (!chk[f] || !tk[f]) { chk[f] = 0; continue; }
        chk[f] = 0;
        if (!memcmp(p, og[f], 3)) { tk[f] = 0; state_put(f, NULL); }   // already its own setting
        else if (p[0] != 0) {   // not haku's direct mode: changed elsewhere meanwhile, that is its setting now
            tk[f] = 0; state_put(f, NULL);
            flog("channel %d was changed outside haku; its old saved setting is dropped", f + 1);
        }
    }
    int chg = 0;
    for (int f = 0; f < FANS; f++) {
        BYTE *p = r + ctl_fan[f];
        if (want[f] >= 0) {
            if (!tk[f]) {   // its own setting on disk before haku touches it; if that fails it is left alone, tried again next round
                if (!state_put(f, p)) { cer[f] = ER_SAVE; continue; }
                memcpy(og[f], p, 3); tk[f] = 1;
            }
            BYTE n[3] = { 0, (BYTE)(want[f] >> 8), (BYTE)want[f] };
            if (memcmp(p, n, 3)) { memcpy(p, n, 3); chg = 1; }
        } else if (want[f] == -1 && tk[f] && memcmp(p, og[f], 3)) { memcpy(p, og[f], 3); chg = 1; }
    }
    int ok = 1;
    if (chg) {
        int c = crc16usb(r + 1, 0x3C1 - 3); r[0x3BF] = (BYTE)(c >> 8); r[0x3C0] = (BYTE)c;
        PAUSE(200);   // the controller wants a pause between reports
        if (!SET_FEATURE(r, flen)) {
            flog("writing to the QUADRO failed (error %lu)", GetLastError());
            memcpy(taken, tk, sizeof(tk)); memcpy(orig, og, sizeof(og));   // maybe half done: still to give back
            return not_sent(res, ER_WRITE);
        }
        PAUSE(200);
        memset(b, 0, flen); b[0] = 3;
        if (!GET_FEATURE(b, flen) || !crc_ok(b)) { flog("could not read the QUADRO's settings back after writing"); ok = 0; }
    } else memcpy(b, r, flen);   // nothing to change: what was just read is the check
    for (int f = 0; f < FANS; f++) {
        if (res[f] == 2 || cer[f] == ER_SAVE) continue;
        int same = ok && !memcmp(b + ctl_fan[f], r + ctl_fan[f], 3);
        res[f] = same;
        if (!same) cer[f] = ok ? ER_KEPT : ER_BACK;
        if (!same && ok) flog("the QUADRO did not keep the setting of channel %d", f + 1);
        if (same && want[f] == -1) { tk[f] = 0; state_put(f, NULL); }
    }
    memcpy(taken, tk, sizeof(tk)); memcpy(orig, og, sizeof(og));
    return ok;
}

// every taken channel its own setting back; how many could not be. Caller holds io.
static int restore_all(void) {
    int want[FANS] = { -1, -1, -1, -1 }, res[FANS], left = 0;
    for (int f = 0; f < FANS; f++) left += taken[f];
    for (int k = 0; left && k < 2; k++) {
        if (k) PAUSE(500);
        ctl_write(want, res); left = 0;
        for (int f = 0; f < FANS; f++) left += taken[f];
    }
    for (int f = 0; f < FANS; f++) if (!taken[f]) sent[f] = -1;
    if (left) flog("%d channel(s) could not be given back to the QUADRO; haku gives them back when it next starts", left);
    EnterCriticalSection(&cs); rfail = left; for (int f = 0; f < FANS; f++) ch[f].own = taken[f]; LeaveCriticalSection(&cs);
    return left;
}

// "30:25,50:40,85:100": 2..8 points, temperature 0..110 °C rising strictly, speed 0..100 %; anything else: 0
static int curve_parse(const char *s, int *x, int *y) {
    int n = 0;
    for (const char *p = s;;) {
        char *e;
        if (!isdigit((BYTE)*p)) return 0;
        long a = strtol(p, &e, 10);
        if (*e != ':' || !isdigit((BYTE)e[1])) return 0;
        long b = strtol(e + 1, &e, 10);
        if (n == 8 || a > 110 || b > 100 || (n && a <= x[n - 1])) return 0;
        x[n] = (int)a; y[n] = (int)b; n++;
        if (!*e) break;
        if (*e != ',') return 0;
        p = e + 1;
    }
    return n >= 2 ? n : 0;
}
static int curve_at(const char *s, float t) {   // percent at t, flat outside the points; -1 when the curve does not read
    int x[8], y[8], n = curve_parse(s, x, y);
    if (!n) return -1;
    if (t <= x[0]) return y[0];
    for (int i = 1; i < n; i++) if (t <= x[i]) return (int)(y[i - 1] + (y[i] - y[i - 1]) * (t - x[i - 1]) / (x[i] - x[i - 1]) + .5f);
    return y[n - 1];
}

typedef struct { int want, em, smooth; } tgt_t;   // want: centi-percent, -1 as is, -2 wait (leave untouched)
static int pct_key(int f, const char *k, int d) { char b[24]; snprintf(b, sizeof(b), "fan%d_%s", f + 1, k); return cfg_geti("fans", b, d); }
static void targets(tgt_t *t) {
    DWORD now = GetTickCount(); int ng = 0;
    for (int f = 0; f < NCH; f++) {
        char k[24]; t[f].want = -1; t[f].em = EM_NONE; t[f].smooth = 0;
        snprintf(k, sizeof(k), "fan%d", f + 1);
        const char *m = cfg_get("fans", k, "");
        int lo = pct_key(f, "min", MIN_PCT), pct;
        lo = lo < MIN_PCT ? MIN_PCT : lo > 100 ? 100 : lo;
        if (!_stricmp(m, "fix")) pct = pct_key(f, "pct", 50);
        else if (!_stricmp(m, "curve")) {
            snprintf(k, sizeof(k), "fan%d_src", f + 1);
            // unset: the CPU where it reads, else the GPU
            const char *src = cfg_get("fans", k, isnan(cpu_temp()) ? "gpu" : "cpu");
            float v = NAN; int em = EM_NONE, gpu = !_stricmp(src, "gpu"), cpu = !_stricmp(src, "cpu");
            if (cpu) { if (isnan(v = cpu_temp())) em = EM_NOSENSOR; }
            else if (gpu) {
                if (!ng++ && !need_gpu) InterlockedExchange(&need_since, (LONG)now);   // the grace starts now
                LONG g = gpu_c; DWORD at = (DWORD)gpu_at;
                if (!at || now - at > STALE_MS) em = at ? EM_STALE : EM_NOSENSOR;
                else if (g <= -1000) em = EM_NOSENSOR;
                else v = (float)g;
                if (em && now - (DWORD)need_since < GRACE_MS) { t[f].want = -2; continue; }   // NVML still loading
            } else if ((src[0] == 't' || src[0] == 'T') && src[1] >= '1' && src[1] <= '4' && !src[2]) {
                EnterCriticalSection(&cs);
                int s = st.temp[src[1] - '1'], fresh = st.link && now - st.seen <= STALE_MS;
                LeaveCriticalSection(&cs);
                if (!fresh) em = EM_STALE;
                else if (s == 0x7FFF) em = EM_NOSENSOR;
                else v = (short)s / 100.0f;
            } else em = EM_CURVE;
            snprintf(k, sizeof(k), "fan%d_curve", f + 1);
            if (!em && (pct = curve_at(cfg_get("fans", k, cpu ? "40:30,60:45,75:75,90:100" : gpu ? "30:30,50:45,70:75,85:100" : "25:30,30:40,35:70,40:100"), v)) < 0) em = EM_CURVE;
            if (em) { pct = pct_key(f, "fail", 100); t[f].em = em; }
            else t[f].smooth = 1;
        } else continue;
        pct = pct < lo ? lo : pct > 100 ? 100 : pct;
        t[f].want = pct * 100;
    }
    InterlockedExchange(&need_gpu, ng != 0);
}

// a curve slows by at most fanN_down per round from what was last sent; anything else as asked
static int eased(int f, const tgt_t *t, int sent_) {
    int dn = pct_key(f, "down", DOWN_PCT); dn = (dn < 1 ? 1 : dn > 100 ? 100 : dn) * 100;
    return t->want >= 0 && t->smooth && sent_ >= 0 && t->want < sent_ - dn ? sent_ - dn : t->want;
}

// one control round: targets, a curve eased down, written and read back; the page's view of each channel
static void control(void) {
    tgt_t t[NCH]; targets(t);
    int want[FANS], res[FANS], fail = 0;
    EnterCriticalSection(&io);
    if (paused || !bound) { LeaveCriticalSection(&io); return; }
    for (int f = 0; f < FANS; f++) want[f] = eased(f, &t[f], sent[f]);
    ctl_write(want, res);
    for (int f = 0; f < FANS; f++) {
        if (want[f] >= 0 && res[f] >= 0) sent[f] = want[f];
        if (!taken[f]) sent[f] = -1;
        fail += want[f] == -1 && taken[f];
    }
    EnterCriticalSection(&cs);
    rfail = fail;
    for (int f = 0; f < FANS; f++) {
        ch[f].want = t[f].want; ch[f].out = sent[f]; ch[f].em = t[f].em; ch[f].own = taken[f]; ch[f].er = cer[f];
        ch[f].ok = res[f] == 2 ? 1 : res[f];
    }
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&io);
}

// the first report after (re)connecting names the QUADRO: the same one keeps what was taken, another one loads its own
static void bind_serial(void) {
    wchar_t s[16];
    EnterCriticalSection(&cs); swprintf(s, 16, L"%05d-%05d", st.ser1, st.ser2); LeaveCriticalSection(&cs);
    EnterCriticalSection(&io);
    if (wcscmp(s, sec)) {
        int any = 0; for (int f = 0; f < FANS; f++) any |= taken[f];
        if (any) flog("another QUADRO is connected; what haku keeps for the first one stays saved for it");
        wcscpy_s(sec, 16, s); state_load();
    }
    for (int f = 0; f < FANS; f++) chk[f] = taken[f];
    bound = 1;
    LeaveCriticalSection(&io);
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
    st.on = st.link = 1; st.seen = GetTickCount();
    st.ser1 = be16(r + 3); st.ser2 = be16(r + 5); st.fw = be16(r + 0xD); st.flow = be16(r + 0x6E);
    for (int t = 0; t < 4; t++) st.temp[t] = be16(r + 0x34 + 2 * t);
    for (int f = 0; f < FANS; f++) {
        const BYTE *p = r + st_fan[f];
        st.f[f].duty = be16(p); st.f[f].cv = be16(p + 2); st.f[f].ma = be16(p + 4); st.f[f].cw = be16(p + 6); st.f[f].rpm = be16(p + 8);
    }
    LeaveCriticalSection(&cs);
}

// ---- the board's fan controls (sio.c): channel FANS+b+1 is control b. Callers hold io.
// fans-state.ini [board-<chip>] fanN = its mode and duty ("MMCC") while haku holds it
static int bstate_put(int b, int m, int c) {
    wchar_t p[MAX_PATH], k[8], v[8]; state_path(p); swprintf(k, 8, L"fan%d", FANS + b + 1);
    if (m >= 0) swprintf(v, 8, L"%02X%02X", m, c);
    if (WritePrivateProfileStringW(bsec, k, m >= 0 ? v : NULL, p)) return 1;
    if (m >= 0) flog("could not save the board's own setting of channel %d to disk (error %lu); not taking it", FANS + b + 1, GetLastError());
    return 0;
}
static void bstate_load(void) {
    wchar_t p[MAX_PATH], k[8], v[8]; state_path(p);
    for (int b = 0; b < bn; b++) {
        unsigned m, c; swprintf(k, 8, L"fan%d", FANS + b + 1);
        GetPrivateProfileStringW(bsec, k, L"", v, 8, p);
        btaken[b] = wcslen(v) == 4 && swscanf_s(v, L"%2x%2x", &m, &c) == 2;
        if (btaken[b]) { borig[b][0] = m; borig[b][1] = c; }
        bchk[b] = btaken[b]; bsent[b] = -1;
    }
}

// want: -2 leave, -1 its own setting back (if taken), else centi-percent in manual mode.
// 1 read back as sent (or already so), 0 sent but not read back so, -1 not sent, 2 not touched; ber[b]: why not.
static int board_write(int b, int want) {
    int m, c, v = (want * 255 + 5000) / 10000;
    ber[b] = ER_NONE;
    if (want == -2 || (want == -1 && !btaken[b])) return 2;
    if (!sio_get(b, &m, &c)) { ber[b] = ER_READ; return -1; }
    if (bchk[b]) {   // first look after starting or waking: is what was kept still what to give back?
        bchk[b] = 0;
        if (m == borig[b][0] && c == borig[b][1]) { btaken[b] = 0; bstate_put(b, -1, 0); }
        else if (m != 0) {   // not manual: the BIOS or another program set it meanwhile, that is its setting now
            btaken[b] = 0; bstate_put(b, -1, 0);
            flog("channel %d was changed outside haku; its old saved setting is dropped", FANS + b + 1);
        }
        if (want == -1 && !btaken[b]) return 2;
    }
    if (want == -1) {
        if ((m != borig[b][0] || c != borig[b][1]) && (!sio_put(b, borig[b][0], borig[b][1]) || !sio_get(b, &m, &c))) { ber[b] = ER_WRITE; return -1; }
        if (m != borig[b][0] || c != borig[b][1]) { ber[b] = ER_KEPT; flog("the board did not take back the setting of channel %d", FANS + b + 1); return 0; }
        btaken[b] = 0; bstate_put(b, -1, 0);
        return 1;
    }
    if (!btaken[b]) {   // its own setting on disk before haku touches it; if that fails it is left alone, tried again next round
        if (!bstate_put(b, m, c)) { ber[b] = ER_SAVE; return -1; }
        borig[b][0] = m; borig[b][1] = c; btaken[b] = 1;
    }
    if ((m != 0 || c != v) && (!sio_put(b, 0, v) || !sio_get(b, &m, &c))) { ber[b] = ER_WRITE; return -1; }
    if (m != 0 || c != v) { ber[b] = ER_KEPT; flog("the board did not keep the setting of channel %d", FANS + b + 1); return 0; }
    return 1;
}

// every taken control its own setting back; how many could not be
static int board_restore(void) {
    int left = 0;
    for (int b = 0; b < bn; b++) left += btaken[b];
    for (int k = 0; left && k < 2; k++) {
        if (k) PAUSE(500);
        left = 0;
        for (int b = 0; b < bn; b++) if (btaken[b]) { board_write(b, -1); left += btaken[b]; }
    }
    for (int b = 0; b < bn; b++) if (!btaken[b]) bsent[b] = -1;
    if (left) flog("%d channel(s) could not be given back to the board; haku gives them back when it next starts", left);
    EnterCriticalSection(&cs); brfail = left; for (int b = 0; b < bn; b++) ch[FANS + b].own = btaken[b]; LeaveCriticalSection(&cs);
    return left;
}

// one round on the board: its readings, then the same targets and easing as the QUADRO's
static void board_round(void) {
    int rpm[BF], pct[BF], res[BF], fail = 0, ok = sio_read(rpm, pct);
    tgt_t t[NCH]; targets(t);
    EnterCriticalSection(&cs);
    for (int b = 0; b < bn; b++) { bd.rpm[b] = ok ? rpm[b] : -1; bd.pct[b] = ok ? pct[b] : -1; }
    LeaveCriticalSection(&cs);
    EnterCriticalSection(&io);
    if (paused) { LeaveCriticalSection(&io); return; }
    for (int b = 0; b < bn; b++) {
        int f = FANS + b, want = eased(f, &t[f], bsent[b]);
        res[b] = board_write(b, want);
        if (want >= 0 && res[b] >= 0) bsent[b] = want;
        if (!btaken[b]) bsent[b] = -1;
        fail += want == -1 && btaken[b];
    }
    EnterCriticalSection(&cs);
    brfail = fail;
    for (int b = 0; b < bn; b++) {
        int f = FANS + b;
        ch[f].want = t[f].want; ch[f].out = bsent[b]; ch[f].em = t[f].em; ch[f].own = btaken[b]; ch[f].er = ber[b];
        ch[f].ok = res[b] == 2 ? 1 : res[b];
    }
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&io);
}

static unsigned __stdcall board_thread(void *arg) {
    (void)arg;
    char chip[16] = ""; int n = sio_open(chip, sizeof(chip));
    if (!n) return 0;
    EnterCriticalSection(&io);
    swprintf(bsec, 24, L"board-%hs", chip); bn = n < BF ? n : BF; bstate_load();
    LeaveCriticalSection(&io);
    EnterCriticalSection(&cs); strcpy_s(bd.chip, sizeof(bd.chip), chip); bd.n = bn; LeaveCriticalSection(&cs);
    do board_round(); while (WaitForSingleObject(stop_ev, 2000) == WAIT_TIMEOUT);
    EnterCriticalSection(&io); board_restore(); LeaveCriticalSection(&io);
    sio_close();
    return 0;
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
            EnterCriticalSection(&io); hc = h; bad_crc = 0; bound = 0; LeaveCriticalSection(&io);
            misses = 0;
            logf_("fans: QUADRO found");
        }
        OVERLAPPED ov = { 0 }; ov.hEvent = ev; ResetEvent(ev);
        DWORD got = 0; int ok = 0, err = 0;
        if (in_len <= (int)sizeof(r) && (ReadFile(rd, r, in_len, &got, &ov) || GetLastError() == ERROR_IO_PENDING)) {
            HANDLE w[2] = { stop_ev, ev };
            DWORD wr = WaitForMultipleObjects(2, w, FALSE, 2000);
            if (wr != WAIT_OBJECT_0 + 1) CancelIo(rd);
            ok = GetOverlappedResult(rd, &ov, &got, TRUE) && got > 0xA4;
            if (!ok) err = GetLastError();
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_TIMEOUT) ok = -1;
        } else err = GetLastError();
        if (ok == 1) {
            misses = 0;
            if (r[0] == 1) { parse(r); if (!bound) bind_serial(); }
        } else if (ok == 0 || ++misses >= 5) {   // unplugged (or silent for 10 s): let go, look again later; what was taken stays
            EnterCriticalSection(&io); CloseHandle(hc); hc = INVALID_HANDLE_VALUE; bound = 0; LeaveCriticalSection(&io);
            CloseHandle(rd); rd = INVALID_HANDLE_VALUE;
            EnterCriticalSection(&cs); st.link = 0; LeaveCriticalSection(&cs);
            if (ok == 0) flog("lost the QUADRO (USB error %d)", err);
            else flog("lost the QUADRO: no reports for 10 s");
            if (WaitForSingleObject(stop_ev, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        if (GetTickCount() - last_ctl >= 2000) { last_ctl = GetTickCount(); control(); }
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
    bth = (HANDLE)_beginthreadex(NULL, 0, board_thread, NULL, 0, NULL);
}

void fans_stop(void) {
    if (!th) return;
    SetEvent(stop_ev);
    WaitForSingleObject(th, 3000);
    CloseHandle(th); th = NULL;
    if (bth) { WaitForSingleObject(bth, 3000); CloseHandle(bth); bth = NULL; }
}

// sleep: the QUADRO's and the board's own settings back now (the machine may not wake with haku); after waking, taken again
void fans_suspend(int on) {
    if (!th) return;
    EnterCriticalSection(&io);
    paused = on;
    if (on) { restore_all(); board_restore(); }
    else {   // one that could not go back: look again before writing
        for (int f = 0; f < FANS; f++) chk[f] = taken[f];
        for (int b = 0; b < bn; b++) bchk[b] = btaken[b];
    }
    LeaveCriticalSection(&io);
}

// at: when the GPU was last asked (the render loop passes the same reading on every frame in between)
void fans_feed(float gpu_temp, DWORD at) { InterlockedExchange(&gpu_c, isnan(gpu_temp) ? -1000 : (LONG)gpu_temp); InterlockedExchange(&gpu_at, (LONG)at); }
int  fans_need_gpu(void) { return need_gpu; }

float fans_water(void) {
    if (!th) return NAN;
    float t = NAN;
    EnterCriticalSection(&cs);
    int fresh = st.link && GetTickCount() - st.seen <= STALE_MS;
    for (int i = 0; fresh && i < 4 && isnan(t); i++) if (st.temp[i] != 0x7FFF) t = (short)st.temp[i] / 100.0f;
    LeaveCriticalSection(&cs);
    return t;
}

int fans_json(char *out, int cap) {
    if (!th) return snprintf(out, cap, "{\"on\":0}");
    DWORD now = GetTickCount(), at = (DWORD)gpu_at; LONG g = gpu_c; float cpu = cpu_temp();
    EnterCriticalSection(&cs);
    // f[i] is channel i+1: the QUADRO's four (null without one), then the board's (b:1)
    int n = snprintf(out, cap, "{\"on\":%d", st.on || bd.n);
    if (st.on || bd.n) {
        n += snprintf(out + n, cap - n, ",\"q\":%d,\"paused\":%d,\"rfail\":%d", st.on, paused, rfail + brfail);
        if (bd.n) n += snprintf(out + n, cap - n, ",\"chip\":\"%s\"", bd.chip);
        if (st.on) {
            n += snprintf(out + n, cap - n, ",\"link\":%d,\"age\":%lu,\"serial\":\"%05d-%05d\",\"fw\":%d,\"flow\":%.1f,\"t\":[",
                          st.link, now - st.seen, st.ser1, st.ser2, st.fw, st.flow / 10.0);
            for (int t = 0; t < 4; t++) {
                if (st.temp[t] == 0x7FFF) n += snprintf(out + n, cap - n, "%snull", t ? "," : "");
                else n += snprintf(out + n, cap - n, "%s%.1f", t ? "," : "", (short)st.temp[t] / 100.0);
            }
            n += snprintf(out + n, cap - n, "]");
        }
        if (isnan(cpu)) n += snprintf(out + n, cap - n, ",\"cpu\":null");
        else n += snprintf(out + n, cap - n, ",\"cpu\":%.0f", cpu);
        if (g > -1000 && at) n += snprintf(out + n, cap - n, ",\"gpu\":%ld,\"gage\":%lu,\"f\":[", g, now - at);
        else n += snprintf(out + n, cap - n, ",\"gpu\":null,\"f\":[");
        for (int f = 0; f < FANS + bd.n; f++) {
            const char *c = f ? "," : "";
            if (f < FANS && !st.on) { n += snprintf(out + n, cap - n, "%snull", c); continue; }
            if (f < FANS) n += snprintf(out + n, cap - n, "%s{\"rpm\":%d,\"pct\":%.1f,\"v\":%.2f,\"w\":%.2f", c, st.f[f].rpm, st.f[f].duty / 100.0, st.f[f].cv / 100.0, st.f[f].cw / 100.0);
            else if (bd.pct[f - FANS] < 0) n += snprintf(out + n, cap - n, "%s{\"b\":1,\"rpm\":-1,\"pct\":null", c);
            else n += snprintf(out + n, cap - n, "%s{\"b\":1,\"rpm\":%d,\"pct\":%d", c, bd.rpm[f - FANS], bd.pct[f - FANS]);
            n += snprintf(out + n, cap - n, ",\"own\":%d,\"want\":%d,\"out\":%d,\"ok\":%d,\"em\":\"%s\",\"err\":\"%s\"}",
                          ch[f].own, ch[f].want < 0 ? ch[f].want : ch[f].want / 100, ch[f].out < 0 ? -1 : ch[f].out / 100, ch[f].ok, em_name[ch[f].em], er_name[ch[f].er]);
        }
        n += snprintf(out + n, cap - n, "],\"log\":[");
        for (int i = 0; i < lgn; i++) n += snprintf(out + n, cap - n, "%s{\"t\":\"%s\",\"m\":\"%s\",\"n\":%d}", i ? "," : "", lg[i].t, lg[i].m, lg[i].n);
        n += snprintf(out + n, cap - n, "]");
    }
    n += snprintf(out + n, cap - n, "}");
    LeaveCriticalSection(&cs);
    return n;
}

#ifdef FANS_TEST   // cl /DFANS_TEST src\fans.c setupapi.lib hid.lib: curves, emergency, writes, reconnects, restore
#include <assert.h>
static BYTE dev[0x3C1];                       // the simulated QUADRO's feature report 3
static int get_fail, set_fail, set_drop = -1;  // set_drop: the channel the device does not keep
static int sim_get(BYTE *r, int n) { if (get_fail) return 0; memcpy(r, dev, n); return 1; }
static void sim_crc(void) { int c = crc16usb(dev + 1, 0x3C1 - 3); dev[0x3BF] = (BYTE)(c >> 8); dev[0x3C0] = (BYTE)c; }
static int sim_set(const BYTE *r, int n) {
    if (set_fail) return 0;
    BYTE keep[3]; if (set_drop >= 0) memcpy(keep, dev + ctl_fan[set_drop], 3);
    memcpy(dev, r, n);
    if (set_drop >= 0) { memcpy(dev + ctl_fan[set_drop], keep, 3); sim_crc(); }
    return 1;
}
static const char *K[16][2]; static int nk;   // the [fans] settings
static void set(const char *k, const char *v) { for (int i = 0; i < nk; i++) if (!strcmp(K[i][0], k)) { K[i][1] = v; return; } K[nk][0] = k; K[nk++][1] = v; }
void logf_(const char *fmt, ...) { (void)fmt; }
const char *cfg_get(const char *s, const char *k, const char *d) { (void)s; for (int i = 0; i < nk; i++) if (!strcmp(K[i][0], k)) return K[i][1] && *K[i][1] ? K[i][1] : d; return d; }   // empty is unset, as in config.c
int cfg_geti(const char *s, const char *k, int d) { const char *v = cfg_get(s, k, NULL); return v ? atoi(v) : d; }
void app_data_path(const wchar_t *name, wchar_t *out) { wchar_t t[MAX_PATH]; GetTempPathW(MAX_PATH, t); swprintf(out, MAX_PATH, L"%shaku-test-%s", t, name); }
static const BYTE O1[3] = { 2, 0x11, 0x22 }, O2[3] = { 1, 0x33, 0x44 };   // the channels' own settings: curve / fixed modes
static int dev_is(int f, int pct) { BYTE *p = dev + ctl_fan[f]; return p[0] == 0 && be16(p + 1) == pct * 100; }
static int saved(int f) { wchar_t p[MAX_PATH], v[8], k[8]; state_path(p); swprintf(k, 8, L"fan%d", f + 1); GetPrivateProfileStringW(sec, k, L"", v, 8, p); return v[0] != 0; }
static void plug(int ser) { st.on = st.link = 1; st.seen = GetTickCount(); st.ser1 = 4976; st.ser2 = ser; hc = (HANDLE)1; bind_serial(); }
static void unlink_(void) { st.link = 0; hc = INVALID_HANDLE_VALUE; bound = 0; }
// the simulated board: its controls' mode and duty
static int smode[BF], scmd[BF], sget_fail, sput_fail, sdrop = -1;   // sdrop: the control the chip does not keep
int sio_open(char *chip, int cap) { (void)chip; (void)cap; return 0; }
void sio_close(void) {}
static float scpu = NAN; float cpu_temp(void) { return scpu; }
int sio_read(int *rpm, int *pct) { for (int b = 0; b < bn; b++) { rpm[b] = 900; pct[b] = scmd[b] * 100 / 255; } return 1; }
int sio_get(int i, int *m, int *c) { if (sget_fail) return 0; *m = smode[i]; *c = scmd[i]; return 1; }
int sio_put(int i, int m, int c) { if (sput_fail) return 0; if (i != sdrop) { smode[i] = m; scmd[i] = c; } return 1; }
static int bsaved(int b) { wchar_t p[MAX_PATH], v[8], k[8]; state_path(p); swprintf(k, 8, L"fan%d", FANS + b + 1); GetPrivateProfileStringW(bsec, k, L"", v, 8, p); return v[0] != 0; }
static int bis(int b, int m, int c) { return smode[b] == m && scmd[b] == c; }
int main(void) {
    InitializeCriticalSection(&cs); InitializeCriticalSection(&io);
    wchar_t ini[MAX_PATH]; state_path(ini); DeleteFileW(ini);
    flen = 0x3C1;
    // curves: points in order, in range, readable; flat outside
    const char *c = "30:25,50:40,70:80,85:100";
    assert(curve_at(c, 20) == 25 && curve_at(c, 30) == 25 && curve_at(c, 40) == 33 && curve_at(c, 60) == 60 && curve_at(c, 90) == 100);
    assert(curve_at("50:40,30:25", 40) == -1 && curve_at("30:20,30:40", 30) == -1);          // out of order, same temperature twice
    assert(curve_at("", 50) == -1 && curve_at("x", 50) == -1 && curve_at("40:50", 10) == -1); // empty, garbage, one point
    assert(curve_at("30:20,120:50", 50) == -1 && curve_at("30:20,60:101", 50) == -1 && curve_at("30:-5,60:50", 50) == -1);
    assert(curve_at("30:20,60:50,", 50) == -1 && curve_at("30:20,60:50x", 50) == -1 && curve_at(" 30:20,60:50", 50) == -1);
    assert(curve_at("10:20,20:30,30:40,40:50,50:60,60:70,70:80,80:90", 85) == 90 && curve_at("10:20,20:30,30:40,40:50,50:60,60:70,70:80,80:90,90:100", 85) == -1);
    // targets: a GPU curve without a fresh GPU reading runs at the channel's emergency speed, not at the curve's last point
    tgt_t t[NCH]; DWORD now = GetTickCount();
    set("fan1", "curve"); set("fan1_curve", "30:20,90:20");
    fans_feed(NAN, 0); targets(t); assert(t[0].want == -2);                    // NVML still loading: left alone for a moment
    need_since = (LONG)(now - 10000);
    targets(t); assert(t[0].want == 10000 && t[0].em == EM_NOSENSOR);         // never read
    fans_feed(NAN, GetTickCount()); targets(t); assert(t[0].em == EM_NOSENSOR); // read, nothing there
    fans_feed(50, GetTickCount() - 6000); targets(t); assert(t[0].want == 10000 && t[0].em == EM_STALE);
    fans_feed(50, GetTickCount()); targets(t); assert(t[0].want == 2000 && !t[0].em && t[0].smooth);
    set("fan1_fail", "70"); set("fan1_curve", "30:20,20:90"); targets(t); assert(t[0].want == 7000 && t[0].em == EM_CURVE);
    set("fan1_src", "t9"); set("fan1_curve", "30:20,90:20"); targets(t); assert(t[0].em == EM_CURVE);
    // the CPU: followed when unset and it reads; asked for and not reading is no sensor
    set("fan1_src", NULL); set("fan1_curve", "30:20,90:80"); fans_feed(NAN, GetTickCount());
    scpu = 60; targets(t); assert(!t[0].em && t[0].want == 5000);
    set("fan1_curve", NULL); targets(t); assert(t[0].want == 4500);           // its own default curve
    set("fan1_src", "cpu"); scpu = NAN; targets(t); assert(t[0].em == EM_NOSENSOR && t[0].want == 7000);
    set("fan1_src", NULL); set("fan1_curve", "30:20,90:20"); targets(t); assert(t[0].want == -2);   // unset falls back to the GPU: its grace starts
    // a QUADRO sensor: missing, stale, lost link, fresh; the channel's floor
    set("fan1_src", "t2"); st.temp[1] = 0x7FFF; st.link = 1; st.seen = GetTickCount();
    targets(t); assert(t[0].em == EM_NOSENSOR && t[0].want == 7000);
    st.temp[1] = 3140; targets(t); assert(!t[0].em && t[0].want == 2000);
    set("fan1_min", "35"); targets(t); assert(t[0].want == 3500);
    set("fan1_min", "5"); targets(t); assert(t[0].want == 2000);              // never under haku's floor
    st.seen = GetTickCount() - 6000; targets(t); assert(t[0].em == EM_STALE);
    st.seen = GetTickCount(); st.link = 0; targets(t); assert(t[0].em == EM_STALE);
    set("fan1", "fix"); set("fan1_pct", "5"); targets(t); assert(t[0].want == 2000 && !t[0].smooth);
    set("fan1_pct", "150"); targets(t); assert(t[0].want == 10000);
    set("fan1", ""); set("fan1_min", NULL); set("fan1_fail", NULL);
    // writes: taken, its own bytes on disk first, read back
    memcpy(dev + ctl_fan[0], O1, 3); memcpy(dev + ctl_fan[1], O2, 3); sim_crc();
    plug(37680);
    int w[FANS] = { 4000, -2, -2, -2 }, res[FANS];
    assert(ctl_write(w, res) && res[0] == 1 && res[1] == 2 && taken[0] && dev_is(0, 40) && saved(0) && !saved(1));
    set_fail = 1; w[0] = 5000; assert(!ctl_write(w, res) && res[0] == -1 && dev_is(0, 40)); set_fail = 0;
    get_fail = 1; assert(!ctl_write(w, res) && res[0] == -1); get_fail = 0;
    dev[100] ^= 1; assert(!ctl_write(w, res) && res[0] == -1 && dev_is(0, 40)); dev[100] ^= 1; bad_crc = 0;   // checksum off: hands off
    set_drop = 1; w[1] = 3000; assert(ctl_write(w, res) && res[0] == 1 && res[1] == 0 && dev_is(0, 50)); set_drop = -1;   // not kept: not confirmed
    w[1] = -1; assert(ctl_write(w, res) && res[1] == 1 && !taken[1] && !saved(1) && !memcmp(dev + ctl_fan[1], O2, 3));
    // the USB link drops with the QUADRO powered, comes back: the original stays the QUADRO's, not haku's 50 %
    unlink_(); w[0] = -1; assert(!ctl_write(w, res) && res[0] == -1 && taken[0]);
    plug(37680); assert(taken[0] && !memcmp(orig[0], O1, 3));
    w[0] = 6000; assert(ctl_write(w, res) && dev_is(0, 60) && !memcmp(orig[0], O1, 3));
    // another QUADRO on the same port: nothing of the first one is written to it, and the first one's stays saved
    unlink_(); plug(11111); assert(!taken[0]);
    w[0] = -1; assert(ctl_write(w, res) && res[0] == 2 && dev_is(0, 60));
    unlink_(); plug(37680); assert(taken[0] && !memcmp(orig[0], O1, 3));
    // a crash: nothing in memory, the saved setting goes back at the next start
    unlink_(); memset(taken, 0, sizeof(taken)); memset(orig, 0, sizeof(orig)); sec[0] = 0;
    plug(37680); assert(taken[0] && !memcmp(orig[0], O1, 3));
    assert(!restore_all() && !memcmp(dev + ctl_fan[0], O1, 3) && !saved(0) && !rfail);
    // saved, but changed elsewhere meanwhile (not haku's direct mode): dropped, not written over
    w[0] = 4000; ctl_write(w, res); assert(dev_is(0, 40));
    unlink_(); memset(taken, 0, sizeof(taken)); sec[0] = 0; dev[ctl_fan[0]] = 3; sim_crc();
    plug(37680); w[0] = -1; assert(ctl_write(w, res) && !taken[0] && !saved(0) && dev[ctl_fan[0]] == 3);
    // giving back fails: reported, and kept on disk for the next start
    memcpy(dev + ctl_fan[0], O1, 3); sim_crc(); w[0] = 4000; ctl_write(w, res);
    set_fail = 1; assert(restore_all() == 1 && rfail == 1 && taken[0] && saved(0)); set_fail = 0;
    assert(!restore_all() && !memcmp(dev + ctl_fan[0], O1, 3) && !saved(0));
    // a control round: a curve speeds up at once and slows gently; an emergency is not eased
    set("fan1", "curve"); set("fan1_src", "t1"); set("fan1_curve", "20:20,40:100"); st.link = 1;
    st.temp[0] = 4000; st.seen = GetTickCount(); control(); assert(dev_is(0, 100) && ch[0].ok == 1 && ch[0].own);
    st.temp[0] = 2000; st.seen = GetTickCount(); control(); assert(dev_is(0, 97));
    control(); assert(dev_is(0, 94));
    st.temp[0] = 0x7FFF; control(); assert(dev_is(0, 100) && !strcmp(em_name[ch[0].em], "nosensor"));
    set("fan1", "fix"); set("fan1_pct", "30"); control(); assert(dev_is(0, 30));
    set("fan1", ""); control(); assert(!memcmp(dev + ctl_fan[0], O1, 3) && !ch[0].own && !saved(0));
    // the channel's own setting cannot be saved: it is not touched, the page is told why, the next round tries again
    DeleteFileW(ini); assert(CreateDirectoryW(ini, NULL));
    set("fan1", "fix"); control(); assert(!memcmp(dev + ctl_fan[0], O1, 3) && !taken[0] && ch[0].ok == -1 && ch[0].er == ER_SAVE && !ch[0].own);
    control(); assert(!memcmp(dev + ctl_fan[0], O1, 3) && !taken[0]);
    RemoveDirectoryW(ini);
    control(); assert(dev_is(0, 30) && taken[0] && saved(0) && !ch[0].er && ch[0].ok == 1 && !memcmp(orig[0], O1, 3));
    // the sensor lost and the write failing together: the reason, the speed asked and that it did not land, each on its own
    set("fan1", "curve"); st.temp[0] = 0x7FFF; st.seen = GetTickCount(); set_fail = 1;
    control(); assert(ch[0].em == EM_NOSENSOR && ch[0].want == 10000 && ch[0].ok == -1 && ch[0].er == ER_WRITE && dev_is(0, 30));
    set_fail = 0; control(); assert(ch[0].em == EM_NOSENSOR && dev_is(0, 100) && ch[0].ok == 1 && !ch[0].er);
    // the curve first, then the floor: 20:20,40:100 at 30 °C is 60 %, which a floor of 50 leaves alone
    set("fan1_min", "50"); st.temp[0] = 3000; targets(t); assert(t[0].want == 6000);
    st.temp[0] = 2200; targets(t); assert(t[0].want == 5000); set("fan1_min", NULL);
    // how fast it slows is the channel's own; an emergency is never eased
    set("fan1_down", "10"); st.temp[0] = 2000; control(); assert(dev_is(0, 90)); control(); assert(dev_is(0, 80));
    st.temp[0] = 0x7FFF; control(); assert(dev_is(0, 100));
    set("fan1_down", "100"); st.temp[0] = 2000; control(); assert(dev_is(0, 20));
    set("fan1_down", "0"); st.temp[0] = 4000; control(); st.temp[0] = 2000; control(); assert(dev_is(0, 99));   // under 1: 1
    set("fan1", ""); set("fan1_down", NULL); control(); assert(!memcmp(dev + ctl_fan[0], O1, 3) && !taken[0]);
    // the board: taken in manual mode with its own mode and duty on disk first, the other control left alone
    wcscpy_s(bsec, 24, L"board-TEST"); bn = 2; smode[0] = smode[1] = 5; scmd[0] = 0x80; scmd[1] = 0x40;
    set("fan5", "fix"); set("fan5_pct", "40"); board_round();
    assert(bis(0, 0, 102) && btaken[0] && bsaved(0) && ch[4].own && ch[4].ok == 1 && bis(1, 5, 0x40) && !btaken[1] && bd.rpm[0] == 900);
    set("fan5", ""); board_round(); assert(bis(0, 5, 0x80) && !btaken[0] && !bsaved(0) && !ch[4].own);
    // a crash: the saved setting goes back at the next start
    set("fan5", "fix"); board_round(); memset(btaken, 0, sizeof(btaken)); memset(borig, 0, sizeof(borig));
    bstate_load(); assert(btaken[0] && bchk[0] && borig[0][0] == 5 && borig[0][1] == 0x80);
    set("fan5", ""); board_round(); assert(bis(0, 5, 0x80) && !btaken[0] && !bsaved(0));
    // changed by the BIOS or another program meanwhile: dropped, not written over
    set("fan5", "fix"); board_round(); smode[0] = 3; bstate_load();
    set("fan5", ""); board_round(); assert(smode[0] == 3 && !btaken[0] && !bsaved(0));
    // still haku's manual duty after a crash: taken on, given back when asked
    smode[0] = 5; scmd[0] = 0x80; set("fan5", "fix"); board_round(); bstate_load();
    board_round(); assert(btaken[0] && bis(0, 0, 102) && !bchk[0]);
    set("fan5", ""); board_round(); assert(bis(0, 5, 0x80) && !btaken[0]);
    // not readable, not saved, not kept: not touched or not confirmed, and the page is told why
    set("fan5", "fix"); sget_fail = 1; board_round(); assert(!btaken[0] && ch[4].ok == -1 && ch[4].er == ER_READ); sget_fail = 0;
    DeleteFileW(ini); assert(CreateDirectoryW(ini, NULL));
    board_round(); assert(bis(0, 5, 0x80) && !btaken[0] && ch[4].er == ER_SAVE); RemoveDirectoryW(ini);
    sdrop = 0; board_round(); assert(btaken[0] && ch[4].ok == 0 && ch[4].er == ER_KEPT && bis(0, 5, 0x80)); sdrop = -1;
    board_round(); assert(bis(0, 0, 102) && ch[4].ok == 1 && !ch[4].er);
    // giving back fails: reported, kept on disk; then it goes back
    sput_fail = 1; assert(board_restore() == 1 && brfail == 1 && btaken[0] && bsaved(0)); sput_fail = 0;
    assert(!board_restore() && !brfail && bis(0, 5, 0x80) && !bsaved(0));
    set("fan5_pct", NULL);
    // the page's view
    static char js[8192]; th = (HANDLE)1; strcpy_s(bd.chip, sizeof(bd.chip), "TEST"); bd.n = 2; fans_json(js, sizeof(js));
    assert(strstr(js, "\"link\":1") && strstr(js, "\"log\":[") && strstr(js, "\"em\":\"\"") && strstr(js, "\"err\":\"\""));
    assert(strstr(js, "\"q\":1") && strstr(js, "\"chip\":\"TEST\"") && strstr(js, "{\"b\":1,\"rpm\":900"));
    st.on = 0; fans_json(js, sizeof(js)); th = NULL;
    assert(strstr(js, "\"on\":1,\"q\":0") && strstr(js, "\"f\":[null,null,null,null,{\"b\":1") && !strstr(js, "\"link\""));
    DeleteFileW(ini);
    puts("fans: ok");
    return 0;
}
#endif
