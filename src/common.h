// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAX_LEDS     2048   // whole scene (LAN strips can be long)
#define EXT_SLOTS    16     // LAN / bridge devices (devices.c)
#define MAX_PALETTE  8
#define NANO_MAX     8      // Nanoleaf controllers

typedef struct { float r, g, b; } rgbf;

typedef enum { DEV_RAM0, DEV_RAM1, DEV_GPU, DEV_BOARD, DEV_LIGHT, DEV_NANO, DEV_EXT, DEV_COUNT } dev_id;   // DEV_EXT: slot = zone - ZONE_EXT0, DEV_NANO: zone - ZONE_NANO0

// One physical LED with its position in a shared "scene" space.
//   x, y  : 0..1 (y = 0 at top)
//   path  : 0..1 position along the chain RAM0 -> RAM1 -> GPU (used by flowing effects)
//   fill  : 0..1 position inside its own device (bottom->top for RAM, left->right for GPU)
// Colour zones: each can follow the effect, use its own palette, or hold a fixed colour.
enum { ZONE_RAM, ZONE_GPU, ZONE_NANO0, ZONE_LIGHT0 = ZONE_NANO0 + NANO_MAX, ZONE_EXT0 = ZONE_LIGHT0 + 8, MAX_ZONES = ZONE_EXT0 + EXT_SLOTS };

typedef struct {
    dev_id dev;
    int    index;
    int    zone;
    float  x, y, path, fill;
    float  zpath;      // 0..1 position inside its own zone (for zones running their own effect)
} led_t;

typedef struct {
    led_t leds[MAX_LEDS];
    int   count;
} scene_t;

// Live sensor values, NAN when unavailable.
typedef struct {
    float gpu_temp;
    float water_temp;
    float flow;        // l/h or pump rpm fraction 0..1
    float audio_level; // 0..1
    float audio_bass;  // 0..1
} sensors_t;

// ---- config.c
void        cfg_load(const wchar_t *path);
int         cfg_changed_on_disk(void);
const char *cfg_get(const char *section, const char *key, const char *def);
float       cfg_getf(const char *section, const char *key, float def);
int         cfg_geti(const char *section, const char *key, int def);
void        cfg_set(const char *section, const char *key, const char *value);
void        cfg_save_if_dirty(void);
void        cfg_set_and_save(const char *section, const char *key, const char *value);
int         cfg_palette(const char *section, rgbf *out, int max);
const wchar_t *cfg_path(void);
int         cfg_json(char *out, int cap);   // whole config as {"section":{"key":"value"}}
void        cfg_remove_section(const char *section);
typedef struct { char key[64], val[256]; } cfg_item;
int         cfg_items(const char *section, cfg_item *out, int max);      // a section's keys, copied
int         cfg_sections(const char *prefix, char (*out)[64], int max);  // section names starting with prefix

// ---- effects.c
typedef struct {
    const char    *id;
    const wchar_t *title;      // English
    const wchar_t *title_ru;
} effect_info;

extern const effect_info g_effects[];
extern const int         g_effect_count;
int  effect_index(const char *id);
const wchar_t *effect_title(int i);   // in the chosen language
void effects_render(int effect, const scene_t *sc, const sensors_t *sn, double dt, rgbf *out);
void effects_reset(void);
int  effects_bake(int effect, const scene_t *sc, int dev, int zone, int max_frames, int stride, rgbf *out, float *step);   // a device's loop, see effects.c
void zone_section(int zone, char *out, int cap);
int  effects_need_gpu_temp(void);
int  effects_need_audio(void);
int  effects_zone_white(int zone, int *kelvin);      // 1 if the zone is in "white light" mode   // "zone.ram", "zone.gpu", "zone.light1"...

// ---- dev_msi.c (MSI Mystic Light: onboard LED + JRAINBOW1 ARGB header)
int  msi_open(void);
int  msi_send(const rgbf *gpu, int n, const rgbf *board);
void msi_restore(void);
void msi_close(void);

// ---- dev_ene.c (ENE DRAM RGB via PawnIO)
int  ene_open(void);
int  ene_count(void);
int  ene_send(int stick, const rgbf *c, int n);
void ene_restore(void);
void ene_close(void);

// ---- dev_aidot.c (AiDot Wi-Fi bulbs, local LAN protocol, own thread)
int  lights_start(void);
int  lights_count(void);
int  lights_online(void);
const char *lights_name(int i);
void lights_submit(const rgbf *c, const int *kelvin, int n, int enable);   // kelvin[i] > 0: use the white LEDs
void lights_stop(void);
int  lights_is_online(int i);
void lights_suspend(int sleeping);   // PC sleep / resume
int  lights_changed(void);        // 1 once after the key file was reloaded
const char *lights_ip(int i);

// ---- dev_nanoleaf.c (Nanoleaf controllers, local OpenAPI + UDP extControl, a thread each)
// k = slot - 1 (0..NANO_MAX-1): the first controller is [nanoleaf] / zone.nanoleaf, the others [nanoleafN] / zone.nanoleafN
int  nano_start(void);
int  nano_configured(void);       // any controller paired
int  nano_present(int k);
int  nano_count(int k);
int  nano_layout_changed(void);   // 1 once after a panel layout was (re)read or a controller came / went
void nano_panel(int k, int i, float *x, float *y, float *path);
void nano_title(int k, char *out, int cap);
void nano_submit(int k, const rgbf *c, int n, int enable);
#define NANO_MAX_PANELS 64
#define NANO_MAX_FRAMES 60
int  nano_on_device(int k);       // effects play on the panels themselves ([nanoleaf] mode != stream, custom animations work)
int  nano_bake_wanted(int k);     // 1 once when the panels need their animation (again)
void nano_upload(int k, const rgbf *frames, int nframes, int npanels, float step);   // frames[f * NANO_MAX_PANELS + panel]; 0 frames: stream
void nano_stop(void);
void nano_pair_start(void);       // find a new controller and wait for its power button (runs in background)
void nano_pair_start_ip(const char *ip);   // the same with one address (Add by address)
void nano_forget(int slot);
void nano_relayout(void);         // re-read the layouts (after rotate / flip changed)
void nano_suspend(int sleeping);  // PC sleep / resume
int  nano_json(char *out, int cap);

// ---- devices.c (LAN / bridge lights: WLED, OpenRGB, Govee, LIFX, Yeelight, Hue; own worker thread)
void ext_start(void);
void ext_stop(void);
void ext_reload(void);                    // [dev.*] settings changed
void ext_suspend(int sleeping);
int  ext_count(void);                     // configured devices ("slots")
int  ext_slot_leds(int k);                // LEDs of slot k in the scene (0: disabled / not known yet)
int  ext_slot_id(int k);                  // N of its [dev.N] section
int  ext_slot_strip(int k);               // 1: addressable strip, 0: separate lights
void ext_slot_type(int k, char *out, int cap);   // strip, tv, bars, floor, lamp, panels, bulb
int  ext_layout_changed(void);            // 1 once after a device appeared / changed its LED count
void ext_submit(int k, const rgbf *c, int n);
void ext_scan(void);                      // look for devices on the LAN (background)
int  ext_add(const char *kind, const char *host, int sub, const char *name, int leds);
void ext_remove(int id);
int  ext_json(char *out, int cap);

// ---- remote.c (phone / LAN control over HTTP, [remote] enabled=1)
void remote_apply(void);          // start / stop / move to the configured port
void remote_stop(void);
void remote_forget(void);         // drop all paired devices
void remote_new_pin(void);
int  remote_json(char *out, int cap);
void app_remote_cmd(const char *json);   // runs a window command on the UI thread (main.c)
void ui_dispatch(const char *json);      // ui_web.cpp: the window's command handler

// ---- update.c (daily check for a newer GitHub release, [general] update_check)
void update_start(void);
void update_stop(void);
void update_check_now(void);
void update_open_page(void);
int  policy_blocked(char *msg, int mcap, char *url, int ucap);   // this version stopped by its author (update.c)
void app_blocked(const char *msg, const char *url);               // (main.c) let go of the lights, say so, quit
int  update_json(char *out, int cap);

// ---- mood.c (describe a mood -> palette, effect, speed through a local Ollama model)
void mood_request(const char *text, int again);
int  mood_json(char *out, int cap);
// ---- ollama.c (the local model for the mood: started only for requests, set up from the window)
int  ollama_acquire(void);          // 1: Ollama answers (started if needed); then ollama_release()
void ollama_release(void);
int  ollama_installed(void);
void ollama_init(void);             // at start: keeps it out of Windows startup if [mood] ollama_on_demand=1
void ollama_set_on_demand(int on);
void ollama_setup(void);            // installs Ollama (if missing) and pulls the model, in the background
int  ollama_json(char *out, int cap);

// ---- accounts.c (one-time vendor sign-ins that hand out local-control keys)
void accounts_aidot_login(const char *country, const char *email, const char *password);
void accounts_tuya_login(const char *region, const char *access_id, const char *secret);
void accounts_govee_login(const char *api_key);
int  acc_https(const char *section, const char *method, const char *host, const char *path, const char *headers,
               const char *body, char *out, int cap);   // WinHTTP; returns the status (0: no answer)
int  accounts_json(char *out, int cap);

// ---- net.c
int  net_broadcasts(unsigned long *out, int max);   // directed broadcast address of every IPv4 interface (network order)
int  net_addresses(unsigned long *out, int max);    // own IPv4 address of every interface (network order)
int  net_phone_addresses(unsigned long *out, int max);   // the ones a phone can reach, best first (no VM / WSL adapters)
void hotspot_watch_start(void);
void hotspot_watch_stop(void);
int  hotspot_active(void);

// ---- sensors.c
void sensors_init(void);
void sensors_poll(sensors_t *s, int need_gpu, int need_audio);

// ---- audio.cpp (WASAPI loopback, own thread while needed)
void audio_start(void);
void audio_stop(void);
void audio_read(float *level, float *bass);
void sensors_close(void);

// ---- main.c: application state shared with the settings window
#ifdef HAKU_DEV   // build.cmd dev: separate instance for testing, no PC hardware access, no admin rights
#define APP_ID L"haku-control-dev"
#else
#define APP_ID L"haku-control"            // exe, data folder, logon task, window class
#endif
void  app_data_path(const wchar_t *name, wchar_t *out);   // %APPDATA%\haku-control\<name> (MAX_PATH)
int   app_ru(void);                      // [general] lang=ru (English otherwise)
#define TR(en, ru) (app_ru() ? (ru) : (en))
void  app_set_effect(int fx);
#define PRESET_MAX 32
int   app_preset_save(int id, const char *name, const char *effect, const char *palette, const char *speed,
                      const char *brightness, const char *zones);   // id 0: new; returns its number, see main.c
void  app_preset_apply(int id);
void  app_preset_delete(int id);
#define PROFILE_MAX 16
int   app_profile_save(int id, const char *name);   // id 0: new, from the setup as it is now; see main.c
void  app_profile_apply(int id);
void  app_profile_delete(int id);
void  ui_refresh_state(void);   // the open window gets the whole state (settings included) again
void  app_set_brightness(float b, int save_now);
void  app_config_changed(int layout);   // after cfg_set: re-read params (and rebuild LED layout)
void  app_toggle_device(const char *layout_key);   // ram_enabled / gpu_enabled
// settings window (ui_web.cpp) <-> core
int   app_state_json(char *out, int cap);    // effects, config, devices
int   app_status_json(char *out, int cap);   // devices only (polled)
void  update_install(void);                 // download, check and run the latest release's installer (update.c)
void  diag_save(void);                      // diagnostics report to Downloads, in the background (diag.c)
int   diag_json(char *out, int cap);
void  orgb_check_start(void);          // looks for OpenRGB on this PC (drv_openrgb.c), the page is told
int   orgb_json(char *out, int cap);
int   hw_inventory(char *out, int cap);  // the PC hardware as text lines, for the diagnostics (hwinfo.c)
int   app_frame_json(char *out, int cap);    // live LED colours
void  app_set(const char *section, const char *key, const char *value);
void  app_power(void);                       // off <-> previous effect
void  app_open(const char *what);            // "log", "ini", "folder"
void  app_quit(void);
void  app_save_soon(void);                   // write settings.ini after a short pause
int   app_autostart(int set);                // -1: query; 0/1: disable / enable the logon task
HICON app_icon(int big);

// ---- ui_web.cpp: settings window, exists only while open
void  ui_open(HINSTANCE inst);
void  ui_refresh(void);                 // effect/brightness changed elsewhere (hotkey, tray)
int   ui_is_dialog_message(MSG *m);

// ---- log
void logf_(const char *fmt, ...);

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float fractf(float v) { return v - floorf(v); }
static inline rgbf mixc(rgbf a, rgbf b, float t) { rgbf r = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; return r; }
static inline rgbf scalec(rgbf a, float k) { rgbf r = { a.r * k, a.g * k, a.b * k }; return r; }
static inline rgbf addc(rgbf a, rgbf b) { rgbf r = { a.r + b.r, a.g + b.g, a.b + b.b }; return r; }
