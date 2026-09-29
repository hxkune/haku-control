// SPDX-License-Identifier: GPL-3.0-only
// haku control: RGB effects for PC lighting (MSI Mystic Light ARGB header, ENE DRAM) and room lights
// (Nanoleaf, AiDot), all in one scene.
// One render thread computes frames; a second thread feeds the (slower) SMBus RAM sticks.
// Unchanged frames are not sent, and the loop slows down to 5 Hz while nothing changes.
#include "common.h"
#include <shellapi.h>
#include <stdarg.h>
#include <stdlib.h>
#include <process.h>
#include <ctype.h>
#include <timeapi.h>

// visual styles for the settings window controls
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_TRAY      (WM_APP + 1)
#define ID_FX_BASE   1000
#define ID_BRIGHT    1100
#define ID_SETTINGS  1200
#define ID_RELOAD    1201
#define ID_LOG       1202
#define ID_EXIT      1203
#define ID_WINDOW    1204
#define ID_RAM_ON    1205
#define ID_GPU_ON    1206
#define ID_LIGHTS_ON 1207
#define ID_NANO_ON   1300   // + controller 0..NANO_MAX-1
#define WM_REHOTKEY  (WM_APP + 2)
#define WM_REMOTE_CMD (WM_APP + 3)
#define SAVE_TIMER   1
enum { HK_NEXT = 1, HK_PREV, HK_OFF, HK_BUP, HK_BDOWN };

static const char default_ini[] =
"; haku control settings. The file is re-read automatically after it is saved.\r\n"
"; Colours: hex (#RRGGBB), comma separated. Speed: 1..10. Brightness: 0..100.\r\n"
"\r\n"
"[general]\r\n"
"effect=flow\r\n"
"palette=#00C8FF, #7A3CFF, #FF2D95\r\n"
"speed=5\r\n"
"brightness=100\r\n"
"fps=30\r\n"
"lang=en\r\n"
"; 1 = show the first-start guide in the window\r\n"
"welcome=1\r\n"
"\r\n"
"; Each effect can have its own palette and speed, otherwise [general] is used.\r\n"
"[flow]\r\n"
"[caustic]\r\n"
"palette=#00E5FF, #0060FF, #00FFB0\r\n"
"[bubbles]\r\n"
"palette=#001830, #00E5FF, #FFFFFF\r\n"
"[comet]\r\n"
"palette=#FFFFFF, #00C8FF, #7A3CFF\r\n"
"[lava]\r\n"
"palette=#FF2D00, #FF9000, #B0006A\r\n"
"speed=3\r\n"
"[breathe]\r\n"
"; smooth colour -> colour fade, never through black\r\n"
"palette=#00C8FF, #FF2D95\r\n"
"speed=3\r\n"
"[temperature]\r\n"
"source=gpu\r\n"
"cold=35\r\n"
"hot=75\r\n"
"palette=#0050FF, #00FF80, #FFB000, #FF0020\r\n"
"[pump]\r\n"
"[audio]\r\n"
"palette=#00C8FF, #7A3CFF, #FF2D95\r\n"
"[static]\r\n"
"palette=#7A3CFF\r\n"
"\r\n"
"[hotkeys]\r\n"
"next=Ctrl+Alt+Right\r\n"
"prev=Ctrl+Alt+Left\r\n"
"off=Ctrl+Alt+Down\r\n"
"brighter=Ctrl+Alt+PageUp\r\n"
"dimmer=Ctrl+Alt+PageDown\r\n"
"\r\n"
"[layout]\r\n"
"; LEDs on the ARGB header (JRAINBOW1) and a name for it, e.g. Water block\r\n"
"gpu_leds=8\r\n"
"strip_name=\r\n"
"; 1 = reverse the direction\r\n"
"gpu_reverse=0\r\n"
"ram_reverse=0\r\n"
"; 1 = swap the memory sticks\r\n"
"ram_swap=0\r\n"
"; LED on the motherboard itself: 1 = on, 0 = off\r\n"
"board_led=1\r\n"
"\r\n"
"[hotspot]\r\n"
"; 1 = keep the Windows Mobile Hotspot on (for lights that join the PC's own Wi-Fi)\r\n"
"auto=0\r\n";

// ---- shared state
static CRITICAL_SECTION cs;
static volatile LONG running = 1, need_reinit, need_reload;
static int      cur_effect;
static float    brightness = 1.0f;
static volatile LONG cfg_gen;         // bumps on every settings change (the Nanoleaf loop is baked again)
static int      prev_effect = 0;
static scene_t  scene;
static int      gpu_leds = 8, gpu_rev, ram_rev, ram_swap, board_led = 1, ram_on = 1, gpu_on = 1, lights_on = 1, nano_on[NANO_MAX], light_on[8];
// Colour correction for the PWM-driven LEDs (RAM, GPU block, board): colours are sRGB-like, the LEDs are linear,
// so they get a gamma curve; plus a per-device white balance ([calibration] ram_warmth / gpu_warmth, -100..100).
enum { FIX_RAM, FIX_GPU };
static float    fix_gamma = 2.2f, fix_gain[2][3] = { { 1, 1, 1 }, { 1, 1, 1 } };

static void load_calibration(void) {
    fix_gamma = cfg_geti("calibration", "gamma", 1) ? 2.2f : 1.0f;
    for (int d = 0; d < 2; d++) {
        float w = clampf(cfg_getf("calibration", d ? "gpu_warmth" : "ram_warmth", 0) / 100.0f, -1, 1);
        // warmer: less blue and a bit less green; cooler: less red
        fix_gain[d][0] = w < 0 ? 1 + 0.5f * w : 1;
        fix_gain[d][1] = w > 0 ? 1 - 0.3f * w : 1 + 0.1f * w;
        fix_gain[d][2] = w > 0 ? 1 - 0.65f * w : 1;
    }
}

static rgbf led_fix(rgbf c, int d) {
    rgbf r = { powf(clampf(c.r, 0, 1), fix_gamma) * fix_gain[d][0],
               powf(clampf(c.g, 0, 1), fix_gamma) * fix_gain[d][1],
               powf(clampf(c.b, 0, 1), fix_gamma) * fix_gain[d][2] };
    return r;
}
static rgbf     ram_buf[2][8];
static int      ram_n[2];
static HANDLE   ram_event, th_render, th_ram;
static HWND     hwnd;
static NOTIFYICONDATAW nid;
static wchar_t  exe_dir[MAX_PATH], data_dir[MAX_PATH];
static FILE    *logfile;
static int      have_msi, have_ene, fps = 30;
static HINSTANCE app_inst;
static sensors_t last_sensors = { NAN, NAN, NAN, NAN, NAN };
static scene_t  frame_sc;              // last rendered frame, for the live preview in the window
static rgbf     frame_c[MAX_LEDS];
static HICON    tray_icon;

void app_data_path(const wchar_t *name, wchar_t *out) { swprintf(out, MAX_PATH, L"%s%s", data_dir, name); }

void logf_(const char *fmt, ...) {
    if (!logfile) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(logfile, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfprintf(logfile, fmt, ap); va_end(ap);
    fputc('\n', logfile); fflush(logfile);
}

// [layout] switch of Nanoleaf controller k: nanoleaf_enabled, nanoleaf2_enabled...
static void nano_key(int k, char *out, int cap) { if (k) snprintf(out, cap, "nanoleaf%d_enabled", k + 1); else snprintf(out, cap, "nanoleaf_enabled"); }

// ---- scene layout: RAM0 bottom->top, RAM1 bottom->top, then GPU left->right
static void build_scene(void) {
    gpu_leds  = cfg_geti("layout", "gpu_leds", 8);
    if (gpu_leds < 1) gpu_leds = 1; if (gpu_leds > 40) gpu_leds = 40;
    gpu_rev   = cfg_geti("layout", "gpu_reverse", 0);
    ram_rev   = cfg_geti("layout", "ram_reverse", 0);
    ram_swap  = cfg_geti("layout", "ram_swap", 0);
    board_led = cfg_geti("layout", "board_led", 1);
    ram_on    = cfg_geti("layout", "ram_enabled", 1);
    gpu_on    = cfg_geti("layout", "gpu_enabled", 1);
    lights_on = cfg_geti("layout", "lights_enabled", 1);
    for (int k = 0; k < NANO_MAX; k++) { char key[32]; nano_key(k, key, sizeof(key)); nano_on[k] = cfg_geti("layout", key, 1); }
    for (int i = 0; i < 8; i++) { char key[32]; snprintf(key, sizeof(key), "light%d_enabled", i + 1); light_on[i] = cfg_geti("layout", key, 1); }
    load_calibration();
    // disabled devices are left out of the scene, so effects span only what is lit
    int sticks = !ram_on ? 0 : have_ene ? ene_count() : 2;
    if (sticks > 2) sticks = 2;
    int glit = gpu_on ? gpu_leds : 0;

    static scene_t s;   // big: not on the stack
    memset(&s, 0, sizeof(s));
    int total = sticks * 8 + glit;
    if (total == 0) total = 1;
    int k = 0;
    for (int st = 0; st < sticks; st++)
        for (int i = 0; i < 8; i++, k++) {
            led_t *l = &s.leds[s.count++];
            l->dev = st ? DEV_RAM1 : DEV_RAM0; l->index = i; l->zone = ZONE_RAM;
            l->x = 0.06f + st * 0.08f; l->y = 0.9f - i * 0.8f / 7; l->fill = i / 7.0f;
            l->path = (float)k / total; l->zpath = (float)(st * 8 + i) / (sticks * 8);
        }
    for (int i = 0; i < glit; i++, k++) {
        led_t *l = &s.leds[s.count++];
        l->dev = DEV_GPU; l->index = i; l->zone = ZONE_GPU;
        l->x = 0.3f + i * 0.65f / (gpu_leds > 1 ? gpu_leds - 1 : 1); l->y = 0.15f;
        l->fill = gpu_leds > 1 ? (float)i / (gpu_leds - 1) : 0; l->path = (float)k / total; l->zpath = (float)i / glit;
    }
    // room bulbs: extra "LEDs" with their own path positions, spread across the palette
    int nl = lights_on ? lights_count() : 0;
    for (int i = 0; i < nl; i++) {
        if (i < 8 && !light_on[i]) continue;   // switched off on its own: left out, gets black (off)
        led_t *l = &s.leds[s.count++];
        l->dev = DEV_LIGHT; l->index = i; l->zone = ZONE_LIGHT0 + i;
        l->x = nl > 1 ? 0.1f + 0.8f * i / (nl - 1) : 0.5f; l->y = 0.5f;
        l->fill = nl > 1 ? (float)i / (nl - 1) : 0.5f; l->path = (float)i / nl; l->zpath = 0;
    }
    // Nanoleaf panels: real wall positions (from each controller's layout), several controllers side by side
    int nctl = 0, ci = 0;
    for (int k = 0; k < NANO_MAX; k++) if (nano_on[k] && nano_count(k)) nctl++;
    for (int k = 0; k < NANO_MAX; k++) {
        int nn = nano_on[k] ? nano_count(k) : 0;
        if (!nn) continue;
        float x0 = 0.1f + 0.8f * ci / nctl, w = 0.8f / nctl;
        for (int i = 0; i < nn && s.count < MAX_LEDS - 1; i++) {
            led_t *l = &s.leds[s.count++];
            float x = 0.5f, y = 0.5f, path = 0; nano_panel(k, i, &x, &y, &path);
            l->dev = DEV_NANO; l->index = i; l->zone = ZONE_NANO0 + k;
            l->x = x0 + w * x; l->y = 0.1f + 0.8f * y; l->fill = path; l->path = (ci + path) / nctl; l->zpath = path;
        }
        ci++;
    }
    // LAN / bridge devices: strips as rows across the scene, separate lights spread like bulbs
    int ne = ext_count(), rows = 0;
    for (int k = 0; k < ne; k++) if (ext_slot_leds(k)) rows++;
    // by type: floor lamps and light bars stand upright (bars: two of them, the LEDs split), a screen backlight runs
    // around the screen (from the bottom left, clockwise)
    for (int k = 0, row = 0; k < ne; k++) {
        int n = ext_slot_leds(k), strip = ext_slot_strip(k);
        if (!n) continue;
        char ty[12]; ext_slot_type(k, ty, sizeof(ty));
        float ry = rows > 1 ? 0.3f + 0.6f * row / (rows - 1) : 0.6f, rx = rows > 1 ? 0.15f + 0.7f * row / (rows - 1) : 0.5f;
        row++;
        for (int i = 0; i < n && s.count < MAX_LEDS - 1; i++) {
            led_t *l = &s.leds[s.count++];
            float f = n > 1 ? (float)i / (n - 1) : 0.5f;
            l->dev = DEV_EXT; l->index = i; l->zone = ZONE_EXT0 + k;
            l->x = 0.1f + 0.8f * f; l->y = strip ? ry : 0.5f + 0.3f * sinf(f * 6.283f + k);
            l->fill = f; l->path = strip ? f : (float)i / n; l->zpath = f;
            if (!strcmp(ty, "floor")) { l->x = rx; l->y = 0.9f - 0.8f * f; }
            else if (!strcmp(ty, "bars")) {
                int half = (n + 1) / 2, side = i >= half; float g = half > 1 ? (float)(side ? i - half : i) / (half - 1) : 0.5f;
                l->x = rx + (side ? 0.08f : -0.08f); l->y = 0.9f - 0.8f * g; l->fill = g; l->zpath = g;
            } else if (!strcmp(ty, "tv") && n >= 4) {
                float p = f * 4, q = p - (int)p;   // 0..1 left edge up, 1..2 top, 2..3 right edge down, 3..4 bottom
                int e = (int)p; if (e > 3) { e = 3; q = 1; }
                l->x = e == 0 ? 0.1f : e == 1 ? 0.1f + 0.8f * q : e == 2 ? 0.9f : 0.9f - 0.8f * q;
                l->y = e == 0 ? 0.9f - 0.8f * q : e == 1 ? 0.1f : e == 2 ? 0.1f + 0.8f * q : 0.9f;
            }
        }
    }
    if (board_led) {
        led_t *l = &s.leds[s.count++];
        l->dev = DEV_BOARD; l->index = 0; l->zone = ZONE_GPU; l->x = 0.2f; l->y = 0.5f; l->fill = 0.5f; l->path = 0.5f; l->zpath = 0.5f;
    }
    EnterCriticalSection(&cs); scene = s; LeaveCriticalSection(&cs);
}

static void load_config(void) {
    cfg_load(NULL);
    EnterCriticalSection(&cs);
    cur_effect = effect_index(cfg_get("general", "effect", "flow"));
    brightness = clampf(cfg_getf("general", "brightness", 100) / 100.0f, 0, 1);
    fps = cfg_geti("general", "fps", 30);
    if (fps < 5) fps = 5;
    if (fps > 60) fps = 60;
    effects_reset();
    LeaveCriticalSection(&cs);
    build_scene();
}

// ---- threads
static unsigned __stdcall ram_thread(void *p) {
    (void)p;
    while (running) {
        WaitForSingleObject(ram_event, 1000);
        if (!running) break;
        rgbf c[2][8]; int n[2];
        EnterCriticalSection(&cs); memcpy(c, ram_buf, sizeof(c)); memcpy(n, ram_n, sizeof(n)); LeaveCriticalSection(&cs);
        for (int st = 0; st < 2; st++) if (n[st]) ene_send(st, c[st], n[st]);
    }
    return 0;
}

static void open_devices(void) {
#ifdef HAKU_DEV
    return;   // the test build never touches the board or the memory
#endif
    // [devices] msi / ene = 0 leaves that hardware to other software (e.g. OpenRGB)
    have_msi = cfg_geti("devices", "msi", 1) ? msi_open() : 0;
    have_ene = cfg_geti("devices", "ene", 1) ? ene_open() > 0 : 0;
}

static void close_devices(int restore) {
    if (restore) { msi_restore(); ene_restore(); }
    msi_close(); ene_close();
    have_msi = have_ene = 0;
}

static unsigned __stdcall render_thread(void *p) {
    (void)p;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER fq, prev, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&prev);
    DWORD last_cfg_check = GetTickCount();
    rgbf out[MAX_LEDS];
    int idle_frames = 0;

    while (running) {
        if (InterlockedExchange(&need_reinit, 0)) {
            // after resume from sleep: controllers were power-cycled
            close_devices(0); Sleep(1500); open_devices(); build_scene();
        }
        if (nano_layout_changed() | lights_changed() | ext_layout_changed()) { build_scene(); ui_refresh(); }
        if (InterlockedExchange(&need_reload, 0) ||
            (GetTickCount() - last_cfg_check > 1000 && (last_cfg_check = GetTickCount(), cfg_changed_on_disk()))) {
            load_config();
            InterlockedIncrement(&cfg_gen);
            logf_("config reloaded, effect=%s", g_effects[cur_effect].id);
            PostMessageW(hwnd, WM_REHOTKEY, 0, 0);
        }

        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - prev.QuadPart) / fq.QuadPart;
        prev = now;
        if (dt > 0.25) dt = 0.25;

        sensors_t sn; sensors_poll(&sn, effects_need_gpu_temp() || cur_effect == effect_index("temperature"), effects_need_audio() || cur_effect == effect_index("audio"));
        last_sensors = sn;
        static scene_t sc;
        EnterCriticalSection(&cs);
        sc.count = scene.count; memcpy(sc.leds, scene.leds, scene.count * sizeof(led_t));
        int fx = cur_effect; float br = brightness;
        effects_render(fx, &sc, &sn, dt, out);
        int bulb_k[8] = { 0 };
        for (int i = 0; i < sc.count; i++)
            if (sc.leds[i].dev == DEV_LIGHT && sc.leds[i].index < 8) effects_zone_white(sc.leds[i].zone, &bulb_k[sc.leds[i].index]);
        LeaveCriticalSection(&cs);

        rgbf gpu[40] = { 0 }, board = { 0, 0, 0 }, ram[2][8] = { 0 }, bulb[8] = { 0 }; int rn[2] = { 0, 0 }, gn = 0, bn = 0;
        static rgbf nano[NANO_MAX][NANO_MAX_PANELS]; int nn[NANO_MAX] = { 0 };
        static rgbf ext[EXT_SLOTS][512]; int en[EXT_SLOTS] = { 0 };
        for (int i = 0; i < sc.count; i++) {
            rgbf c = scalec(out[i], br);
            out[i] = c;   // kept for the preview (before the white-bulb conversion below)
            const led_t *l = &sc.leds[i];
            switch (l->dev) {
            case DEV_GPU: gpu[gpu_rev ? gpu_leds - 1 - l->index : l->index] = led_fix(c, FIX_GPU); gn++; break;
            case DEV_BOARD: board = led_fix(c, FIX_GPU); break;
            case DEV_LIGHT:
                if (l->index < 8) {
                    // white mode: the bulb's white LEDs take the level, the RGB approximation is not used
                    if (bulb_k[l->index]) { float lv = max(c.r, max(c.g, c.b)); c.r = c.g = c.b = lv; }
                    bulb[l->index] = c;
                    if (l->index >= bn) bn = l->index + 1;
                }
                break;
            case DEV_NANO: {
                int k = l->zone - ZONE_NANO0;
                if (k >= 0 && k < NANO_MAX && l->index < NANO_MAX_PANELS) { nano[k][l->index] = c; if (l->index >= nn[k]) nn[k] = l->index + 1; }
                break;
            }
            case DEV_EXT: {
                int k = l->zone - ZONE_EXT0;
                if (k >= 0 && k < EXT_SLOTS && l->index < 512) { ext[k][l->index] = c; if (l->index >= en[k]) en[k] = l->index + 1; }
                break;
            }
            case DEV_RAM0: case DEV_RAM1: {
                int st = (l->dev == DEV_RAM1) ^ ram_swap;
                ram[st][ram_rev ? 7 - l->index : l->index] = led_fix(c, FIX_RAM); rn[st] = 8;
                break;
            }
            default: break;
            }
        }

        EnterCriticalSection(&cs);
        frame_sc.count = sc.count; memcpy(frame_sc.leds, sc.leds, sc.count * sizeof(led_t)); memcpy(frame_c, out, sc.count * sizeof(rgbf));
        LeaveCriticalSection(&cs);

        // switched-off devices get black (sent once, then skipped as unchanged)
        if (!gpu_on) gn = gpu_leds;
        if (!ram_on) for (int st = 0; st < 2 && st < ene_count(); st++) rn[st] = 8;

        if (lights_count()) { if (lights_on) bn = min(lights_count(), 8); lights_submit(bulb, bulb_k, bn, lights_on); }
        for (int k = 0; k < NANO_MAX; k++) {
            if (!nano_present(k)) continue;
            nano_submit(k, nano[k], nn[k], nano_on[k]);
            // the panels play the effect themselves: bake a new loop when what they show changed (debounced, so a
            // dragged slider sends one animation, not dozens), or when the driver asks after (re)connecting
            static int b_fx[NANO_MAX], b_on[NANO_MAX], b_init; static float b_br[NANO_MAX]; static LONG b_gen[NANO_MAX]; static DWORD b_due[NANO_MAX];
            if (!b_init) { for (int j = 0; j < NANO_MAX; j++) { b_fx[j] = -2; b_on[j] = -1; b_br[j] = -1; b_gen[j] = -1; } b_init = 1; }
            DWORD tnow = GetTickCount();
            if (fx != b_fx[k] || br != b_br[k] || nano_on[k] != b_on[k] || cfg_gen != b_gen[k]) {
                // a new effect goes out at once; sliders (brightness, speed, colours) wait until they rest
                b_due[k] = fx != b_fx[k] || nano_on[k] != b_on[k] ? tnow | 1 : (tnow + 300) | 1;
                b_fx[k] = fx; b_br[k] = br; b_on[k] = nano_on[k]; b_gen[k] = cfg_gen;
            }
            if (nano_bake_wanted(k)) b_due[k] = tnow | 1;
            if (b_due[k] && nano_on[k] && (int)(tnow - b_due[k]) >= 0) {   // switched off: the driver turns the panels off
                static rgbf frames[NANO_MAX_FRAMES * NANO_MAX_PANELS];
                float step = 1; int nf = 0;
                b_due[k] = 0;
                if (nano_on_device(k) && nn[k] > 0) {
                    int maxf = 720 / nn[k]; if (maxf > NANO_MAX_FRAMES) maxf = NANO_MAX_FRAMES; if (maxf < 4) maxf = 4;
                    EnterCriticalSection(&cs); nf = effects_bake(fx, &sc, DEV_NANO, ZONE_NANO0 + k, maxf, NANO_MAX_PANELS, frames, &step); LeaveCriticalSection(&cs);
                    for (int i = 0; i < nf * NANO_MAX_PANELS; i++) frames[i] = scalec(frames[i], br);
                }
                nano_upload(k, frames, nf, nn[k], step);
            }
        }
        for (int k = 0; k < EXT_SLOTS; k++) if (en[k]) ext_submit(k, ext[k], en[k]);

        // "animating" = any LED of the scene changed (not only the PC hardware: a setup may be LAN lights only)
        // (compared at 8 bits: slow fades that don't move a single LED step still let the loop idle)
        static BYTE prev8[MAX_LEDS * 3]; static int prev_count;
        int changed = sc.count != prev_count;
        for (int i = 0; i < sc.count; i++) {
            BYTE q[3] = { (BYTE)(clampf(out[i].r, 0, 1) * 255 + .5f), (BYTE)(clampf(out[i].g, 0, 1) * 255 + .5f), (BYTE)(clampf(out[i].b, 0, 1) * 255 + .5f) };
            if (memcmp(prev8 + i * 3, q, 3)) { memcpy(prev8 + i * 3, q, 3); changed = 1; }
        }
        prev_count = sc.count;
        if (have_msi) changed |= msi_send(gpu, gn, &board) == 1;
        if (have_ene) {
            EnterCriticalSection(&cs);
            if (memcmp(ram_buf, ram, sizeof(ram)) != 0) { memcpy(ram_buf, ram, sizeof(ram)); changed = 1; }
            memcpy(ram_n, rn, sizeof(rn));
            LeaveCriticalSection(&cs);
            SetEvent(ram_event);
        }

        // adaptive pacing: full fps while animating, 5 Hz while the picture is static
        idle_frames = changed ? 0 : idle_frames + 1;
        LONGLONG ms = idle_frames > 10 ? 200 : 1000 / fps;
        LARGE_INTEGER due; due.QuadPart = -ms * 10000;
        SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(timer, INFINITE);
    }
    CloseHandle(timer);
    return 0;
}

// ---- tray / UI
static void update_tip(void) {
    swprintf(nid.szTip, 128, L"haku control — %s, %d%%", effect_title(cur_effect), (int)(brightness * 100 + 0.5f));
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void set_effect(int fx) {
    EnterCriticalSection(&cs);
    if (fx != cur_effect && fx == effect_index("off")) prev_effect = cur_effect;
    cur_effect = fx; effects_reset();
    LeaveCriticalSection(&cs);
    cfg_set_and_save("general", "effect", g_effects[fx].id);
    update_tip();
    ui_refresh();
}

static void set_brightness_ex(float b, int save_now) {
    b = clampf(b, 0.05f, 1);
    EnterCriticalSection(&cs); brightness = b; LeaveCriticalSection(&cs);
    char v[16]; snprintf(v, sizeof(v), "%d", (int)(b * 100 + 0.5f));
    cfg_set("general", "brightness", v);
    if (save_now) cfg_save_if_dirty();
    update_tip();
}

static void set_brightness(float b) { set_brightness_ex(b, 1); ui_refresh(); }

// ---- interface for the settings window (ui_web.cpp); all called on the UI thread
void  app_set_effect(int fx) { set_effect(fx); }
void  app_set_brightness(float b, int save_now) { set_brightness_ex(b, save_now); }
void  app_config_changed(int layout) {
    InterlockedIncrement(&cfg_gen);
    EnterCriticalSection(&cs); effects_reset(); LeaveCriticalSection(&cs);
    if (layout) build_scene();
}

static int effect_off_index(void) { return effect_index("off"); }

void app_toggle_device(const char *key) {
    cfg_set_and_save("layout", key, cfg_geti("layout", key, 1) ? "0" : "1");
    build_scene();
    EnterCriticalSection(&cs); effects_reset(); LeaveCriticalSection(&cs);
    ui_refresh();
}

// ---- window API (ui_web.cpp)
static void open_in_editor(const wchar_t *file);
static HICON make_icon(void);
static int jesc(char *out, int cap, const char *s) {
    int n = 0;
    for (; *s && n < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static int pawnio_installed(void) {
    wchar_t p[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", p, MAX_PATH);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static int status_body(char *out, int cap) {
    char gt[16] = "null";
    if (!isnan(last_sensors.gpu_temp)) snprintf(gt, sizeof(gt), "%d", (int)last_sensors.gpu_temp);
    int n = snprintf(out, cap, "\"effect\":\"%s\",\"brightness\":%d,\"msi\":%d,\"sticks\":%d,\"gpu_temp\":%s,\"hotspot\":%d,\"pawnio\":%d,\"bulbs\":[",
                     g_effects[cur_effect].id, (int)(brightness * 100 + 0.5f), have_msi, have_ene ? ene_count() : 0, gt, hotspot_active(), pawnio_installed());
    for (int i = 0; i < lights_count() && n < cap - 300; i++) {
        char nm[200]; jesc(nm, sizeof(nm), lights_name(i));
        n += snprintf(out + n, cap - n, "%s{\"name\":\"%s\",\"online\":%d,\"ip\":\"%s\"}", i ? "," : "", nm, lights_is_online(i), lights_ip(i));
    }
    n += snprintf(out + n, cap - n, "],\"nano\":");
    n += nano_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"ext\":");
    n += ext_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"remote\":");
    n += remote_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"update\":");
    n += update_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"accounts\":");
    n += accounts_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"mood\":");
    n += mood_json(out + n, cap - n);
    return n;
}

int app_status_json(char *out, int cap) {
    int n = snprintf(out, cap, "{\"type\":\"status\",");
    n += status_body(out + n, cap - n);
    n += snprintf(out + n, cap - n, "}");
    return n;
}

int app_state_json(char *out, int cap) {
    int n = snprintf(out, cap, "{\"type\":\"state\",\"autostart\":%d,\"effects\":[", app_autostart(-1));
    for (int i = 0; i < g_effect_count; i++) {
        char t[128]; WideCharToMultiByte(CP_UTF8, 0, effect_title(i), -1, t, sizeof(t), NULL, NULL);
        n += snprintf(out + n, cap - n, "%s{\"id\":\"%s\",\"title\":\"%s\"}", i ? "," : "", g_effects[i].id, t);
    }
    n += snprintf(out + n, cap - n, "],");
    n += status_body(out + n, cap - n);
    n += snprintf(out + n, cap - n, ",\"cfg\":");
    n += cfg_json(out + n, cap - n);
    n += snprintf(out + n, cap - n, "}");
    return n;
}

// Live colours for the window: [dev, index, "rrggbb"]; LAN devices come as dev 100 + slot and are
// thinned out to at most 60 samples each (index = sample number).
int app_frame_json(char *out, int cap) {
    static scene_t sc; static rgbf c[MAX_LEDS];
    EnterCriticalSection(&cs);
    sc.count = frame_sc.count; memcpy(sc.leds, frame_sc.leds, sc.count * sizeof(led_t)); memcpy(c, frame_c, sc.count * sizeof(rgbf));
    LeaveCriticalSection(&cs);
    int per[EXT_SLOTS] = { 0 };
    for (int i = 0; i < sc.count; i++) if (sc.leds[i].dev == DEV_EXT) { int k = sc.leds[i].zone - ZONE_EXT0; if (k >= 0 && k < EXT_SLOTS) per[k]++; }
    int n = snprintf(out, cap, "{\"type\":\"frame\",\"l\":[");
    int first = 1;
    for (int i = 0; i < sc.count && n < cap - 32; i++) {
        int dev = sc.leds[i].dev, idx = sc.leds[i].index;
        if (dev == DEV_EXT) {
            int k = sc.leds[i].zone - ZONE_EXT0, total = per[k];
            if (total > 60) {
                int step = (total + 59) / 60;
                if (idx % step) continue;
                idx /= step;
            }
            dev = 100 + k;
        } else if (dev == DEV_NANO) dev = 200 + sc.leds[i].zone - ZONE_NANO0;
        n += snprintf(out + n, cap - n, "%s[%d,%d,\"%02x%02x%02x\"]", first ? "" : ",", dev, idx,
                      (int)(clampf(c[i].r, 0, 1) * 255 + .5f), (int)(clampf(c[i].g, 0, 1) * 255 + .5f), (int)(clampf(c[i].b, 0, 1) * 255 + .5f));
        first = 0;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

void app_set(const char *s, const char *k, const char *v) {
    cfg_set(s, k, v);
    SetTimer(hwnd, SAVE_TIMER, 500, NULL);
    if (!_stricmp(s, "general") && !_stricmp(k, "fps")) { int f = atoi(v); fps = f < 5 ? 5 : f > 60 ? 60 : f; }
    if (!_stricmp(s, "hotkeys") || !_stricmp(s, "general")) PostMessageW(hwnd, WM_REHOTKEY, 0, 0);   // also refreshes the tray tip (language)
    if (!_strnicmp(s, "nanoleaf", 8) && (!_stricmp(k, "rotate") || !_stricmp(k, "flip"))) nano_relayout();
    if (!_strnicmp(s, "dev.", 4)) { cfg_save_if_dirty(); ext_reload(); }
    if (!_stricmp(s, "remote")) { cfg_save_if_dirty(); remote_apply(); }
    app_config_changed(!_stricmp(s, "layout") || !_stricmp(s, "calibration"));
}

int app_ru(void) { return !_stricmp(cfg_get("general", "lang", "en"), "ru"); }

void app_power(void) {
    int off = effect_index("off");
    set_effect(cur_effect == off ? prev_effect : off);
}

void app_open(const char *what) {
    wchar_t p[MAX_PATH];
    if (!strcmp(what, "log")) { app_data_path(L"haku-control.log", p); open_in_editor(p); }
    else if (!strcmp(what, "ini")) open_in_editor(cfg_path());
    else if (!strcmp(what, "folder")) ShellExecuteW(NULL, L"open", data_dir, NULL, NULL, SW_SHOWNORMAL);
    else if (!strcmp(what, "release")) update_open_page();
}

void app_remote_cmd(const char *json) {
    DWORD_PTR r;
    SendMessageTimeoutW(hwnd, WM_REMOTE_CMD, 0, (LPARAM)json, SMTO_BLOCK, 3000, &r);
}

void app_save_soon(void) { SetTimer(hwnd, SAVE_TIMER, 500, NULL); }

void app_quit(void) { PostMessageW(hwnd, WM_COMMAND, 1203 /* ID_EXIT */, 0); }

// Window icons: the transparent mark. Small = title bar (always dark) -> white;
// big = taskbar button / Alt+Tab -> white on a dark taskbar, black on a light one.
static int light_taskbar(void);
HICON app_icon(int big) {
    int id = big && light_taskbar() ? 3 : 2;
    return (HICON)LoadImageW(app_inst, MAKEINTRESOURCEW(id), IMAGE_ICON, GetSystemMetrics(big ? SM_CXICON : SM_CXSMICON),
                             GetSystemMetrics(big ? SM_CYICON : SM_CYSMICON), LR_SHARED);
}

// The logon task lives in %windir%\System32\Tasks\haku-control (UTF-16 XML); schtasks switches it.
int app_autostart(int set) {
    wchar_t xml[MAX_PATH];
    ExpandEnvironmentStringsW(L"%windir%\\System32\\Tasks\\" APP_ID, xml, MAX_PATH);
    if (set >= 0) {
        wchar_t cmd[128];
        swprintf(cmd, 128, L"schtasks.exe /Change /TN " APP_ID L" /%s", set ? L"ENABLE" : L"DISABLE");
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
    FILE *f = _wfopen(xml, L"rb");
    if (!f) return -1;
    static wchar_t buf[16384];
    size_t n = fread(buf, 2, 16383, f); fclose(f); buf[n] = 0;
    const wchar_t *st = wcsstr(buf, L"<Settings>"), *en = st ? wcsstr(st, L"</Settings>") : NULL;
    const wchar_t *dis = st ? wcsstr(st, L"<Enabled>false</Enabled>") : NULL;
    return !(dis && (!en || dis < en));
}

static void show_menu(void) {
    HMENU m = CreatePopupMenu(), mb = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_WINDOW, TR(L"Open haku control…", L"Открыть haku control…"));
    SetMenuDefaultItem(m, ID_WINDOW, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_effect_count; i++) {
        if (i == effect_off_index()) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (i == cur_effect ? MF_CHECKED : 0), ID_FX_BASE + i, effect_title(i));
    }
    static const int levels[] = { 10, 25, 50, 75, 100 };
    for (int i = 0; i < 5; i++) {
        wchar_t s[16]; swprintf(s, 16, L"%d%%", levels[i]);
        AppendMenuW(mb, MF_STRING | ((int)(brightness * 100 + 0.5f) == levels[i] ? MF_CHECKED : 0), ID_BRIGHT + levels[i], s);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mb, TR(L"Brightness", L"Яркость"));
    AppendMenuW(m, MF_STRING | (ram_on ? MF_CHECKED : 0), ID_RAM_ON, TR(L"Memory lighting", L"Подсветка памяти"));
    AppendMenuW(m, MF_STRING | (gpu_on ? MF_CHECKED : 0), ID_GPU_ON, TR(L"ARGB strip lighting", L"Подсветка ARGB-ленты"));
    if (lights_count()) AppendMenuW(m, MF_STRING | (lights_on ? MF_CHECKED : 0), ID_LIGHTS_ON, TR(L"AiDot bulbs", L"Лампочки AiDot"));
    for (int k = 0; k < NANO_MAX; k++) if (nano_present(k)) {
        char t[64]; wchar_t w[64]; nano_title(k, t, sizeof(t));
        MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 64);
        AppendMenuW(m, MF_STRING | (nano_on[k] ? MF_CHECKED : 0), ID_NANO_ON + k, w);
    }
    AppendMenuW(m, MF_STRING, ID_SETTINGS, TR(L"Settings file", L"Файл настроек"));
    AppendMenuW(m, MF_STRING, ID_LOG, TR(L"Log", L"Журнал"));
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, TR(L"Quit", L"Выход"));
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

static int parse_hotkey(const char *s, UINT *mods, UINT *vk) {
    *mods = MOD_NOREPEAT; *vk = 0;
    char buf[64]; strcpy_s(buf, sizeof(buf), s);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        while (*tok == ' ') tok++;
        char *e = tok + strlen(tok); while (e > tok && e[-1] == ' ') *--e = 0;
        if (!_stricmp(tok, "Ctrl")) *mods |= MOD_CONTROL;
        else if (!_stricmp(tok, "Alt")) *mods |= MOD_ALT;
        else if (!_stricmp(tok, "Shift")) *mods |= MOD_SHIFT;
        else if (!_stricmp(tok, "Win")) *mods |= MOD_WIN;
        else if (!_stricmp(tok, "Left")) *vk = VK_LEFT;
        else if (!_stricmp(tok, "Right")) *vk = VK_RIGHT;
        else if (!_stricmp(tok, "Up")) *vk = VK_UP;
        else if (!_stricmp(tok, "Down")) *vk = VK_DOWN;
        else if (!_stricmp(tok, "PageUp")) *vk = VK_PRIOR;
        else if (!_stricmp(tok, "PageDown")) *vk = VK_NEXT;
        else if (!_stricmp(tok, "Home")) *vk = VK_HOME;
        else if (!_stricmp(tok, "End")) *vk = VK_END;
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 24) *vk = VK_F1 + atoi(tok + 1) - 1;
        else if (strlen(tok) == 1 && isalnum((unsigned char)tok[0])) *vk = (UINT)toupper((unsigned char)tok[0]);
    }
    return *vk != 0;
}

static void register_hotkeys(void) {
    static const struct { int id; const char *key; } hk[] = {
        { HK_NEXT, "next" }, { HK_PREV, "prev" }, { HK_OFF, "off" }, { HK_BUP, "brighter" }, { HK_BDOWN, "dimmer" } };
    for (int i = 0; i < 5; i++) {
        UnregisterHotKey(hwnd, hk[i].id);
        UINT mods, vk;
        const char *v = cfg_get("hotkeys", hk[i].key, NULL);
        if (v && parse_hotkey(v, &mods, &vk) && !RegisterHotKey(hwnd, hk[i].id, mods, vk))
            logf_("hotkey %s (%s) is taken by another program", hk[i].key, v);
    }
}

static void open_in_editor(const wchar_t *file) {
    ShellExecuteW(NULL, L"open", L"notepad.exe", file, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbar_created, quit_msg, show_msg;
    switch (msg) {
    case WM_CREATE:
        taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        // lets a non-elevated script stop us cleanly (restoring the hardware effects)
        quit_msg = RegisterWindowMessageW(L"haku_control_quit");
        ChangeWindowMessageFilterEx(h, quit_msg, MSGFLT_ALLOW, NULL);
        show_msg = RegisterWindowMessageW(L"haku_control_show");   // second launch opens the settings window
        ChangeWindowMessageFilterEx(h, show_msg, MSGFLT_ALLOW, NULL);
        return 0;
    case WM_TIMER:
        if (wp == SAVE_TIMER) { KillTimer(h, SAVE_TIMER); cfg_save_if_dirty(); }
        return 0;
    case WM_SETTINGCHANGE:
        // light / dark taskbar switched: swap the tray icon
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            HICON old = tray_icon;
            nid.hIcon = tray_icon = make_icon(); nid.uFlags = NIF_ICON;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            if (old) DestroyIcon(old);
        }
        return 0;
    case WM_REMOTE_CMD:
        ui_dispatch((const char *)lp);
        return 0;
    case WM_REHOTKEY:
        register_hotkeys();
        update_tip();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ui_open(app_inst);
        else if (LOWORD(lp) == WM_RBUTTONUP) show_menu();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_FX_BASE && id < ID_FX_BASE + g_effect_count) set_effect(id - ID_FX_BASE);
        else if (id > ID_BRIGHT && id <= ID_BRIGHT + 100) set_brightness((id - ID_BRIGHT) / 100.0f);
        else if (id == ID_WINDOW) ui_open(app_inst);
        else if (id == ID_RAM_ON) app_toggle_device("ram_enabled");
        else if (id == ID_GPU_ON) app_toggle_device("gpu_enabled");
        else if (id == ID_LIGHTS_ON) app_toggle_device("lights_enabled");
        else if (id >= ID_NANO_ON && id < ID_NANO_ON + NANO_MAX) { char key[32]; nano_key(id - ID_NANO_ON, key, sizeof(key)); app_toggle_device(key); }
        else if (id == ID_SETTINGS) open_in_editor(cfg_path());
        else if (id == ID_LOG) app_open("log");
        else if (id == ID_EXIT) DestroyWindow(h);
        return 0;
    }
    case WM_HOTKEY: {
        int n = g_effect_count - 1;   // cycle through everything except "off"
        int off = effect_off_index();
        switch (wp) {
        case HK_NEXT: set_effect(cur_effect >= n - 1 || cur_effect == off ? 0 : cur_effect + 1); break;
        case HK_PREV: set_effect(cur_effect == 0 || cur_effect == off ? n - 1 : cur_effect - 1); break;
        case HK_OFF:  set_effect(cur_effect == off ? prev_effect : off); break;
        case HK_BUP:  set_brightness(brightness + 0.1f); break;
        case HK_BDOWN: set_brightness(brightness - 0.1f); break;
        }
        return 0;
    }
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) { logf_("sleep"); nano_suspend(1); lights_suspend(1); ext_suspend(1); }
        if (wp == PBT_APMRESUMEAUTOMATIC) { InterlockedExchange(&need_reinit, 1); logf_("resume"); }
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) { nano_suspend(0); lights_suspend(0); ext_suspend(0); }
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        // shutdown / log off: room lights get their exit action while the network is still up
        if (wp) { running = 0; SetEvent(ram_event); WaitForSingleObject(th_render, 2000); WaitForSingleObject(th_ram, 2000); close_devices(1); nano_stop(); lights_stop(); ext_stop(); }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == quit_msg && quit_msg) { DestroyWindow(h); return 0; }
    if (msg == show_msg && show_msg) { ui_open(app_inst); return 0; }
    if (msg == taskbar_created && taskbar_created) {   // explorer restarted
        Shell_NotifyIconW(NIM_ADD, &nid);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Tray icon: the haku control mark (res/), white on a dark taskbar, black on a light one.
static int light_taskbar(void) {
    DWORD v = 0, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS && v;
}

static HICON make_icon(void) {
    return (HICON)LoadImageW(app_inst, MAKEINTRESOURCEW(light_taskbar() ? 3 : 2), IMAGE_ICON,
                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    app_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\" APP_ID L"_single_instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(APP_ID, APP_ID);
        if (other) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(other, RegisterWindowMessageW(L"haku_control_show"), 0, 0); }
        return 0;
    }

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t *s = wcsrchr(exe_dir, L'\\'); if (s) s[1] = 0;
    // settings, keys, log and backups live in %APPDATA%\haku-control (the install folder stays read-only)
    ExpandEnvironmentStringsW(L"%APPDATA%\\" APP_ID L"\\", data_dir, MAX_PATH);
    CreateDirectoryW(data_dir, NULL);
    wchar_t p[MAX_PATH];
    app_data_path(L"haku-control.log", p); logfile = _wfopen(p, L"w");
    app_data_path(L"settings.ini", p);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = _wfopen(p, L"wb"); if (f) { fwrite(default_ini, 1, sizeof(default_ini) - 1, f); fclose(f); }
    }

    InitializeCriticalSection(&cs);
    timeBeginPeriod(1);   // 1 ms timer: SMBus waits and frame pacing
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    cfg_load(p);
    open_devices();
    lights_start();
    nano_start();
    ext_start();
    remote_apply();
    update_start();
    hotspot_watch_start();
    sensors_init();
    load_config();
    logf_("started: msi=%d ene=%d sticks=%d effect=%s", have_msi, have_ene, ene_count(), g_effects[cur_effect].id);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = APP_ID;
    RegisterClassW(&wc);
    hwnd = CreateWindowW(APP_ID, APP_ID, 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);

    nid.cbSize = sizeof(nid); nid.hWnd = hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = tray_icon = make_icon();
    Shell_NotifyIconW(NIM_ADD, &nid);
    update_tip();
    register_hotkeys();
    if (wcsstr(GetCommandLineW(), L"--settings")) ui_open(inst);
    if (wcsstr(GetCommandLineW(), L"--scan")) ext_scan();   // look for LAN lights right away (results in the log)

    ram_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    th_ram = (HANDLE)_beginthreadex(NULL, 0, ram_thread, NULL, 0, NULL);
    th_render = (HANDLE)_beginthreadex(NULL, 0, render_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (ui_is_dialog_message(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    running = 0;
    SetEvent(ram_event);
    WaitForSingleObject(th_render, 3000);
    WaitForSingleObject(th_ram, 3000);
    close_devices(1);
    lights_stop();
    nano_stop();
    ext_stop();
    remote_stop();
    update_stop();
    hotspot_watch_stop();
    sensors_close();
    timeEndPeriod(1);
    logf_("exit");
    if (logfile) fclose(logfile);
    CloseHandle(single);
    return 0;
}
