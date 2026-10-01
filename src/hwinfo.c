// SPDX-License-Identifier: GPL-3.0-only
// What PC hardware this is, for the diagnostics report: the motherboard and BIOS, the processor, the memory sticks
// (SMBIOS), the graphics cards with the brand that made the card (PCI subsystem vendor), the SMBus controller (the
// bus RAM lighting is on), the USB devices of RGB makers, PawnIO, and what OpenRGB sees. With this, support for
// someone's hardware can be planned from one file. Read-only: nothing is written to any device.
// The same sources, as a list for the window (hw_scan_*): the board, the memory, the graphics cards and the USB
// devices of lighting makers; the window says for each which way haku lights it (itself, OpenRGB, nothing yet).
#include "common.h"
#include "devices.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <intrin.h>
#include <stdlib.h>
#include <process.h>

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

// ---------------------------------------------------------------- the list for the window
typedef struct { char cat[8], maker[48], name[96], id[24]; } hw_item;
#define HW_MAX 48
static hw_item scan_items[HW_MAX];
static int scan_n, scan_done;
static volatile LONG scan_busy;
static SRWLOCK scan_lk = SRWLOCK_INIT;

static void item(hw_item *it, int *n, const char *cat, const char *maker, const char *name, const char *id) {
    if (*n >= HW_MAX) return;
    hw_item *x = &it[(*n)++];
    snprintf(x->cat, sizeof(x->cat), "%s", cat); snprintf(x->maker, sizeof(x->maker), "%s", maker ? maker : "");
    snprintf(x->name, sizeof(x->name), "%s", name ? name : ""); snprintf(x->id, sizeof(x->id), "%s", id ? id : "");
}

// makers as people know them: "Micro-Star International Co., Ltd." -> MSI; memory without one (or a JEDEC code
// in its place) by its part number: F4-/F5- G.Skill, CM Corsair, KF Kingston Fury, BL Crucial, TF TeamGroup
static const char *short_maker(const char *m, const char *part) {
    static const char *const M[][2] = { { "micro-star", "MSI" }, { "asustek", "ASUS" }, { "gigabyte", "Gigabyte" }, { "asrock", "ASRock" },
        { "corsair", "Corsair" }, { "kingston", "Kingston" }, { "g.skill", "G.Skill" }, { "g skill", "G.Skill" }, { "crucial", "Crucial" },
        { "micron", "Crucial" }, { "teamgroup", "TeamGroup" }, { "team group", "TeamGroup" }, { "samsung", "Samsung" }, { "sk hynix", "SK hynix" },
        { "hynix", "SK hynix" }, { "adata", "ADATA" }, { "xpg", "ADATA XPG" }, { "patriot", "Patriot" }, { "biostar", "Biostar" } };
    char low[96]; snprintf(low, sizeof(low), "%s", m ? m : ""); _strlwr_s(low, sizeof(low));
    for (int i = 0; i < (int)(sizeof(M) / sizeof(M[0])); i++) if (strstr(low, M[i][0])) return M[i][1];
    if (part) {
        static const char *const P[][2] = { { "F4-", "G.Skill" }, { "F5-", "G.Skill" }, { "CM", "Corsair" }, { "KF", "Kingston Fury" },
            { "KHX", "Kingston HyperX" }, { "BL", "Crucial" }, { "TF", "TeamGroup" }, { "TDT", "TeamGroup" }, { "AX", "ADATA XPG" } };
        for (int i = 0; i < (int)(sizeof(P) / sizeof(P[0])); i++) if (!strncmp(part, P[i][0], strlen(P[i][0]))) return P[i][1];
    }
    if (!*low || !strcmp(low, "unknown") || !strcmp(low, "undefined") || (strlen(low) <= 6 && strspn(low, "0123456789abcdef") == strlen(low))) return "";
    return m;
}

// the board and the memory (sticks of one kind as one line: "2 x CMH32GX5M2B6000C40")
static void scan_smbios(hw_item *it, int *n) {
    DWORD sz = GetSystemFirmwareTable('RSMB', 0, NULL, 0);
    BYTE *raw = sz ? malloc(sz) : NULL;
    if (!raw || GetSystemFirmwareTable('RSMB', 0, raw, sz) != sz) { free(raw); return; }
    DWORD len; memcpy(&len, raw + 4, 4);
    const BYTE *p = raw + 8, *end = raw + 8 + min(len, sz - 8);
    char part[96] = "", maker[48] = ""; int sticks = 0, mixed = 0;
    while (p + 4 <= end && p[1] >= 4) {
        const BYTE *h = p;
        int type = h[0], hl = h[1];
        if (type == 2 && hl >= 7) { const char *mk = smb_str(h, h[4]); item(it, n, "board", short_maker(mk, NULL), smb_str(h, h[5]), ""); }
        else if (type == 17 && hl >= 0x1B) {
            WORD size; memcpy(&size, h + 0x0C, 2);
            if (size && size != 0xFFFF) {
                const char *pn = smb_str(h, h[0x1A]), *mk = smb_str(h, h[0x17]);
                if (!sticks) { snprintf(part, sizeof(part), "%s", pn); snprintf(maker, sizeof(maker), "%s", mk); }
                else if (strcmp(part, pn)) mixed = 1;
                sticks++;
            }
        }
        p += hl;
        while (p + 1 < end && (p[0] || p[1])) p++;
        p += 2;
        if (type == 127) break;
    }
    if (sticks) {
        char nm[128]; snprintf(nm, sizeof(nm), mixed ? "%d sticks" : "%d \xC3\x97 %s", sticks, part);
        item(it, n, "ram", short_maker(maker, part), nm, "");
    }
    free(raw);
}

// graphics cards with their maker; not the ones built into the processor (no lighting of their own)
static void scan_gpus(hw_item *it, int *n) {
    static const GUID DISPLAY = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HDEVINFO set = SetupDiGetClassDevsW(&DISPLAY, NULL, NULL, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA di = { sizeof(di) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &di); i++) {
        wchar_t hw[1024] = L"", name[256] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_HARDWAREID, NULL, (BYTE *)hw, sizeof(hw) - 4, NULL)) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_FRIENDLYNAME, NULL, (BYTE *)name, sizeof(name) - 2, NULL))
            SetupDiGetDeviceRegistryPropertyW(set, &di, SPDRP_DEVICEDESC, NULL, (BYTE *)name, sizeof(name) - 2, NULL);
        char first[160], nm[256];
        WideCharToMultiByte(CP_UTF8, 0, hw, -1, first, sizeof(first), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, sizeof(nm), NULL, NULL);
        if (!strstr(first, "PCI\\VEN_") || strstr(first, "VEN_8086") || strstr(first, "VEN_1414")) continue;   // Intel built-in, Microsoft basic
        int digit = 0; for (const char *c = nm; *c; c++) if (*c >= '0' && *c <= '9') digit = 1;
        if (strstr(nm, "Radeon") && !digit) continue;   // "AMD Radeon(TM) Graphics": the processor's
        const char *ss = strstr(first, "SUBSYS_");
        char sv[5] = "", id[24] = "";
        if (ss && strlen(ss) >= 15) memcpy(sv, ss + 11, 4);
        const char *maker = sv[0] ? pci_vendor((unsigned)strtoul(sv, NULL, 16)) : NULL;
        const char *ven = strstr(first, "VEN_"), *dev = strstr(first, "DEV_");
        if (ven && dev) snprintf(id, sizeof(id), "%.4s:%.4s", ven + 4, dev + 4);
        item(it, n, "gpu", maker, nm, id);
    }
    SetupDiDestroyDeviceInfoList(set);
}

// USB devices of lighting makers, one per VID:PID
static void scan_usb(hw_item *it, int *n) {
    GUID hid; HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
    unsigned seen[128]; int ns = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &ifd) && ns < 128; i++) {
        BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (void *)buf;
        det->cbSize = sizeof(*det);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), NULL, NULL)) continue;
        wchar_t low[512]; wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE); _wcslwr_s(low, 512);
        const wchar_t *v = wcsstr(low, L"vid_"), *pp = wcsstr(low, L"pid_");
        if (!v || !pp) continue;
        unsigned vid = wcstoul(v + 4, NULL, 16), pid = wcstoul(pp + 4, NULL, 16), key = vid << 16 | pid;
        const char *maker = usb_maker(vid);
        if (!maker) continue;
        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == key) dup = 1;
        if (dup) continue;
        seen[ns++] = key;
        char prod[128] = "";
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t w[128] = L"";
            if (HidD_GetProductString(h, w, sizeof(w))) WideCharToMultiByte(CP_UTF8, 0, w, -1, prod, sizeof(prod), NULL, NULL);
            CloseHandle(h);
        }
        for (char *c = prod; *c; c++) if ((unsigned char)*c < 32) *c = ' ';
        for (int k = (int)strlen(prod); k > 0 && prod[k - 1] == ' '; ) prod[--k] = 0;
        char id[24]; snprintf(id, sizeof(id), "%04X:%04X", vid, pid);
        item(it, n, "usb", maker, prod, id);
    }
    SetupDiDestroyDeviceInfoList(set);
}

static unsigned __stdcall scan_run(void *arg) {
    (void)arg;
    static hw_item it[HW_MAX]; int n = 0;
    scan_smbios(it, &n); scan_gpus(it, &n); scan_usb(it, &n);
    AcquireSRWLockExclusive(&scan_lk);
    memcpy(scan_items, it, sizeof(it)); scan_n = n; scan_done = 1;
    ReleaseSRWLockExclusive(&scan_lk);
    logf_("hardware: %d items found", n);
    InterlockedExchange(&scan_busy, 0);
    ui_refresh();
    return 0;
}

void hw_scan_start(void) {
    if (InterlockedCompareExchange(&scan_busy, 1, 0)) return;
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, scan_run, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&scan_busy, 0);
}

// "hw":{"busy":0,"done":1,"items":[{"cat":"usb","maker":"Lian Li","name":"...","id":"0CF2:A200"}]}
int hw_scan_json(char *out, int cap) {
    AcquireSRWLockShared(&scan_lk);
    int n = snprintf(out, cap, "\"hw\":{\"busy\":%ld,\"done\":%d,\"items\":[", scan_busy, scan_done);
    for (int i = 0; i < scan_n && n < cap - 400; i++) {
        hw_item *x = &scan_items[i];
        n += snprintf(out + n, cap - n, "%s{\"cat\":\"%s\",\"id\":\"%s\",\"maker\":\"", i ? "," : "", x->cat, x->id);
        n += json_escape_to(out + n, cap - n, x->maker);
        n += snprintf(out + n, cap - n, "\",\"name\":\"");
        n += json_escape_to(out + n, cap - n, x->name);
        n += snprintf(out + n, cap - n, "\"}");
    }
    n += snprintf(out + n, cap - n, "]}");
    ReleaseSRWLockShared(&scan_lk);
    return n;
}
