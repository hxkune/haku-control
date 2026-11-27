// SPDX-License-Identifier: GPL-3.0-only
// The first-run guide's "Evening light": a few zones show warm white for a while, over whatever they show, also when
// the lights are off. Nothing is saved while it shows: it ends by itself (PV_MS, so a closed window cannot leave it
// on), by "back", by "keep" (which then saves it), or when the light is changed by hand from anywhere. A "back" or
// "keep" that comes after that finds nothing to end and changes nothing.
// Main thread starts and ends it; the render thread reads it (one 64-bit mask, written last on start, first on end).
#pragma once
#include "common.h"

#define PV_MS      45000
#define PV_KELVIN  2700
#define PV_LEVEL   0.55f   // also what "keep" saves: [zone.X] brightness = 55

typedef struct { volatile LONG64 zones; volatile LONG at; } preview_t;

// zones in use now (0: none, or its time is up)
static inline unsigned long long pv_active(const preview_t *p, DWORD tk) {
    unsigned long long m = (unsigned long long)p->zones;
    return m && tk - (DWORD)p->at < PV_MS ? m : 0;
}
static inline int pv_has(unsigned long long m, int zone) { return zone >= 0 && zone < 64 && (m >> zone & 1); }
static inline void pv_start(preview_t *p, unsigned long long zones, DWORD tk) { p->at = (LONG)tk; InterlockedExchange64(&p->zones, (LONG64)zones); }
// ends it; returns the zones it still had (0: it had already ended)
static inline unsigned long long pv_end(preview_t *p, DWORD tk) {
    unsigned long long m = (unsigned long long)InterlockedExchange64(&p->zones, 0);
    return m && tk - (DWORD)p->at < PV_MS ? m : 0;
}

// The test level of a white-only light: a steady, gentle step from what it shows when the test starts (never full).
static inline float pv_test_level(float now) { return now > 0.45f ? 0.15f : 0.65f; }
