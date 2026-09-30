// SPDX-License-Identifier: GPL-3.0-only
// What PC hardware this is, for the diagnostics report: the motherboard and BIOS, the processor, the memory sticks
// (SMBIOS), the graphics cards with the brand that made the card (PCI subsystem vendor), the SMBus controller (the
// bus RAM lighting is on), the USB devices of RGB makers, PawnIO, and what OpenRGB sees. With this, support for
// someone's hardware can be planned from one file. Read-only: nothing is written to any device.
#include "common.h"
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <intrin.h>
#include <stdlib.h>

typedef struct { char *p; int n, cap; } hw_buf;

static void put(hw_buf *b, const char *fmt, ...) {
    if (b->n >= b->cap - 1) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0) b->n += min(k, b->cap - 1 - b->n);
}

// ---- SMBIOS: board (type 2), BIOS (type 0), system (type 1), memory devices (type 17)
// string idx of a structure, without the padding spaces some firmware leaves (a few buffers, for one put())
static const char *smb_str(const BYTE *h, int idx) {
    static char ring[8][96]; static int k;
    char *o = ring[k++ & 7]; o[0] = 0;
    if (!idx) return o;
    const char *s = (const char *)h + h[1];
    for (int i = 1; i < idx && *s; i++) s += strlen(s) + 1;
    while (*s == ' ') s++;
    snprintf(o, 96, "%s", s);
    for (int n = (int)strlen(o); n > 0 && o[n - 1] == ' '; ) o[--n] = 0;
    return o;
}

static void add_smbios(hw_buf *b) {
    DWORD sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    BYTE *raw = sz ? malloc(sz) : NULL;
    if (!raw || GetSystemFirmwareTable('RSMB', 0, raw, sz) != sz) { put(b, "SMBIOS: not readable\r\n"); free(raw); return; }
    DWORD len; memcpy(&len, raw + 4, 4);
    const BYTE *p = raw + 8, *end = raw + 8 + min(len, sz - 8);
    int sticks = 0;
    while (p + 4 <= end && p[1] >= 4) {
        const BYTE *h = p;
        int type = h[0], hl = h[1];
        if (type == 0 && hl >= 9) put(b, "BIOS: %s %s (%s)\r\n", smb_str(h, h[4]), smb_str(h, h[5]), smb_str(h, h[8]));
        else if (type == 1 && hl >= 6) put(b, "system: %s %s\r\n", smb_str(h, h[4]), smb_str(h, h[5]));
        else if (type == 2 && hl >= 7) put(b, "motherboard: %s %s (rev %s)\r\n", smb_str(h, h[4]), smb_str(h, h[5]), smb_str(h, h[6]));
        else if (type == 17 && hl >= 0x1B) {
            WORD size; memcpy(&size, h + 0x0C, 2);
            DWORD mb = size & 0x8000 ? (size & 0x7FFF) / 1024 : size;   // KB when the top bit is set
            if (size == 0x7FFF && hl >= 0x20) memcpy(&mb, h + 0x1C, 4);   // extended size, MB
            if (size && size != 0xFFFF) {
                WORD speed = 0, conf = 0; memcpy(&speed, h + 0x15, 2);
                if (hl >= 0x22) memcpy(&conf, h + 0x20, 2);
                put(b, "memory %s: %lu MB, %s %s, %u MT/s (running %u)\r\n", smb_str(h, h[0x10]), mb, smb_str(h, h[0x17]), smb_str(h, h[0x1A]), speed, conf);
                sticks++;
            }
        }
        p += hl;                                            // formatted area, then the strings up to a double zero
        while (p + 1 < end && (p[0] || p[1])) p++;
        p += 2;
        if (type == 127) break;
    }
    if (!sticks) put(b, "memory: no sticks listed\r\n");
    free(raw);
}

static void add_cpu(hw_buf *b) {
    int r[4]; char vendor[13] = { 0 }, brand[49] = { 0 };
    __cpuid(r, 0); memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4);
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + i * 16, r, 16); }
    char *s = brand; while (*s == ' ') s++;
    for (int n = (int)strlen(s); n > 0 && s[n - 1] == ' '; ) s[--n] = 0;
    put(b, "processor: %s (%s)\r\n", s, vendor);
}

// card makers by PCI subsystem vendor
static const char *pci_vendor(unsigned v) {
    switch (v) {
    case 0x1043: return "ASUS";      case 0x1462: return "MSI";        case 0x1458: return "Gigabyte";
    case 0x3842: return "EVGA";      case 0x19DA: return "Zotac";      case 0x1569: return "Palit";
    case 0x10B0: return "Gainward";  case 0x1DA2: return "Sapphire";   case 0x148C: return "PowerColor";
    case 0x1849: return "ASRock";    case 0x1682: return "XFX";        case 0x196E: return "PNY";
    case 0x7377: return "Colorful";  case 0x10DE: return "NVIDIA";     case 0x1002: return "AMD";
    case 0x8086: return "Intel";     case 0x1B4C: return "KFA2/Galax"; case 0x174B: return "Sapphire (PC Partner)";
    case 0x1ACC: return "Point of View"; case 0x1B0A: return "Inno3D";
    default:     return NULL;
    }
}

// devices of one setup class (or all PCI devices when cls is NULL) whose first hardware id contains want (or all)
static void add_devices(hw_buf *b, const GUID *cls, const char *label, const char *want, int cards) {
    HDEVINFO set = cls ? SetupDiGetClassDevsW(cls, NULL, NULL, DIGCF_PRESENT) : SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA di = { sizeof(di) };
    int n = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &di); i++) {
        wchar_t hw[1024] = L"", name[256] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_HARDWAREID, NULL, (BYTE *)hw, sizeof(hw) - 4, NULL)) continue;
        char ids[1024] = ""; int k = 0;   // every id of the device, for the match
        for (const wchar_t *h = hw; *h && k < 1000; h += wcslen(h) + 1) k += WideCharToMultiByte(CP_UTF8, 0, h, -1, ids + k, 1024 - k, NULL, NULL);
        for (int j = 0; j < k - 1; j++) if (!ids[j]) ids[j] = ' ';
        if (want && !strstr(ids, want)) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof(name) - 2, NULL))
            SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name) - 2, NULL);
        char nm[256]; WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, sizeof(nm), NULL, NULL);
        char first[160]; WideCharToMultiByte(CP_UTF8, 0, hw, -1, first, sizeof(first), NULL, NULL);
        const char *ss = strstr(first, "SUBSYS_");
        char sv[5] = "";
        if (ss && strlen(ss) >= 15) memcpy(sv, ss + 11, 4);   // SUBSYS_ddddvvvv: vvvv is the card maker
        const char *maker = sv[0] && cards ? pci_vendor((unsigned)strtoul(sv, NULL, 16)) : NULL;
        put(b, "%s: %s%s%s%s [%s]\r\n", label, nm, maker ? " (card by " : "", maker ? maker : "", maker ? ")" : "", first);
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!n) put(b, "%s: none found\r\n", label);
}

// USB HID devices of lighting / peripheral makers, one line per VID:PID
static const char *usb_maker(unsigned v) {
    switch (v) {
    case 0x1462: return "MSI";        case 0x0B05: return "ASUS";      case 0x048D: return "ITE (Gigabyte RGB Fusion)";
    case 0x26CE: return "ASRock";     case 0x1B1C: return "Corsair";   case 0x1E71: return "NZXT";
    case 0x1532: return "Razer";      case 0x046D: return "Logitech";  case 0x1038: return "SteelSeries";
    case 0x0CF2: return "Lian Li";    case 0x2516: return "Cooler Master"; case 0x3633: return "DeepCool";
    case 0x37FA: return "Nanoleaf";   case 0x0951: return "Kingston/HyperX"; case 0x03F0: return "HP/HyperX";
    case 0x1044: return "Gigabyte";   case 0x2F68: return "Thermaltake"; case 0x264A: return "Thermaltake";
    case 0x0416: return "Winbond (Lian Li / Thermalright)"; case 0x1A86: return "WCH (hubs, some ARGB)";
    case 0x3402: return "Glorious";   case 0x258A: return "SINO WEALTH (keyboards)"; case 0x1B80: return "Wooting";
    case 0x31E3: return "Wooting";    case 0x2433: return "ASETEK";    case 0x16D0: return "(shared VID, hobby devices)";
    default:     return NULL;
    }
}

static void add_usb(hw_buf *b) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    unsigned seen[128]; int ns = 0, shown = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd) && ns < 128; i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        const wchar_t *v = wcsstr(low, L"vid_"), *p = wcsstr(low, L"pid_");
        if (!v || !p) continue;
        unsigned vid = wcstoul(v + 4, NULL, 16), pid = wcstoul(p + 4, NULL, 16), key = vid << 16 | pid;
        const char *maker = usb_maker(vid);
        if (!maker) continue;
        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == key) dup = 1;
        if (dup) continue;
        seen[ns++] = key;
        char prod[128] = "";
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);   // no access: names only
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t w[128] = L"";
            if (HidD_GetProductString(h, w, sizeof(w))) WideCharToMultiByte(CP_UTF8, 0, w, -1, prod, sizeof(prod), NULL, NULL);
            CloseHandle(h);
        }
        put(b, "usb: %04X:%04X %s%s%s\r\n", vid, pid, maker, prod[0] ? " - " : "", prod);
        shown++;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!shown) put(b, "usb: no devices of known RGB makers\r\n");
}

static void add_openrgb(hw_buf *b) {
    static orgb_ctl c[64];
    net_init();
    int n = orgb_list(c, 64);
    if (n < 0) { put(b, "OpenRGB: no SDK server on this PC (127.0.0.1:6742)\r\n"); return; }
    put(b, "OpenRGB: SDK server answers, %d controllers\r\n", n);
    for (int i = 0; i < n; i++) put(b, "  #%d %s (%s, %d LEDs)\r\n", c[i].idx, c[i].name, c[i].kind, c[i].leds);
}

int hw_inventory(char *out, int cap) {
    hw_buf b = { out, 0, cap };
    out[0] = 0;
    add_smbios(&b);
    add_cpu(&b);
    static const GUID DISPLAY = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    add_devices(&b, &DISPLAY, "graphics", NULL, 1);
    add_devices(&b, NULL, "smbus", "CC_0C05", 0);
    add_usb(&b);
    put(&b, "PawnIO: %s\r\n", GetFileAttributesW(L"C:\\Program Files\\PawnIO\\PawnIOLib.dll") != INVALID_FILE_ATTRIBUTES ? "installed" : "not installed");
    add_openrgb(&b);
    return b.n;
}
