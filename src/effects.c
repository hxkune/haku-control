// SPDX-License-Identifier: GPL-3.0-only
// Effects. Every effect is a pure function of (led position, time, sensors, palette),
// except bubbles which keeps a tiny particle list. Where an effect needs chance (twinkles, drops, flames) it comes
// from a hash of the LED and of whole time steps, so it still is a pure function: it looks the same on every
// device kind, and baked loops (Nanoleaf) get it too.
// Rainbow, Fire, Ocean, Twinkle, Meteor, Plasma, Aurora, Ripple, Matrix and Candle follow well-known LED effects
// (FastLED's Fire2012 and Pacifica, MIT; WLED's effects, EUPL-1.2), written anew as fields over the whole scene:
// every LED of every device takes its colour from its place in the room, so a strip, a keyboard, a Nanoleaf wall
// and a single bulb all show one picture.
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
    { "rainbow",     L"Rainbow",      L"Радуга",          L"Arc-en-ciel" },
    { "wave",        L"Colour wave",  L"Цветная волна",   L"Vague de couleurs" },
    { "fire",        L"Fire",         L"Огонь",           L"Feu" },
    { "ocean",       L"Ocean",        L"Океан",           L"Océan" },
    { "twinkle",     L"Constellation", L"Созвездие",      L"Constellation" },   // the ids stay: saved settings name them
    { "meteor",      L"Meteor shower", L"Метеорный дождь", L"Pluie de météores" },
    { "plasma",      L"Plasma",       L"Плазма",          L"Plasma" },
    { "aurora",      L"Aurora",       L"Северное сияние", L"Aurore" },
    { "ripple",      L"Ripple",       L"Капли",           L"Gouttes" },
    { "matrix",      L"Matrix",       L"Матрица",         L"Matrice" },
    { "candle",      L"Candle",       L"Свеча",           L"Bougie" },
    { "pendulum",    L"Pendulum",     L"Маятник",         L"Pendule" },
    { "interference", L"Interference", L"Интерференция",  L"Interférences" },
    { "steps",       L"Steps",        L"Ступени",         L"Paliers" },
    { "custom",      L"Constructor",  L"Конструктор",     L"Créateur" },
    { "screen",      L"Screen",       L"Экран",           L"Écran" },
    { "temperature", L"Temperature",  L"Температура",     L"Température" },
    { "pump",        L"Pump flow",    L"Поток по насосу", L"Flux de la pompe" },
    { "audio",       L"Audio",        L"Звук",            L"Audio" },
    { "static",      L"Static colour", L"Статичный цвет", L"Couleur fixe" },
    { "off",         L"Off",          L"Выключить",       L"Éteindre" },
};
const int g_effect_count = sizeof(g_effects) / sizeof(g_effects[0]);

enum { FX_FLOW, FX_CAUSTIC, FX_BUBBLES, FX_COMET, FX_LAVA, FX_BREATHE,
       FX_RAINBOW, FX_WAVE, FX_FIRE, FX_OCEAN, FX_TWINKLE, FX_METEOR, FX_PLASMA, FX_AURORA, FX_RIPPLE, FX_MATRIX, FX_CANDLE,
       FX_PENDULUM, FX_INTERF, FX_STEPS, FX_CUSTOM, FX_SCREEN,
       FX_TEMP, FX_PUMP, FX_AUDIO, FX_STATIC, FX_OFF, FX_N };

// the newer effects' own colours when they have none set (fire is not blue and pink); the others take [general]
static const struct { int fx; int n; unsigned rgb[6]; } FX_DEFAULT[] = {
    { FX_RAINBOW, 6, { 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF } },
    { FX_WAVE,    5, { 0xFF0050, 0xFF9000, 0x00FF80, 0x0090FF, 0x9030FF } },
    { FX_FIRE,    5, { 0x200000, 0xC01000, 0xFF5000, 0xFFB020, 0xFFF0A0 } },
    { FX_OCEAN,   5, { 0x000820, 0x003070, 0x0070B0, 0x00B0D0, 0xA0F0FF } },
    { FX_TWINKLE, 3, { 0xFFFFFF, 0xFFE8A0, 0xA0C8FF } },
    { FX_METEOR,  3, { 0xFFFFFF, 0x80C0FF, 0x3040FF } },
    { FX_PLASMA,  4, { 0xFF0080, 0x8000FF, 0x0080FF, 0x00FFC0 } },
    { FX_AURORA,  4, { 0x00FF90, 0x00C0FF, 0x7040FF, 0xFF40C0 } },
    { FX_RIPPLE,  3, { 0x40E0FF, 0x4060FF, 0xC040FF } },
    { FX_MATRIX,  2, { 0x00FF40, 0xC0FFC0 } },
    { FX_CANDLE,  3, { 0xFF7A1A, 0xFFB347, 0xFF5500 } },
    { FX_PENDULUM, 3, { 0x00E0FF, 0x8040FF, 0xFF40A0 } },
    { FX_INTERF,  4, { 0x000830, 0x0060FF, 0x00E0FF, 0xE0FFFF } },
    { FX_STEPS,   4, { 0xFF3060, 0xFFA000, 0x30E070, 0x3090FF } },
    { FX_CUSTOM,  3, { 0x00C8FF, 0x7A5CFF, 0xFF4FB0 } },
};

// Effect settings, [<effect>] key=value, in the units the window shows them (seconds, percent, a choice by number).
// Out of range (or not a number) is clamped, so a hand-edited config cannot break an effect. ui/fxpreview.js FX_PARAMS
// shows the same keys with the same defaults.
enum { P_BR_CYCLE, P_BR_DEPTH, P_BR_SINGLE, P_BR_WAVE,
       P_PE_MOTION, P_PE_AXIS, P_PE_WIDTH, P_PE_SOFT, P_PE_TAIL, P_PE_MIRROR, P_PE_BG,
       P_TW_DENSITY, P_TW_LIFE, P_TW_HALO, P_TW_VARIETY, P_TW_BG,
       P_ME_RATE, P_ME_TAIL, P_ME_SPREAD, P_ME_DIR, P_ME_BG,
       P_IN_SOURCES, P_IN_SCALE, P_IN_CONTRAST, P_IN_ANGLE,
       P_AU_LAYERS, P_ST_SEGMENTS, P_ST_PAUSE, P_ST_DIR, P_ST_HARD,
       P_FI_HEIGHT, P_CA_FLICKER, P_MX_STYLE,
       P_AD_MODE, P_AD_SENS, P_AD_DIR, P_AD_PEAK, P_AD_DECAY, P_AD_GAP, P_AD_SHAPE, P_N };
static const struct { int fx; const char *key; float def, lo, hi; } FXP[P_N] = {
    { FX_BREATHE, "cycle", 0, 0, 30 },          // seconds per breath; 0: from the speed, as before
    { FX_BREATHE, "depth", 50, 0, 100 },        // % it dims; 0: the old endless crossfade
    { FX_BREATHE, "single", 0, 0, 1 },          // 1: only the first colour
    { FX_BREATHE, "wave", 0, 0, 100 },          // % of a breath it lags from one end of the chain to the other
    { FX_PENDULUM, "motion", 0, 0, 1 },         // 0 swing (slows at the ends), 1 bounce (even speed)
    { FX_PENDULUM, "axis", 0, 0, 2 },           // 0 along the chain, 1 left-right, 2 up-down
    { FX_PENDULUM, "width", 18, 3, 60 },        // % of the length
    { FX_PENDULUM, "soft", 80, 0, 100 },
    { FX_PENDULUM, "tail", 25, 0, 100 },
    { FX_PENDULUM, "mirror", 0, 0, 1 },
    { FX_PENDULUM, "bg", 4, 0, 30 },
    { FX_TWINKLE, "density", 35, 5, 100 },      // % of the LEDs holding a star at a time
    { FX_TWINKLE, "life", 3, 1, 10 },           // seconds a star lives
    { FX_TWINKLE, "halo", 30, 0, 100 },
    { FX_TWINKLE, "variety", 100, 0, 100 },     // 0: all the first colour
    { FX_TWINKLE, "bg", 3, 0, 20 },
    { FX_METEOR, "rate", 4, 1, 20 },            // meteors per 10 s (fewer when they are slow: one at a time per lane)
    { FX_METEOR, "tail", 35, 5, 60 },
    { FX_METEOR, "spread", 40, 0, 100 },        // % their speeds differ
    { FX_METEOR, "dir", 0, 0, 2 },              // 0 forward, 1 back, 2 both
    { FX_METEOR, "bg", 2, 0, 20 },
    { FX_INTERF, "sources", 2, 2, 3 },
    { FX_INTERF, "scale", 8, 2, 20 },           // rings across the room
    { FX_INTERF, "contrast", 60, 0, 100 },
    { FX_INTERF, "angle", 0, 0, 360 },          // where the sources sit (2D only)
    { FX_AURORA, "layers", 2, 1, 3 },
    { FX_STEPS, "segments", 6, 2, 16 },
    { FX_STEPS, "pause", 1, 0, 5 },             // seconds the full chain holds
    { FX_STEPS, "dir", 0, 0, 2 },               // 0 forward, 1 back, 2 from the middle
    { FX_STEPS, "hard", 0, 0, 1 },
    { FX_FIRE, "height", 75, 30, 100 },
    { FX_CANDLE, "flicker", 50, 0, 100 },
    { FX_MATRIX, "style", 0, 0, 1 },            // 0 code, 1 rain
    { FX_AUDIO, "mode", 0, 0, 1 },              // 0 level, 1 beat
    { FX_AUDIO, "sens", 50, 0, 100 },
    { FX_AUDIO, "dir", 0, 0, 3 },               // 0 by device, 1 up, 2 down, 3 from the middle
    { FX_AUDIO, "peak", 1, 0, 1 },
    { FX_AUDIO, "decay", 350, 100, 1000 },      // ms a beat's flash takes to fade
    { FX_AUDIO, "gap", 250, 100, 800 },         // ms at least between two beats
    { FX_AUDIO, "shape", 0, 0, 1 },             // 0 the whole light flashes, 1 a wave from the middle
};
static float pv[P_N];
#define P(i) pv[i]
static int wrapi(int k, int n) { k %= n; return k < 0 ? k + n : k; }

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
static int   zsteady[MAX_ZONES];          // the device holds one steady colour only: shown still, this frame
static int   sync_all = 1;
static int   last_fx = -1;
static int   used_temp, used_audio;       // some LED shows the temperature / audio effect
// Screen: which part of the screen a zone takes ([zone.*] screen=): auto picks by the device's shape (a strip runs
// around the edges, a flat device like Nanoleaf or a keyboard is a mosaic of the screen, one light its average)
enum { SCR_AUTO, SCR_WHOLE, SCR_BORDER, SCR_LEFT, SCR_RIGHT, SCR_TOP, SCR_BOTTOM };
static int   zscreen[MAX_ZONES];
static int   zn[MAX_ZONES];                                 // LEDs of each zone in the scene
static float zbox[MAX_ZONES][4];                            // and their extent: min x, min y, max x, max y

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
// Colour wave: which way it runs through the room ([wave] dir) and how many times the palette fits across ([wave] size)
enum { WAVE_RIGHT, WAVE_LEFT, WAVE_DOWN, WAVE_UP, WAVE_OUT, WAVE_IN };
static int   wave_dir;
static float wave_bands;

typedef struct { float x, y, v, c; } bubble;
static bubble bub[64];
static int    nbub;
static float  temp_smooth = NAN, level_smooth, bass_smooth;
static int    loaded;
static int    scene_flat;                 // all LEDs on (nearly) one line: 2D effects run along the chain
static float  temp_gap; static int temp_lost = 1;
static float  peak_lv, peak_hold, beat_age = 9, bass_avg, since_sound, quiet_for = 99;
static float  audio_in;                   // 0..1: the sound picture fading in (sound starting) and out (silence, capture lost)
static int    beat_n, audio_have, was_temp, was_audio, beat_armed = 1;
static unsigned rng_s = 12345;             // bubbles' chance: own, so a run can be repeated (tests)
static float rnd(void) { rng_s = rng_s * 1664525U + 1013904223U; return (rng_s >> 8) / 16777216.0f; }

// ---- the constructor ([custom]): up to 8 layers drawn bottom to top, l1..l8, each a line of key:value words, e.g.
// "s:ring c:p0 dur:2 per:3 b:add". The window, a profile or a phone writes them: every word is checked and clamped
// here, an unknown one is skipped, so a line can never break the effect. Nothing but these shapes runs: no code.
// ui/fxcomp.js draws the same layers for the previews (tools/sim/preview_check.js compares the two).
enum { CS_FILL, CS_GRAD, CS_WAVE, CS_RING, CS_SPARK, CS_DOTS, CS_NOISE, CS_N };
static const char *const CS_NAME[CS_N] = { "fill", "grad", "wave", "ring", "spark", "dots", "noise" };
enum { CB_NORMAL, CB_ADD, CB_SCREEN, CB_MUL, CB_MASK, CB_N };
static const char *const CB_NAME[CB_N] = { "normal", "add", "screen", "mul", "mask" };
enum { LP_O, LP_X, LP_Y, LP_A, LP_SZ, LP_W, LP_SF, LP_V, LP_CNT, LP_DUR, LP_PER, LP_LIFE, LP_TAIL, LP_FADE, LP_INV, LP_HIDE,
       LP_AA, LP_AT, LP_MA, LP_NUM };
#define LP_MOD 13   // o..tail can be animated or follow the sound
static const struct { const char *key; float def, lo, hi; } LPD[LP_NUM] = {
    { "o", 100, 0, 100 },      // opacity, %
    { "x", 50, 0, 100 },       // centre, % of the room (on a line: % along it)
    { "y", 50, 0, 100 },
    { "a", 0, 0, 360 },        // direction, degrees
    { "sz", 100, 1, 400 },     // size: repeats across the room (gradient, wave, noise), a ring's largest radius
    { "w", 10, 1, 100 },       // width: a ring's line, a spark, a dot's share of its step, %
    { "sf", 50, 0, 100 },      // softness of the edges, %
    { "v", 0, -200, 200 },     // movement, % of the room a second
    { "n", 8, 1, 60 },         // dots along the chain; sparks every 10 s
    { "dur", 2, 0.1f, 20 },    // seconds a ring takes to grow
    { "per", 3, 0.1f, 30 },    // seconds from one ring to the next
    { "life", 1, 0.1f, 10 },   // seconds a spark lives
    { "tail", 10, 0, 50 },     // a spark's tail, % of the room
    { "fade", 1, 0, 1 },       // a ring fades as it grows
    { "inv", 0, 0, 1 },        // a mask lets through where it is dark instead
    { "hide", 0, 0, 1 },
    { "aa", 0, -400, 400 },    // the animation: how far the "ap" setting swings either way
    { "at", 4, 0.2f, 60 },     // and in how many seconds
    { "ma", 0, -400, 400 },    // the sound: how far the "mp" setting goes at full level
};
#define COMP_MAX 8
typedef struct { int shape, blend, col, ap, mp, ms; rgbf rgb; float p[LP_NUM]; } clayer;   // col: -1 by the shape, -2 rgb, else a palette colour
static clayer cl[COMP_MAX];
static int ncl, comp_audio;
// what the constructor shows before anything is made (and the first tutorial): "the reactor"
static const char *const COMP_DEFAULT[] = { "s:fill c:#000a30", "s:ring c:p0 dur:2 per:3 w:12 sf:80 b:add", "s:spark c:p n:6 life:1.2 tail:12 v:35 w:6 b:add" };

static int lp_index(const char *k) { for (int i = 0; i < LP_NUM; i++) if (!strcmp(LPD[i].key, k)) return i; return -1; }
static float lp_clamp(int i, float v) { return v >= LPD[i].lo ? (v <= LPD[i].hi ? v : LPD[i].hi) : LPD[i].lo; }   // (NaN: lo)
static int comp_parse(const char *line, clayer *L) {
    char b[256], *ctx = NULL;   // a config line is shorter
    strncpy_s(b, sizeof(b), line, _TRUNCATE);
    L->shape = -1; L->blend = CB_NORMAL; L->col = -1; L->ap = L->mp = -1; L->ms = 0; L->rgb.r = L->rgb.g = L->rgb.b = 1;
    for (int i = 0; i < LP_NUM; i++) L->p[i] = LPD[i].def;
    for (char *w = strtok_s(b, " \t", &ctx); w; w = strtok_s(NULL, " \t", &ctx)) {
        char *v = strchr(w, ':'); if (!v) continue;
        *v++ = 0;
        if (!strcmp(w, "s")) { for (int k = 0; k < CS_N; k++) if (!strcmp(v, CS_NAME[k])) L->shape = k; }
        else if (!strcmp(w, "b")) { for (int k = 0; k < CB_N; k++) if (!strcmp(v, CB_NAME[k])) L->blend = k; }
        else if (!strcmp(w, "c")) {
            if (v[0] == '#' && strlen(v) == 7 && strspn(v + 1, "0123456789abcdefABCDEF") == 6) {
                unsigned x = (unsigned)strtoul(v + 1, NULL, 16);
                L->col = -2; L->rgb.r = (x >> 16 & 255) / 255.0f; L->rgb.g = (x >> 8 & 255) / 255.0f; L->rgb.b = (x & 255) / 255.0f;
            } else if (v[0] == 'p' && v[1] >= '0' && v[1] <= '7' && !v[2]) L->col = v[1] - '0';
            else L->col = -1;
        }
        else if (!strcmp(w, "ap") || !strcmp(w, "mp")) { int k = lp_index(v); if (k >= 0 && k < LP_MOD) *(w[0] == 'a' ? &L->ap : &L->mp) = k; }
        else if (!strcmp(w, "ms")) L->ms = !strcmp(v, "bass") ? 1 : !strcmp(v, "beat") ? 2 : 0;
        else {
            int k = lp_index(w); if (k < 0) continue;
            char *end; double d = strtod(v, &end);
            L->p[k] = lp_clamp(k, end != v && isfinite(d) ? (float)d : NAN);
        }
    }
    return L->shape >= 0;
}
static void comp_load(void) {
    ncl = comp_audio = 0;
    for (int i = 0; i < COMP_MAX; i++) {
        char k[8]; snprintf(k, sizeof(k), "l%d", i + 1);
        const char *v = cfg_get("custom", k, "");
        if (v[0] && comp_parse(v, &cl[ncl])) ncl++;
    }
    if (!ncl) for (int i = 0; i < (int)(sizeof(COMP_DEFAULT) / sizeof(COMP_DEFAULT[0])); i++) ncl += comp_parse(COMP_DEFAULT[i], &cl[ncl]);
    for (int i = 0; i < ncl; i++) if (cl[i].mp >= 0 && cl[i].p[LP_MA] != 0 && cl[i].p[LP_HIDE] < 0.5f) comp_audio = 1;
}

static void load_params(void) {
    for (int e = 0; e < FX_N; e++) {
        const char *sec = g_effects[e].id;
        fx_npal[e] = cfg_palette(sec, fx_pal[e], MAX_PALETTE);
        for (int d = 0; !fx_npal[e] && d < (int)(sizeof(FX_DEFAULT) / sizeof(FX_DEFAULT[0])); d++)
            if (FX_DEFAULT[d].fx == e) {
                for (int k = 0; k < FX_DEFAULT[d].n && k < MAX_PALETTE; k++) {
                    unsigned v = FX_DEFAULT[d].rgb[k];
                    fx_pal[e][k].r = ((v >> 16) & 255) / 255.0f; fx_pal[e][k].g = ((v >> 8) & 255) / 255.0f; fx_pal[e][k].b = (v & 255) / 255.0f;
                }
                fx_npal[e] = FX_DEFAULT[d].n;
            }
        if (!fx_npal[e]) fx_npal[e] = cfg_palette("general", fx_pal[e], MAX_PALETTE);
        if (!fx_npal[e]) { rgbf d[3] = { { 0, .78f, 1 }, { .48f, .24f, 1 }, { 1, .18f, .58f } }; memcpy(fx_pal[e], d, sizeof(d)); fx_npal[e] = 3; }
        float sp = cfg_getf(sec, "speed", cfg_getf("general", "speed", 5)) / 5.0f;
        fx_speed[e] = sp >= 0 ? (sp <= 4 ? sp : 4) : 1;   // (also not a number)
    }
    for (int i = 0; i < P_N; i++) {
        float v = cfg_getf(g_effects[FXP[i].fx].id, FXP[i].key, FXP[i].def);
        pv[i] = v >= FXP[i].lo ? (v <= FXP[i].hi ? v : FXP[i].hi) : FXP[i].lo;
    }
    if (P(P_BR_CYCLE) >= 1) fx_speed[FX_BREATHE] = 1 / (0.35f * P(P_BR_CYCLE));   // a set breath length takes over from the speed
    for (int z = 0; z < MAX_ZONES; z++) {
        char zs[32]; zone_section(z, zs, sizeof(zs));
        const char *m = cfg_get(zs, "mode", "effect");
        znpal[z] = cfg_palette(zs, zpal[z], MAX_PALETTE);
        zmode[z] = !_stricmp(m, "white") ? ZMODE_WHITE
                 : !znpal[z] ? ZMODE_EFFECT : !_stricmp(m, "static") ? ZMODE_STATIC : !_stricmp(m, "palette") ? ZMODE_PALETTE : ZMODE_EFFECT;
        zkelvin[z] = (int)clampf(cfg_getf(zs, "kelvin", 4000), 2000, 7000);
        zlevel[z]  = clampf(cfg_getf(zs, "brightness", 100) / 100.0f, 0.01f, 1);   // down to 1% (Key Lights go to 3)
        const char *sa = cfg_get(zs, "screen", "auto");
        zscreen[z] = !_stricmp(sa, "whole") ? SCR_WHOLE : !_stricmp(sa, "border") ? SCR_BORDER : !_stricmp(sa, "left") ? SCR_LEFT
                   : !_stricmp(sa, "right") ? SCR_RIGHT : !_stricmp(sa, "top") ? SCR_TOP : !_stricmp(sa, "bottom") ? SCR_BOTTOM : SCR_AUTO;
        const char *e = cfg_get(zs, "effect", "");
        zeffect[z] = !e[0] || !_stricmp(e, "sync") ? -1 : effect_index(e);
    }
    sync_all   = cfg_geti("general", "sync", 1);
    {
        const char *d = cfg_get("wave", "dir", "right");
        wave_dir = !_stricmp(d, "left") ? WAVE_LEFT : !_stricmp(d, "down") ? WAVE_DOWN : !_stricmp(d, "up") ? WAVE_UP
                 : !_stricmp(d, "out") ? WAVE_OUT : !_stricmp(d, "in") ? WAVE_IN : WAVE_RIGHT;
        wave_bands = clampf(cfg_getf("wave", "size", 2), 1, 4);
    }
    param_cold = cfg_getf("temperature", "cold", 30.0f);
    param_hot  = cfg_getf("temperature", "hot", 75.0f);
    temp_water = _stricmp(cfg_get("temperature", "source", "gpu"), "water") == 0;
    comp_load();
    loaded = 1;
}

void effects_reset(void) { loaded = 0; nbub = 0; rng_s = 12345; }

int effects_audio_state(void) { return !used_audio ? -2 : !audio_have ? -1 : quiet_for > 1.5f ? 0 : 1; }
int effects_temp_state(void) { return !used_temp ? -2 : temp_lost ? 0 : 1; }

// an effect's own palette as it lights (its section, its default, the general one); the caller holds cs
int effects_palette(int e, rgbf *out, int max) {
    if (e < 0 || e >= FX_N) return 0;
    int n = fx_npal[e] < max ? fx_npal[e] : max;
    memcpy(out, fx_pal[e], n * sizeof(rgbf));
    return n;
}

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

// chance without state: a hash of whole numbers, and smooth value noise over it
static unsigned hash32(unsigned x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static float hashf(unsigned x) { return (hash32(x) & 0xFFFFFF) / 16777216.0f; }
static float vnoise(float x, unsigned seed) {
    float i = floorf(x), f = x - i; unsigned k = (unsigned)(int)i;
    float a = hashf(k * 0x9E3779B1U ^ seed), b = hashf((k + 1) * 0x9E3779B1U ^ seed);
    return a + (b - a) * smooth01(f);
}
static float vnoise2(float x, float y, unsigned seed) {
    float j = floorf(y), f = y - j; unsigned k = (unsigned)(int)j;
    return vnoise(x, seed ^ hash32(k)) * (1 - smooth01(f)) + vnoise(x, seed ^ hash32(k + 1)) * smooth01(f);
}
// one LED, the same every frame
static unsigned led_key(const led_t *l) { return hash32((unsigned)l->dev * 100003U + (unsigned)l->zone * 1009U + (unsigned)l->index); }
static rgbf hsv(float h, float s, float v) {
    h = fractf(h) * 6; int i = (int)h; float f = h - i, p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
    rgbf c;
    switch (i % 6) { case 0: c.r = v; c.g = u; c.b = p; break; case 1: c.r = q; c.g = v; c.b = p; break; case 2: c.r = p; c.g = v; c.b = u; break;
                     case 3: c.r = p; c.g = q; c.b = v; break; case 4: c.r = u; c.g = p; c.b = v; break; default: c.r = v; c.g = p; c.b = q; }
    return c;
}

// a point on the screen's edge, going round from the bottom left, clockwise (as a backlight strip is laid), and
// the patch of screen just inside it
static int screen_edge(float f, rgbf *c) {
    f = fractf(f) * 4; int e = (int)f; float q = f - e, d = 0.2f, w = 0.08f;
    float u = e == 0 ? 0 : e == 1 ? q : e == 2 ? 1 : 1 - q, v = e == 0 ? 1 - q : e == 1 ? 0 : e == 2 ? q : 1;
    if (e == 0 || e == 2) return screen_area(u ? 1 - d : 0, v - w, u ? 1 : d, v + w, c);
    return screen_area(u - w, v ? 1 - d : 0, u + w, v ? 1 : d, c);
}
static rgbf screen_led(const led_t *l, int z) {
    rgbf c = { 0, 0, 0 };
    int m = zscreen[z];
    float w = zbox[z][2] - zbox[z][0], h = zbox[z][3] - zbox[z][1];
    if (m == SCR_AUTO) m = zn[z] <= 1 ? SCR_WHOLE : (w > 0.05f && h > 0.05f) ? -1 : SCR_BORDER;
    int ok;
    switch (m) {
    case -1: {   // a mosaic: the device's own extent laid over the screen
        float u = (l->x - zbox[z][0]) / w, v = (l->y - zbox[z][1]) / h, k = 0.12f;
        ok = screen_area(u - k, v - k, u + k, v + k, &c); break;
    }
    case SCR_BORDER: ok = screen_edge(l->fill, &c); break;
    case SCR_LEFT:   ok = screen_area(0, 0, 0.25f, 1, &c); break;
    case SCR_RIGHT:  ok = screen_area(0.75f, 0, 1, 1, &c); break;
    case SCR_TOP:    ok = screen_area(0, 0, 1, 0.25f, &c); break;
    case SCR_BOTTOM: ok = screen_area(0, 0.75f, 1, 1, &c); break;
    default:         ok = screen_area(0, 0, 1, 1, &c); break;
    }
    if (!ok) return c;
    // a little more colour than the average gives (averages go grey)
    float g = (c.r + c.g + c.b) / 3, s = 1.35f;
    c.r = g + (c.r - g) * s; c.g = g + (c.g - g) * s; c.b = g + (c.b - g) * s;
    return c;
}

static float caustic_n(float x, float y, float t) {
    float n = 0.5f + 0.5f * sinf(x * 13 + t * 1.7f) * sinf(y * 9 - t * 1.3f + sinf(x * 5 + t));
    return n * n;
}

// Constellation: an LED's life in slots of its own length; in a slot it holds a star or not (density), which fades
// in, shines and fades out, so nothing pops on or off. hue: the star's place in the palette.
static float star(unsigned k, float t, float life, float dens, float *hue) {
    float q = t / (life * (0.7f + 0.6f * hashf(k ^ 0x51u))) + hashf(k ^ 0x93u), f = q - floorf(q);
    unsigned id = hash32(k ^ ((unsigned)(int)floorf(q) * 0x9E3779B1U));
    *hue = hashf(id ^ 0xC0u);
    if (hashf(id) >= dens) return 0;
    return smooth01(f / 0.25f) * smooth01((1 - f) / 0.25f);
}

// The constructor's layers at one point: (px, py) in the room, or along the chain on a line (flat), t in seconds.
// Each shape gives a coverage a (0..1) and a place h in the palette; the blend puts it over what is below.
static float comp_band(float d, float half, float soft) { return 1 - smooth01((d - half * (1 - soft)) / fmaxf(2 * half * soft, 0.004f)); }
static float comp_shape(const clayer *L, const float *p, float px, float py, int flat, float t, int li, float *h) {
    float ar = p[LP_A] * 0.0174533f, ca = cosf(ar), sa = sinf(ar), mv = t * p[LP_V] / 100, sf = p[LP_SF] / 100;
    float u = flat ? px : (px - 0.5f) * ca + (py - 0.5f) * sa + 0.5f;
    *h = 0;
    switch (L->shape) {
    case CS_FILL: *h = mv; return 1;
    case CS_GRAD: *h = u * p[LP_SZ] / 100 - mv; return 1;
    case CS_WAVE: { float ph = u * p[LP_SZ] / 100 - mv; *h = ph; return 0.5f + 0.5f * cosf(6.2831853f * ph); }
    case CS_NOISE: { float k = p[LP_SZ] / 25; float a = vnoise2(px * k + mv * ca, (flat ? 0 : py * k) + mv * sa, 7u + (unsigned)li); *h = a; return a; }
    case CS_DOTS: {   // along the chain (2D dots would fall between the LEDs of a sparse room)
        float ph = (px - mv) * p[LP_CNT], f = ph - floorf(ph);
        *h = floorf(ph) / p[LP_CNT];
        return comp_band(fabsf(f - 0.5f), p[LP_W] / 200, sf);
    }
    case CS_RING: {
        float per = p[LP_PER], q = t / per, age = (q - floorf(q)) * per, dur = p[LP_DUR];
        if (age > dur) return 0;
        float cx = p[LP_X] / 100, cy = p[LP_Y] / 100, d = flat ? fabsf(px - cx) : sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
        *h = fractf(floorf(q) * 0.618034f);
        return comp_band(fabsf(d - age / dur * p[LP_SZ] / 200), p[LP_W] / 200, sf) * (p[LP_FADE] >= 0.5f ? 1 - age / dur : 1);
    }
    case CS_SPARK: {  // eight lanes, each lighting a spark in turn at a place, moment and heading of chance
        float life = p[LP_LIFE], per = fmaxf(life * 1.05f, 80 / p[LP_CNT]), spd = p[LP_V] / 100, r = p[LP_W] / 200, best = 0;
        for (int m = 0; m < 8; m++) {
            float q = t / per + m * 0.125f, qs = floorf(q);
            unsigned id = hash32((unsigned)(int)qs * 8U + (unsigned)m + 0x5A1u + (unsigned)li * 0x1000u);
            float age = (q - qs) * per - hashf(id ^ 5u) * (per - life);
            if (age < 0 || age > life) continue;
            float da = hashf(id ^ 3u) * 6.2831853f, dx = flat ? (id & 0x100 ? 1.0f : -1.0f) : cosf(da), dy = flat ? 0 : sinf(da);
            float hx = hashf(id ^ 1u) + dx * spd * age, hy = (flat ? 0.5f : hashf(id ^ 2u)) + dy * spd * age;
            float ox = px - hx, oy = (flat ? 0.5f : py) - hy, tl = fminf(p[LP_TAIL] / 100, fabsf(spd) * age);
            float back = clampf(-(ox * dx + oy * dy) * (spd < 0 ? -1 : 1), 0, tl), sg = spd < 0 ? -1.0f : 1.0f;
            float qx = ox + dx * back * sg, qy = oy + dy * back * sg, e = (qx * qx + qy * qy) / (r * r);
            float k = expf(-e) * (tl > 0 ? 1 - back / tl : 1) * smooth01(age / 0.1f) * smooth01((life - age) / (life * 0.4f));
            if (k > best) { best = k; *h = hashf(id ^ 4u); }
        }
        return best;
    }
    }
    return 0;
}
static rgbf comp_color(float px, float py, int flat, float t) {
    rgbf acc = { 0, 0, 0 };
    float snd[3] = { level_smooth, bass_smooth, expf(-beat_age / 0.35f) * audio_in };
    for (int i = 0; i < ncl; i++) {
        const clayer *L = &cl[i];
        if (L->p[LP_HIDE] >= 0.5f) continue;
        float p[LP_NUM]; memcpy(p, L->p, sizeof(p));
        if (L->ap >= 0) p[L->ap] = lp_clamp(L->ap, p[L->ap] + p[LP_AA] * sinf(6.2831853f * t / p[LP_AT]));
        if (L->mp >= 0 && used_audio) p[L->mp] = lp_clamp(L->mp, p[L->mp] + p[LP_MA] * snd[L->ms]);
        float h, a = comp_shape(L, p, px, py, flat, t, i, &h), k = a * p[LP_O] / 100;
        rgbf c = L->col == -2 ? L->rgb : L->col >= 0 ? pal[L->col % npal] : palc(h);
        switch (L->blend) {
        case CB_ADD: acc = addc(acc, scalec(c, k)); break;
        case CB_SCREEN:
            acc.r = 1 - (1 - fminf(acc.r, 1)) * (1 - c.r * k); acc.g = 1 - (1 - fminf(acc.g, 1)) * (1 - c.g * k); acc.b = 1 - (1 - fminf(acc.b, 1)) * (1 - c.b * k);
            break;
        case CB_MUL: acc.r *= 1 + (c.r - 1) * k; acc.g *= 1 + (c.g - 1) * k; acc.b *= 1 + (c.b - 1) * k; break;
        case CB_MASK: acc = scalec(acc, 1 + ((p[LP_INV] >= 0.5f ? 1 - a : a) - 1) * p[LP_O] / 100); break;   // only its shape: what is below shows through it
        default: acc = mixc(acc, c, k); break;
        }
    }
    return acc;
}

// One LED's colour for the effect clocks clk[] and a bubble list: the live ones, or local ones when baking ahead.
static rgbf led_color(int fx, const led_t *l, const double *clk, const bubble *bb, int nb) {
    float x = l->x, y = l->y;
    rgbf c = { 0, 0, 0 };
    int z = l->zone >= 0 && l->zone < MAX_ZONES ? l->zone : ZONE_GPU;
    int e = fx == FX_OFF ? FX_OFF : zone_fx(z);
    if (e != FX_OFF && zmode[z] == ZMODE_WHITE) return scalec(kelvin_rgb(zkelvin[z]), zlevel[z]);
    if (e != FX_OFF && zmode[z] == ZMODE_STATIC) return scalec(zpal[z][0], zlevel[z]);
    // no animation there: its palette's first colour, else the effect's, so the colour it holds is a known one
    if (e != FX_OFF && zsteady[z]) return scalec(zmode[z] == ZMODE_PALETTE && znpal[z] ? zpal[z][0] : fx_pal[e][0], zlevel[z]);
    if (zmode[z] == ZMODE_PALETTE) { pal = zpal[z]; npal = znpal[z]; } else { pal = fx_pal[e]; npal = fx_npal[e]; }

    // own effect / no sync: position inside the device and a per-device clock offset
    int solo = zeffect[z] >= 0 || !sync_all;
    float path = solo ? l->zpath : l->path;
    float t = (float)clk[e] + (solo ? z * 2.37f : 0);
    int one = zn[z] == 1;   // a single light (a bulb, a one-LED zone): no pattern to show along it

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
        // one breath per colour: it dims to (1 - depth) and the colour changes around the darkest point, in a window
        // that narrows as it gets darker; depth 0 is the old endless crossfade. Both are periodic: no seam.
        float mn = 1 - P(P_BR_DEPTH) / 100, w = 0.2f + 0.8f * mn;
        float ph = t * 0.35f - P(P_BR_WAVE) / 100 * path, cp = ph + 0.5f;
        int k = (int)floorf(cp);
        c = P(P_BR_SINGLE) >= 0.5f ? pal[0] : mixc(pal[wrapi(k, npal)], pal[wrapi(k + 1, npal)], smooth01((cp - k - 0.5f) / w + 0.5f));
        c = scalec(c, mn + (1 - mn) * 0.5f * (1 - cosf(6.2831853f * fractf(ph))));
        break;
    }
    case FX_TEMP: {
        if (temp_lost) { rgbf g = { 0.55f, 0.55f, 0.6f }; c = scalec(g, 0.09f + 0.04f * sinf(t * 1.2f)); break; }   // no reading: a dim grey, not a made-up cold colour
        float p = (temp_smooth - param_cold) / (param_hot - param_cold);
        c = scalec(grad(p), 0.8f + 0.2f * caustic_n(x, y, t * 0.5f));
        break;
    }
    case FX_AUDIO: {
        // by device: RAM fills bottom->top, GPU and the others from the centre outwards
        int dm = (int)P(P_AD_DIR), mid = dm == 3 || (dm == 0 && (l->dev == DEV_GPU || l->dev == DEV_EXT));
        float pos = mid ? fabsf(l->fill - 0.5f) * 2 : dm == 2 ? 1 - l->fill : l->fill, lv = clampf(level_smooth * 1.15f, 0, 1);
        if (P(P_AD_MODE) >= 0.5f) {   // beat: a flash on each bass hit, the next palette colour each time
            float pulse = expf(-beat_age / (P(P_AD_DECAY) / 1000)), b = pulse;
            if (P(P_AD_SHAPE) >= 0.5f && !one) b = pulse * smooth01(1 - fabsf(fabsf(l->fill - 0.5f) * 2 - (1 - pulse) * 1.2f) / 0.3f);
            c = scalec(pal[beat_n % npal], 0.06f + 0.94f * b * audio_in);
        } else if (one) {             // a single light: its brightness is the level
            c = scalec(grad(lv), 0.08f + 0.92f * lv);
        } else {                      // level: a bar, coloured by height; the bass lifts it
            float lit = smooth01((lv - pos) / 0.12f + 0.5f) * smooth01(lv / 0.06f);   // (nothing lit at silence)
            c = scalec(grad(pos * 0.85f + bass_smooth * 0.15f), 0.08f + 0.92f * lit * (0.7f + 0.3f * bass_smooth) * audio_in);
            float pk = clampf(peak_lv * 1.15f, 0, 1), d = fabsf(pos - pk) / 0.04f;
            if (P(P_AD_PEAK) >= 0.5f && pk > 0.05f && d < 1) c = addc(c, scalec(pal[npal - 1], (1 - d) * (1 - d) * audio_in));
        }
        break;
    }
    case FX_RAINBOW:   // the whole hue circle along the chain, turning
        c = hsv(path * 0.9f - t * 0.15f, 1, 1);
        break;
    case FX_WAVE: {    // after SignalRGB's Color Wave: the palette's colours in bands, sweeping across the whole room
        // one way (by where each LED is, so every device takes the wave as it passes), or from / to the middle;
        // a device on its own effect runs it along itself
        float u = solo ? path : wave_dir == WAVE_RIGHT ? x : wave_dir == WAVE_LEFT ? 1 - x : wave_dir == WAVE_DOWN ? y : wave_dir == WAVE_UP ? 1 - y
                : sqrtf((x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f)) * 1.4f;
        if (wave_dir == WAVE_IN && !solo) u = 1 - u;
        c = palc(u * wave_bands - t * 0.18f);
        break;
    }
    case FX_FIRE: {    // flames from the bottom (y = 1) up, flickering; the palette runs from embers to the hottest
        float n = vnoise2(x * 7 + vnoise(t * 0.7f, 11) * 2, y * 5 + t * 1.9f, 3);
        float h = clampf(0.85f - (1 - y) * 0.85f * 75 / P(P_FI_HEIGHT) + (n - 0.5f) * 0.75f + 0.12f * vnoise(t * 9 + x * 13, led_key(l) & 1023), 0, 1);
        c = grad(h);
        break;
    }
    case FX_OCEAN: {   // after FastLED's Pacifica: layers of slow waves, foam where they meet
        float u = path + y * 0.35f;
        float w = 0.4f * sinf(u * 9 + t * 0.71f) + 0.3f * sinf(u * 13 - t * 1.13f + 1.3f) + 0.2f * sinf(u * 21 + t * 1.7f + 2.1f) + 0.1f * sinf(u * 31 - t * 2.3f);
        float v = 0.5f + 0.5f * w;
        c = grad(v * 0.85f);
        if (v > 0.8f) c = addc(c, scalec(pal[npal - 1], (v - 0.8f) * 2.5f));
        break;
    }
    case FX_TWINKLE: { // constellation: stars come and go on a dark sky, with a faint halo on their neighbours
        // a single light always holds a star (else it would sit dark most of the time)
        float life = P(P_TW_LIFE), dens = one ? 1 : P(P_TW_DENSITY) / 100, var = P(P_TW_VARIETY) / 100, halo = P(P_TW_HALO) / 100, hue;
        float b = star(led_key(l), t, life, dens, &hue);
        c = addc(scalec(pal[npal - 1], P(P_TW_BG) / 100), scalec(palc(hue * var), b));
        // shortcut: neighbours by LED number, which on a panel or keyboard is only roughly "next to"
        for (int o = -2; halo > 0 && !one && o <= 2; o++) {
            if (!o) continue;
            float nb = star(hash32((unsigned)l->dev * 100003U + (unsigned)l->zone * 1009U + (unsigned)(l->index + o)), t, life, dens, &hue);
            if (nb > 0) c = addc(c, scalec(palc(hue * var), nb * halo * (o == 1 || o == -1 ? 0.35f : 0.12f)));
        }
        break;
    }
    case FX_METEOR: {  // a shower: four lanes, each sends a meteor in turn, each at its own speed and moment
        c = scalec(pal[npal - 1], P(P_ME_BG) / 100);
        float tail = P(P_ME_TAIL) / 100, spread = P(P_ME_SPREAD) / 100;
        // a lane's next meteor waits for the last one to leave (so the rate tops out when they are slow)
        float slot = fmaxf(40 / P(P_ME_RATE), (1.15f + tail) / (0.45f * (1 - 0.5f * spread)));
        int dir = (int)P(P_ME_DIR);
        for (int m = 0; m < 4; m++) {
            float q = t / slot + m * 0.25f, qs = floorf(q);
            unsigned id = hash32((unsigned)(int)qs * 4U + (unsigned)m + 0x3A1u);
            float v = 0.45f * (1 + spread * (hashf(id) - 0.5f)), start = hashf(id ^ 5u) * (slot - (1.15f + tail) / v);
            float u = dir == 1 || (dir == 2 && (id & 0x100)) ? 1 - path : path, d = ((q - qs) * slot - start) * v - 0.05f - u;
            if (d < -0.02f || d > tail) continue;
            float k = d < 0 ? 1 + d / 0.02f : 1 - d / tail;   // a soft front, then the fading tail
            c = addc(c, scalec(mixc(pal[0], pal[npal > 1 ? 1 : 0], fmaxf(d, 0) / tail), k * k));
        }
        break;
    }
    case FX_PLASMA: {
        float v = sinf(x * 10 + t) + sinf(y * 8 - t * 1.3f) + sinf((x + y) * 7 + t * 0.7f) + sinf(sqrtf((x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f)) * 14 - t * 1.6f);
        c = palc(v * 0.125f + 0.5f + t * 0.04f);
        break;
    }
    case FX_AURORA: {  // layers of curtains drifting along at their own slow pace, brighter at the top
        int nl = (int)P(P_AU_LAYERS);
        float u = path, tt = t * 0.6f;
        rgbf acc = { 0, 0, 0 };
        for (int i = 0; i < nl; i++) {
            float sp = 1 - i * 0.3f;
            float wave = sinf(u * (6 + 2.5f * i) + tt * 0.5f * sp + i * 1.9f + 2 * sinf(u * (2.3f + 0.6f * i) - tt * 0.3f * sp));
            float curtain = smooth01((wave + 1) * 0.5f); curtain *= curtain;
            float shimmer = 0.85f + 0.15f * sinf(u * 23 + tt * 1.1f + i * 2.3f);
            rgbf col = grad(clampf(0.5f + 0.5f * sinf(u * 2 + tt * 0.2f + y * 1.5f + i * 2.1f), 0, 1));
            acc = addc(acc, scalec(col, curtain * shimmer * (i ? 0.6f : 1)));
        }
        c = addc(scalec(pal[0], 0.03f), scalec(acc, (0.35f + 0.65f * (1 - y)) / (1 + 0.2f * (nl - 1))));
        break;
    }
    case FX_RIPPLE: {  // drops: rings spreading from places in the room
        c = scalec(pal[0], 0.03f);
        for (int k = 0; k < 4; k++) {
            float tt = t * 0.45f + k * 0.25f, slot = floorf(tt), age = tt - slot;
            unsigned id = (unsigned)(int)slot * 4U + (unsigned)k;
            float cx = hashf(id * 2654435761U), cy = hashf(id * 2654435761U ^ 0x1234u), d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            float e = (d - age * 0.9f) / 0.06f, ring = expf(-e * e) * (1 - age) * (1 - age);
            c = addc(c, scalec(palc(hashf(id ^ 0x77u)), ring));
        }
        break;
    }
    case FX_MATRIX: {  // code rain: columns falling at their own pace, a bright head and a fading trail
        unsigned col = (unsigned)(int)floorf(x * 24);
        float sp = 0.35f + hashf(col * 31u) * 0.5f, head = fractf(t * sp * 0.6f + hashf(col * 17u)) * 1.6f - 0.3f, dist = head - y;
        c = scalec(pal[0], 0.01f);
        if (P(P_MX_STYLE) >= 0.5f) {   // rain: calm drops, each its own palette colour, short tails, some columns resting
            float q = t * sp * 0.6f + hashf(col * 17u), qs = floorf(q);
            unsigned id = hash32(col * 31u ^ (unsigned)(int)qs * 0x9E3779B1U);
            float dd = (q - qs) * 1.6f - 0.3f - y;
            if (hashf(id) < 0.65f && dd >= 0 && dd < 0.25f) { float b = 1 - dd / 0.25f; c = scalec(palc(hashf(id ^ 0xD1u)), b * b); }
        } else if (dist >= 0 && dist < 0.45f) {
            float b = 1 - dist / 0.45f; b *= b;
            float flick = 0.7f + 0.3f * hashf(led_key(l) ^ (unsigned)(int)floorf(t * 15) * 2246822519U);
            c = scalec(dist < 0.03f && npal > 1 ? pal[1] : pal[0], b * flick);
        }
        break;
    }
    case FX_CANDLE: {  // a warm flame: each device flickers on its own, now and then it dips
        unsigned zs = (unsigned)z * 13u;
        float fl = P(P_CA_FLICKER) / 100, f = vnoise(t * 5, zs + 1) * 0.6f + vnoise(t * 9, zs + 2) * 0.4f;
        float dip = 1 - 0.8f * fl * smooth01((0.22f - vnoise(t * 1.3f, zs + 5)) / 0.16f);   // eases down and back, no jump
        float b = (1 - 0.9f * fl + 0.9f * fl * f) * dip * (0.93f + 0.07f * hashf(led_key(l)));
        c = scalec(mixc(pal[0], pal[npal > 1 ? 1 : 0], vnoise(t * 2, zs + 9)), b);
        break;
    }
    case FX_PENDULUM: {   // a band swinging end to end (or bouncing at an even speed), lighting the palette laid along the length
        int ax = (int)P(P_PE_AXIS), bounce = P(P_PE_MOTION) >= 0.5f;
        float u = solo || scene_flat || ax == 0 ? path : ax == 1 ? x : y;
        if (P(P_PE_MIRROR) >= 0.5f) u = fabsf(2 * u - 1);   // two bands, meeting in the middle
        float ph = fractf(t * 0.25f), pos = bounce ? 1 - fabsf(2 * ph - 1) : 0.5f - 0.5f * cosf(6.2831853f * ph);
        float half = P(P_PE_WIDTH) / 200, soft = P(P_PE_SOFT) / 100, d = fabsf(u - pos);
        float band = 1 - smooth01((d - half * (1 - soft)) / fmaxf(2 * half * soft, 0.004f));
        // the trail: how long ago the band last went over this point (twice a cycle: going and coming back)
        float tail = P(P_PE_TAIL) / 200, tr = 0;
        if (tail > 0) {
            float p1 = bounce ? u / 2 : acosf(clampf(1 - 2 * u, -1, 1)) / 6.2831853f, ago = fminf(fractf(ph - p1), fractf(ph + p1));
            if (ago < tail) { tr = 1 - ago / tail; tr *= tr * 0.7f; }
        }
        c = scalec(grad(u), P(P_PE_BG) / 100 + (1 - P(P_PE_BG) / 100) * fmaxf(band, tr));
        break;
    }
    case FX_INTERF: {     // waves from two or three points crossing: still fringes with ripples running through
        float k = 6.2831853f * P(P_IN_SCALE), v = 0;
        if (solo || scene_flat) {   // on a line: two waves running opposite ways at slightly different pitches
            v = 0.5f * (sinf(k * path - t * 2) + sinf(k * 1.13f * path + t * 1.7f));
        } else {
            int ns = (int)P(P_IN_SOURCES);
            float a0 = P(P_IN_ANGLE) * 0.0174533f + t * 0.05f;
            for (int i = 0; i < ns; i++) {
                float a = a0 + i * 6.2831853f / ns, sx = 0.5f + 0.38f * cosf(a) - x, sy = 0.5f + 0.38f * sinf(a) - y;
                v += sinf(k * sqrtf(sx * sx + sy * sy) - t * 2 + i * 1.1f);
            }
            v /= ns;
        }
        float m = clampf(0.5f + 0.5f * v * (0.5f + 1.5f * P(P_IN_CONTRAST) / 100), 0, 1);
        c = scalec(grad(m), 0.12f + 0.88f * m);
        break;
    }
    case FX_STEPS: {      // the chain fills segment by segment with the next colour, holds, then the next one
        int sg = (int)P(P_ST_SEGMENTS), dir = (int)P(P_ST_DIR);
        float u = dir == 1 ? 1 - path : dir == 2 ? fabsf(2 * path - 1) : path;
        float cyc = sg * 0.5f + P(P_ST_PAUSE), q = t / cyc, qs = floorf(q), lit = (q - qs) * cyc / 0.5f, g = fminf(floorf(u * sg), sg - 1.0f);
        float f = P(P_ST_HARD) >= 0.5f ? (lit > g + 0.5f ? 1.0f : 0.0f) : smooth01(lit - g);
        int k = (int)qs;
        rgbf dark = scalec(pal[0], 0.04f);   // one colour: it fills in and then out
        rgbf a = npal > 1 ? pal[wrapi(k, npal)] : (k & 1) ? pal[0] : dark, b = npal > 1 ? pal[wrapi(k + 1, npal)] : (k & 1) ? dark : pal[0];
        c = mixc(a, b, f);
        break;
    }
    case FX_CUSTOM: {     // 2D shapes over the room; along the chain on a line or on a device of its own
        int fl = solo || scene_flat;
        c = comp_color(fl ? path : x, fl ? 0.5f : y, fl, t);
        break;
    }
    case FX_SCREEN:
        c = screen_led(l, z);
        break;
    case FX_STATIC:
        c = pal[0];
        break;
    case FX_OFF:
    default:
        break;
    }
    c = scalec(c, zlevel[z]);
    // 0..1, and a NaN (it fails every comparison) becomes 0
    c.r = c.r > 0 ? (c.r < 1 ? c.r : 1) : 0; c.g = c.g > 0 ? (c.g < 1 ? c.g : 1) : 0; c.b = c.b > 0 ? (c.b < 1 ? c.b : 1) : 0;
    return c;
}

void effects_render(int fx, const scene_t *sc, const sensors_t *sn, double dt, rgbf *out) {
    if (!loaded || fx != last_fx) { load_params(); if (fx != last_fx) nbub = 0; }
    last_fx = fx;

    for (int k = 0, n = ext_count(); k < EXT_SLOTS; k++) zsteady[ZONE_EXT0 + k] = k < n && ext_slot_steady(k);

    // which effects are on screen this frame
    int uses[FX_N] = { 0 }, bub_leds[MAX_LEDS], nbl = 0;
    for (int i = 0; i < sc->count; i++) {
        int z = sc->leds[i].zone, e = fx == FX_OFF ? FX_OFF : zone_fx(z);
        if (zsteady[z] && e != FX_OFF) continue;   // still: needs no screen, sound or bubbles
        uses[e] = 1;
        if (e == FX_BUBBLES) bub_leds[nbl++] = i;
    }
    used_temp = uses[FX_TEMP];
    int aud = uses[FX_AUDIO] || (uses[FX_CUSTOM] && comp_audio);   // the constructor's sound-led layers use the same reading
    used_audio = aud;
    if (uses[FX_SCREEN]) screen_use();
    {
        // each zone's LED count and extent (the screen's mosaic / edge / average choice, single lights), and whether
        // the whole scene is one line
        float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
        for (int z = 0; z < MAX_ZONES; z++) { zn[z] = 0; zbox[z][0] = zbox[z][1] = 1e9f; zbox[z][2] = zbox[z][3] = -1e9f; }
        for (int i = 0; i < sc->count; i++) {
            const led_t *l = &sc->leds[i]; int z = l->zone;
            if (l->x < bx0) bx0 = l->x; if (l->y < by0) by0 = l->y; if (l->x > bx1) bx1 = l->x; if (l->y > by1) by1 = l->y;
            if (z < 0 || z >= MAX_ZONES) continue;
            zn[z]++;
            if (l->x < zbox[z][0]) zbox[z][0] = l->x; if (l->y < zbox[z][1]) zbox[z][1] = l->y;
            if (l->x > zbox[z][2]) zbox[z][2] = l->x; if (l->y > zbox[z][3]) zbox[z][3] = l->y;
        }
        scene_flat = bx1 - bx0 < 0.05f || by1 - by0 < 0.05f;
    }

    // a jump in time (sleep, clock change, a stalled frame) moves the effects on by at most a second
    if (!(dt >= 0)) dt = 0; else if (dt > 1) dt = 1;
    // effect clocks; the pump effect follows the flow sensor (0..1) when there is one
    for (int e = 0; e < FX_N; e++) {
        float spd = fx_speed[e];
        if (e == FX_PUMP && !isnan(sn->flow)) spd = 0.3f + 2.2f * clampf(sn->flow, 0, 1);
        fx_t[e] += dt * spd;
        // effects read their clock as a float: kept small so it stays fine-grained after days on
        // shortcut: one small jump every ~4.5 h of the effect's clock, a seamless wrap would need every effect's period
        if (fx_t[e] >= 16384) fx_t[e] -= 16384;
    }

    // an effect that was not on screen starts from no reading: no old value, no flash of old sound
    if (uses[FX_TEMP] && !was_temp) { temp_smooth = NAN; temp_gap = 0; }
    if (aud && !was_audio) { level_smooth = bass_smooth = peak_lv = peak_hold = bass_avg = since_sound = audio_in = 0; beat_age = 9; quiet_for = 99; beat_armed = 1; }
    was_temp = uses[FX_TEMP]; was_audio = aud;
    if (uses[FX_TEMP]) {
        float v = temp_water ? sn->water_temp : sn->gpu_temp;
        if (!isnan(v)) { temp_smooth = isnan(temp_smooth) ? v : temp_smooth + (v - temp_smooth) * clampf((float)dt * 0.8f, 0, 1); temp_gap = 0; }
        else temp_gap += (float)dt;
        temp_lost = isnan(temp_smooth) || temp_gap > 5;   // a short gap keeps the last value; a long one is said so
    }
    if (aud) {
        audio_have = !isnan(sn->audio_level);
        float g = powf(2, (P(P_AD_SENS) - 50) / 25);   // sensitivity: x0.25 .. x4
        float lv = audio_have ? fminf(sn->audio_level * g, 1) : 0, bs = audio_have && !isnan(sn->audio_bass) ? fminf(sn->audio_bass * g, 1) : 0;
        quiet_for = lv > 0.01f ? 0 : quiet_for + (float)dt;
        since_sound = quiet_for > 0.5f ? 0 : since_sound + (float)dt;
        float in = clampf(since_sound / 0.4f, 0, 1);    // sound starting (or the capture) comes in softly, no flash
        audio_in = in > audio_in ? in : audio_in + (in - audio_in) * clampf((float)dt * 4, 0, 1);
        // fast attack, slow release
        level_smooth += (lv * in - level_smooth) * clampf((float)dt * (lv * in > level_smooth ? 25.0f : 4.0f), 0, 1);
        bass_smooth  += (bs * in - bass_smooth)  * clampf((float)dt * (bs * in > bass_smooth ? 30.0f : 5.0f), 0, 1);
        // the level's peak: follows at once, holds a moment, then falls slowly
        if (level_smooth >= peak_lv) { peak_lv = level_smooth; peak_hold = 0.6f; }
        else if ((peak_hold -= (float)dt) < 0) peak_lv = fmaxf(peak_lv - (float)dt * 0.4f, level_smooth);
        // a beat: the bass jumping well above its own recent average (not a tempo: no BPM is worked out)
        bass_avg += (bs - bass_avg) * clampf((float)dt * 2, 0, 1);
        beat_age += (float)dt;
        // one beat per jump: the next needs the bass to have dropped back near its average first
        if (bs < bass_avg * 1.1f) beat_armed = 1;
        if (beat_armed && since_sound > 0.5f && bs > 0.2f && bs > bass_avg * (1.25f + 0.01f * (100 - P(P_AD_SENS))) && beat_age > P(P_AD_GAP) / 1000) { beat_age = 0; beat_n++; beat_armed = 0; }
        if (beat_age > 60) beat_age = 60;
    }
    {
        // bubbles rise in the "lanes" (distinct x positions) of the LEDs showing them
        float spd = fx_speed[FX_BUBBLES];
        if (nbl && nbub < 64 && rnd() < dt * spd * (2 + 2.0f * nbl / (sc->count ? sc->count : 1))) {
            const led_t *l = &sc->leds[bub_leds[(int)(rnd() * nbl) % nbl]];
            bubble b = { l->x, 1.15f, 0.25f + 0.2f * rnd(), 0.33f + 0.5f * rnd() };
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
    if (!loaded || fx != last_fx) { load_params(); if (fx != last_fx) nbub = 0; }   // zones following the main effect follow this one
    last_fx = fx;
    int idx[MAX_LEDS], n = 0, e0 = -1, mixed = 0, z0 = 0;
    for (int i = 0; i < sc->count; i++) {
        const led_t *l = &sc->leds[i];
        if (l->dev != dev || (zone >= 0 && l->zone != zone) || l->index >= stride) continue;
        int z = l->zone >= 0 && l->zone < MAX_ZONES ? l->zone : ZONE_GPU, e = fx == FX_OFF ? FX_OFF : zone_fx(z);
        if (e == FX_TEMP || e == FX_AUDIO || e == FX_SCREEN || (e == FX_CUSTOM && comp_audio)) return 0;   // live: streamed
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
    case FX_PENDULUM: period = 4; break;
    case FX_STEPS:   period = (P(P_ST_SEGMENTS) * 0.5 + P(P_ST_PAUSE)) * (np > 1 ? np : 2); break;
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
            if (nlb < 64 && rnd() < sub * bs * 4) {
                const led_t *l = &sc->leds[idx[(int)(rnd() * n) % n]];
                bubble b = { l->x, 1.15f, 0.25f + 0.2f * rnd(), 0.33f + 0.5f * rnd() };
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
