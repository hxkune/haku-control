// SPDX-License-Identifier: GPL-3.0-only
// fanprobe: which fan chip a board has and what it reads, for testers. Needs admin, PawnIO, and LpcIO.bin next to the exe.
//   fanprobe                 read-only: board, Super I/O chips, fans / temps, raw register dumps, USB fan hubs -> fanprobe.txt
//   fanprobe set I PCT SECS  NCT6687D: manual duty on control I (2..7, never 0 CPU or 1 PUMP) for SECS s, then the saved state back
//                            NCT6799D: control I (0..6), PCT 30..100, SECS 1..30, then its mode and duty back
//   build.cmd's environment, then:
//   cl /nologo /O2 /MT /D_CRT_SECURE_NO_WARNINGS tools\fanprobe\fanprobe.c setupapi.lib advapi32.lib /Fe:dist\fanprobe\fanprobe.exe /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'"
// Register layouts: LibreHardwareMonitor (Nct677X.cs, IT87XX.cs, LpcIO.cs); only the NCT6687D one is checked on hardware.
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *pfn_open)(PHANDLE);
typedef HRESULT (WINAPI *pfn_load)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pfn_exec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
static pfn_exec p_exec; static HANDLE h, mtx; static FILE *out; static int base;

static void say(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    va_start(a, fmt); vfprintf(out, fmt, a); va_end(a); fflush(out);
}
static ULONG64 ex(const char *fn, ULONG64 a, ULONG64 b, int nin, int nout, HRESULT *hr) {
    ULONG64 in[2] = { a, b }, o[1] = { 0 }; SIZE_T rs = 0;
    HRESULT r = p_exec(h, fn, in, nin, o, nout, &rs);
    if (hr) *hr = r; else if (r) say("  %s(%llx) failed 0x%08lx\n", fn, a, r);
    return o[0];
}
static void outb(int port, int v) { ex("ioctl_pio_outb", port, v, 2, 0, NULL); }
static int inb(int port) { return (int)ex("ioctl_pio_inb", port, 0, 1, 1, NULL); }
static int sio(int reg) { return (int)ex("ioctl_superio_inb", reg, 0, 1, 1, NULL); }
static void sio_out(int reg, int v) { ex("ioctl_superio_outb", reg, v, 2, 0, NULL); }
// the mutex other monitors (LibreHardwareMonitor, HWiNFO, FanControl) hold around Super I/O access; recursive per thread
static void lock(void) { WaitForSingleObject(mtx, 2000); }
static void unlock(void) { ReleaseMutex(mtx); }

static void raw(const char *what, int from, int to, int (*get)(int)) {
    lock(); say("%s raw %03X..%03X\n", what, from, to);
    for (int a = from; a <= to; a += 16) { say("  %03X:", a); for (int k = 0; k < 16; k++) say(" %02X", get(a + k)); say("\n"); }
    unlock();
}

// --- NCT6687D: EC space, page at base+4, index at base+5, data at base+6; 0xFF on the page port = free
static void ec_page(int addr) {
    int t = 0; while (inb(base + 4) != 0xFF && t++ < 500) Sleep(1);
    if (t >= 500) outb(base + 4, 0xFF);
    outb(base + 4, addr >> 8); outb(base + 5, addr & 0xFF);
}
static int rd(int addr) { ec_page(addr); int v = inb(base + 6); outb(base + 4, 0xFF); return v; }
static void wr(int addr, int v) { ec_page(addr); outb(base + 6, v); outb(base + 4, 0xFF); }

static void nct6687_round(void) {
    lock();
    say("RPM :"); for (int i = 0; i < 16; i++) say(" %5d", rd(0x140 + 2 * i) << 8 | rd(0x141 + 2 * i)); say("\n");
    say("PWM%%:"); for (int i = 0; i < 8; i++) say(" %3d", (rd(0x160 + i) * 100 + 127) / 255);
    say("   cmd:"); for (int i = 0; i < 8; i++) say(" %3d", rd(0xA28 + i));
    say("   mode A00=%02X\n", rd(0xA00));
    say("T   :"); for (int i = 0; i < 11; i++) say(" %5.1f", (signed char)rd(0x100 + 2 * i) + 0.5 * (rd(0x101 + 2 * i) >> 7 & 1)); say("\n");
    unlock();
}

// "set" state, also used by the Ctrl+C / window-close handler
static int set_i = -1, saved_bit, saved_cmd;
static void restore(void) {
    if (set_i < 0) return;
    lock();
    wr(0xA01, 0x80); Sleep(50);
    wr(0xA00, (rd(0xA00) & ~(1 << set_i)) | saved_bit);
    wr(0xA28 + set_i, saved_cmd);
    wr(0xA01, 0x40); Sleep(50);
    unlock();
    say("restored control %d: mode bit %d, cmd %d\n", set_i, !!saved_bit, saved_cmd);
    set_i = -1;
}
static void nrestore(void);
static BOOL WINAPI on_ctrl(DWORD t) { (void)t; restore(); nrestore(); return FALSE; }

static int can_set;
static void nct6687(int risky, int argc, char **argv) {
    can_set = 1;
    say("NCT6687D. RPM 0 CPU, 1 PUMP, 2..7 SYS 1..6. PWM/cmd 0 CPU, 1 PUMP, 2..7 SYS 1..6\n");
    for (int r = 0; r < 5; r++) { nct6687_round(); Sleep(1000); }
    raw("EC", 0x100, 0x1FF, rd);
    raw("EC", 0xA00, 0xA3F, rd);
    if (argc != 5 || strcmp(argv[1], "set")) return;
    int i = atoi(argv[2]), pct = atoi(argv[3]), secs = atoi(argv[4]);
    if (risky) { say("set refused: this board likely has the NCT6687D-R, its registers differ\n"); return; }
    if (i < 2 || i > 7 || pct < 20 || pct > 100 || secs < 1 || secs > 60) { say("set: I 2..7, PCT 20..100, SECS 1..60\n"); return; }
    lock();
    saved_bit = rd(0xA00) & (1 << i); saved_cmd = rd(0xA28 + i); set_i = i;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    wr(0xA01, 0x80); Sleep(50);
    wr(0xA00, rd(0xA00) | (1 << i));
    wr(0xA28 + i, pct * 255 / 100);
    wr(0xA01, 0x40); Sleep(50);
    unlock();
    say("control %d -> %d%% for %d s (saved bit %d, cmd %d)\n", i, pct, secs, !!saved_bit, saved_cmd);
    for (int s = 0; s < secs; s += 2) { nct6687_round(); Sleep(2000); }
    restore();
    Sleep(3000); nct6687_round();
}

// --- other Nuvoton (NCT679x, NCT6683/6686): banked, bank select at register 0x4E, index base+5, data base+6
static int nrd(int a) { outb(base + 5, 0x4E); outb(base + 6, a >> 8); outb(base + 5, a & 0xFF); return inb(base + 6); }
static void nwr(int a, int v) { outb(base + 5, 0x4E); outb(base + 6, a >> 8); outb(base + 5, a & 0xFF); outb(base + 6, v); }
// NCT6799D (LHM's NCT6797D/98D/99D layout): duty now, the duty asked in manual mode, the mode in the high nibble (0 manual)
static const int n_out[7] = { 0x001, 0x003, 0x011, 0x013, 0x015, 0x017, 0x029 },
                 n_cmd[7] = { 0x109, 0x209, 0x309, 0x809, 0x909, 0xA09, 0xB09 },
                 n_mode[7] = { 0x102, 0x202, 0x302, 0x802, 0x902, 0xA02, 0xB02 };
static int nset_i = -1, nsaved_mode, nsaved_cmd;
static void n_round(int is6799) {
    lock();
    say("RPM 4C0..4CF:"); for (int i = 0; i < 8; i++) say(" %5d", nrd(0x4C0 + 2 * i) << 8 | nrd(0x4C1 + 2 * i));
    if (is6799) {
        say("\nPWM%%:"); for (int i = 0; i < 7; i++) say(" %3d", (nrd(n_out[i]) * 100 + 127) / 255);
        say("   mode:"); for (int i = 0; i < 7; i++) say(" %X", nrd(n_mode[i]) >> 4);
        say("   cmd:"); for (int i = 0; i < 7; i++) say(" %3d", nrd(n_cmd[i]));
    }
    say("\n"); unlock();
}
static void nrestore(void) {
    if (nset_i < 0) return;
    lock(); nwr(n_cmd[nset_i], nsaved_cmd); nwr(n_mode[nset_i], nsaved_mode); unlock();
    say("restored control %d: mode %02X, cmd %d\n", nset_i, nsaved_mode, nsaved_cmd);
    nset_i = -1;
}
static void nuvoton(int is6799, int argc, char **argv) {
    lock(); outb(base + 5, 0x4E); int bank = inb(base + 6); unlock();
    if (is6799) say("NCT6799D. RPM 0..7 and controls 0..6 in the chip's order, not the board's header names\n");
    for (int r = 0; r < 5; r++) { n_round(is6799); Sleep(1000); }
    for (int pass = 0; pass < 2; pass++) { raw("bank", 0x000, 0xBFF, nrd); Sleep(2000); }
    if (is6799 && argc == 5 && !strcmp(argv[1], "set")) {
        can_set = 1;
        int i = atoi(argv[2]), pct = atoi(argv[3]), secs = atoi(argv[4]);
        if (i < 0 || i > 6 || pct < 30 || pct > 100 || secs < 1 || secs > 30) say("set: I 0..6, PCT 30..100, SECS 1..30\n");
        else {
            lock();
            nsaved_mode = nrd(n_mode[i]); nsaved_cmd = nrd(n_cmd[i]); nset_i = i;
            SetConsoleCtrlHandler(on_ctrl, TRUE);
            nwr(n_mode[i], nsaved_mode & 0x0F); nwr(n_cmd[i], pct * 255 / 100);
            unlock();
            say("control %d -> %d%% for %d s (saved mode %02X, cmd %d)\n", i, pct, secs, nsaved_mode, nsaved_cmd);
            for (int s = 0; s < secs; s += 2) { n_round(1); Sleep(2000); }
            nrestore();
            Sleep(3000); n_round(1);
        }
    }
    lock(); outb(base + 5, 0x4E); outb(base + 6, bank); unlock();
}

// --- ITE IT86xx/IT87xx/IT89xx: index base+5, data base+6, 256 registers
static int ird(int a) { outb(base + 5, a); return inb(base + 6); }
static void ite(void) {
    static const int lo[6] = { 0x0D, 0x0E, 0x0F, 0x80, 0x82, 0x4C }, hi[6] = { 0x18, 0x19, 0x1A, 0x81, 0x83, 0x4D };
    for (int r = 0; r < 5; r++) {
        lock();
        say("RPM :"); for (int i = 0; i < 6; i++) { int c = ird(hi[i]) << 8 | ird(lo[i]); say(" %5d", c > 0 && c < 0xFFFF ? 1350000 / (2 * c) : 0); }
        say("   T:"); for (int i = 0; i < 6; i++) say(" %4d", (signed char)ird(0x29 + i)); say("\n");
        unlock(); Sleep(1000);
    }
    for (int pass = 0; pass < 2; pass++) { raw("ITE", 0x00, 0xFF, ird); Sleep(2000); }
}

// in config mode: the hardware monitor's base address for logical device ldn, 0 if none
static int hwm_base(int ldn) {
    HRESULT hr; ex("ioctl_find_bars", 0, 0, 0, 0, &hr);
    sio_out(0x07, ldn);
    int a1 = sio(0x60) << 8 | sio(0x61); Sleep(1); int a2 = sio(0x60) << 8 | sio(0x61);
    if ((a1 & 7) == 5) a1 &= 0xFFF8, a2 &= 0xFFF8;
    say("  LDN %02X: active %02X, base %04X/%04X, reg 28 %02X, find_bars 0x%08lx\n", ldn, sio(0x30), a1, a2, sio(0x28), hr);
    return !hr && a1 == a2 && a1 >= 0x100 && !(a1 & 0xF007) ? a1 : 0;
}

static wchar_t board[256];
static void reg(const wchar_t *key, const wchar_t *val, const char *label, wchar_t *keep) {
    wchar_t s[256] = L""; DWORD n = sizeof(s);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, val, RRF_RT_REG_SZ, NULL, s, &n);
    say("%s: %ls\n", label, s);
    if (keep) wcscpy(keep, s);
}

static void hubs(void) {
    static const struct { unsigned vid; const char *who; } v[] = {
        { 0x0C70, "Aqua Computer" }, { 0x1B1C, "Corsair" }, { 0x1E71, "NZXT" }, { 0x0CF2, "Lian Li" },
        { 0x264A, "Thermaltake" }, { 0x2516, "Cooler Master" }, { 0x1044, "Gigabyte" }, { 0x0B05, "ASUS" },
        { 0x0416, "Winbond (Lian Li wireless)" },
    };
    say("USB devices from fan / cooling vendors:\n");
    HDEVINFO di = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dd = { sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t id[512] = L"", name[256] = L""; unsigned vid, pid;
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_HARDWAREID, NULL, (BYTE *)id, sizeof(id) - 4, NULL)) continue;
        const wchar_t *p = wcsstr(id, L"VID_");
        if (!p || wcsstr(id, L"&MI_") || swscanf(p, L"VID_%4x&PID_%4x", &vid, &pid) != 2) continue;
        for (int k = 0; k < (int)(sizeof(v) / sizeof(v[0])); k++) if (v[k].vid == vid) {
            SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name) - 4, NULL);
            say("  %04X:%04X %s: %ls\n", vid, pid, v[k].who, name);
        }
    }
    SetupDiDestroyDeviceInfoList(di);
}

static int done(int code) {
    say("--- end. Send fanprobe.txt (next to fanprobe.exe) back, and say which headers your fans are on.\n");
    DWORD pl[2]; // only when started by double-click, so the window does not vanish
    if (GetConsoleProcessList(pl, 2) == 1) { printf("Press Enter to close.\n"); getchar(); }
    return code;
}

int main(int argc, char **argv) {
    wchar_t dir[MAX_PATH], p[MAX_PATH]; GetModuleFileNameW(NULL, dir, MAX_PATH); *(wcsrchr(dir, L'\\') + 1) = 0;
    swprintf(p, MAX_PATH, L"%sfanprobe.txt", dir); out = _wfopen(p, L"a");
    if (!out) { printf("cannot write fanprobe.txt next to the exe: move the folder out of Program Files / Downloads zip\n"); getchar(); return 1; }
    SYSTEMTIME st; GetLocalTime(&st);
    say("--- fanprobe 2, %04d-%02d-%02d %02d:%02d:%02d%s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, argc > 1 ? " (set)" : "");
    const wchar_t *bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    wchar_t maker[256];
    reg(bios, L"BaseBoardManufacturer", "board maker", maker);
    reg(bios, L"BaseBoardProduct", "board", board);
    reg(bios, L"BIOSVersion", "BIOS", NULL);
    reg(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", "CPU", NULL);
    reg(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", "Windows build", NULL);
    // MSI X870/B850/B840/B860/Z890 boards carry the NCT6687D-R: same chip id, other registers
    int risky = wcsstr(maker, L"Micro-Star") && (wcsstr(board, L"X870") || wcsstr(board, L"B850") || wcsstr(board, L"B840") || wcsstr(board, L"B860") || wcsstr(board, L"Z890"));

    HMODULE lib = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { say("PawnIO is not installed: get it from https://pawnio.eu and run fanprobe again\n"); return done(1); }
    pfn_open p_open = (pfn_open)GetProcAddress(lib, "pawnio_open");
    pfn_load p_load = (pfn_load)GetProcAddress(lib, "pawnio_load");
    p_exec = (pfn_exec)GetProcAddress(lib, "pawnio_execute");
    swprintf(p, MAX_PATH, L"%sLpcIO.bin", dir);
    FILE *f = _wfopen(p, L"rb"); if (!f) { say("LpcIO.bin is missing: keep it next to fanprobe.exe\n"); return done(1); }
    static BYTE blob[256 * 1024]; SIZE_T n = fread(blob, 1, sizeof(blob), f); fclose(f);
    if (p_open(&h)) { say("pawnio_open failed (not run as administrator?)\n"); return done(1); }
    if (p_load(h, blob, n)) { say("LpcIO.bin load failed\n"); return done(1); }
    mtx = CreateMutexA(NULL, FALSE, "Global\\Access_ISABUS.HTP.Method");

    int found = 0;
    for (int slot = 0; slot < 2; slot++) {
        int port = slot ? 0x4E : 0x2E, a = 0;
        lock();
        ex("ioctl_select_slot", slot, 0, 1, 0, NULL);
        outb(port, 0x87); outb(port, 0x87);
        int id = sio(0x20), rev = sio(0x21);
        say("port %02X, Nuvoton entry: id %02X %02X\n", port, id, rev);
        int nct = id >= 0xB0 && id <= 0xDF, is6687 = id == 0xD5 && rev == 0x92;
        if (nct) a = hwm_base(0x0B);
        outb(port, 0xAA);
        unlock();
        if (nct) {
            if (a) { base = a; found = 1; if (is6687) nct6687(risky, argc, argv); else nuvoton(id == 0xD8, argc, argv); }
            continue;
        }
        lock();
        outb(port, 0x87); outb(port, 0x01); outb(port, 0x55); outb(port, slot ? 0xAA : 0x55);
        int iid = sio(0x20) << 8 | sio(0x21);
        say("port %02X, ITE entry: id %04X\n", port, iid);
        int it = iid >> 8 == 0x86 || iid >> 8 == 0x87 || iid >> 8 == 0x89;
        if (it) a = hwm_base(0x04);
        if (!slot) sio_out(0x02, 0x02); // LHM leaves the secondary chip on 0x4E in config mode
        unlock();
        if (it && a) { base = a; found = 1; ite(); }
    }
    if (!found) say("no known fan chip found: the raw ids above still help\n");
    if (argc > 1 && !can_set) say("set: only the NCT6687D and NCT6799D for now\n");
    hubs();
    return done(0);
}
