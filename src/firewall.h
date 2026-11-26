// SPDX-License-Identifier: GPL-3.0-only
// The inbound firewall rule for the phone page and for lights answering a scan on the local network (firewall.c).
#pragma once
#include <windows.h>

typedef struct {
    int ok;          // the rule is as asked (there, as it should be / gone)
    int added, removed, migrated;   // what this run changed
    int blocks;      // enabled inbound Block rules for this exe: they win over any allow rule, and are left alone
    int unclear;     // rules that look like haku's but are not surely its own: left alone
    HRESULT err;     // the first failed system call, 0 when none
    wchar_t block[128];   // the first block rule's name, for the log
} fw_result_t;

// want 1: exactly one allow rule of haku's own for exe (Private networks, local addresses only); an older haku rule
// for exe (any network) is replaced by it. want 0: haku's own and the older rule removed. Nothing else is touched.
int fw_sync(const wchar_t *exe, int want, fw_result_t *res);
