// SPDX-License-Identifier: GPL-3.0-only
// Sensors. GPU temperature comes from NVML (nvml.dll ships with the NVIDIA driver), else from WDDM (AMD, Intel).
// NVML is loaded only while an effect needs it and unloaded 30 s after the last use,
// because the library alone costs more memory than the rest of the program.
// Audio level comes from audio.cpp, started the same lazy way. Water temperature / flow (Aqua Computer QUADRO) is still to come.
#include "common.h"

typedef int (*pfn_nvml_init)(void);
typedef int (*pfn_nvml_handle)(unsigned int, void **);
typedef int (*pfn_nvml_temp)(void *, int, unsigned int *);
typedef int (*pfn_nvml_shutdown)(void);

static HMODULE           nvml;
static pfn_nvml_temp     nvml_temp;
static pfn_nvml_shutdown nvml_shutdown;
static void             *gpu;
static DWORD             last_poll, last_need, nvml_failed_at, last_audio_need;
static int               audio_on;
static sensors_t         cache = { NAN, NAN, NAN, NAN, NAN };

static void nvml_unload(void) {
    if (gpu && nvml_shutdown) nvml_shutdown();
    if (nvml) FreeLibrary(nvml);
    nvml = NULL; gpu = NULL; nvml_temp = NULL;
}

static int nvml_load(void) {
    nvml = LoadLibraryW(L"nvml.dll");
    if (!nvml) {
        wchar_t p[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", p, MAX_PATH);
        nvml = LoadLibraryW(p);
    }
    if (!nvml) { logf_("sensors: nvml.dll not found"); return 0; }
    pfn_nvml_init   init   = (pfn_nvml_init)GetProcAddress(nvml, "nvmlInit_v2");
    pfn_nvml_handle handle = (pfn_nvml_handle)GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2");
    nvml_temp     = (pfn_nvml_temp)GetProcAddress(nvml, "nvmlDeviceGetTemperature");
    nvml_shutdown = (pfn_nvml_shutdown)GetProcAddress(nvml, "nvmlShutdown");
    if (!init || !handle || !nvml_temp || init() != 0 || handle(0, &gpu) != 0) {
        logf_("sensors: NVML init failed");
        nvml_unload();
        return 0;
    }
    logf_("sensors: NVML loaded");
    return 1;
}

// WDDM's own reading of the card, the one Task Manager shows: AMD and Intel cards, and NVIDIA without NVML. Windows 10
// 2004+ and a driver that reports it; integrated GPUs mostly say 0, so the hottest adapter that says more counts.
// The d3dkmthk.h structures, declared here: that header wants the driver kit's NTSTATUS and a WDDM 2.7 target.
typedef struct { UINT h; LUID luid; ULONG sources; BOOL moves; } kmt_adapter;   // D3DKMT_ADAPTERINFO
typedef struct { ULONG n; kmt_adapter *a; } kmt_enum;                             // D3DKMT_ENUMADAPTERS2
typedef struct { UINT h; int type; void *data; UINT size; } kmt_query;            // D3DKMT_QUERYADAPTERINFO
typedef struct { UINT32 index; ULONGLONG mem_f, mem_max, mem_oc, mem_bw, pcie_bw; ULONG rpm, power, temp; UCHAR ovr; } kmt_perf;   // D3DKMT_ADAPTER_PERFDATA, temp in 0.1 °C
#define KMT_PERFDATA 62   // KMTQAITYPE_ADAPTERPERFDATA
typedef LONG (APIENTRY *pfn_kmt)(void *);
static pfn_kmt kmt_enum2, kmt_query_, kmt_close;

static float kmt_temp(void) {
    static int tried, said;
    if (!tried) {
        tried = 1;
        HMODULE g = LoadLibraryW(L"gdi32.dll");
        kmt_enum2 = (pfn_kmt)GetProcAddress(g, "D3DKMTEnumAdapters2");
        kmt_query_ = (pfn_kmt)GetProcAddress(g, "D3DKMTQueryAdapterInfo");
        kmt_close = (pfn_kmt)GetProcAddress(g, "D3DKMTCloseAdapter");
    }
    kmt_adapter a[16]; kmt_enum e = { 16, a }; float best = NAN;
    if (!kmt_enum2 || !kmt_query_ || !kmt_close || kmt_enum2(&e)) return NAN;
    for (ULONG i = 0; i < e.n && i < 16; i++) {
        kmt_perf p = { 0 }; kmt_query q = { a[i].h, KMT_PERFDATA, &p, sizeof(p) };
        if (!kmt_query_(&q) && p.temp && (isnan(best) || p.temp / 10.0f > best)) best = p.temp / 10.0f;
        UINT c = a[i].h; kmt_close(&c);
    }
    if (!isnan(best) && !said) { said = 1; logf_("sensors: GPU temperature from WDDM"); }
    return best;
}

void sensors_init(void) {}

void sensors_poll(sensors_t *s, int need_gpu, int need_audio) {
    DWORD now = GetTickCount();
    if (need_audio) {
        last_audio_need = now;
        if (!audio_on) { audio_start(); audio_on = 1; }
    } else if (audio_on && now - last_audio_need > 30000) { audio_stop(); audio_on = 0; }
    if (audio_on) audio_read(&cache.audio_level, &cache.audio_bass);
    else cache.audio_level = cache.audio_bass = NAN;
    if (need_gpu) {
        last_need = now;
        // retry a failed load at most once a minute
        if (!gpu && (!nvml_failed_at || now - nvml_failed_at > 60000) && !nvml_load()) nvml_failed_at = now;
    } else if (gpu && now - last_need > 30000) {
        nvml_unload();
        cache.gpu_temp = NAN;
        logf_("sensors: NVML unloaded (not in use)");
    }
    if (now - last_poll >= 1000) {
        last_poll = now;
        unsigned int t;
        cache.gpu_temp = gpu ? (nvml_temp(gpu, 0, &t) == 0 ? (float)t : NAN) : last_need && now - last_need <= 30000 ? kmt_temp() : NAN;
        cache.gpu_at = now;
    }
    *s = cache;
}

void sensors_close(void) { nvml_unload(); if (audio_on) { audio_stop(); audio_on = 0; } }
