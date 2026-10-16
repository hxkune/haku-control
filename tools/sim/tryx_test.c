// SPDX-License-Identifier: GPL-3.0-only
// Checks for the TRYX PANORAMA frames (src/tryx_frame.h) against a frame TRYX KANALI sent (canalis' capture).
//   build.cmd's environment, then: cl /nologo tools\sim\tryx_test.c /Fe:%TEMP%\tryx_test.exe && %TEMP%\tryx_test.exe
#include <string.h>
#include <stdio.h>
#include "../../src/tryx_frame.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

int main(void) {
    const char *conn = "POST conn 1\r\nSeqNumber=0\r\nDate=1791902261234\r\n\r\n";
    int n = (int)strlen(conn);
    unsigned char f[256]; int k = tryx_frame((const unsigned char *)conn, n, f);
    CHECK(k == n + 5 && f[0] == 0x5A && f[1] == 0x00 && f[2] == 0x35 && !memcmp(f + 3, conn, n) && f[k - 2] == 0x4F && f[k - 1] == 0x5A,
          "conn frame as captured");
    CHECK(tryx_crc((const unsigned char *)conn, n) == 0x4F, "crc");
    k = tryx_unframe(f + 1, k - 2);
    CHECK(k == n && !memcmp(f + 1, conn, n), "conn back from the frame");

    // 5A / 5B inside are escaped and come back; no 5A between the markers
    const unsigned char m[] = { 'a', 0x5A, 'b', 0x5B, 0x5B, 0x01, 'c' };
    k = tryx_frame(m, sizeof(m), f);
    int inner = 0; for (int i = 1; i < k - 1; i++) inner += f[i] == 0x5A;
    CHECK(!inner && k == (int)sizeof(m) + 5 + 3, "escaped");
    CHECK(tryx_unframe(f + 1, k - 2) == (int)sizeof(m) && !memcmp(f + 1, m, sizeof(m)), "unescaped");
    CHECK(tryx_unframe(f, 2) == -1, "too short");
    printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails != 0;
}
