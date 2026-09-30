// SPDX-License-Identifier: GPL-3.0-only
// Effects. Every effect is a pure function of (led position, time, sensors, palette),
// except bubbles which keeps a tiny particle list.
// Each colour zone (RAM, GPU block, Nanoleaf, every bulb) either follows the main effect or runs
// its own one ([zone.*] effect=...). With [general] sync=1 the main effect spans all devices as one
// chain; zones on their own effect, or everything with sync=0, run on their own position and clock.
#include "common.h"
#include <stdlib.h>

const effect_info g_effects[] = {
    { "flow",        L"Flow",         L"Течение",         L"Flux" },
    { "caustic",     L"Caustics",     L"Каустика",        L"Caustiques" },
    { "bubbles",     L"Bubbles",      L"Пузырьки",        L"Bulles" },
    { "comet",       L"Comet",        L"Комета",          L"Comète" },
    { "lava",        L"Lava",         L"Лава",            L"Lave" },
    { "breathe",     L"Breathe",      L"Дыхание",         L"Respiration" },
    { "temperature", L"Temperature",  L"Температура",     L"Température" },
    { "pump",        L"Pump flow",    L"Поток по насосу", L"Flux de la pompe" },
    { "audio",       L"Audio",        L"Звук",            L"Audio" },
    { "static",      L"Static colour", L"Статичный цвет", L"Couleur fixe" },
    { "off",         L"Off",          L"Выключить",       L"Éteindre" },
};
const int g_effect_count = sizeof(g_effects) / sizeof(g_effects[0]);

enum { FX_FLOW, FX_CAUSTIC, FX_BUBBLES, FX_COMET, FX_LAVA, FX_BREATHE, FX_TEMP, FX_PUMP, FX_AUDIO, FX_STATIC, FX_OFF, FX_N };

const wchar_t *effect_title(int i) { return TR(g_effects[i].title, g_effects[i].title_ru, g_effects[i].title_fr); }

int effect_index(const char *id) {
    for (int i = 0; i < g_effect_count; i++) if (_stricmp(g_effects[i].id, id) == 0) return i;
    return 0;
}

// ---- parameters (re-read from config on reset)
static rgbf  fx_pal[FX_N][MAX_PALETTE];   // every effect's own palette
static int   fx_npal[FX_N];
static float fx_speed[FX_N];              // 1.0 = default
static double fx_t[FX_N];                 // effect clocks, already scaled by speed
static const rgbf *pal;                   // palette used for the LED being rendered
static int   npal;

enum { ZMODE_EFFECT, ZMODE_PALETTE, ZMODE_STATIC, ZMODE_WHITE };
static int   zmode[MAX_ZONES];
static int   zeffect[MAX_ZONES];          // -1: follow the main effect
static int   zkelvin[MAX_ZONES];
static float zlevel[MAX_ZONES];           // per-zone brightness 0..1
static rgbf  zpal[MAX_ZONES][MAX_PALETTE];
static int   znpal[MAX_ZONES];
static int   sync_all = 1;
static int   last_fx = -1;
static int   used_temp, used_audio;       // some LED shows the temperature / audio effect

// Colour temperature -> RGB (Tanner Helland's approximation), for LEDs without a white channel.
static rgbf kelvin_rgb(int k) {
    float t = k / 100.0f, r, g, b;
    if (t <= 66) { r = 255; g = 99.47f * logf(t) - 161.12f; }
    else { r = 329.70f * powf(t - 60, -0.1332f); g = 288.12f * powf(t - 60, -0.0755f); }
    b = t >= 66 ? 255 : t <= 19 ? 0 : 138.52f * logf(t - 10) - 305.04f;
    rgbf c = { clampf(r, 0, 255) / 255, clampf(g, 0, 255) / 255, clampf(b, 0, 255) / 255 };
    return c;
}

static int zone_fx(int z) { return zeffect[z] >= 0 ? zeffect[z] : last_fx; }

int effects_zone_white(int zone, int *kelvin) {
    if (zone < 0 || zone >= MAX_ZONES || zmode[zone] != ZMODE_WHITE || last_fx == FX_OFF || zone_fx(zone) == FX_OFF) return 0;
    *kelvin = zkelvin[zone];
    return 1;
}

int effects_need_gpu_temp(void) { return used_temp; }
int effects_need_audio(void) { return used_audio; }

void zone_section(int zone, char *out, int cap) {
    if (zone == ZONE_RAM) snprintf(out, cap, "zone.ram");
    else if (zone == ZONE_GPU) snprintf(out, cap, "zone.gpu");
    else if (zone >= ZONE_NANO0 && zone < ZONE_NANO0 + NANO_MAX) { if (zone == ZONE_NANO0) snprintf(out, cap, "zone.nanoleaf"); else snprintf(out, cap, "zone.nanoleaf%d", zone - ZONE_NANO0 + 1); }
    else if (zone >= ZONE_EXT0) snprintf(out, cap, "zone.dev%d", ext_slot_id(zone - ZONE_EXT0));
    else snprintf(out, cap, "zone.light%d", zone - ZONE_LIGHT0 + 1);
}

static float param_cold, param_hot;
static int   temp_water;

typedef struct { float x, y, v, c; } bubble;
static bubble bub[64];
static int    nbub;
static float  temp_smooth = NAN, level_smooth, bass_smooth;
static int    loaded;

static void load_params(void) {
    for (int e = 0; e < FX_N; e++) {
        const char *sec = g_effects[e].id;
        fx_npal[e] = cfg_palette(sec, fx_pal[e], MAX_PALETTE);
        if (!fx_npal[e]) fx_npal[e] = cfg_palette("general", fx_pal[e], MAX_PALETTE);
        if (!fx_npal[e]) { rgbf d[3] = { { 0, .78f, 1 }, { .48f, .24f, 1 }, { 1, .18f, .58f } }; memcpy(fx_pal[e], d, sizeof(d)); fx_npal[e] = 3; }
        fx_speed[e] = cfg_getf(sec, "speed", cfg_getf("general", "speed", 5)) / 5.0f;
    }
    for (int z = 0; z < MAX_ZONES; z++) {
        char zs[32]; zone_section(z, zs, sizeof(zs));
        const char *m = cfg_get(zs, "mode", "effect");
        znpal[z] = cfg_palette(zs, zpal[z], MAX_PALETTE);
        zmode[z] = !_stricmp(m, "white") ? ZMODE_WHITE
                 : !znpal[z] ? ZMODE_EFFECT : !_stricmp(m, "static") ? ZMODE_STATIC : !_stricmp(m, "palette") ? ZMODE_PALETTE : ZMODE_EFFECT;
        zkelvin[z] = (int)clampf(cfg_getf(zs, "kelvin", 4000), 2700, 6500);
        zlevel[z]  = clampf(cfg_getf(zs, "brightness", 100) / 100.0f, 0.05f, 1);
        const char *e = cfg_get(zs, "effect", "");
        zeffect[z] = !e[0] || !_stricmp(e, "sync") ? -1 : effect_index(e);
    }
    sync_all   = cfg_geti("general", "sync", 1);
    param_cold = cfg_getf("temperature", "cold", 30.0f);
    param_hot  = cfg_getf("temperature", "hot", 75.0f);
    temp_water = _stricmp(cfg_get("temperature", "source", "gpu"), "water") == 0;
    loaded = 1;
}

void effects_reset(void) { loaded = 0; nbub = 0; }

// cyclic palette lookup
static rgbf palc(float p) {
    p = fractf(p) * npal;
    int i = (int)p; float f = p - i;
    return mixc(pal[i % npal], pal[(i + 1) % npal], f);
}

// clamped gradient lookup (first colour at 0, last at 1)
static rgbf grad(float p) {
    if (npal == 1) return pal[0];
    p = clampf(p, 0, 1) * (npal - 1);
    int i = (int)p; if (i >= npal - 1) return pal[npal - 1];
    return mixc(pal[i], pal[i + 1], p - i);
}

static float smooth01(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }

static float caustic_n(float x, float y, float t) {
    float n = 0.5f + 0.5f * sinf(x * 13 + t * 1.7f) * sinf(y * 9 - t * 1.3f + sinf(x * 5 + t));
    return n * n;
}

// One LED's colour for the effect clocks clk[] and a bubble list: the live ones, or local ones when baking ahead.
static rgbf led_color(int fx, const led_t *l, const double *clk, const bubble *bb, int nb) {
    float x = l->x, y = l->y;
    rgbf c = { 0, 0, 0 };
    int z = l->zone >= 0 && l->zone < MAX_ZONES ? l->zone : ZONE_GPU;
    int e = fx == FX_OFF ? FX_OFF : zone_fx(z);
    if (e != FX_OFF && zmode[z] == ZMODE_WHITE) return scalec(kelvin_rgb(zkelvin[z]), zlevel[z]);
    if (e != FX_OFF && zmode[z] == ZMODE_STATIC) return scalec(zpal[z][0], zlevel[z]);
    if (zmode[z] == ZMODE_PALETTE) { pal = zpal[z]; npal = znpal[z]; } else { pal = fx_pal[e]; npal = fx_npal[e]; }

    // own effect / no sync: position inside the device and a per-device clock offset
    int solo = zeffect[z] >= 0 || !sync_all;
    float path = solo ? l->zpath : l->path;
    float t = (float)clk[e] + (solo ? z * 2.37f : 0);

    switch (e) {
    case FX_FLOW:
    case FX_PUMP:
        c = palc(path - t * 0.12f);
        break;
    case FX_CAUSTIC:
        c = scalec(palc(x * 0.6f + t * 0.03f), 0.25f + 0.75f * caustic_n(x, y, t));
        break;
    case FX_BUBBLES: {
        c = scalec(pal[0], 0.12f);
        for (int k = 0; k < nb; k++) {
            if (fabsf(bb[k].x - x) > 0.01f) continue;
            float d = (bb[k].y - y) / (l->dev == DEV_GPU || l->dev == DEV_EXT ? 0.12f : l->dev == DEV_NANO ? 0.18f : 0.07f);
            c = addc(c, scalec(palc(bb[k].c), expf(-d * d)));
        }
        break;
    }
    case FX_COMET: {
        float head = fractf(t * 0.2f) * 1.35f, d = head - path;
        c = scalec(pal[npal > 2 ? 2 : npal - 1], 0.05f);
        if (d >= 0 && d < 0.3f) {
            float k = 1 - d / 0.3f;
            c = addc(c, scalec(mixc(pal[0], pal[npal > 1 ? 1 : 0], d / 0.3f), k * k));
        }
        break;
    }
    case FX_LAVA: {
        float v = sinf(x * 6 + t * 0.8f) + sinf(y * 5 - t * 0.6f) + sinf((x + y) * 4 + t * 0.4f);
        c = palc(v / 6 + t * 0.02f);
        break;
    }
    case FX_BREATHE: {
        // fades colour -> next colour -> ... (never through black)
        float ph = t * 0.35f;
        int k = (int)floorf(ph);
        c = mixc(pal[k % npal], pal[(k + 1) % npal], smooth01(ph - k));
        break;
    }
    case FX_TEMP: {
        float p = isnan(temp_smooth) ? 0 : (temp_smooth - param_cold) / (param_hot - param_cold);
        c = scalec(grad(p), 0.8f + 0.2f * caustic_n(x, y, t * 0.5f));
        break;
    }
    case FX_AUDIO: {
        // VU: RAM fills bottom->top, GPU from the centre outwards; bass lifts brightness
        float pos = l->dev == DEV_GPU || l->dev == DEV_EXT ? fabsf(l->fill - 0.5f) * 2 : l->fill;
        float lit = smooth01((level_smooth * 1.15f - pos) / 0.12f + 0.5f);
        c = scalec(grad(pos * 0.7f + bass_smooth * 0.3f), 0.1f + 0.9f * lit * (0.6f + 0.4f * bass_smooth));
        break;
    }
    case FX_STATIC:
        c = pal[0];
        break;
    case FX_OFF:
    default:
        break;
    }
    c = scalec(c, zlevel[z]);
    c.r = clampf(c.r, 0, 1); c.g = clampf(c.g, 0, 1); c.b = clampf(c.b, 0, 1);
    return c;
}

void effects_render(int fx, const scene_t *sc, const sensors_t *sn, double dt, rgbf *out) {
    if (!loaded || fx != last_fx) { load_params(); if (fx != last_fx) nbub = 0; }
    last_fx = fx;

    // which effects are on screen this frame
    int uses[FX_N] = { 0 }, bub_leds[MAX_LEDS], nbl = 0;
    for (int i = 0; i < sc->count; i++) {
        int z = sc->leds[i].zone, e = fx == FX_OFF ? FX_OFF : zone_fx(z);
        uses[e] = 1;
        if (e == FX_BUBBLES) bub_leds[nbl++] = i;
    }
    used_temp = uses[FX_TEMP];
    used_audio = uses[FX_AUDIO];

    // effect clocks; the pump effect follows the flow sensor (0..1) when there is one
    for (int e = 0; e < FX_N; e++) {
        float spd = fx_speed[e];
        if (e == FX_PUMP && !isnan(sn->flow)) spd = 0.3f + 2.2f * clampf(sn->flow, 0, 1);
        fx_t[e] += dt * spd;
    }

    if (uses[FX_TEMP]) {
        float v = temp_water ? sn->water_temp : sn->gpu_temp;
        if (!isnan(v)) temp_smooth = isnan(temp_smooth) ? v : temp_smooth + (v - temp_smooth) * clampf((float)dt * 0.8f, 0, 1);
    }
    if (uses[FX_AUDIO]) {
        float lv = isnan(sn->audio_level) ? 0 : sn->audio_level, bs = isnan(sn->audio_bass) ? 0 : sn->audio_bass;
        // fast attack, slow release
        level_smooth += (lv - level_smooth) * clampf((float)dt * (lv > level_smooth ? 25.0f : 4.0f), 0, 1);
        bass_smooth  += (bs - bass_smooth)  * clampf((float)dt * (bs > bass_smooth ? 30.0f : 5.0f), 0, 1);
    }
    {
        // bubbles rise in the "lanes" (distinct x positions) of the LEDs showing them
        float spd = fx_speed[FX_BUBBLES];
        if (nbl && nbub < 64 && (float)rand() / RAND_MAX < dt * spd * (2 + 2.0f * nbl / (sc->count ? sc->count : 1))) {
            const led_t *l = &sc->leds[bub_leds[rand() % nbl]];
            bubble b = { l->x, 1.15f, 0.25f + 0.2f * rand() / RAND_MAX, 0.33f + 0.5f * rand() / RAND_MAX };
            bub[nbub++] = b;
        }
        for (int i = 0; i < nbub; i++) bub[i].y -= bub[i].v * (float)dt * spd;
        for (int i = 0; i < nbub;) if (bub[i].y < -0.2f) bub[i] = bub[--nbub]; else i++;
    }

    for (int i = 0; i < sc->count; i++) out[i] = led_color(fx, &sc->leds[i], fx_t, bub, nbub);
}

// Precomputes one device's LEDs as a looping animation the device plays on its own (Nanoleaf), so nothing has to
// be streamed. Periodic effects loop over exactly one period (seamless); the others over ~24 s, where the device's
// fade hides the seam. Frames are 0.3..1 s apart, in whole tenths (the device's time unit); the live clocks are
// not touched. out[frame * stride + led index]; returns the frame count, 0 when the device shows something live
// (temperature, audio) that has to be streamed.
int effects_bake(int fx, const scene_t *sc, int dev, int zone, int max_frames, int stride, rgbf *out, float *step_s) {
    if (!loaded) load_params();
    int idx[MAX_LEDS], n = 0, e0 = -1, mixed = 0, z0 = 0;
    for (int i = 0; i < sc->count; i++) {
        const led_t *l = &sc->leds[i];
        if (l->dev != dev || (zone >= 0 && l->zone != zone) || l->index >= stride) continue;
        int z = l->zone >= 0 && l->zone < MAX_ZONES ? l->zone : ZONE_GPU, e = fx == FX_OFF ? FX_OFF : zone_fx(z);
        if (e == FX_TEMP || e == FX_AUDIO) return 0;
        if (e0 < 0) { e0 = e; z0 = z; } else if (e != e0) mixed = 1;
        idx[n++] = i;
    }
    if (!n) return 0;

    float spd = fx_speed[e0] > 0.05f ? fx_speed[e0] : 0.05f;
    int np = zmode[z0] == ZMODE_PALETTE ? znpal[z0] : fx_npal[e0];
    double period;   // in effect-clock units
    switch (e0) {
    case FX_FLOW: case FX_PUMP: period = 1 / 0.12; break;
    case FX_COMET:   period = 5; break;
    case FX_BREATHE: period = (np > 0 ? np : 1) / 0.35; break;
    case FX_LAVA:    period = 2 * 3.14159265 / 0.2; break;   // the waves repeat; the slow palette drift makes a soft seam
    default:         period = 24 * spd; break;
    }
    if (mixed) period = 24 * spd;
    double secs = period / spd;
    if (secs < 2) secs = 2; if (secs > 60) secs = 60;
    int still = e0 == FX_OFF || e0 == FX_STATIC || zmode[z0] == ZMODE_WHITE || zmode[z0] == ZMODE_STATIC;
    int nf = still && !mixed ? 1 : (int)(secs / 0.5 + 0.5);
    if (nf > max_frames) nf = max_frames;
    if (nf < 1) nf = 1;
    float step = nf == 1 ? 1.0f : (float)((int)(secs / nf * 10 + 0.5)) / 10;
    if (step < 0.1f) step = 0.1f;
    if (nf > 1) { int f2 = (int)(secs / step + 0.5); if (f2 >= 2 && f2 <= max_frames) nf = f2; }

    // bubbles: a local swarm over this device's lanes, warmed up for 3 s so the loop starts full
    bubble lb[64]; int nlb = 0;
    double clk[FX_N];
    const double sub = 0.05;
    for (double t = -3; t < (nf - 1) * step + 1e-6; t += sub) {
        if (e0 == FX_BUBBLES || mixed) {
            float bs = fx_speed[FX_BUBBLES];
            if (nlb < 64 && (float)rand() / RAND_MAX < sub * bs * 4) {
                const led_t *l = &sc->leds[idx[rand() % n]];
                bubble b = { l->x, 1.15f, 0.25f + 0.2f * rand() / RAND_MAX, 0.33f + 0.5f * rand() / RAND_MAX };
                lb[nlb++] = b;
            }
            for (int i = 0; i < nlb; i++) lb[i].y -= lb[i].v * (float)sub * bs;
            for (int i = 0; i < nlb;) if (lb[i].y < -0.2f) lb[i] = lb[--nlb]; else i++;
        }
        if (t < -1e-6) continue;
        int f = (int)(t / step + 0.5);
        if (fabs(t - f * step) > sub / 2 + 1e-6 || f >= nf) continue;
        for (int e = 0; e < FX_N; e++) clk[e] = fx_t[e] + f * step * fx_speed[e];
        for (int k = 0; k < n; k++) out[f * stride + sc->leds[idx[k]].index] = led_color(fx, &sc->leds[idx[k]], clk, lb, nlb);
    }
    *step_s = step;
    return nf;
}
