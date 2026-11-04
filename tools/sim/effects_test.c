// SPDX-License-Identifier: GPL-3.0-only
// Checks for the effects engine (src/effects.c), no devices: every effect on a strip, a ring, a 16x16 panel, a single
// lamp, no LEDs and 2048 LEDs gives colours in 0..1 (also with settings out of range or not numbers, a one-colour
// palette, odd frame times, a jump in time); the same run twice gives the same frames; 30 and 120 frames a second
// land on the same picture; the calm effects never jump between frames and loop without a seam; Off is black;
// a single lamp still shows the moving effects; temperature and sound say when there is no reading instead of
// faking one; the old effect ids are all still there. Prints the time a frame takes, and with a folder argument
// writes pictures of each effect (time running down) to look at; "dump <file>" writes strip colours for
// tools/sim/preview_check.js.
//   in build.cmd's environment:
//   cl /nologo /utf-8 /O2 /Isrc tools\sim\effects_test.c /Fe:%TEMP%\effects_test.exe && %TEMP%\effects_test.exe [out folder]
#define _CRT_SECURE_NO_WARNINGS
#include "../../src/effects.c"
#include <stdio.h>
#include <string.h>

// the engine as just started: effects_reset() alone keeps the clocks running (it is called on every settings change)
static void fresh(void) { effects_reset(); memset(fx_t, 0, sizeof(fx_t)); was_temp = was_audio = 0; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

// ---- what the engine needs from the rest of haku: settings in memory
static struct { char sec[32], key[32], val[256]; } kv[256];
static int nkv;
const char *cfg_get(const char *section, const char *key, const char *def) {
    for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].sec, section) && !strcmp(kv[i].key, key)) return kv[i].val[0] ? kv[i].val : def;
    return def;
}
float cfg_getf(const char *s, const char *k, float def) { const char *v = cfg_get(s, k, NULL); return v ? (float)atof(v) : def; }
int cfg_geti(const char *s, const char *k, int def) { const char *v = cfg_get(s, k, NULL); return v ? atoi(v) : def; }
void cfg_set(const char *section, const char *key, const char *value) {
    int i = 0;
    while (i < nkv && (strcmp(kv[i].sec, section) || strcmp(kv[i].key, key))) i++;
    if (i == nkv && nkv < 256) nkv++;
    strcpy_s(kv[i].sec, sizeof(kv[i].sec), section); strcpy_s(kv[i].key, sizeof(kv[i].key), key);
    strcpy_s(kv[i].val, sizeof(kv[i].val), value);
}
int cfg_palette(const char *section, rgbf *out, int max) {
    const char *v = cfg_get(section, "palette", NULL);
    int n = 0;
    while (v && n < max && (v = strchr(v, '#'))) {
        unsigned x = (unsigned)strtoul(++v, NULL, 16);
        out[n].r = (x >> 16 & 255) / 255.0f; out[n].g = (x >> 8 & 255) / 255.0f; out[n].b = (x & 255) / 255.0f; n++;
    }
    return n;
}
static int steady0;
int ext_count(void) { return 1; }
int ext_slot_steady(int k) { return k == 0 && steady0; }
int ext_slot_id(int k) { return k + 1; }
void screen_use(void) {}
int screen_area(float u0, float v0, float u1, float v1, rgbf *out) { (void)u0; (void)v0; (void)u1; (void)v1; out->r = 0.2f; out->g = 0.4f; out->b = 0.6f; return 1; }
int app_lang(void) { return 0; }
void logf_(const char *fmt, ...) { (void)fmt; }

static void reset_cfg(void) { nkv = 0; fresh(); }

// ---- scenes, as layout.c lays them out: x, y 0..1 (y = 0 at the top), path along the chain, fill inside the device
static scene_t sc;
static void strip(int n) {   // one addressable strip (an external device's zone)
    sc.count = n;
    for (int i = 0; i < n; i++) {
        float u = n > 1 ? (float)i / (n - 1) : 0;
        led_t l = { DEV_EXT, i, ZONE_EXT0, u, 0.5f, u, u, u }; sc.leds[i] = l;
    }
}
static void ring(int n) {    // a fan ring
    sc.count = n;
    for (int i = 0; i < n; i++) {
        float a = 6.2831853f * i / n, u = (float)i / n;
        led_t l = { DEV_EXT, i, ZONE_EXT0, 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a), u, u, u }; sc.leds[i] = l;
    }
}
static void panel(int w, int h) {   // a matrix, wired row by row (serpentine)
    sc.count = w * h;
    for (int i = 0; i < w * h; i++) {
        int r = i / w, c = r & 1 ? w - 1 - i % w : i % w;
        float u = (float)i / (w * h - 1);
        led_t l = { DEV_EXT, i, ZONE_EXT0, (float)c / (w - 1), (float)r / (h - 1), u, u, u }; sc.leds[i] = l;
    }
}
static void lamp(void) {     // a smart bulb: one light
    sc.count = 1;
    led_t l = { DEV_LIGHT, 0, ZONE_LIGHT0, 0.5f, 0.5f, 0, 0, 0 }; sc.leds[0] = l;
}

static sensors_t sn;
static void sensors_ok(void) { sn.gpu_temp = 55; sn.water_temp = 32; sn.flow = 0.5f; sn.audio_level = 0.5f; sn.audio_bass = 0.4f; sn.gpu_at = 1; }

static rgbf out[MAX_LEDS], prev[MAX_LEDS], ref[MAX_LEDS];
static int bounded(int n) {
    for (int i = 0; i < n; i++) if (!(out[i].r >= 0 && out[i].r <= 1 && out[i].g >= 0 && out[i].g <= 1 && out[i].b >= 0 && out[i].b <= 1)) return 0;
    return 1;
}
static float maxdiff(const rgbf *a, const rgbf *b, int n) {
    float m = 0;
    for (int i = 0; i < n; i++) m = fmaxf(m, fmaxf(fabsf(a[i].r - b[i].r), fmaxf(fabsf(a[i].g - b[i].g), fabsf(a[i].b - b[i].b))));
    return m;
}
static float maxlum(int n) { float m = 0; for (int i = 0; i < n; i++) m = fmaxf(m, fmaxf(out[i].r, fmaxf(out[i].g, out[i].b))); return m; }
// runs fx for secs at fps from a fresh start; ok stays 1 while every frame is in 0..1
static int run(int fx, double secs, double fps) {
    fresh();
    int ok = 1, frames = (int)(secs * fps + 0.5);
    for (int f = 0; f < frames; f++) { effects_render(fx, &sc, &sn, 1 / fps, out); ok &= bounded(sc.count); }
    return ok;
}

// ---- pictures: a 24-bit BMP, w x h cells of s px
static unsigned char img[1600 * 1600 * 3];
static void bmp(const char *dir, const char *name, int w, int h, int s) {
    char p[400]; snprintf(p, sizeof(p), "%s\\%s.bmp", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) return;
    int W = w * s, H = h * s, row = (W * 3 + 3) & ~3, size = 54 + row * H;
    unsigned char hd[54] = { 'B', 'M' };
    *(int *)(hd + 2) = size; *(int *)(hd + 10) = 54; *(int *)(hd + 14) = 40; *(int *)(hd + 18) = W; *(int *)(hd + 22) = -H;
    *(short *)(hd + 26) = 1; *(short *)(hd + 28) = 24;
    fwrite(hd, 1, 54, f);
    static unsigned char line[1600 * 3 + 4];
    for (int y = 0; y < H; y++) {
        memset(line, 0, sizeof(line));
        for (int x = 0; x < W; x++) { const unsigned char *c = img + ((y / s) * w + x / s) * 3; line[x * 3] = c[2]; line[x * 3 + 1] = c[1]; line[x * 3 + 2] = c[0]; }
        fwrite(line, 1, row, f);
    }
    fclose(f);
}
static void put(int w, int x, int y, rgbf c) { unsigned char *p = img + (y * w + x) * 3; p[0] = (unsigned char)(c.r * 255 + 0.5f); p[1] = (unsigned char)(c.g * 255 + 0.5f); p[2] = (unsigned char)(c.b * 255 + 0.5f); }
static void pictures(const char *dir) {
    sensors_ok();
    for (int fx = 0; fx < FX_N; fx++) {
        const char *id = g_effects[fx].id; char nm[64];
        // strip of 60 and ring of 24: one row per 1/15 s, 12 s
        int kinds[2] = { 60, 24 };
        for (int k = 0; k < 2; k++) {
            if (k == 0) strip(60); else ring(24);
            fresh();
            for (int row = 0; row < 180; row++) {
                effects_render(fx, &sc, &sn, 1 / 15.0, out);
                for (int i = 0; i < sc.count; i++) put(sc.count, i, row, out[i]);
            }
            snprintf(nm, sizeof(nm), "%s_%s%d", id, k ? "ring" : "strip", kinds[k]);
            bmp(dir, nm, sc.count, 180, 4);
        }
        // panel 16x16: 8 moments, 1 s apart, side by side
        panel(16, 16); fresh();
        for (int m = 0; m < 8; m++) {
            for (int f = 0; f < 15; f++) effects_render(fx, &sc, &sn, 1 / 15.0, out);
            for (int i = 0; i < sc.count; i++) put(16 * 8 + 7, m * 17 + (int)(sc.leds[i].x * 15 + 0.5f), (int)(sc.leds[i].y * 15 + 0.5f), out[i]);
        }
        for (int m = 0; m < 7; m++) for (int y = 0; y < 16; y++) { rgbf g = { 0.15f, 0.15f, 0.15f }; put(16 * 8 + 7, m * 17 + 16, y, g); }
        snprintf(nm, sizeof(nm), "%s_panel16", id); bmp(dir, nm, 16 * 8 + 7, 16, 8);
        // a lamp: its colour over 12 s
        lamp(); fresh();
        for (int row = 0; row < 180; row++) { effects_render(fx, &sc, &sn, 1 / 15.0, out); for (int x = 0; x < 8; x++) put(8, x, row, out[0]); }
        snprintf(nm, sizeof(nm), "%s_lamp", id); bmp(dir, nm, 8, 180, 3);
    }
    printf("     pictures in %s\n", dir);
}

// ---- "dump <file>": colours of a 28-LED strip for a set of effects, settings and moments, as JSON, for
// tools/sim/preview_check.js to compare with the window's previews (ui/fxpreview.js)
static void dump(const char *file) {
    static const char *cases[][9] = {   // effect, then key/value pairs
        { "flow" }, { "caustic" }, { "comet" }, { "lava" }, { "rainbow" }, { "ocean" }, { "plasma" }, { "ripple" }, { "pump" },
        { "wave" }, { "wave", "dir", "left", "size", "3" }, { "wave", "dir", "out" }, { "wave", "dir", "in", "size", "1" },
        { "breathe" }, { "breathe", "depth", "0" }, { "breathe", "single", "1", "wave", "60" }, { "breathe", "wave", "30", "depth", "20" },
        { "breathe", "palette", "#7A5CFF", "cycle", "8" },
        { "pendulum" }, { "pendulum", "motion", "1" }, { "pendulum", "mirror", "1", "tail", "40", "width", "24" }, { "pendulum", "tail", "0", "soft", "0" },
        { "twinkle" }, { "twinkle", "density", "100", "halo", "0" }, { "twinkle", "life", "6", "variety", "60", "bg", "20" },
        { "meteor" }, { "meteor", "dir", "1" }, { "meteor", "dir", "2", "spread", "100", "rate", "20" }, { "meteor", "tail", "60" },
        { "interference" }, { "interference", "scale", "20", "contrast", "100" },
        { "aurora", "layers", "1" }, { "aurora" }, { "aurora", "layers", "3" },
        { "steps" }, { "steps", "dir", "1" }, { "steps", "dir", "2", "hard", "1" }, { "steps", "segments", "16", "pause", "0" },
        { "steps", "palette", "#FF3060" },
        { "fire" }, { "fire", "height", "30" }, { "candle" }, { "candle", "flicker", "0" }, { "candle", "flicker", "100" },
        { "matrix" }, { "matrix", "style", "1" }, { "static" }, { "off" },
        // the constructor: the reactor (its default), the dot mask over a moving gradient, and every shape and blend
        { "custom" },
        { "custom", "l1", "s:grad c:p v:20", "l2", "s:dots n:7 w:40 sf:30 b:mask" },
        { "custom", "l1", "s:wave c:p sz:250 v:-30 a:90", "l2", "s:noise c:#ff8000 sz:180 v:40 b:mul o:70", "l3", "s:fill c:p2 o:30 b:screen" },
        { "custom", "l1", "s:ring c:p1 x:20 dur:1.5 per:2.5 w:30 sf:0 fade:0", "l2", "s:spark c:p n:40 life:2 tail:30 v:-60 w:20 b:add", "l3", "s:dots n:5 w:50 inv:1 b:mask o:80" },
        { "custom", "l1", "s:grad c:p ap:sz aa:80 at:3", "l2", "s:wave c:p1 ap:o aa:-60 at:5 b:add", "l3", "s:fill c:p0 hide:1" },
        { "custom", "palette", "#FF0000, #00FF00", "l1", "s:spark c:p0 n:60 life:0.5 tail:0 v:0 w:100 b:screen" },
    };
    static const double T[] = { 0.37, 7.3, 41.9, 123.45 };
    FILE *f = fopen(file, "w"); if (!f) { printf("FAIL cannot write %s\n", file); fails++; return; }
    fputs("[\n", f);
    int first = 1;
    for (int c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++) {
        const char *id = cases[c][0]; int fx = effect_index(id);
        for (int k = 0; k < 4; k++) {
            reset_cfg(); strip(28);
            for (int j = 1; j < 9 && cases[c][j]; j += 2) cfg_set(id, cases[c][j], cases[c][j + 1]);
            fresh(); effects_render(fx, &sc, &sn, 0, out);
            fx_t[fx] = T[k];
            fprintf(f, "%s{\"id\":\"%s\",\"p\":{", first ? "" : ",\n", id); first = 0;
            for (int j = 1; j < 9 && cases[c][j]; j += 2) fprintf(f, "%s\"%s\":\"%s\"", j > 1 ? "," : "", cases[c][j], cases[c][j + 1]);
            fprintf(f, "},\"t\":%.5f,\"pal\":[", T[k]);
            for (int j = 0; j < fx_npal[fx]; j++) { rgbf q = fx_pal[fx][j]; fprintf(f, "%s[%.5f,%.5f,%.5f]", j ? "," : "", q.r, q.g, q.b); }
            fputs("],\"c\":[", f);
            for (int i = 0; i < sc.count; i++) { rgbf q = led_color(fx, &sc.leds[i], fx_t, NULL, 0); fprintf(f, "%s[%.5f,%.5f,%.5f]", i ? "," : "", q.r, q.g, q.b); }
            fputs("]}", f);
        }
    }
    fputs("\n]\n", f); fclose(f);
    printf("     dump in %s\n", file);
}

int main(int argc, char **argv) {
    char what[200];
    if (argc > 2 && !strcmp(argv[1], "dump")) { sensors_ok(); dump(argv[2]); return fails; }
    reset_cfg(); sensors_ok();

    // ---- the ids saved settings and profiles use are all still there, and Off is last (the hotkey cycle needs it)
    static const char *old_ids[] = { "flow", "breathe", "comet", "rainbow", "wave", "fire", "ocean", "twinkle", "meteor", "plasma",
                                     "aurora", "ripple", "matrix", "candle", "screen", "temperature", "audio", "pump", "bubbles", "lava",
                                     "caustic", "static", "off" };
    int all = 1;
    for (int i = 0; i < (int)(sizeof(old_ids) / sizeof(old_ids[0])); i++) {
        int k = effect_index(old_ids[i]);
        if (k < 0 || k >= g_effect_count || strcmp(g_effects[k].id, old_ids[i])) { all = 0; printf("     missing: %s\n", old_ids[i]); }
    }
    CHECK(all, "every effect id saved settings may name is still there");
    CHECK(!strcmp(g_effects[g_effect_count - 1].id, "off") && g_effect_count == FX_N, "off is the last effect");
    CHECK(effect_index("pendulum") == FX_PENDULUM && effect_index("interference") == FX_INTERF && effect_index("steps") == FX_STEPS, "the new effects are found by id");

    // ---- every effect, every shape: colours in 0..1
    void (*shapes[])(void) = { NULL };
    (void)shapes;
    const char *shape_name[] = { "strip of 60", "strip of 2", "ring of 24", "16x16 panel", "single lamp", "2048 LEDs" };
    for (int s = 0; s < 6; s++) {
        int ok = 1;
        for (int fx = 0; fx < FX_N; fx++) {
            switch (s) { case 0: strip(60); break; case 1: strip(2); break; case 2: ring(24); break; case 3: panel(16, 16); break; case 4: lamp(); break; default: panel(64, 32); }
            if (!run(fx, s == 5 ? 2 : 20, 30)) { ok = 0; printf("     %s out of range\n", g_effects[fx].id); }
        }
        snprintf(what, sizeof(what), "every effect on a %s stays in 0..1", shape_name[s]); CHECK(ok, what);
    }
    {
        sc.count = 0; int ok = 1;
        for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 1, 30);
        CHECK(ok, "no LEDs at all: nothing breaks");
    }

    // ---- settings out of range or not numbers, a one- and an eight-colour palette, odd frame times
    {
        const char *bad[] = { "1e30", "-1e30", "nan", "abc", "inf", "-0" };
        int ok = 1;
        strip(60);
        for (int b = 0; b < 6; b++) {
            reset_cfg();
            for (int i = 0; i < P_N; i++) cfg_set(g_effects[FXP[i].fx].id, FXP[i].key, bad[b]);
            for (int fx = 0; fx < FX_N; fx++) { cfg_set(g_effects[fx].id, "speed", bad[b]); ok &= run(fx, 10, 30); }
            fresh(); effects_render(FX_FLOW, &sc, &sn, 1 / 30.0, out);
            for (int i = 0; i < P_N; i++) if (!(pv[i] >= FXP[i].lo && pv[i] <= FXP[i].hi)) { ok = 0; printf("     %s.%s=%s read as %g\n", g_effects[FXP[i].fx].id, FXP[i].key, bad[b], pv[i]); }
        }
        CHECK(ok, "settings huge, negative, not a number or not numeric: clamped, colours in 0..1");
        reset_cfg();
        for (int i = 0; i < P_N; i++) { char v[32]; snprintf(v, sizeof(v), "%g", FXP[i].hi); cfg_set(g_effects[FXP[i].fx].id, FXP[i].key, v); }
        ok = 1; for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 10, 30);
        reset_cfg();
        for (int i = 0; i < P_N; i++) { char v[32]; snprintf(v, sizeof(v), "%g", FXP[i].lo); cfg_set(g_effects[FXP[i].fx].id, FXP[i].key, v); }
        for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 10, 30);
        CHECK(ok, "every setting at its smallest and at its largest: colours in 0..1");
        reset_cfg(); ok = 1;
        for (int fx = 0; fx < FX_N; fx++) cfg_set(g_effects[fx].id, "palette", "#FF0000");
        for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 10, 30);
        for (int fx = 0; fx < FX_N; fx++) cfg_set(g_effects[fx].id, "palette", "#FF0000, #00FF00, #0000FF, #FFFF00, #FF00FF, #00FFFF, #FFFFFF, #808080");
        fresh();
        for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 10, 30);
        for (int fx = 0; fx < FX_N; fx++) cfg_set(g_effects[fx].id, "palette", "garbage");
        fresh();
        for (int fx = 0; fx < FX_N; fx++) ok &= run(fx, 5, 30);
        CHECK(ok, "one colour, eight colours, an unreadable palette: colours in 0..1");
        reset_cfg(); ok = 1;
        for (int fx = 0; fx < FX_N; fx++) {
            fresh();
            double dts[] = { 0, 1e-6, 0.25, 0.001, 5, 0.25, 1e5, 1 / 60.0, 3e7, 0 };
            for (int k = 0; k < 10; k++) { effects_render(fx, &sc, &sn, dts[k], out); ok &= bounded(sc.count); }
        }
        CHECK(ok, "frame times of 0, a microsecond, 5 s, a day, a year (a time jump): colours in 0..1");
    }

    // ---- the same run twice gives the same frames (bubbles too: their chance is the engine's own)
    {
        strip(60); reset_cfg(); int ok = 1;
        for (int fx = 0; fx < FX_N; fx++) {
            run(fx, 8, 30); memcpy(ref, out, sizeof(rgbf) * sc.count);
            run(fx, 8, 30);
            if (maxdiff(ref, out, sc.count) != 0) { ok = 0; printf("     %s differs\n", g_effects[fx].id); }
        }
        CHECK(ok, "the same run twice: the same frames, every effect");
    }

    // ---- 30 and 120 frames a second end on the same picture (time drives the effects, not the frame count)
    {
        strip(60); reset_cfg(); int ok = 1;
        for (int fx = 0; fx < FX_N; fx++) {
            if (fx == FX_BUBBLES || fx == FX_OFF) continue;   // bubbles are born by chance each frame: same rate, not the same bubbles
            run(fx, 7.3, 30); memcpy(ref, out, sizeof(rgbf) * sc.count);
            run(fx, 7.3, 120);
            float d = maxdiff(ref, out, sc.count);
            if (d > 0.02f) { ok = 0; printf("     %s: %.3f\n", g_effects[fx].id, d); }
        }
        CHECK(ok, "30 and 120 frames a second: the same picture at 7.3 s (within 2%), every effect but bubbles");
    }

    // ---- the calm and new effects never jump between two frames, and the looping ones have no seam
    {
        int calm[] = { FX_BREATHE, FX_AURORA, FX_TWINKLE, FX_CANDLE, FX_INTERF, FX_PENDULUM, FX_STEPS, FX_LAVA };
        float limit[] = { 0.03f, 0.06f, 0.06f, 0.12f, 0.12f, 0.15f, 0.12f, 0.06f };
        strip(60); reset_cfg();
        for (int k = 0; k < 8; k++) {
            fresh(); float worst = 0;
            effects_render(calm[k], &sc, &sn, 1 / 60.0, out);
            for (int f = 0; f < 60 * 40; f++) {
                memcpy(prev, out, sizeof(rgbf) * sc.count);
                effects_render(calm[k], &sc, &sn, 1 / 60.0, out);
                worst = fmaxf(worst, maxdiff(prev, out, sc.count));
            }
            snprintf(what, sizeof(what), "%s at 60 fps: no step over %.2f between frames over 40 s (worst %.3f)", g_effects[calm[k]].id, limit[k], worst);
            CHECK(worst <= limit[k], what);
        }
        // breathe at each depth: a breath per colour and back to the start after them all
        int ok = 1;
        cfg_set("breathe", "palette", "#FF0000, #00FF00, #0000FF");
        const char *depths[] = { "0", "50", "100" };
        for (int d = 0; d < 3; d++) {
            cfg_set("breathe", "depth", depths[d]); fresh();
            effects_render(FX_BREATHE, &sc, &sn, 0.37, out); memcpy(ref, out, sizeof(rgbf) * sc.count);
            for (int f = 0; f < 300; f++) effects_render(FX_BREATHE, &sc, &sn, 3 / 0.35 / 300, out);
            if (maxdiff(ref, out, sc.count) > 1e-3f) { ok = 0; printf("     depth %s: %.4f\n", depths[d], maxdiff(ref, out, sc.count)); }
        }
        CHECK(ok, "breathe: after one breath per colour it is exactly where it began (no seam), at depth 0, 50, 100");
        cfg_set("breathe", "depth", "100"); cfg_set("breathe", "single", "1"); fresh();
        float lo = 9, hi = 0;
        for (int f = 0; f < 200; f++) { effects_render(FX_BREATHE, &sc, &sn, 0.03, out); lo = fminf(lo, maxlum(sc.count)); hi = fmaxf(hi, maxlum(sc.count)); }
        CHECK(lo < 0.02f && hi > 0.98f, "breathe at full depth: from dark to full");
        cfg_set("breathe", "cycle", "10"); fresh(); for (int f = 0; f < 50; f++) effects_render(FX_BREATHE, &sc, &sn, 0.1, out);   // (a frame moves time on by 1 s at most)
        CHECK(maxlum(sc.count) > 0.98f, "breathe with a 10 s cycle: brightest after 5 s");
        reset_cfg();
    }

    // ---- a single lamp still shows the effects that move along a chain
    {
        lamp(); reset_cfg();
        int moving[] = { FX_METEOR, FX_PENDULUM, FX_TWINKLE, FX_STEPS, FX_COMET, FX_BREATHE };
        for (int k = 0; k < 6; k++) {
            fresh(); float lo = 9, hi = 0;
            for (int f = 0; f < 30 * 60; f++) { effects_render(moving[k], &sc, &sn, 1 / 30.0, out); float m = maxlum(1); lo = fminf(lo, m); hi = fmaxf(hi, m); }
            snprintf(what, sizeof(what), "%s on a single lamp: it changes (%.2f .. %.2f over a minute)", g_effects[moving[k]].id, lo, hi);
            CHECK(hi - lo > 0.3f, what);
        }
        fresh(); int lit = 0;
        for (int f = 0; f < 300; f++) { effects_render(FX_TWINKLE, &sc, &sn, 1 / 30.0, out); lit += maxlum(1) > 0.5f; }
        CHECK(lit > 100, "constellation on a single lamp: lit most of the time, not dark waiting for a rare star");
    }

    // ---- constellation: at the density asked, every star fades in and out
    {
        strip(300); reset_cfg(); cfg_set("twinkle", "halo", "0"); cfg_set("twinkle", "bg", "0"); fresh();
        double lit = 0; int frames = 0;
        for (int f = 0; f < 30 * 60; f++) {
            effects_render(FX_TWINKLE, &sc, &sn, 1 / 30.0, out);
            if (f % 10) continue;
            int n = 0; for (int i = 0; i < sc.count; i++) n += fmaxf(out[i].r, fmaxf(out[i].g, out[i].b)) > 0.02f;
            lit += (double)n / sc.count; frames++;
        }
        lit /= frames;
        snprintf(what, sizeof(what), "constellation 35%%: about a third of the LEDs hold a star (%.0f%%)", lit * 100);
        CHECK(lit > 0.25 && lit < 0.42, what);
    }

    // ---- meteors: the shower comes at about the rate asked, and stops at the top of a slow lane
    {
        strip(120); reset_cfg(); cfg_set("meteor", "spread", "0"); cfg_set("meteor", "bg", "0"); cfg_set("meteor", "rate", "10"); fresh();
        int heads = 0, was = 0;
        for (int f = 0; f < 60 * 60; f++) {
            effects_render(FX_METEOR, &sc, &sn, 1 / 60.0, out);
            int on = fmaxf(out[0].r, fmaxf(out[0].g, out[0].b)) > 0.5f;   // a meteor passing the first LED
            heads += on && !was; was = on;
        }
        snprintf(what, sizeof(what), "meteor shower at 10 per 10 s: %d passed the first LED in 60 s (lanes cap it near 60)", heads);
        CHECK(heads >= 30 && heads <= 64, what);
    }

    // ---- Off is black; a device that holds one steady colour shows one still colour
    {
        strip(60); reset_cfg(); run(FX_OFF, 2, 30);
        CHECK(maxlum(sc.count) == 0, "off: every LED black");
        steady0 = 1; fresh();
        effects_render(FX_RAINBOW, &sc, &sn, 0.1, out); memcpy(ref, out, sizeof(rgbf) * sc.count);
        int still = 1;
        for (int f = 0; f < 100; f++) { effects_render(FX_RAINBOW, &sc, &sn, 0.1, out); still &= maxdiff(ref, out, sc.count) == 0; }
        CHECK(still && maxlum(sc.count) > 0.1f, "a steady-only device: one still colour, not a stream of changes");
        steady0 = 0;
    }

    // ---- temperature: a short gap keeps the last reading, a lost sensor is shown as such, never as 0 degrees
    {
        strip(60); reset_cfg(); sensors_ok();
        cfg_set("temperature", "palette", "#0000FF, #FF0000");
        run(FX_TEMP, 3, 30); memcpy(ref, out, sizeof(rgbf) * sc.count);
        CHECK(effects_temp_state() == 1, "temperature with a reading: state 'reading'");
        sn.gpu_temp = NAN;
        for (int f = 0; f < 90; f++) effects_render(FX_TEMP, &sc, &sn, 1 / 30.0, out);
        CHECK(effects_temp_state() == 1 && maxdiff(ref, out, sc.count) < 0.25f, "temperature, 3 s without a reading: keeps the last colour");
        for (int f = 0; f < 90; f++) effects_render(FX_TEMP, &sc, &sn, 1 / 30.0, out);
        CHECK(effects_temp_state() == 0 && maxlum(sc.count) < 0.15f, "temperature, 6 s without a reading: 'no reading', a dim grey");
        int grey = 1; for (int i = 0; i < sc.count; i++) grey &= fabsf(out[i].r - out[i].g) < 0.02f;
        CHECK(grey, "...grey, not the cold colour of a made-up low temperature");
        sn.gpu_temp = NAN; run(FX_TEMP, 1, 30);
        CHECK(effects_temp_state() == 0, "temperature never read: 'no reading' at once");
        sensors_ok(); run(FX_TEMP, 1, 30);
        CHECK(effects_temp_state() == 1, "the sensor back: 'reading' again");
    }

    // ---- sound: no capture, silence, sound starting softly, a beat not faster than the gap
    {
        strip(60); reset_cfg(); sensors_ok();
        sn.audio_level = sn.audio_bass = NAN; run(FX_AUDIO, 2, 30);
        snprintf(what, sizeof(what), "sound effect, no capture: state 'no signal' (%d), dim (%.2f), no flash", effects_audio_state(), maxlum(sc.count));
        CHECK(effects_audio_state() == -1 && maxlum(sc.count) < 0.1f, what);
        sn.audio_level = sn.audio_bass = 0; run(FX_AUDIO, 3, 30);
        snprintf(what, sizeof(what), "sound effect in silence: state 'silent' (%d), stays dim (%.2f)", effects_audio_state(), maxlum(sc.count));
        CHECK(effects_audio_state() == 0 && maxlum(sc.count) < 0.1f, what);
        // sound arriving at full level after silence
        sn.audio_level = 1; sn.audio_bass = 1;
        effects_render(FX_AUDIO, &sc, &sn, 1 / 30.0, out); float first = maxlum(sc.count);
        for (int f = 0; f < 30; f++) effects_render(FX_AUDIO, &sc, &sn, 1 / 30.0, out);
        snprintf(what, sizeof(what), "sound starting at full: the first frame dim (%.2f), full after a second (%.2f)", first, maxlum(sc.count));
        CHECK(first < 0.3f && maxlum(sc.count) > 0.9f && effects_audio_state() == 1, what);
        // a loud signal then the capture lost: back to dim
        sn.audio_level = sn.audio_bass = NAN;
        for (int f = 0; f < 60; f++) effects_render(FX_AUDIO, &sc, &sn, 1 / 30.0, out);
        snprintf(what, sizeof(what), "capture lost while loud: 'no signal' (%d), dim within 2 s (%.2f)", effects_audio_state(), maxlum(sc.count));
        CHECK(effects_audio_state() == -1 && maxlum(sc.count) < 0.1f, what);
        // beats: bass hits 20 times a second, the gap (250 ms) lets at most 4 a second through
        reset_cfg(); cfg_set("audio", "mode", "1"); fresh();
        int b0 = beat_n;
        for (int f = 0; f < 600; f++) {
            sn.audio_level = 0.6f; sn.audio_bass = (f / 3) & 1 ? 0.9f : 0.1f;   // 60 fps: on and off every 50 ms
            effects_render(FX_AUDIO, &sc, &sn, 1 / 60.0, out);
        }
        snprintf(what, sizeof(what), "beat with hits every 100 ms: %d in 10 s, never closer than the 250 ms gap (40 at most)", beat_n - b0);
        CHECK(beat_n - b0 >= 20 && beat_n - b0 <= 40, what);
        b0 = beat_n;
        for (int f = 0; f < 600; f++) { sn.audio_level = 0.6f; sn.audio_bass = (f % 30) < 3 ? 0.9f : 0.05f; effects_render(FX_AUDIO, &sc, &sn, 1 / 60.0, out); }
        snprintf(what, sizeof(what), "beat with a hit every half second: %d in 10 s (20 hits)", beat_n - b0);
        CHECK(beat_n - b0 >= 18 && beat_n - b0 <= 20, what);
        b0 = beat_n;
        for (int f = 0; f < 600; f++) { sn.audio_level = 0.6f; sn.audio_bass = 0.6f; effects_render(FX_AUDIO, &sc, &sn, 1 / 60.0, out); }
        snprintf(what, sizeof(what), "a steady bass (no hits): no beats (%d)", beat_n - b0);
        CHECK(beat_n - b0 <= 1, what);
        reset_cfg(); sensors_ok();
    }

    // ---- the constructor: layers made of plain shapes, settings checked, the sound only when a layer asks for it
    {
        int fx = effect_index("custom"), ok = 1;
        CHECK(fx == FX_CUSTOM, "the constructor is found by its id");
        // nothing made yet: the reactor, not black
        panel(16, 16); reset_cfg(); fresh();
        effects_render(fx, &sc, &sn, 0.4, out);
        int corner = 0, centre = 8 * 16 + 8;
        snprintf(what, sizeof(what), "nothing made: the reactor shows (dark blue corner %.2f %.2f %.2f, the ring lit %.2f)", out[corner].r, out[corner].g, out[corner].b, maxlum(sc.count));
        CHECK(ncl == 3 && out[corner].b > 0.1f && out[corner].b < 0.3f && out[corner].r < 0.05f && maxlum(sc.count) > 0.4f, what);
        (void)centre;
        // the ring grows from the middle over 2 s: at 0.4 s lit near the middle, at 1.6 s lit near the edge, gone at 2.5 s
        {
            float r1 = 0, r2 = 0, l1 = 0, l2 = 0;
            for (int f = 0; f < 3; f++) {
                fresh(); effects_render(fx, &sc, &sn, 0, out); cl[2].p[LP_HIDE] = 1;   // the sparks off: the ring alone
                fx_t[fx] = f == 0 ? 0.4 : f == 1 ? 1.6 : 2.5;
                for (int i = 0; i < sc.count; i++) {
                    const led_t *l = &sc.leds[i]; rgbf q = led_color(fx, l, fx_t, NULL, 0);
                    float d = sqrtf((l->x - 0.5f) * (l->x - 0.5f) + (l->y - 0.5f) * (l->y - 0.5f)), g = q.g;   // the ring is cyan, the sky has no green
                    if (f == 0 && g > l1) { l1 = g; r1 = d; }
                    if (f == 1 && g > l2) { l2 = g; r2 = d; }
                    if (f == 2 && d > 0.05f && d < 0.95f && g > 0.12f) ok = 0;   // only sparks may be lit then
                }
            }
            snprintf(what, sizeof(what), "the ring grows (brightest at %.2f then %.2f from the middle) and fades (%.2f then %.2f), gone between rings", r1, r2, l1, l2);
            CHECK(r1 < 0.2f && r2 > 0.3f && r2 < 0.5f && l1 > l2 && l2 > 0.05f && ok, what);
        }
        // the gradient under a dot mask: every other step dark, the lit ones change colour as the gradient moves
        strip(42); reset_cfg();
        cfg_set("custom", "l1", "s:grad c:p v:25"); cfg_set("custom", "l2", "s:dots n:14 w:30 sf:0 b:mask"); fresh();
        effects_render(fx, &sc, &sn, 0, out); memcpy(ref, out, sizeof(rgbf) * sc.count);
        int dark = 0, lit = 0;
        for (int i = 0; i < sc.count; i++) { float m = fmaxf(out[i].r, fmaxf(out[i].g, out[i].b)); dark += m < 0.02f; lit += m > 0.3f; }
        for (int f = 0; f < 30; f++) effects_render(fx, &sc, &sn, 1 / 30.0, out);
        float moved = maxdiff(ref, out, sc.count);
        snprintf(what, sizeof(what), "a dot mask over a moving gradient: %d dark and %d lit of 42, the lit ones move on (%.2f in 1 s)", dark, lit, moved);
        CHECK(dark >= 20 && lit >= 8 && moved > 0.1f, what);
        // garbage, out of range, huge and missing values in every layer: clamped, 0..1, on every shape
        const char *bad[] = { "s:ring x:1e30 y:-1e30 dur:nan per:0 w:abc sf:inf sz:-0 b:mask inv:7",
                              "s:spark n:0 life:1e9 tail:-5 v:1e30 w:0 c:#zzzzzz",
                              "s:dots n:1e30 w:nan c:p9 b:mul ap:v aa:1e30 at:0",
                              "s:noise sz:nan v:-1e30 mp:o ma:1e30 ms:beat b:screen",
                              "s:wave sz:1e30 a:-90 c:#ffffff00 o:200",
                              "s:grad v:200 ap:a aa:400 at:0.2 b:add",
                              "s:unknown x:50", "no colons at all ::: :: :" };
        reset_cfg();
        for (int i = 0; i < 8; i++) { char k[4]; snprintf(k, sizeof(k), "l%d", i + 1); cfg_set("custom", k, bad[i]); }
        ok = 1;
        for (int sh = 0; sh < 5; sh++) {
            switch (sh) { case 0: strip(60); break; case 1: ring(24); break; case 2: panel(16, 16); break; case 3: lamp(); break; default: strip(2); }
            ok &= run(fx, 10, 30);
        }
        for (int i = 0; i < ncl; i++) for (int k = 0; k < LP_NUM; k++) if (!(cl[i].p[k] >= LPD[k].lo && cl[i].p[k] <= LPD[k].hi)) { ok = 0; printf("     layer %d %s = %g\n", i + 1, LPD[k].key, cl[i].p[k]); }
        snprintf(what, sizeof(what), "layers with garbage, huge, NaN and unknown words: %d of 8 kept (unknown shapes dropped), all clamped, colours in 0..1", ncl);
        CHECK(ok && ncl == 6, what);
        // a line longer than a config line: cut, not overrun
        { static char lng[2000]; memset(lng, 'x', sizeof(lng) - 1); memcpy(lng, "s:fill c:p0 ", 12); clayer L; CHECK(comp_parse(lng, &L) && L.col == 0, "a 2000-character layer line: read up to its limit, nothing overrun"); }
        // the sound: only a layer that asks for it turns the capture on, and it is then live (not baked)
        strip(12); reset_cfg(); fresh(); effects_render(fx, &sc, &sn, 1 / 30.0, out);
        int quiet = effects_need_audio();
        cfg_set("custom", "l1", "s:fill c:p0 o:20 mp:o ma:80 ms:level"); fresh();
        sensors_ok(); sn.audio_level = 0;
        for (int f = 0; f < 30; f++) effects_render(fx, &sc, &sn, 1 / 30.0, out);
        float low = maxlum(sc.count);
        sn.audio_level = 0.9f;
        for (int f = 0; f < 60; f++) effects_render(fx, &sc, &sn, 1 / 30.0, out);
        float high = maxlum(sc.count);
        for (int i = 0; i < sc.count; i++) { sc.leds[i].dev = DEV_NANO; sc.leds[i].zone = ZONE_NANO0; }
        static rgbf fr[240 * 12]; float st = 0;
        int baked = effects_bake(fx, &sc, DEV_NANO, ZONE_NANO0, 240, 12, fr, &st);
        snprintf(what, sizeof(what), "a layer led by the sound: capture off without one (%d), on with it (%d), brighter when loud (%.2f -> %.2f), streamed not baked (%d)", quiet, effects_need_audio(), low, high, baked);
        CHECK(!quiet && effects_need_audio() && high > low + 0.3f && baked == 0, what);
        sensors_ok(); reset_cfg();
    }

    // ---- bubbles stay within their list however long they run
    {
        panel(16, 16); reset_cfg(); int ok = 1;
        cfg_set("bubbles", "speed", "20"); fresh();
        for (int f = 0; f < 30 * 120; f++) { effects_render(FX_BUBBLES, &sc, &sn, 1 / 30.0, out); ok &= nbub <= 64; }
        CHECK(ok, "bubbles, fast, two minutes: never more than 64 alive");
    }

    // ---- a device's baked loop (static-only and on-device loops): the last frame leads into the first
    {
        reset_cfg(); strip(12);
        for (int i = 0; i < sc.count; i++) { sc.leds[i].dev = DEV_NANO; sc.leds[i].zone = ZONE_NANO0; }
        static rgbf frames[240 * 12]; float step = 0;
        int loops[] = { FX_BREATHE, FX_PENDULUM, FX_STEPS, FX_FLOW };
        for (int k = 0; k < 4; k++) {
            int nf = effects_bake(loops[k], &sc, DEV_NANO, ZONE_NANO0, 240, 12, frames, &step), seam = nf > 1;
            // the seam against the steps next to it (the motion there), not the loop's biggest step
            float nb = nf > 2 ? fmaxf(maxdiff(frames, frames + 12, 12), maxdiff(frames + (nf - 2) * 12, frames + (nf - 1) * 12, 12)) : 0;
            float wrap = nf > 1 ? maxdiff(frames + (nf - 1) * 12, frames, 12) : 1;
            snprintf(what, sizeof(what), "%s baked into %d frames: the loop's seam (%.3f) no bigger than the steps next to it (%.3f)", g_effects[loops[k]].id, nf, wrap, nb);
            CHECK(seam && wrap <= nb * 1.5f + 0.02f, what);
        }
    }

    // ---- the time a frame takes (this PC, /O2): every effect, three sizes
    {
        reset_cfg(); sensors_ok();
        int sizes[] = { 60, 300, 2048 };
        printf("     time per frame, microseconds (strip of 60 / 300 LEDs / 64x32 panel):\n");
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double worst[3] = { 0 };
        for (int fx = 0; fx < FX_N; fx++) {
            printf("     %-13s", g_effects[fx].id);
            for (int s = 0; s < 3; s++) {
                if (s == 2) panel(64, 32); else strip(sizes[s]);
                fresh(); effects_render(fx, &sc, &sn, 1 / 30.0, out);
                LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
                int n = s == 2 ? 100 : 400;
                for (int f = 0; f < n; f++) effects_render(fx, &sc, &sn, 1 / 30.0, out);
                QueryPerformanceCounter(&b);
                double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / fq.QuadPart / n;
                worst[s] = fmax(worst[s], us);
                printf(" %8.1f", us);
            }
            printf("\n");
        }
        snprintf(what, sizeof(what), "the slowest effect at 2048 LEDs takes %.2f ms a frame (under 8 ms, a quarter of a 30 fps frame)", worst[2] / 1000);
        CHECK(worst[2] < 8000, what);
    }

    if (argc > 1) pictures(argv[1]);
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
