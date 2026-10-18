// SPDX-License-Identifier: MPL-2.0
// The CPU's temperature for fan curves, through PawnIO (modules next to the exe): AMD Zen (family 17h and newer) reads
// Tctl over SMN with AMDFamily17.bin, the temperature AMD meant for fan control; Intel reads the package (or core 0)
// digital readout below TjMax over MSRs with IntelMSR.bin. Registers and formulas are ported from LibreHardwareMonitor
// (LibreHardwareMonitorLib/Hardware/Cpu/Amd17Cpu.cs and IntelCpu.cs).
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Copyright (C) LibreHardwareMonitor and Contributors. Partial Copyright (C) Michael Möller <mmoeller@openhardwaremonitor.org>.
// shortcut: one reading per CPU, no per-core or CCD temperatures; AMD before Zen, Hygon and Zhaoxin read nothing, add
// them when someone has one.
#include "common.h"
#include <intrin.h>

typedef HRESULT (WINAPI *pfn_open)(PHANDLE);
typedef HRESULT (WINAPI *pfn_load)(HANDLE, const UCHAR *, SIZE_T);
typedef HRESULT (WINAPI *pfn_exec)(HANDLE, PCSTR, const ULONG64 *, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
typedef HRESULT (WINAPI *pfn_close)(HANDLE);
static pfn_exec p_exec;
static HANDLE h, mtx;
static int amd, pkg, tjmax, never, tries;
static float last = NAN; static DWORD at, failed_at;
static SRWLOCK lk = SRWLOCK_INIT;   // the fans' two threads and the page ask

static int ex(const char *fn, ULONG64 a, ULONG64 *o) { SIZE_T rs = 0; return !p_exec(h, fn, &a, 1, o, 1, &rs); }

static int cpu_open(void) {
    int r[4]; char v[13];
    __cpuid(r, 0); int max = r[0];
    memcpy(v, &r[1], 4); memcpy(v + 4, &r[3], 4); memcpy(v + 8, &r[2], 4); v[12] = 0;
    __cpuid(r, 1); int fam = (r[0] >> 8) & 0xF; if (fam == 0xF) fam += (r[0] >> 20) & 0xFF;
    amd = !strcmp(v, "AuthenticAMD");
    if (amd ? fam < 0x17 : strcmp(v, "GenuineIntel") || max < 6) { never = 1; logf_("cputemp: %s family %Xh not supported", v, fam); return 0; }
    if (!amd) {
        __cpuid(r, 6);
        if (!(r[0] & 1)) { never = 1; logf_("cputemp: no digital thermal sensor"); return 0; }
        pkg = (r[0] >> 6) & 1;
    }
    // what may come right later (PawnIO installed meanwhile) is logged once
    int first = !tries++;
    HMODULE lib = LoadLibraryW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll");
    if (!lib) { if (first) logf_("cputemp: PawnIO not installed"); return 0; }
    pfn_open p_open = (pfn_open)GetProcAddress(lib, "pawnio_open");
    pfn_load p_load = (pfn_load)GetProcAddress(lib, "pawnio_load");
    pfn_close p_close = (pfn_close)GetProcAddress(lib, "pawnio_close");
    p_exec = (pfn_exec)GetProcAddress(lib, "pawnio_execute");
    wchar_t mp[MAX_PATH]; GetModuleFileNameW(NULL, mp, MAX_PATH);
    wchar_t *s = wcsrchr(mp, L'\\'); if (s) s[1] = 0;
    wcscat_s(mp, MAX_PATH, amd ? L"AMDFamily17.bin" : L"IntelMSR.bin");
    FILE *f = _wfopen(mp, L"rb");
    if (!f) { never = 1; logf_("cputemp: %ls missing next to exe", wcsrchr(mp, L'\\') + 1); return 0; }
    static BYTE blob[256 * 1024]; SIZE_T len = fread(blob, 1, sizeof(blob), f); fclose(f);
    if (!p_open || !p_load || !p_exec || !p_close || p_open(&h)) { if (first) logf_("cputemp: pawnio_open failed (not elevated?)"); h = NULL; return 0; }
    if (p_load(h, blob, len)) { never = 1; logf_("cputemp: module load failed"); p_close(h); h = NULL; return 0; }
    if (amd) mtx = CreateMutexA(NULL, FALSE, "Global\\Access_PCI");   // what other monitors hold around SMN
    else { ULONG64 t; tjmax = ex("ioctl_read_msr", 0x1A2, &t) ? (int)(t >> 16 & 0xFF) : 0; if (!tjmax) tjmax = 100; }   // IA32_TEMPERATURE_TARGET
    logf_("cputemp: %s family %Xh%s", v, fam, amd ? ", Tctl" : pkg ? ", package" : ", core 0");
    return 1;
}

static float cpu_read(void) {
    ULONG64 v;
    if (amd) {
        DWORD w = mtx ? WaitForSingleObject(mtx, 100) : WAIT_OBJECT_0;
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return NAN;
        int ok = ex("ioctl_read_smn", 0x00059800, &v);   // THM_TCON_CUR_TMP
        if (mtx) ReleaseMutex(mtx);
        if (!ok) return NAN;
        float t = ((v >> 21) & 0x7FF) / 8.0f;
        return v & 0x80000 ? t - 49 : t;   // the -49..206 range
    }
    // IA32_PACKAGE_THERM_STATUS, else IA32_THERM_STATUS with its "reading valid" bit
    if (!ex("ioctl_read_msr", pkg ? 0x1B1 : 0x19C, &v) || (!pkg && !(v & 0x80000000))) return NAN;
    return (float)(tjmax - (int)(v >> 16 & 0x7F));
}

static void try_open(DWORD now) { if (!h && !never && (!failed_at || now - failed_at > 60000) && !cpu_open()) failed_at = now ? now : 1; }

// reads (at most once a second): only for a curve that follows the CPU, since reading it while the board's
// firmware reads it too sent board fans up and down on an ASUS X870E with fixed speeds (0.4.33)
float cpu_temp(void) {
    AcquireSRWLockExclusive(&lk);
    DWORD now = GetTickCount();
    try_open(now);
    if (h && now - at >= 1000) { at = now; last = cpu_read(); }
    float t = last;
    ReleaseSRWLockExclusive(&lk);
    return t;
}

// for the page, without reading: whether the CPU can be followed, and its last reading while one is followed
int cpu_can(void) { AcquireSRWLockExclusive(&lk); try_open(GetTickCount()); int ok = h != NULL; ReleaseSRWLockExclusive(&lk); return ok; }
float cpu_last(void) { AcquireSRWLockExclusive(&lk); float t = at && GetTickCount() - at < 5000 ? last : NAN; ReleaseSRWLockExclusive(&lk); return t; }
