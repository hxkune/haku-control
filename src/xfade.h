// SPDX-License-Identifier: GPL-3.0-only
// The blend of a profile switch: each LED starts from its own colour in the last frame, found by what it is (its
// device, a LAN one by its [dev.N] id, and its number), not by where it sits in the scene, which moves when a device
// is switched on or off or the list changes order. An LED the old frame did not have just starts.
#pragma once
#include "common.h"
#include <string.h>

#define XF_HASH 4096   // twice MAX_LEDS, so the probe runs stay short
typedef struct { rgbf c[MAX_LEDS]; unsigned key[XF_HASH]; short at[XF_HASH]; } xfade_t;   // at: 1 + the LED in c, 0 free

// unit[zone]: what a zone stands for (a LAN device's id; the zone itself for the rest)
static unsigned xf_key(const led_t *l, const int *unit) {
    return (unsigned)l->dev << 24 | (unsigned)(unit[l->zone] & 0xfff) << 12 | (unsigned)(l->index & 0xfff);
}
static unsigned xf_slot(unsigned k) { return (k * 2654435761u) >> 20; }   // the top 12 bits: 0..XF_HASH-1

static void xf_take(xfade_t *x, const scene_t *sc, const int *unit, const rgbf *c) {
    memset(x->at, 0, sizeof(x->at));
    for (int i = 0; i < sc->count; i++) {
        unsigned k = xf_key(&sc->leds[i], unit), h = xf_slot(k);
        while (x->at[h] && x->key[h] != k) h = (h + 1) & (XF_HASH - 1);
        x->key[h] = k; x->at[h] = (short)(i + 1); x->c[i] = c[i];
    }
}

// this LED's colour in the old frame, NULL when it was not in it
static const rgbf *xf_from(const xfade_t *x, const led_t *l, const int *unit) {
    unsigned k = xf_key(l, unit);
    for (unsigned h = xf_slot(k); x->at[h]; h = (h + 1) & (XF_HASH - 1)) if (x->key[h] == k) return &x->c[x->at[h] - 1];
    return NULL;
}
