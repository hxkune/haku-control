// SPDX-License-Identifier: GPL-3.0-only
// TRYX PANORAMA serial frames (drv_tryx.c): 5A | escaped( length u16 BE | message | crc ) | 5A. The length counts
// the unescaped frame with both 5A markers; the crc is the low byte of the sum of the length bytes and the message.
// Inside, 5A goes out as 5B 01 and 5B as 5B 02, so 5A only ever marks the ends.
#pragma once

// out needs 2 * n + 8 bytes; returns the frame's size.
static int tryx_frame(const unsigned char *msg, int n, unsigned char *out) {
    int len = n + 5, k = 0;
    unsigned char hd[2] = { (unsigned char)(len >> 8), (unsigned char)len }, crc = (unsigned char)(hd[0] + hd[1]);
    for (int i = 0; i < n; i++) crc = (unsigned char)(crc + msg[i]);
    out[k++] = 0x5A;
    for (int i = 0; i < n + 3; i++) {
        unsigned char b = i < 2 ? hd[i] : i < n + 2 ? msg[i - 2] : crc;
        if (b == 0x5A || b == 0x5B) { out[k++] = 0x5B; out[k++] = b == 0x5A ? 1 : 2; } else out[k++] = b;
    }
    out[k++] = 0x5A;
    return k;
}

// The crc of a message as tryx_frame() would send it: the device drops the crc before it unescapes, so a crc of
// 5A or 5B breaks the frame whatever is done; the sender pads the message instead.
static unsigned char tryx_crc(const unsigned char *msg, int n) {
    int len = n + 5; unsigned char crc = (unsigned char)((len >> 8) + len);
    for (int i = 0; i < n; i++) crc = (unsigned char)(crc + msg[i]);
    return crc;
}

// The bytes between two 5A markers, unescaped, back to the message (length and crc dropped, the crc not checked:
// the device does not either). Returns the message's size, -1 if too short. In place.
static int tryx_unframe(unsigned char *p, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) p[k++] = p[i] == 0x5B && i + 1 < n ? (p[++i] == 1 ? 0x5A : 0x5B) : p[i];
    if (k < 3) return -1;
    memmove(p, p + 2, k - 3);
    return k - 3;
}
