// SPDX-License-Identifier: MPL-2.0
// The board's fan controls on a Nuvoton NCT67xx Super I/O, through PawnIO and its LpcIO module (LpcIO.bin next to the exe).
// The chip ids, the register layout and how manual control is taken and given back are ported from LibreHardwareMonitor
// (LibreHardwareMonitorLib/Hardware/Motherboard/Lpc/LpcIO.cs and Nct677X.cs).
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (C) LibreHardwareMonitor and Contributors. Partial Copyright (C) Michael Möller <mmoeller@openhardwaremonitor.org>.
// shortcut: only the NCT6779D and newer of LHM's default Nuvoton group; the NCT6687D (EC), the older NCT677x and ITE
// chips have other layouts, add them when a tester has one.
#include "common.h"

typedef HRESULT (WINAPI *pfn_open)(PHANDLE);
typedef HRESULT (WINAPI *pfn_load)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pfn_exec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pfn_close)(HANDLE);
static pfn_exec p_exec; static pfn_close p_close;
static HANDLE h, mtx;
static int slot, port, base, n, newpwm, iolock, err;

static const struct { int id, rev, mask, n, newpwm, iolock; const char *name; } chips[] = {
    { 0xC5, 0x60, 0xF0, 5, 0, 0, "NCT6779D" },  { 0xC8, 0x03, 0xFF, 6, 0, 1, "NCT6791D" },
    { 0xC9, 0x11, 0xFF, 6, 0, 1, "NCT6792D" },  { 0xC9, 0x13, 0xFF, 6, 0, 1, "NCT6792DA" },
    { 0xD1, 0x21, 0xFF, 6, 0, 1, "NCT6793D" },  { 0xD3, 0x52, 0xFF, 6, 0, 1, "NCT6795D" },
    { 0xD4, 0x23, 0xFF, 6, 0, 1, "NCT6796D" },  { 0xD4, 0x2A, 0xFF, 7, 0, 1, "NCT6796D-R" },
    { 0xD4, 0x51, 0xFF, 7, 1, 1, "NCT6797D" },  { 0xD4, 0x2B, 0xFF, 7, 1, 1, "NCT6798D" },
    { 0xD8, 0x02, 0xFF, 7, 1, 1, "NCT6799D" },  { 0xD8, 0x06, 0xFF, 7, 0, 1, "NCT6701D" },
};
// the duty now, the duty asked in manual mode, the mode (0 manual), the fan's tach count
static const int OUT_OLD[7] = { 0x001, 0x003, 0x011, 0x013, 0x015, 0x017, 0x029 },
                 OUT_NEW[7] = { 0x001, 0x003, 0x011, 0x013, 0x015, 0xA09, 0xB09 },
                 CMD[7] = { 0x109, 0x209, 0x309, 0x809, 0x909, 0xA09, 0xB09 },
                 MODE[7] = { 0x102, 0x202, 0x302, 0x802, 0x902, 0xA02, 0xB02 },
                 CNT[7] = { 0x4B0, 0x4B2, 0x4B4, 0x4B6, 0x4B8, 0x4BA, 0x4CC };

static ULONG64 ex(const char *fn, ULONG64 a, ULONG64 b, int nin, int nout) {
    ULONG64 in[2] = { a, b }, o[1] = { 0 }; SIZE_T rs = 0;
    if (p_exec(h, fn, in, nin, o, nout, &rs)) err = 1;
    return o[0];
}
static void outb(int p, int v) { ex("ioctl_pio_outb", p, v, 2, 0); }
static int inb(int p) { return (int)ex("ioctl_pio_inb", p, 0, 1, 1) & 0xFF; }
static int cfg_rd(int r) { return (int)ex("ioctl_superio_inb", r, 0, 1, 1) & 0xFF; }
static void cfg_wr(int r, int v) { ex("ioctl_superio_outb", r, v, 2, 0); }
// the mutex other monitors (LibreHardwareMonitor, HWiNFO, FanControl) hold around Super I/O access
static int lock(void) { DWORD w = WaitForSingleObject(mtx, 500); err = 0; return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED; }
static void unlock(void) { ReleaseMutex(mtx); }
// banked: the bank at register 0x4E, then the register; the bank is not put back (LHM does not either)
static int rd(int a) { outb(base + 5, 0x4E); outb(base + 6, a >> 8); outb(base + 5, a & 0xFF); return inb(base + 6); }
static void wr(int a, int v) { outb(base + 5, 0x4E); outb(base + 6, a >> 8); outb(base + 5, a & 0xFF); outb(base + 6, v); }
static int vendor_ok(void) { return (rd(0x804F) << 8 | rd(0x004F)) == 0x5CA3; }
// the newer chips lock their I/O space after a while (or after sleep): config register 0x28 bit 4
static void open_io(void) {
    ex("ioctl_select_slot", slot, 0, 1, 0);
    outb(port, 0x87); outb(port, 0x87);
    int r = cfg_rd(0x28); if (r & 0x10) cfg_wr(0x28, r & ~0x10);
    outb(port, 0xAA);
}

// the chip on port 2E or 4E: a known id and revision, a sane hardware monitor base, Nuvoton's vendor id there
static int find_chip(char *chip, int cap) {
    for (slot = 0; slot < 2; slot++) {
        port = slot ? 0x4E : 0x2E;
        if (!lock()) continue;
        ex("ioctl_select_slot", slot, 0, 1, 0);
        outb(port, 0x87); outb(port, 0x87);
        int id = cfg_rd(0x20), rev = cfg_rd(0x21), k = -1, a = 0, v = 0;
        for (int i = 0; i < (int)(sizeof(chips) / sizeof(chips[0])); i++)
            if (chips[i].id == id && chips[i].rev == (rev & chips[i].mask)) k = i;
        if (k >= 0) {
            ex("ioctl_find_bars", 0, 0, 0, 0);
            cfg_wr(0x07, 0x0B);   // the hardware monitor's logical device
            a = cfg_rd(0x60) << 8 | cfg_rd(0x61); Sleep(1); v = cfg_rd(0x60) << 8 | cfg_rd(0x61);
            if (chips[k].iolock) { int r = cfg_rd(0x28); if (r & 0x10) cfg_wr(0x28, r & ~0x10); }
        }
        outb(port, 0xAA);
        if (k < 0) { unlock(); if (!err) logf_("sio: port %02X id %02X %02X, not a known fan chip", port, id, rev); continue; }
        if ((a & 7) == 5) a &= 0xFFF8, v &= 0xFFF8;
        if (err || a != v || a < 0x100 || (a & 0xF007)) { unlock(); logf_("sio: %s at port %02X, bad base %04X/%04X", chips[k].name, port, a, v); continue; }
        base = a;
        int ok = vendor_ok();
        unlock();
        if (!ok) { logf_("sio: %s at %04X, not Nuvoton's vendor id: left alone", chips[k].name, a); continue; }
        n = chips[k].n; newpwm = chips[k].newpwm; iolock = chips[k].iolock;
        snprintf(chip, cap, "%s", chips[k].name);
        logf_("sio: %s at %04X, %d fan controls", chip, a, n);
        return n;
    }
    return 0;
}

int sio_open(char *chip, int cap) {
    HMODULE lib = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { logf_("sio: PawnIO not installed"); return 0; }
    pfn_open p_open = (pfn_open)GetProcAddress(lib, "pawnio_open");
    pfn_load p_load = (pfn_load)GetProcAddress(lib, "pawnio_load");
    p_exec = (pfn_exec)GetProcAddress(lib, "pawnio_execute");
    p_close = (pfn_close)GetProcAddress(lib, "pawnio_close");
    wchar_t mp[MAX_PATH]; GetModuleFileNameW(NULL, mp, MAX_PATH);
    wchar_t *s = wcsrchr(mp, L'\\'); if (s) s[1] = 0;
    wcscat_s(mp, MAX_PATH, L"LpcIO.bin");
    FILE *f = _wfopen(mp, L"rb");
    if (!f) { logf_("sio: LpcIO.bin missing next to exe"); return 0; }
    static BYTE blob[256 * 1024]; SIZE_T len = fread(blob, 1, sizeof(blob), f); fclose(f);
    if (!p_open || !p_load || !p_exec || !p_close || p_open(&h)) { logf_("sio: pawnio_open failed (not elevated?)"); h = NULL; return 0; }
    if (p_load(h, blob, len)) { logf_("sio: module load failed"); p_close(h); h = NULL; return 0; }
    mtx = CreateMutexA(NULL, FALSE, "Global\\Access_ISABUS.HTP.Method");
    int r = find_chip(chip, cap);
    if (!r) { p_close(h); h = NULL; }
    return r;
}

void sio_close(void) { if (h) { p_close(h); h = NULL; n = 0; } }

// rpm: -1 when it does not read, 0 stopped; pct: the duty now
int sio_read(int *rpm, int *pct) {
    if (!h || !lock()) return 0;
    if (iolock && !vendor_ok()) open_io();
    for (int i = 0; i < n; i++) {
        int c = rd(CNT[i]) << 5 | (rd(CNT[i] + 1) & 0x1F);
        rpm[i] = c >= 0x1FFF ? 0 : c < 0x15 ? -1 : 1350000 / c;
        pct[i] = (rd((newpwm ? OUT_NEW : OUT_OLD)[i]) * 100 + 127) / 255;
    }
    int ok = !err; unlock();
    return ok;
}

int sio_get(int i, int *mode, int *cmd) {
    if (!h || i < 0 || i >= n || !lock()) return 0;
    *mode = rd(MODE[i]); *cmd = rd(CMD[i]);
    int ok = !err; unlock();
    return ok;
}

#if defined(HAKU_DEV) && !defined(SIO_TEST)   // the test build only looks
int sio_put(int i, int mode, int cmd) { (void)i; (void)mode; (void)cmd; return 0; }
#else
// the whole mode byte, then the duty: 0 and a duty takes it (manual), the saved pair gives it back
int sio_put(int i, int mode, int cmd) {
    if (!h || i < 0 || i >= n || !lock()) return 0;
    wr(MODE[i], mode & 0xFF); wr(CMD[i], cmd & 0xFF);
    int ok = !err; unlock();
    return ok;
}
#endif

#ifdef SIO_TEST   // cl /DSIO_TEST src\sio.c: detection, reads and writes against a simulated chip; PawnIO is not loaded
#include <assert.h>
#include <process.h>
void logf_(const char *fmt, ...) { (void)fmt; }
// the simulated Super I/O: per slot its config registers (of the hardware monitor's device), then its banked registers
static struct { int cfg[256], lockable; } sl[2];
static int cur, idx, bank, hw[0x10000], fail_at = -1, calls;   // fail_at: the call that returns an error
static int cfgv(int r) { return sl[cur].cfg[r]; }
static int locked(void) { return sl[cur].lockable && (sl[cur].cfg[0x28] & 0x10); }
static int hbase(void) { return (cfgv(0x60) << 8 | cfgv(0x61)) & 0xFFF8; }
static HRESULT WINAPI fake_exec(HANDLE hh, PCSTR fn, const ULONG64 *in, SIZE_T nin, PULONG64 out, SIZE_T nout, PSIZE_T rs) {
    (void)hh; (void)nin; (void)nout; (void)rs;
    if (calls++ == fail_at) return E_FAIL;
    int a = (int)in[0], v = nin > 1 ? (int)in[1] & 0xFF : 0;
    if (!strcmp(fn, "ioctl_select_slot")) cur = a;
    else if (!strcmp(fn, "ioctl_superio_inb")) out[0] = cfgv(a);
    else if (!strcmp(fn, "ioctl_superio_outb")) sl[cur].cfg[a] = v;
    else if (!strcmp(fn, "ioctl_pio_outb")) {
        if (locked() || !hbase()) return 0;
        if (a == hbase() + 5) idx = v;
        else if (a == hbase() + 6) { if (idx == 0x4E) bank = v; else hw[bank << 8 | idx] = v; }
    } else if (!strcmp(fn, "ioctl_pio_inb"))
        out[0] = locked() || !hbase() || a != hbase() + 6 ? 0xFF : idx == 0x4E ? bank : hw[bank << 8 | idx];
    return 0;
}
static void chip_at(int s, int id, int rev, int b, int lockable) {
    memset(&sl[s], 0, sizeof(sl[s])); sl[s].cfg[0x20] = id; sl[s].cfg[0x21] = rev;
    sl[s].cfg[0x60] = b >> 8; sl[s].cfg[0x61] = b & 0xFF; sl[s].lockable = lockable;
}
static void reset(void) { memset(sl, 0, sizeof(sl)); memset(hw, 0, sizeof(hw)); hw[0x804F] = 0x5C; hw[0x004F] = 0xA3; n = 0; fail_at = -1; }
static int found(char *c) { calls = 0; return find_chip(c, 16); }
static unsigned __stdcall hold(void *e) {   // another monitor holds the Super I/O mutex until told
    HANDLE *x = e; WaitForSingleObject(mtx, INFINITE); SetEvent(x[0]); WaitForSingleObject(x[1], INFINITE); ReleaseMutex(mtx); return 0;
}
int main(void) {
    char c[16]; int rpm[7], pct[7], m, d;
    p_exec = fake_exec; h = (HANDLE)1; mtx = CreateMutexA(NULL, FALSE, NULL);
    // nothing on 2E, an NCT6798D on 4E: its 7 controls and the newer duty registers
    reset(); chip_at(1, 0xD4, 0x2B, 0x290, 1);
    assert(found(c) == 7 && !strcmp(c, "NCT6798D") && port == 0x4E && base == 0x290 && newpwm && iolock);
    // an unknown chip on 2E does not stop the look on 4E
    reset(); chip_at(0, 0xB4, 0x70, 0x290, 0); chip_at(1, 0xD4, 0x2B, 0x290, 1); assert(found(c) == 7 && port == 0x4E);
    // the NCT6779D: any revision 6x, 5 controls; a revision outside it is not taken
    reset(); chip_at(0, 0xC5, 0x62, 0x290, 0); assert(found(c) == 5 && !strcmp(c, "NCT6779D") && !newpwm && !iolock);
    reset(); chip_at(0, 0xC5, 0x72, 0x290, 0); assert(!found(c));
    // a revision of a known id that is not in the table: left alone
    reset(); chip_at(0, 0xD4, 0x99, 0x290, 1); assert(!found(c));
    // the base: low bits 5 as LHM reads them, not under 100, not misaligned, not above FFF
    reset(); chip_at(0, 0xD4, 0x2B, 0x295, 1); assert(found(c) == 7 && base == 0x290);
    reset(); chip_at(0, 0xD4, 0x2B, 0x0050, 1); assert(!found(c));
    reset(); chip_at(0, 0xD4, 0x2B, 0x0292, 1); assert(!found(c));
    reset(); chip_at(0, 0xD4, 0x2B, 0x1290, 1); assert(!found(c));
    // another vendor's id at the base: left alone
    reset(); chip_at(0, 0xD4, 0x2B, 0x290, 1); hw[0x804F] = 0x12; assert(!found(c));
    // its I/O locked at the start: unlocked once, then read
    reset(); chip_at(0, 0xD4, 0x2B, 0x290, 1); sl[0].cfg[0x28] = 0x10; assert(found(c) == 7 && !(sl[0].cfg[0x28] & 0x10));
    // a failing PawnIO call during the look: not taken
    reset(); chip_at(0, 0xD4, 0x2B, 0x290, 1); fail_at = 9; assert(!found(c));
    // reads: rpm from the 13-bit count (stopped, not reading), the duty in %
    reset(); chip_at(0, 0xD4, 0x2B, 0x290, 1); assert(found(c) == 7);
    hw[0x4B0] = 1000 >> 5; hw[0x4B1] = 1000 & 0x1F; hw[0x4B2] = 0xFF; hw[0x4B3] = 0x1F; hw[0x4B4] = 0; hw[0x4B5] = 0x10;
    hw[0x001] = 128; hw[0x003] = 255; hw[0xA09] = 0; hw[0x011] = 77;
    assert(sio_read(rpm, pct) && rpm[0] == 1350 && rpm[1] == 0 && rpm[2] == -1 && pct[0] == 50 && pct[1] == 100 && pct[2] == 30 && pct[5] == 0);
    // a write and its read back: the mode byte, then the duty, at the control's own registers
    hw[0x102] = 5; hw[0x109] = 0x80;
    assert(sio_get(0, &m, &d) && m == 5 && d == 0x80);
    assert(sio_put(0, 0, 102) && hw[0x102] == 0 && hw[0x109] == 102 && sio_get(0, &m, &d) && m == 0 && d == 102);
    assert(sio_put(6, 0, 51) && hw[0xB02] == 0 && hw[0xB09] == 51 && hw[0x202] == 0);
    assert(!sio_put(7, 0, 1) && !sio_put(-1, 0, 1) && !sio_get(7, &m, &d));    // outside its controls
    // locked again after sleep: reads find the vendor id gone, unlock, and read on
    sl[0].cfg[0x28] |= 0x10; assert(sio_read(rpm, pct) && rpm[0] == 1350 && !(sl[0].cfg[0x28] & 0x10));
    // a PawnIO call fails: the read or write says so
    calls = 0; fail_at = 2; assert(!sio_get(0, &m, &d)); fail_at = -1;
    calls = 0; fail_at = 5; assert(!sio_put(0, 5, 0x80)); fail_at = -1;
    assert(sio_put(0, 5, 0x80) && hw[0x102] == 5 && hw[0x109] == 0x80);
    // another monitor holds the Super I/O mutex: nothing read or written while it does
    HANDLE e[2] = { CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL) };
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, hold, e, 0, NULL); WaitForSingleObject(e[0], INFINITE);
    assert(!sio_get(0, &m, &d) && !sio_put(0, 0, 1) && !sio_read(rpm, pct) && hw[0x102] == 5);
    SetEvent(e[1]); WaitForSingleObject(t, INFINITE); CloseHandle(t);
    assert(sio_get(0, &m, &d) && m == 5);
    // closed: nothing
    h = NULL; assert(!sio_get(0, &m, &d) && !sio_read(rpm, pct));
    puts("sio: ok");
    return 0;
}
#endif
