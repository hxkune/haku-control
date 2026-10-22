// SPDX-License-Identifier: GPL-3.0-only
// Checks for the OpenRGB driver (src/drv_openrgb.c) against the fake SDK server: the mode set once and read back,
// frames never setting it again, no frame sent twice, one steady colour only on a change that holds, a mode that
// does not take or a lost connection not shown as working, same-named sticks told apart by place, one board's
// connectors sharing its mode.
//   python tools\sim\fake_devices.py openrgb --orgb-test --orgb-port=6799   (in another window), then in
//   build.cmd's environment:
//   cl /nologo /Isrc tools\sim\orgb_test.c src\netutil.c ws2_32.lib winhttp.lib bcrypt.lib wintrust.lib crypt32.lib /Fe:%TEMP%\orgb_test.exe && %TEMP%\orgb_test.exe
#include "../../src/drv_openrgb.c"
#include <stdio.h>
#include <stdarg.h>

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

typedef struct { unsigned custom, mode, leds, zone_leds; } counts_t;
static counts_t counts(unsigned idx) {
    counts_t k = { 99, 99, 99, 99 };
    unsigned char b[16];
    Sleep(50);   // the server reads the device's connection on its own thread: what was sent there counted first
    if (send_pkt(ctl, idx, 9000, NULL, 0) && recv_pkt(ctl, 9000, b, sizeof(b), NULL) == 16) memcpy(&k, b, 16);
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

int main(void) {
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
    done(d, LEAVE_KEEP);

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
