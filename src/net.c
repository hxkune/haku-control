// SPDX-License-Identifier: GPL-3.0-only
// Network helpers: per-interface broadcast addresses, and a watchdog that keeps the
// Windows Mobile Hotspot on (Nanoleaf / bulbs live on the PC's own Wi-Fi network).
// Turning the hotspot on needs WinRT, so that is done by the small haku-control-hotspot.exe,
// started only when the hotspot is found off.
#include "common.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <process.h>
#include <stdlib.h>

#define HOTSPOT_IP "192.168.137.1"   // Windows ICS default gateway address

static int for_each_ipv4(int (*fn)(ULONG addr, int prefix, void *ctx), void *ctx) {
    ULONG len = 64 * 1024;   // called from several threads: own buffer per call
    IP_ADAPTER_ADDRESSES *all = malloc(len), *aa = all;
    int hit = 0;
    if (all && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, all, &len) == NO_ERROR)
        for (; aa && !hit; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = aa->FirstUnicastAddress; u && !hit; u = u->Next) {
                ULONG a = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((a >> 16) == 0xA9FE) continue;   // 169.254/16: no network behind it
                hit = fn(a, u->OnLinkPrefixLength, ctx);
            }
        }
    free(all);
    return hit;
}

typedef struct { ULONG *out; int max, n; } bc_ctx;
static int add_bcast(ULONG a, int prefix, void *p) {
    bc_ctx *c = p;
    if (prefix <= 0 || prefix >= 31 || c->n >= c->max) return 0;
    ULONG mask = 0xFFFFFFFFu << (32 - prefix);
    c->out[c->n++] = htonl(a | ~mask);
    return 0;
}

int net_broadcasts(ULONG *out, int max) {
    bc_ctx c = { out, max, 0 };
    for_each_ipv4(add_bcast, &c);
    return c.n;
}

static int add_addr(ULONG a, int prefix, void *p) {
    bc_ctx *c = p;
    (void)prefix;
    if (c->n < c->max) c->out[c->n++] = htonl(a);
    return 0;
}

int net_addresses(ULONG *out, int max) {
    bc_ctx c = { out, max, 0 };
    for_each_ipv4(add_addr, &c);
    return c.n;
}

// Addresses a phone on the same network can open, best first: adapters with a default gateway (the home Wi-Fi /
// Ethernet), then the PC's own hotspot, then the rest. Adapters of virtual machines, WSL and containers (Hyper-V
// vEthernet, VirtualBox, VMware, Docker...) are left out: no phone is ever on those networks.
int net_phone_addresses(ULONG *out, int max) {
    ULONG len = 64 * 1024;
    IP_ADAPTER_ADDRESSES *all = malloc(len), *aa;
    ULONG got[3][16]; int ng[3] = { 0, 0, 0 };
    struct in_addr hs; inet_pton(AF_INET, HOTSPOT_IP, &hs);
    if (all && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                    NULL, all, &len) == NO_ERROR)
        for (aa = all; aa; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            wchar_t nm[512]; swprintf(nm, 512, L"%s | %s", aa->FriendlyName ? aa->FriendlyName : L"", aa->Description ? aa->Description : L"");
            _wcslwr_s(nm, 512);
            static const wchar_t *const VIRT[] = { L"vethernet", L"hyper-v", L"virtualbox", L"vmware", L"docker", L"wsl", L"vbox", L"parallels", L"npcap" };
            int virt = 0;
            for (int i = 0; i < (int)(sizeof(VIRT) / sizeof(VIRT[0])); i++) if (wcsstr(nm, VIRT[i])) virt = 1;
            if (virt) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = aa->FirstUnicastAddress; u; u = u->Next) {
                ULONG a = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
                if ((ntohl(a) >> 16) == 0xA9FE) continue;
                int rank = aa->FirstGatewayAddress ? 0 : a == hs.s_addr ? 1 : 2;
                if (ng[rank] < 16) got[rank][ng[rank]++] = a;
            }
        }
    free(all);
    int n = 0;
    for (int r = 0; r < 3; r++) for (int i = 0; i < ng[r] && n < max; i++) out[n++] = got[r][i];
    return n;
}

static int is_hotspot_ip(ULONG a, int prefix, void *p) {
    (void)prefix; (void)p;
    struct in_addr h; inet_pton(AF_INET, HOTSPOT_IP, &h);
    return a == ntohl(h.s_addr);
}

int hotspot_active(void) { return for_each_ipv4(is_hotspot_ip, NULL); }

// ---------------------------------------------------------------- hotspot watchdog
static HANDLE hs_thread, hs_stop;

static void start_hotspot(void) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *s = wcsrchr(exe, L'\\'); if (s) s[1] = 0;
    wcscat_s(exe, MAX_PATH, L"haku-control-hotspot.exe");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, NULL, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        logf_("hotspot: cannot start helper (%lu)", GetLastError());
        return;
    }
    DWORD code = 99;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    static const char *why[] = { "on", "started", "no connection to share", "refused by Windows", "WinRT error" };
    logf_("hotspot: %s", code < 5 ? why[code] : "timeout");
}

static unsigned __stdcall hs_fn(void *p) {
    (void)p;
    DWORD wait = 3000;   // first check shortly after start
    while (WaitForSingleObject(hs_stop, wait) == WAIT_TIMEOUT) {
        wait = 30000;
        if (!cfg_geti("hotspot", "auto", 0)) continue;
        if (for_each_ipv4(is_hotspot_ip, NULL)) continue;
        start_hotspot();
        wait = 60000;
    }
    return 0;
}

void hotspot_watch_start(void) {
    if (hs_thread) return;
    hs_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    hs_thread = (HANDLE)_beginthreadex(NULL, 0, hs_fn, NULL, 0, NULL);
}

void hotspot_watch_stop(void) {
    if (!hs_thread) return;
    SetEvent(hs_stop);
    WaitForSingleObject(hs_thread, 35000);
    CloseHandle(hs_thread); CloseHandle(hs_stop);
    hs_thread = hs_stop = NULL;
}
