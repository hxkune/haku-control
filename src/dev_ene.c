// SPDX-License-Identifier: GPL-3.0-only
// ENE DRAM RGB over PawnIO + SmbusPIIX4.bin (AMD chipsets). Only the verified controller version
// "AUDA0-E6K5-0101" (e.g. G.Skill Trident Z5 RGB) is driven; anything else is left alone.
// Only already-mapped controllers are used (no address remapping), and the save register
// is never written: leaving direct mode returns the stick to its own stored effect.
#include "common.h"

typedef HRESULT (WINAPI *pfn_open)(PHANDLE);
typedef HRESULT (WINAPI *pfn_load)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pfn_exec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pfn_close)(HANDLE);
static pfn_open p_open; static pfn_load p_load; static pfn_exec p_exec; static pfn_close p_close;

static HANDLE bus, mtx;

#define MAX_STICKS 4
static int  addr[MAX_STICKS], nleds[MAX_STICKS], nsticks;
static int  direct[MAX_STICKS];
static BYTE last[MAX_STICKS][8 * 3];

enum { SMB_WRITE = 0, SMB_READ = 1, SZ_BYTE = 1, SZ_BYTE_DATA = 2, SZ_WORD_DATA = 3, SZ_BLOCK_DATA = 5 };

static int xfer(int a, int rw, int cmd, int size, BYTE *data) {
    ULONG64 in[9] = { (ULONG64)a, (ULONG64)rw, (ULONG64)cmd, (ULONG64)size };
    ULONG64 out[5] = { 0 }; SIZE_T rs = 0;
    if (data) memcpy(&in[4], data, 34);
    HRESULT st = p_exec(bus, "ioctl_smbus_xfer", in, 9, out, 5, &rs);
    if (data) memcpy(data, &out[0], 34);
    return st ? -1 : 0;
}

static void set_reg(int a, int reg) {
    BYTE d[34] = { (BYTE)(reg >> 8), (BYTE)(reg & 0xFF) };   // word, byte-swapped as ENE expects
    xfer(a, SMB_WRITE, 0x00, SZ_WORD_DATA, d);
}

static int reg_read(int a, int reg) {
    BYTE d[34] = { 0 };
    set_reg(a, reg);
    return xfer(a, SMB_READ, 0x81, SZ_BYTE_DATA, d) < 0 ? -1 : d[0];
}

static void reg_write(int a, int reg, BYTE v) {
    BYTE d[34] = { v };
    set_reg(a, reg);
    xfer(a, SMB_WRITE, 0x01, SZ_BYTE_DATA, d);
}

static int probe(int a) {
    BYTE d[34] = { 0 };
    if (xfer(a, SMB_READ, 0, SZ_BYTE, d) < 0) return 0;
    for (int r = 0xA0; r < 0xB0; r++) {
        BYTE v[34] = { 0 };
        if (xfer(a, SMB_READ, r, SZ_BYTE_DATA, v) < 0 || v[0] != r - 0xA0) return 0;
    }
    char name[16];
    for (int i = 0; i < 16; i++) name[i] = (char)reg_read(a, 0x1000 + i);
    name[15] = 0;
    if (strcmp(name, "AUDA0-E6K5-0101") != 0) { logf_("ene: 0x%02X unknown controller '%s', skipped", a, name); return 0; }
    int n = reg_read(a, 0x1C00 + 2);
    if (n <= 0 || n > 8) return 0;
    return n;
}

int ene_open(void) {
    HMODULE lib = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { logf_("ene: PawnIO not installed"); return 0; }
    p_open = (pfn_open)GetProcAddress(lib, "pawnio_open");
    p_load = (pfn_load)GetProcAddress(lib, "pawnio_load");
    p_exec = (pfn_exec)GetProcAddress(lib, "pawnio_execute");
    p_close = (pfn_close)GetProcAddress(lib, "pawnio_close");

    wchar_t mp[MAX_PATH]; GetModuleFileNameW(NULL, mp, MAX_PATH);
    wchar_t *s = wcsrchr(mp, L'\\'); if (s) s[1] = 0;
    wcscat_s(mp, MAX_PATH, L"SmbusPIIX4.bin");
    FILE *f = _wfopen(mp, L"rb");
    if (!f) { logf_("ene: SmbusPIIX4.bin missing next to exe"); return 0; }
    static BYTE blob[256 * 1024]; SIZE_T n = fread(blob, 1, sizeof(blob), f); fclose(f);

    if (p_open(&bus)) { logf_("ene: pawnio_open failed (not elevated?)"); return 0; }
    if (p_load(bus, blob, n)) { logf_("ene: module load failed"); p_close(bus); bus = NULL; return 0; }
    // PawnIO wait mode while the SMBus transfer completes: 0 = spin, 1 = spin briefly then sleep, 2 = sleep.
    // With the 1 ms timer set in main, 2 still gives ~24 RAM frames/s.
    ULONG64 in[1] = { (ULONG64)cfg_geti("advanced", "smbus_wait", 2) }, o[1]; SIZE_T rs;
    p_exec(bus, "ioctl_set_sleep_mode", in, 1, NULL, 0, &rs);
    in[0] = 0; p_exec(bus, "ioctl_piix4_port_sel", in, 1, o, 1, &rs);
    mtx = CreateMutexA(NULL, FALSE, "Global\\Access_SMBUS.HTP.Method");

    WaitForSingleObject(mtx, INFINITE);
    for (int a = 0x70; a <= 0x77 && nsticks < MAX_STICKS; a++) {
        int leds = probe(a);
        if (leds) { addr[nsticks] = a; nleds[nsticks] = leds; nsticks++; logf_("ene: stick at 0x%02X, %d LEDs", a, leds); }
    }
    ReleaseMutex(mtx);
    memset(last, 0xFF, sizeof(last));
    return nsticks;
}

int ene_count(void) { return nsticks; }

static BYTE to8(float v) { return (BYTE)(clampf(v, 0, 1) * 255.0f + 0.5f); }

// Sends the whole stick in one SMBus block write (the controller auto-increments the
// register pointer, verified by read-back), and nothing at all if the colours are unchanged.
// Returns 1 if written.
int ene_send(int k, const rgbf *c, int n) {
    if (k >= nsticks) return 0;
    int a = addr[k];
    if (n > nleds[k]) n = nleds[k];
    BYTE d[34] = { (BYTE)(n * 3) };
    for (int i = 0; i < n; i++) {           // ENE order R,B,G
        d[1 + i * 3] = to8(c[i].r); d[2 + i * 3] = to8(c[i].b); d[3 + i * 3] = to8(c[i].g);
    }
    if (direct[k] && memcmp(last[k], d + 1, n * 3) == 0) return 0;
    WaitForSingleObject(mtx, INFINITE);
    if (!direct[k]) { reg_write(a, 0x8020, 1); reg_write(a, 0x80A0, 1); direct[k] = 1; }
    set_reg(a, 0x8100);
    xfer(a, SMB_WRITE, 0x03, SZ_BLOCK_DATA, d);
    ReleaseMutex(mtx);
    memcpy(last[k], d + 1, n * 3);
    return 1;
}

void ene_restore(void) {
    if (!bus) return;
    WaitForSingleObject(mtx, INFINITE);
    for (int k = 0; k < nsticks; k++)
        if (direct[k]) { reg_write(addr[k], 0x8020, 0); reg_write(addr[k], 0x80A0, 1); direct[k] = 0; }
    ReleaseMutex(mtx);
    memset(last, 0xFF, sizeof(last));
    logf_("ene: restored stick effects");
}

void ene_close(void) {
    if (bus) p_close(bus);
    bus = NULL; nsticks = 0;
    if (mtx) CloseHandle(mtx);
    mtx = NULL;
}
