// SPDX-License-Identifier: GPL-3.0-only
// Checks for the profile-switch blend (src/xfade.h): each LED blends from its own old colour, whatever moved.
//   build.cmd's environment, then: cl /nologo tools\sim\xfade_test.c /Fe:%TEMP%\xfade_test.exe && %TEMP%\xfade_test.exe
#include "../../src/xfade.h"
#include <stdio.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static scene_t sc; static int unit[MAX_ZONES]; static xfade_t xf;
static void add(dev_id dev, int zone, int index) { led_t *l = &sc.leds[sc.count++]; l->dev = dev; l->zone = zone; l->index = index; }
static int from(dev_id dev, int zone, int index, float *r) {
    led_t l = { dev, index, zone }; const rgbf *c = xf_from(&xf, &l, unit);
    if (c) *r = c->r; return c != NULL;
}

int main(void) {
    for (int z = 0; z < MAX_ZONES; z++) unit[z] = z;
    // old profile: the GPU (3 LEDs), LAN device 7 in slot 0 (2 LEDs), LAN device 9 in slot 1 (2 LEDs)
    rgbf c[MAX_LEDS];
    add(DEV_GPU, ZONE_GPU, 0); add(DEV_GPU, ZONE_GPU, 1); add(DEV_GPU, ZONE_GPU, 2);
    unit[ZONE_EXT0] = 7; add(DEV_EXT, ZONE_EXT0, 0); add(DEV_EXT, ZONE_EXT0, 1);
    unit[ZONE_EXT0 + 1] = 9; add(DEV_EXT, ZONE_EXT0 + 1, 0); add(DEV_EXT, ZONE_EXT0 + 1, 1);
    for (int i = 0; i < sc.count; i++) c[i].r = c[i].g = c[i].b = (float)(i + 1);   // GPU 1..3, dev 7: 4, 5, dev 9: 6, 7
    xf_take(&xf, &sc, unit, c);

    // new profile: GPU off, device 7 off, device 9 now in slot 0, a new device 12 in slot 1
    unit[ZONE_EXT0] = 9; unit[ZONE_EXT0 + 1] = 12;
    float r = 0;
    CHECK(from(DEV_EXT, ZONE_EXT0, 1, &r) && r == 7, "a device that moved up the list blends from its own colour");
    CHECK(!from(DEV_EXT, ZONE_EXT0 + 1, 0, &r), "a device new to this profile just starts");
    CHECK(from(DEV_GPU, ZONE_GPU, 2, &r) && r == 3, "the GPU's LEDs by their number");
    CHECK(!from(DEV_BOARD, ZONE_GPU, 0, &r), "the board is not the GPU's first LED (same zone)");
    CHECK(!from(DEV_GPU, ZONE_GPU, 3, &r), "an LED past the old count just starts");

    // a big scene: every one found, none mixed up
    sc.count = 0; for (int z = 0; z < MAX_ZONES; z++) unit[z] = z;
    for (int k = 0; k < 4; k++) { unit[ZONE_EXT0 + k] = 100 + k; for (int i = 0; i < 500; i++) add(DEV_EXT, ZONE_EXT0 + k, i); }
    for (int i = 0; i < sc.count; i++) c[i].r = (float)i;
    xf_take(&xf, &sc, unit, c);
    int ok = 1;
    for (int k = 0; k < 4; k++) for (int i = 0; i < 500; i++) if (!from(DEV_EXT, ZONE_EXT0 + k, i, &r) || r != k * 500 + i) ok = 0;
    CHECK(ok, "2000 LEDs each find their own colour");

    printf(fails ? "xfade: %d FAILED\n" : "xfade: all ok\n", fails);
    return fails != 0;
}
