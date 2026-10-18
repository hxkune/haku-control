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
    p_close(h); h = NULL;
    return 0;
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

#ifdef HAKU_DEV   // the test build only looks
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
