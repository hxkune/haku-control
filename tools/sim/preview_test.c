// SPDX-License-Identifier: GPL-3.0-only
// Checks for the guide's "Evening light" (src/preview.h): it ends by itself, "back" and "keep" after a change by hand
// find nothing to end, and the white-light test step is gentle and steady.
//   build.cmd's environment, then: cl /nologo /Isrc tools\sim\preview_test.c /Fe:%TEMP%\preview_test.exe && %TEMP%\preview_test.exe
#include "../../src/preview.h"
#include <stdio.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

int main(void) {
    preview_t p = { 0 };
    const DWORD t0 = 0xFFFFF000u;   // just before GetTickCount() wraps: the deadline still holds
    const unsigned long long two = 1ull << ZONE_LIGHT0 | 1ull << (ZONE_EXT0 + 3);
    CHECK(MAX_ZONES <= 64, "every zone fits the mask");

    CHECK(!pv_active(&p, t0), "nothing at start");
    pv_start(&p, two, t0);
    CHECK(pv_active(&p, t0 + 1000) == two, "the two zones show it");
    CHECK(pv_has(pv_active(&p, t0 + 1000), ZONE_LIGHT0) && !pv_has(pv_active(&p, t0 + 1000), ZONE_LIGHT0 + 1), "only the zones chosen");
    CHECK(!pv_has(two, -1) && !pv_has(two, 64), "no zone outside the mask");

    // the window closed: nothing ends it, its own time does
    CHECK(pv_active(&p, t0 + PV_MS - 1) == two, "still on just before its time");
    CHECK(!pv_active(&p, t0 + PV_MS), "off by itself once its time is up (across the tick wrap)");
    CHECK(!pv_end(&p, t0 + PV_MS + 10), "keep after its time: nothing to save");

    // back: what showed before comes back (the overlay is gone, also over lights that were off)
    pv_start(&p, two, t0);
    CHECK(pv_end(&p, t0 + 2000) == two, "back ends it");
    CHECK(!pv_active(&p, t0 + 2001), "and nothing is drawn over the lights any more");

    // a change by hand (tray, phone) ends it first: the late back or keep then changes nothing
    pv_start(&p, two, t0);
    pv_end(&p, t0 + 3000);                       // set_effect / set_brightness_ex / a profile
    CHECK(!pv_end(&p, t0 + 3500), "a late back after a change by hand finds nothing");
    CHECK(!pv_active(&p, t0 + 3500), "the hand's choice stays");

    // started again: a new deadline
    pv_start(&p, two, t0 + 40000);
    CHECK(pv_active(&p, t0 + 40000 + PV_MS - 1) == two, "a new start gets its own time");

    // white-only lights: one steady step, never to full, never to black
    CHECK(pv_test_level(1.0f) == 0.15f && pv_test_level(0.8f) == 0.15f, "a bright white light dims");
    CHECK(pv_test_level(0.0f) == 0.65f && pv_test_level(0.3f) == 0.65f, "a dim or off one comes up, not to full");
    CHECK(PV_LEVEL < 1 && PV_LEVEL > 0.3f, "the preview is not full brightness");

    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
