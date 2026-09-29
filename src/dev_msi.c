// SPDX-License-Identifier: GPL-3.0-only
// MSI Mystic Light, 185-byte protocol, per-LED "sync" direct mode (protocol as documented by OpenRGB).
// Only boards whose 185-byte layout was verified are opened (currently MS-7D73, MPG B650I EDGE WIFI).
// Layout of the 0x53 per-LED packet for this board: [0] onboard LED, [1..40] JRAINBOW1.
// The board's own configuration is snapshotted on start and written back on exit
// (the save-to-flash flag is never set).
#include "common.h"
#include <setupapi.h>
#include <hidsdi.h>

#define FEAT_LEN 761
#define CFG_LEN  185
#define PL_LEN   (5 + 240 * 3)

static HANDLE dev = INVALID_HANDLE_VALUE;
static BYTE   orig[CFG_LEN];
static BYTE   last[PL_LEN];
static int    have_orig, direct_on;

static HANDLE find_device(void) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    HANDLE h = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd); i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        if (!wcsstr(low, L"vid_1462&pid_7d73")) continue;
        h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

static BOOL set_feature(const BYTE *data, int len) {
    BYTE buf[FEAT_LEN] = { 0 };
    memcpy(buf, data, len);
    return HidD_SetFeature(dev, buf, FEAT_LEN);
}

static void backup_path(wchar_t *p) { app_data_path(L"msi_backup.bin", p); }

static void build_enable(BYTE *en) {
    memset(en, 0, CFG_LEN);
    en[0] = 0x52;
    static const int zones10[] = { 1, 11, 21, 64, 74, 84, 94, 104, 114, 124, 134, 144, 154, 164, 174 };
    for (int i = 0; i < 15; i++) en[zones10[i]] = 1;               // effect = static
    en[31] = 1; en[42] = 1; en[53] = 1;
    en[1 + 4] = 0x08;  en[1 + 8] = 0x80;                           // j_rgb_1
    en[11 + 4] = 0x2A; en[11 + 8] = 0x80;                          // j_pipe_1
    en[21 + 4] = 0x2A; en[21 + 8] = 0x80;                          // j_pipe_2
    en[31 + 4] = 0x29; en[31 + 8] = 0x80; en[31 + 10] = 40;        // j_rainbow_1, 40 LEDs (sync mode)
    en[42 + 4] = 0x29; en[42 + 8] = 0x80; en[42 + 10] = 40;        // j_rainbow_2
    en[53 + 4] = 0x29; en[53 + 8] = 0x82; en[53 + 10] = 120;       // j_corsair
    en[64 + 4] = 0x28; en[64 + 8] = 0x80;                          // j_corsair_outerll120
    en[74] = 0x25; en[74 + 4] = 0xA9; en[74 + 8] = 0xBF;           // on_board_led: direct, full sync
    for (int z = 84; z <= 164; z += 10) { en[z + 4] = 0x28; en[z + 8] = 0x80; }
    en[174 + 4] = 0x2A; en[174 + 8] = 0x80;                        // j_rgb_2
}

int msi_open(void) {
    dev = find_device();
    if (dev == INVALID_HANDLE_VALUE) { logf_("msi: device not found"); return 0; }

    wchar_t bp[MAX_PATH]; backup_path(bp);
    BYTE cur[FEAT_LEN] = { 0x52 };
    if (HidD_GetFeature(dev, cur, FEAT_LEN) && cur[74] != 0x25) {
        // Board is in its own mode: this is the config to restore on exit. Keep a copy on disk
        // in case we crash and the next start finds the board still in direct mode.
        memcpy(orig, cur, CFG_LEN); orig[184] = 0; have_orig = 1;
        FILE *f = _wfopen(bp, L"wb"); if (f) { fwrite(orig, 1, CFG_LEN, f); fclose(f); }
    } else {
        FILE *f = _wfopen(bp, L"rb");
        if (f) { have_orig = fread(orig, 1, CFG_LEN, f) == CFG_LEN; fclose(f); orig[184] = 0; }
    }
    logf_("msi: opened, restore snapshot %s", have_orig ? "ok" : "missing");

    BYTE en[CFG_LEN]; build_enable(en);
    if (!set_feature(en, CFG_LEN)) { logf_("msi: enable direct failed %lu", GetLastError()); CloseHandle(dev); dev = INVALID_HANDLE_VALUE; return 0; }
    direct_on = 1;
    memset(last, 0xFF, sizeof(last));
    return 1;
}

static BYTE to8(float v) { return (BYTE)(clampf(v, 0, 1) * 255.0f + 0.5f); }

// Returns 1 if a packet was sent, 0 if unchanged, -1 on error.
int msi_send(const rgbf *gpu, int n, const rgbf *board) {
    if (dev == INVALID_HANDLE_VALUE) return -1;
    BYTE pl[PL_LEN] = { 0x53, 0x25, 0x06, 0x00, 0x00 };
    if (board) { pl[5] = to8(board->r); pl[6] = to8(board->g); pl[7] = to8(board->b); }
    for (int i = 0; i < n && i < 40; i++) {
        BYTE *c = &pl[5 + (1 + i) * 3];
        c[0] = to8(gpu[i].r); c[1] = to8(gpu[i].g); c[2] = to8(gpu[i].b);
    }
    if (memcmp(pl, last, PL_LEN) == 0) return 0;
    if (!set_feature(pl, PL_LEN)) {
        DWORD e = GetLastError();
        logf_("msi: frame failed %lu, reopening", e);
        CloseHandle(dev);
        dev = INVALID_HANDLE_VALUE;
        if (!msi_open()) return -1;   // e.g. after resume from sleep
        return set_feature(pl, PL_LEN) ? 1 : -1;
    }
    memcpy(last, pl, PL_LEN);
    return 1;
}

void msi_restore(void) {
    if (dev == INVALID_HANDLE_VALUE || !direct_on || !have_orig) return;
    // The Mystic Light app sends the configuration twice.
    set_feature(orig, CFG_LEN); Sleep(20); set_feature(orig, CFG_LEN);
    direct_on = 0;
    logf_("msi: restored board effect");
}

void msi_close(void) {
    if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
    dev = INVALID_HANDLE_VALUE;
}
