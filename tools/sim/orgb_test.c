// SPDX-License-Identifier: GPL-3.0-only
// Checks for the OpenRGB driver (src/drv_openrgb.c) against the fake SDK server: the mode set once and read back,
// frames never setting it again, no frame sent twice, one steady colour only on a change that holds (dark and lit
// again at once), red -> off -> on through each way of driving it, one connector off with the other lit, a mode that
// does not take or a lost connection not shown as working, same-named sticks told apart by place, one board's
// connectors sharing its mode; no memory left behind by connects and scans on their short-lived threads; the send
// pace of devices.c (never past a device's limit, no burst after a pause, across a tick wrap) and which finished
// connect it takes over.
//   python tools\sim\fake_devices.py openrgb --orgb-test --orgb-port=6799   (in another window), then in
//   build.cmd's environment:
//   cl /nologo /Isrc tools\sim\orgb_test.c src\netutil.c ws2_32.lib winhttp.lib bcrypt.lib wintrust.lib crypt32.lib /Fe:%TEMP%\orgb_test.exe && %TEMP%\orgb_test.exe
#include "../../src/drv_openrgb.c"
#include <stdio.h>
#include <stdarg.h>
#include <psapi.h>

// ---- what the driver needs from the rest of haku: settings in memory, the log on screen
static struct { char sec[16], key[32], val[256]; } kv[64];
static int nkv;
const char *cfg_get(const char *section, const char *key, const char *def) {
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].sec, section) && !strcmp(kv[i].key, key)) return kv[i].val;
    return def;
}
void cfg_set(const char *section, const char *key, const char *value) {
    int i = 0;
    while (i < nkv && (strcmp(kv[i].sec, section) || strcmp(kv[i].key, key))) i++;
    if (i == nkv && nkv < 64) nkv++;
    strcpy_s(kv[i].sec, sizeof(kv[i].sec), section); strcpy_s(kv[i].key, sizeof(kv[i].key), key);
    strcpy_s(kv[i].val, sizeof(kv[i].val), value);
}
int cfg_save_if_dirty(void) { return 1; }
void logf_(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("     log: "); vprintf(fmt, a); printf("\n"); va_end(a); }
static int reloads;
void ext_reload(void) { reloads++; }
int ext_orgb_given_back(void) { return 0; }
int orgbapp_ensure(int (*answers)(void)) { (void)answers; return 0; }
int orgbapp_exe(wchar_t *out, int running) { (void)out; (void)running; return 0; }
int orgbapp_json(char *out, int cap) { (void)out; (void)cap; return 0; }
void orgbapp_add_new(const orgb_ctl *c, int n) { (void)c; (void)n; }
void ui_refresh(void) {}
int net_broadcasts(unsigned long *out, int max) { (void)out; (void)max; return 0; }
int net_addresses(unsigned long *out, int max) { (void)out; (void)max; return 0; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

#define HOST "127.0.0.1:6799"
static SOCKET ctl;   // the test's own connection, for the server's counts and its faults

typedef struct { unsigned custom, mode, leds, zone_leds, last; } counts_t;   // last: the colour it shows, 0xBBGGRR
static counts_t counts(unsigned idx) {
    counts_t k = { 99, 99, 99, 99, 99 };
    unsigned char b[20];
    Sleep(50);   // the server reads the device's connection on its own thread: what was sent there counted first
    if (send_pkt(ctl, idx, 9000, NULL, 0) && recv_pkt(ctl, 9000, b, sizeof(b), NULL) == 20) memcpy(&k, b, 20);
    return k;
}
static void fault(unsigned idx, unsigned id, unsigned char v) { send_pkt(ctl, idx, id, &v, 1); Sleep(50); }
static int active(unsigned idx) { ctrl_t *c = malloc(sizeof(ctrl_t)); int a = read_ctrl(ctl, idx, c) ? c->active : -1; free(c); return a; }

static ext_dev *dev(int id, int sub) {
    ext_dev *d = calloc(1, sizeof(ext_dev));
    d->id = id; d->drv = &drv_openrgb; d->sub = sub; d->sock = INVALID_SOCKET;
    strcpy_s(d->host, sizeof(d->host), HOST);
    return d;
}
static void done(ext_dev *d, int how) { if (d->priv) { drv_openrgb.leave(d, how); drv_openrgb.close(d); } free(d); }
static int frame(ext_dev *d, float r, float g, float b) {
    rgbf c[EXT_MAX_LEDS];
    for (int i = 0; i < d->nleds; i++) c[i] = (rgbf){ r, g, b };
    return drv_openrgb.send(d, c, d->nleds);
}

// ---- the send pace (devices.h): frames every `frame` ms with a few ms of jitter for `ms`, a pause of `pause` ms
// in the middle; how many were sent, the shortest gap, the first gap after the pause
typedef struct { int sent, min_gap, after_pause; } pace_t;
static pace_t pace(DWORD start, int frame, int ms, float fps, float dev_rate, int pause) {
    if (dev_rate > 0 && fps > dev_rate) fps = dev_rate;   // as the worker does
    int iv = (int)(1000 / fps + .999f), hard = dev_rate > 0 ? (int)(1000 / dev_rate + .999f) : 0;
    DWORD due = start, last = start - 1000, prev = 0; pace_t p = { 0, 1 << 30, -1 };
    int paused = 0;
    unsigned rnd = 1;
    for (int t = 0; t < ms; t += frame) {
        if (pause && t >= ms / 2 && t < ms / 2 + pause) { paused = 1; continue; }
        rnd = rnd * 1103515245 + 12345;
        DWORD now = start + t + (rnd >> 16) % 5;
        if (!ext_pace_due(now, due, last, hard)) continue;
        due = ext_pace_next(now, due, iv);
        if (p.sent && (int)(now - prev) < p.min_gap) p.min_gap = (int)(now - prev);
        if (paused == 1) { paused = 2; } else if (paused == 2) { p.after_pause = (int)(now - prev); paused = 3; }
        prev = now; last = now; p.sent++;
    }
    return p;
}
static void pace_checks(void) {
    char what[160];
    static const struct { float fps, dev; } R[] = { { 5, 5 }, { 30, 5 }, { 4, 4 }, { 30, 0 }, { 15, 0 }, { 10, 0 }, { 2, 5 } };
    static const DWORD STARTS[] = { 100000, 0xFFFFFFFFu - 4000 };   // the second runs across the tick counter's wrap
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < (int)(sizeof(R) / sizeof(R[0])); i++) {
            float fps = R[i].dev > 0 && R[i].fps > R[i].dev ? R[i].dev : R[i].fps;
            int hard = R[i].dev > 0 ? (int)(1000 / R[i].dev + .999f) : 0, iv = (int)(1000 / fps + .999f);
            pace_t p = pace(STARTS[s], 33, 10000, R[i].fps, R[i].dev, 0);
            // never past the rate over 10 s (the first frame at 0 counted), and each gap at least the device's limit
            snprintf(what, sizeof(what), "pace %s: %g/s asked, device %g/s: %d sent in 10 s (at most %d), shortest gap %d ms (at least %d)",
                     s ? "across the wrap" : "", R[i].fps, R[i].dev, p.sent, (int)(10 * fps) + 1, p.min_gap, hard);
            CHECK(p.sent <= (int)(10 * fps) + 1 && p.min_gap >= hard && p.sent >= (int)(10 * fps * (fps > 20 ? .6f : .8f)), what);
            p = pace(STARTS[s], 33, 10000, R[i].fps, R[i].dev, 2000);
            snprintf(what, sizeof(what), "pace %s: %g/s, device %g/s, a 2 s pause: first gap after it %d ms (at least %d), shortest %d",
                     s ? "across the wrap" : "", R[i].fps, R[i].dev, p.after_pause, iv - 4, p.min_gap);
            CHECK(p.after_pause >= iv - 4 && p.min_gap >= hard, what);   // -4: the jitter of the frame before it
        }
}

// ---- which finished connect devices.c takes over (devices.h)
static void fate_checks(void) {
    ext_dev cur[2] = { 0 }; int how;
    cur[0].id = 3; cur[0].drv = &drv_openrgb; cur[0].sub = 8; cur[0].gen = 7; cur[0].enabled = 1; strcpy_s(cur[0].host, 64, HOST);
    cur[1] = cur[0]; cur[1].id = 4; cur[1].sub = 9; cur[1].gen = 8;
    ext_dev res = cur[0];
    CHECK(ext_open_fate(cur, 2, &res, &how) == 0 && how < 0, "connect: under its settings, taken over");
    strcpy_s(cur[0].name, 64, "Renamed"); cur[0].reverse = 1;
    CHECK(ext_open_fate(cur, 2, &res, &how) == 0 && how < 0, "connect: renamed meanwhile, still taken over (no reconnect)");
    cur[0].gen = 9;
    CHECK(ext_open_fate(cur, 2, &res, &how) == -1 && how == LEAVE_KEEP, "connect: settings changed meanwhile: closed, nothing sent");
    cur[0].gen = 7; cur[0].enabled = 0;
    CHECK(ext_open_fate(cur, 2, &res, &how) == 0 && how == LEAVE_OFF, "connect: switched off meanwhile: let go dark");
    cur[0].enabled = 1; cur[0].own = 1;
    CHECK(ext_open_fate(cur, 2, &res, &how) == 0 && how == LEAVE_RESTORE, "connect: given back meanwhile: its own mode");
    cur[0].own = 0; cur[0].covered = 4;
    CHECK(ext_open_fate(cur, 2, &res, &how) == 0 && how == LEAVE_KEEP, "connect: its whole board added meanwhile: closed as it is");
    CHECK(ext_open_fate(cur + 1, 1, &res, &how) == -1 && how == LEAVE_RESTORE, "connect: removed meanwhile: given back");
    cur[0].covered = 0; cur[0].sub = 10;
    CHECK(ext_open_fate(cur, 2, &res, &how) == -1 && how == LEAVE_RESTORE, "connect: another device under its number: given back");
}

// ---- a device's history (devices.h): bounded, a repeat counted on its line, the oldest dropped
static void event_checks(void) {
    ext_dev d = { 0 };
    ext_event(&d, EV_ONLINE, 0); ext_event(&d, EV_LOST, 0); ext_event(&d, EV_LOST, 0); ext_event(&d, EV_LOST, 0);
    CHECK(d.nev == 2 && d.ev[1].code == EV_LOST && d.ev[1].n == 3, "events: a link lost three times in a row is one line, x3");
    ext_event(&d, EV_CAP, 1); ext_event(&d, EV_CAP, 3);
    CHECK(d.nev == 4 && d.ev[3].a == 3, "events: another mode is a new line");
    for (int i = 0; i < 300; i++) ext_event(&d, i & 1 ? EV_BACK : EV_LOST, 0);
    CHECK(d.nev == EXT_EV && d.ev[0].code != EV_ONLINE && d.ev[EXT_EV - 1].code == EV_BACK, "events: never past EXT_EV, the oldest gone");
    for (int i = 0; i < 300; i++) ext_event(&d, EV_FAIL, 0);
    CHECK(d.nev == EXT_EV && d.ev[EXT_EV - 1].n == 255, "events: the count stops at 255");
}

// ---- memory: connects and scans as the app runs them, each on a thread of its own
static unsigned __stdcall open_once(void *p) {
    ext_dev *d = p;
    if (drv_openrgb.open(d)) { drv_openrgb.leave(d, LEAVE_KEEP); drv_openrgb.close(d); }
    return 0;
}
static void found_none(const disc_t *x) { (void)x; }
static unsigned __stdcall scan_once(void *p) { (void)p; drv_openrgb.discover(0, found_none); return 0; }
static void on_thread(unsigned (__stdcall *fn)(void *), void *arg) {
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
    WaitForSingleObject(t, 10000); CloseHandle(t);
}
static SIZE_T private_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX m = { sizeof(m) };
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof(m));
    return m.PrivateUsage;
}

int main(void) {
    pace_checks();
    fate_checks(); event_checks();
    unsigned count = 0;
    ctl = orgb_connect(HOST, &count);
    if (ctl == INVALID_SOCKET || count < 13) { printf("FAIL no fake server with --orgb-test on %s\n", HOST); return 1; }
    for (unsigned i = 0; i < count; i++) { fault(i, 9002, i == 9 ? 0 : 1); counts(i); }   // as the server starts, at every run

    // Direct: the mode set once and read back, frames only, the same frame not sent again
    ext_dev *d = dev(1, 1);
    CHECK(drv_openrgb.open(d) && d->cap == CAP_EFFECTS, "Direct: opens, effects");
    counts_t k = counts(1);
    CHECK(k.mode == 1 && k.custom == 0 && active(1) == 0, "Direct: one UPDATEMODE, no SETCUSTOMMODE, Direct active");
    for (int i = 0; i < 5; i++) frame(d, 1, 0, 0);
    frame(d, 0, 1, 0);
    k = counts(1);
    CHECK(k.leds == 2 && k.mode == 0, "Direct: 6 frames, 2 different: 2 sent, no mode command");
    done(d, LEAVE_RESTORE);
    k = counts(1);
    CHECK(k.mode == 1 && active(1) == 1, "Direct: its own mode (Static) given back");

    // a mode that does not take: not shown as working
    fault(2, 9001, 1);
    d = dev(2, 2);
    CHECK(!drv_openrgb.open(d) && d->cap == CAP_FAILED && strstr(d->info, "did not switch"), "stuck mode: not online, says it did not switch");
    done(d, LEAVE_KEEP);
    fault(2, 9001, 0);

    // one steady colour only: sent at once, then only for a change that holds, never repeated
    d = dev(3, 8);
    CHECK(drv_openrgb.open(d) && d->cap == CAP_STEADY, "Static only: opens, steady colour");
    k = counts(8);
    CHECK(k.mode == 1 && active(8) == 0, "Static only: Static set on open");
    for (int i = 0; i < 10; i++) frame(d, 1, 0, 0);
    CHECK(counts(8).mode == 1, "Static only: the first colour sent once");
    for (int i = 0; i < 20; i++) { frame(d, i / 20.0f, 0, 1); Sleep(20); }
    CHECK(counts(8).mode == 0, "Static only: an animation is not played as mode changes");
    frame(d, 0, 1, 0); Sleep(700); frame(d, 0, 1, 0); frame(d, 0, 1, 0);
    CHECK(counts(8).mode == 1, "Static only: a new colour that holds sent once");
    frame(d, 0, 0, 0);
    k = counts(8);
    CHECK(k.mode == 1 && k.last == 0, "Static only: dark at once, not held back");
    frame(d, 0, 1, 0);
    k = counts(8);
    CHECK(k.mode == 1 && k.last == 0x00FF00, "Static only: lit again at once");
    done(d, LEAVE_KEEP);

    // red, switched off, switched on again: each way of driving it (Direct, Static per LED, one steady colour)
    static const struct { int sub, stat; const char *name; } W[] = { { 1, 0, "Direct" }, { 1, 1, "Static per LED" }, { 8, 0, "one steady colour" } };
    for (int w = 0; w < 3; w++) {
        char what[120];
        d = dev(20 + w, W[w].sub); d->cfg_static = W[w].stat;
        int up = drv_openrgb.open(d); counts(W[w].sub);
        frame(d, 1, 0, 0);
        k = counts(W[w].sub);
        snprintf(what, sizeof(what), "%s: red shown (%06X)", W[w].name, k.last);
        CHECK(up && k.last == 0x0000FF, what);
        drv_openrgb.leave(d, LEAVE_OFF); drv_openrgb.close(d);
        k = counts(W[w].sub);
        // dark the way its colour is set: one steady colour through its mode, the others by their LEDs
        snprintf(what, sizeof(what), "%s: off is dark (%06X), %s", W[w].name, k.last, W[w].sub == 8 ? "by its mode" : "by its LEDs");
        CHECK(k.last == 0 && (W[w].sub == 8 ? k.mode == 1 && k.leds == 0 : k.leds == 1 && k.mode == 0), what);
        up = drv_openrgb.open(d);
        frame(d, 1, 0, 0);
        k = counts(W[w].sub);
        snprintf(what, sizeof(what), "%s: on again, red back (%06X)", W[w].name, k.last);
        CHECK(up && k.last == 0x0000FF, what);
        done(d, LEAVE_KEEP);
    }

    // no mode haku can set
    d = dev(4, 9);
    CHECK(!drv_openrgb.open(d) && d->cap == CAP_UNSUPPORTED, "no usable mode: not online, unsupported");
    CHECK(counts(9).mode == 0, "no usable mode: nothing sent");
    done(d, LEAVE_KEEP);

    // same-named sticks: found by place, not guessed
    cfg_set("dev.5", "match", "Test Stick"); cfg_set("dev.5", "loc", "I2C: PIIX4, address 0x59");
    d = dev(5, 10);
    CHECK(drv_openrgb.open(d) && ((orgb_t *)d->priv)->idx == 11 && !strcmp(cfg_get("dev.5", "sub", ""), "11"), "sticks: the one at its place, number kept");
    done(d, LEAVE_KEEP);
    cfg_set("dev.6", "match", "Test Stick");
    d = dev(6, 40);
    CHECK(!drv_openrgb.open(d) && d->cap == CAP_AMBIGUOUS, "sticks: no place known, two of the name: not guessed");
    done(d, LEAVE_KEEP);

    // a board's two connectors: the mode set by the first, taken by the second, given back by the last
    counts(12);
    ext_dev *a = dev(7, 12 + PART_STEP), *b = dev(8, 12 + 2 * PART_STEP);
    CHECK(drv_openrgb.open(a) && drv_openrgb.open(b), "connectors: both open");
    CHECK(counts(12).mode == 1, "connectors: one mode command for both");
    frame(a, 1, 0, 0); frame(b, 0, 0, 1); frame(a, 1, 0, 0);
    CHECK(counts(12).zone_leds == 2, "connectors: zone frames, the repeat not sent");
    drv_openrgb.leave(a, LEAVE_OFF);
    k = counts(12);
    CHECK(k.zone_leds == 1 && k.last == 0 && k.mode == 0, "connectors: one switched off: its zone dark, the mode left");
    frame(b, 0, 1, 0);
    k = counts(12);
    CHECK(k.zone_leds == 1 && k.last == 0x00FF00, "connectors: the other still lit");
    drv_openrgb.leave(a, LEAVE_RESTORE); drv_openrgb.close(a); free(a);
    CHECK(counts(12).mode == 0, "connectors: the first to let go leaves the mode");
    done(b, LEAVE_RESTORE);
    CHECK(counts(12).mode == 1, "connectors: the last gives the mode back");

    // OpenRGB changed the mode under a second connector's reconnect: set again
    a = dev(7, 12 + PART_STEP);
    drv_openrgb.open(a); counts(12);
    fault(12, 9002, 1);
    b = dev(8, 12 + 2 * PART_STEP);
    CHECK(drv_openrgb.open(b) && counts(12).mode == 1, "connectors: mode lost under them, set again");
    done(b, LEAVE_KEEP); done(a, LEAVE_KEEP);

    // connects (and fails), reconnects and scans, 40 of each on threads of their own: no buffer left per thread
    ext_dev *m = dev(10, 1);
    on_thread(open_once, m);   // the first one's own costs (winsock, the CRT) not counted
    SIZE_T before = private_bytes();
    for (int i = 0; i < 40; i++) {
        m->id = i % 3 ? 10 : 11; m->sub = i % 3 ? 1 : 9;   // 9: no mode haku can set, the open fails after reading it
        on_thread(open_once, m);
        on_thread(scan_once, NULL);
    }
    SIZE_T grew = private_bytes() - before;
    char what[120]; snprintf(what, sizeof(what), "memory: 40 connects and 40 scans on their threads, %d KiB more (under 4 MiB)", (int)(grew >> 10));
    CHECK(grew < (4u << 20), what);
    free(m);
    counts(1); counts(9);

    // the connection lost: the next frame says so even when nothing would be sent
    d = dev(9, 0);
    drv_openrgb.open(d);
    frame(d, 1, 1, 1);
    send_pkt(d->sock, 0, 9003, NULL, 0); Sleep(100);
    CHECK(!frame(d, 1, 1, 1), "lost connection: the unchanged frame reports it");
    done(d, LEAVE_KEEP);

    closesocket(ctl);
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
