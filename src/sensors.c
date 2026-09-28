// Sensors. GPU temperature comes from NVML (nvml.dll ships with the NVIDIA driver).
// NVML is loaded only while an effect needs it and unloaded 30 s after the last use,
// because the library alone costs more memory than the rest of the program.
// Water temperature / flow (Aqua Computer QUADRO) and audio are filled in by later modules.
#include "common.h"

typedef int (*pfn_nvml_init)(void);
typedef int (*pfn_nvml_handle)(unsigned int, void **);
typedef int (*pfn_nvml_temp)(void *, int, unsigned int *);
typedef int (*pfn_nvml_shutdown)(void);

static HMODULE           nvml;
static pfn_nvml_temp     nvml_temp;
static pfn_nvml_shutdown nvml_shutdown;
static void             *gpu;
static DWORD             last_poll, last_need, nvml_failed_at;
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

void sensors_init(void) {}

void sensors_poll(sensors_t *s, int need_gpu) {
    DWORD now = GetTickCount();
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
        cache.gpu_temp = (gpu && nvml_temp(gpu, 0, &t) == 0) ? (float)t : NAN;
    }
    *s = cache;
}

void sensors_close(void) { nvml_unload(); }
