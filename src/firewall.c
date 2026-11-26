// SPDX-License-Identifier: GPL-3.0-only
// The inbound firewall rule (firewall.h). haku changes only:
//  - its own rule, known by its name and its group together, for Private networks and local addresses only;
//  - the one rule versions up to 0.4.86 added ("haku control", every network), when it is plainly that one: the only
//    rule of that name, inbound, allow, every network, for this exe, with exactly the addresses they set.
// Anything else stays as it is. A Block rule for this exe (Windows adds one when its "allow access?" question went
// unanswered) wins over any allow rule: it is reported, never changed. The system is reached through ops: Windows'
// INetFwPolicy2 (the same in every language, unlike netsh's text), or a list in memory in FW_TEST.
#define COBJMACROS
#include <windows.h>
#include <netfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "firewall.h"
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define OUR_NAME  L"haku control - local network"
#define OUR_GROUP L"haku control"
#define OLD_NAME  L"haku control"
#define ADDRS     L"LocalSubnet,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,100.64.0.0/10"
#define MAX_RULES 64

typedef struct { wchar_t name[128], group[128], app[MAX_PATH], remote[512]; int in, allow, enabled; long profiles; } fw_rule_t;
typedef struct {
    int (*list)(const wchar_t *exe, fw_rule_t *out, int max, HRESULT *err);   // the rules that keep(); -1 failed
    HRESULT (*remove)(const wchar_t *name);   // one rule of that name
    HRESULT (*add)(const fw_rule_t *r);
} fw_ops_t;

// ---------------------------------------------------------------- the plan (pure)
static int same_app(const wchar_t *stored, const wchar_t *exe) {
    wchar_t x[MAX_PATH];
    if (!stored[0] || !ExpandEnvironmentStringsW(stored, x, MAX_PATH)) return 0;
    return !_wcsicmp(x, exe);
}

static int by_text(const void *a, const void *b) { return wcscmp((const wchar_t *)a, (const wchar_t *)b); }

// Windows gives addresses back in its own form (10.0.0.0/255.0.0.0, its own order): both sides in that form, sorted
static int canon(const wchar_t *in, wchar_t out[16][40]) {
    int n = 0;
    for (const wchar_t *p = in; *p && n < 16;) {
        while (*p == L' ' || *p == L',') p++;
        if (!*p) break;
        int l = 0; wchar_t t[40];
        while (*p && *p != L',' && l < 39) t[l++] = (wchar_t)towlower(*p++);
        while (*p && *p != L',') p++;
        while (l && t[l - 1] == L' ') l--;
        t[l] = 0;
        wchar_t *s = wcschr(t, L'/'), *e;
        long bits = s ? wcstol(s + 1, &e, 10) : -1;
        if (s && *e == 0 && e != s + 1 && bits >= 0 && bits <= 32 && !wcschr(s + 1, L'.')) {
            unsigned m = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
            swprintf(s + 1, 39 - (s + 1 - t), L"%u.%u.%u.%u", m >> 24, (m >> 16) & 255, (m >> 8) & 255, m & 255);
        }
        wcscpy_s(out[n++], 40, t);
    }
    qsort(out, n, sizeof(out[0]), by_text);
    return n;
}
static int same_addrs(const wchar_t *a, const wchar_t *b) {
    wchar_t x[16][40], y[16][40];
    int n = canon(a, x);
    if (n != canon(b, y)) return 0;
    for (int i = 0; i < n; i++) if (wcscmp(x[i], y[i])) return 0;
    return 1;
}

static int ours(const fw_rule_t *r) { return !wcscmp(r->name, OUR_NAME) && !wcscmp(r->group, OUR_GROUP); }
static int current(const fw_rule_t *r, const wchar_t *exe) {
    return ours(r) && r->in && r->allow && r->enabled && r->profiles == NET_FW_PROFILE2_PRIVATE && same_app(r->app, exe) && same_addrs(r->remote, ADDRS);
}
static int old_rule(const fw_rule_t *r, const wchar_t *exe) {   // netsh ... profile=any gives every profile there is
    return !wcscmp(r->name, OLD_NAME) && !r->group[0] && r->in && r->allow && (r->profiles == NET_FW_PROFILE2_ALL || r->profiles == 7) &&
           same_app(r->app, exe) && same_addrs(r->remote, ADDRS);
}
static int keep(const fw_rule_t *r, const wchar_t *exe) {   // what the plan looks at
    return !wcscmp(r->name, OUR_NAME) || !wcscmp(r->name, OLD_NAME) || same_app(r->app, exe);
}

typedef struct { int del_ours, del_old, add, n_ours, n_ok; } plan_t;
static void plan(const fw_rule_t *r, int n, const wchar_t *exe, int want, plan_t *p, fw_result_t *res) {
    int foreign = 0, olds = 0, old_i = -1;
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < n; i++) {
        if (!wcscmp(r[i].name, OUR_NAME)) { if (ours(&r[i])) { p->n_ours++; p->n_ok += current(&r[i], exe); } else foreign++; }
        else if (!wcscmp(r[i].name, OLD_NAME)) { olds++; if (old_rule(&r[i], exe)) old_i = i; }
        if (r[i].in && !r[i].allow && r[i].enabled && same_app(r[i].app, exe) && !res->blocks++)
            wcsncpy_s(res->block, 128, r[i].name, _TRUNCATE);
    }
    // a rule is removed by its name only, so only when every rule of that name is the one meant
    p->del_old = olds == 1 && old_i >= 0;
    res->unclear = foreign + olds - p->del_old;
    if (foreign) return;
    if (!want) p->del_ours = p->n_ours;
    else if (p->n_ours != 1 || p->n_ok != 1) { p->del_ours = p->n_ours; p->add = 1; }
}

static int sync_with(const fw_ops_t *ops, const wchar_t *exe, int want, fw_result_t *res) {
    memset(res, 0, sizeof(*res));
    fw_rule_t *r = calloc(MAX_RULES, sizeof(*r));
    if (!r) { res->err = E_OUTOFMEMORY; return 0; }
    plan_t p;
    int n = ops->list(exe, r, MAX_RULES, &res->err);
    if (n >= 0) {
        plan(r, n, exe, want, &p, res);
        HRESULT hr = S_OK;
        for (int k = 0; k < p.del_ours; k++) {
            HRESULT h = ops->remove(OUR_NAME);
            if (SUCCEEDED(h)) res->removed++;
            else if (!k) { hr = h; break; }   // a later one may fail only because the first took every rule of the name
        }
        if (SUCCEEDED(hr) && p.del_old && SUCCEEDED(hr = ops->remove(OLD_NAME))) res->migrated = 1;
        if (SUCCEEDED(hr) && p.add) {
            fw_rule_t a = { OUR_NAME, OUR_GROUP, L"", ADDRS, 1, 1, 1, NET_FW_PROFILE2_PRIVATE };
            wcsncpy_s(a.app, MAX_PATH, exe, _TRUNCATE);
            if (SUCCEEDED(hr = ops->add(&a))) res->added = 1;
        }
        if (FAILED(hr)) res->err = hr;
        // what is there now decides, not what the calls said
        plan_t q; fw_result_t again = { 0 };
        if ((n = ops->list(exe, r, MAX_RULES, &hr)) >= 0) {
            plan(r, n, exe, want, &q, &again);
            res->ok = want ? q.n_ours == 1 && q.n_ok == 1 : q.n_ours == 0;
        } else if (!res->err) res->err = hr;
    }
    free(r);
    return res->ok;
}

// ---------------------------------------------------------------- Windows (INetFwPolicy2)
static const GUID CLSID_Policy2 = { 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
static const GUID IID_Policy2   = { 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
static const GUID CLSID_Rule    = { 0x2C5BC43E, 0x3369, 0x4C33, { 0xAB, 0x0C, 0xBE, 0x94, 0x69, 0x67, 0x7A, 0xF4 } };
static const GUID IID_Rule      = { 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };
static const GUID IID_EnumVar   = { 0x00020404, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static HRESULT com_rules(INetFwRules **rules) {
    INetFwPolicy2 *p = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_Policy2, NULL, CLSCTX_INPROC_SERVER, &IID_Policy2, (void **)&p);
    if (SUCCEEDED(hr)) { hr = INetFwPolicy2_get_Rules(p, rules); INetFwPolicy2_Release(p); }
    return hr;
}
static void take(BSTR b, wchar_t *out, int cap) { wcsncpy_s(out, cap, b ? b : L"", _TRUNCATE); SysFreeString(b); }

static int com_list(const wchar_t *exe, fw_rule_t *out, int max, HRESULT *err) {
    INetFwRules *rules = NULL; IUnknown *u = NULL; IEnumVARIANT *en = NULL;
    int n = 0;
    HRESULT hr = com_rules(&rules);
    if (SUCCEEDED(hr)) hr = INetFwRules_get__NewEnum(rules, &u);
    if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(u, &IID_EnumVar, (void **)&en);
    if (SUCCEEDED(hr)) {
        VARIANT v; ULONG got = 0;
        VariantInit(&v);
        while (IEnumVARIANT_Next(en, 1, &v, &got) == S_OK && got) {
            INetFwRule *r = NULL;
            if (V_VT(&v) == VT_DISPATCH && V_DISPATCH(&v) && SUCCEEDED(IDispatch_QueryInterface(V_DISPATCH(&v), &IID_Rule, (void **)&r))) {
                fw_rule_t x = { 0 }; BSTR b = NULL; NET_FW_RULE_DIRECTION d = 0; NET_FW_ACTION a = 0; VARIANT_BOOL e = 0;
                if (SUCCEEDED(INetFwRule_get_Name(r, &b))) take(b, x.name, 128);
                b = NULL; if (SUCCEEDED(INetFwRule_get_Grouping(r, &b))) take(b, x.group, 128);
                b = NULL; if (SUCCEEDED(INetFwRule_get_ApplicationName(r, &b))) take(b, x.app, MAX_PATH);
                b = NULL; if (SUCCEEDED(INetFwRule_get_RemoteAddresses(r, &b))) take(b, x.remote, 512);
                if (SUCCEEDED(INetFwRule_get_Direction(r, &d))) x.in = d == NET_FW_RULE_DIR_IN;
                if (SUCCEEDED(INetFwRule_get_Action(r, &a))) x.allow = a == NET_FW_ACTION_ALLOW;
                if (SUCCEEDED(INetFwRule_get_Enabled(r, &e))) x.enabled = e != VARIANT_FALSE;
                INetFwRule_get_Profiles(r, &x.profiles);
                if (keep(&x, exe)) { if (n < max) out[n] = x; n++; }
                INetFwRule_Release(r);
            }
            VariantClear(&v);
        }
    }
    if (en) IEnumVARIANT_Release(en);
    if (u) IUnknown_Release(u);
    if (rules) INetFwRules_Release(rules);
    if (SUCCEEDED(hr) && n > max) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);   // not all seen: nothing is changed
    if (FAILED(hr)) { *err = hr; return -1; }
    return n;
}

static HRESULT com_remove(const wchar_t *name) {
    INetFwRules *rules = NULL;
    HRESULT hr = com_rules(&rules);
    if (SUCCEEDED(hr)) {
        BSTR b = SysAllocString(name);
        hr = b ? INetFwRules_Remove(rules, b) : E_OUTOFMEMORY;
        SysFreeString(b); INetFwRules_Release(rules);
    }
    return hr;
}

static HRESULT com_add(const fw_rule_t *x) {
    INetFwRules *rules = NULL; INetFwRule *r = NULL;
    HRESULT hr = com_rules(&rules);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&CLSID_Rule, NULL, CLSCTX_INPROC_SERVER, &IID_Rule, (void **)&r);
    BSTR name = SysAllocString(x->name), group = SysAllocString(x->group), app = SysAllocString(x->app), remote = SysAllocString(x->remote),
         desc = SysAllocString(L"Added by haku control for its phone page and for lights on the local network. haku changes this rule only.");
    if (SUCCEEDED(hr) && !(name && group && app && remote && desc)) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Name(r, name);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Description(r, desc);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Grouping(r, group);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_ApplicationName(r, app);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Direction(r, NET_FW_RULE_DIR_IN);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Action(r, NET_FW_ACTION_ALLOW);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Profiles(r, x->profiles);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_RemoteAddresses(r, remote);
    if (SUCCEEDED(hr)) hr = INetFwRule_put_Enabled(r, VARIANT_TRUE);
    if (SUCCEEDED(hr)) hr = INetFwRules_Add(rules, r);
    SysFreeString(name); SysFreeString(group); SysFreeString(app); SysFreeString(remote); SysFreeString(desc);
    if (r) INetFwRule_Release(r);
    if (rules) INetFwRules_Release(rules);
    return hr;
}

static const fw_ops_t com_ops = { com_list, com_remove, com_add };

int fw_sync(const wchar_t *exe, int want, fw_result_t *res) {
    HRESULT ci = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   // S_FALSE / RPC_E_CHANGED_MODE: already set up
    int ok = sync_with(&com_ops, exe, want, res);
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

#ifdef FW_TEST
// cl /DFW_TEST src\firewall.c: the plan against a firewall kept in memory (the PC's own firewall is never opened)
#include <assert.h>
#define EXE L"C:\\Program Files\\haku-control\\haku-control.exe"
// how Windows gives the old and the new rule's addresses back
#define NORM L"LocalSubnet,10.0.0.0/255.0.0.0,172.16.0.0/255.240.0.0,192.168.0.0/255.255.0.0,100.64.0.0/255.192.0.0"
static fw_rule_t fk[MAX_RULES]; static int nfk, calls, fail_list;
static HRESULT fail_add;
static int f_list(const wchar_t *exe, fw_rule_t *out, int max, HRESULT *err) {
    if (fail_list) { *err = E_ACCESSDENIED; return -1; }
    int n = 0;
    for (int i = 0; i < nfk; i++) if (keep(&fk[i], exe)) { if (n < max) out[n] = fk[i]; n++; }
    return n;
}
static HRESULT f_remove(const wchar_t *name) {
    calls++;
    for (int i = 0; i < nfk; i++) if (!wcscmp(fk[i].name, name)) { fk[i] = fk[--nfk]; return S_OK; }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
static HRESULT f_add(const fw_rule_t *r) {
    calls++;
    if (fail_add) return fail_add;
    assert(!wcscmp(r->remote, ADDRS));
    fk[nfk] = *r; wcscpy_s(fk[nfk].remote, 512, NORM); nfk++;
    return S_OK;
}
static const fw_ops_t fake = { f_list, f_remove, f_add };
static fw_rule_t rule(const wchar_t *name, const wchar_t *group, const wchar_t *app, const wchar_t *remote, int allow, long profiles) {
    fw_rule_t r = { 0 };
    wcscpy_s(r.name, 128, name); wcscpy_s(r.group, 128, group); wcscpy_s(r.app, MAX_PATH, app); wcscpy_s(r.remote, 512, remote);
    r.in = 1; r.allow = allow; r.enabled = 1; r.profiles = profiles;
    return r;
}
static int count(const wchar_t *name) { int n = 0; for (int i = 0; i < nfk; i++) n += !wcscmp(fk[i].name, name); return n; }
static int has_current(void) { int n = 0; for (int i = 0; i < nfk; i++) n += current(&fk[i], EXE); return n; }

int main(void) {
    fw_result_t r;
    wchar_t x[16][40];
    assert(same_addrs(ADDRS, NORM) && !same_addrs(ADDRS, L"LocalSubnet,10.0.0.0/8") && !same_addrs(L"*", ADDRS));
    assert(canon(L" 10.0.0.0/8 ,LocalSubnet", x) == 2 && !wcscmp(x[0], L"10.0.0.0/255.0.0.0") && !wcscmp(x[1], L"localsubnet"));
    assert(same_app(L"C:\\PROGRAM FILES\\haku-control\\haku-control.exe", EXE) && !same_app(L"", EXE));

    // clean install: one rule, Private networks only
    nfk = 0; calls = 0;
    assert(sync_with(&fake, EXE, 1, &r) && r.added && !r.migrated && !r.blocks && !r.unclear && nfk == 1 && has_current() == 1);
    assert(fk[0].profiles == NET_FW_PROFILE2_PRIVATE && !wcscmp(fk[0].group, OUR_GROUP));
    // run again (every start): nothing is called, nothing doubles
    calls = 0;
    assert(sync_with(&fake, EXE, 1, &r) && !r.added && calls == 0 && nfk == 1);
    assert(sync_with(&fake, EXE, 1, &r) && calls == 0 && nfk == 1);

    // upgrade from 0.4.86: the old any-network rule goes, the new one comes, the user's own rules stay
    nfk = 0;
    fk[nfk++] = rule(OLD_NAME, L"", EXE, NORM, 1, NET_FW_PROFILE2_ALL);
    fk[nfk++] = rule(L"Steam", L"", L"C:\\Steam\\steam.exe", L"*", 1, NET_FW_PROFILE2_ALL);
    fk[nfk++] = rule(L"my haku tweak", L"", EXE, L"192.168.1.5", 1, NET_FW_PROFILE2_PUBLIC);
    assert(sync_with(&fake, EXE, 1, &r) && r.added && r.migrated && !r.unclear);
    assert(nfk == 3 && !count(OLD_NAME) && count(L"Steam") && count(L"my haku tweak") && has_current() == 1);
    calls = 0;
    assert(sync_with(&fake, EXE, 1, &r) && calls == 0 && nfk == 3);

    // an old-named rule changed by hand (other addresses), or two of that name: whose it is is unclear, so it stays
    nfk = 0;
    fk[nfk++] = rule(OLD_NAME, L"", EXE, L"LocalSubnet", 1, NET_FW_PROFILE2_ALL);
    assert(sync_with(&fake, EXE, 1, &r) && r.added && !r.migrated && r.unclear == 1 && count(OLD_NAME) == 1 && has_current() == 1);
    nfk = 0;
    fk[nfk++] = rule(OLD_NAME, L"", EXE, NORM, 1, NET_FW_PROFILE2_ALL);
    fk[nfk++] = rule(OLD_NAME, L"", L"D:\\other\\app.exe", NORM, 1, NET_FW_PROFILE2_ALL);
    assert(sync_with(&fake, EXE, 1, &r) && !r.migrated && r.unclear == 2 && count(OLD_NAME) == 2 && has_current() == 1);
    // another copy's old rule (other folder): not this exe, so not haku's to judge here
    nfk = 0;
    fk[nfk++] = rule(OLD_NAME, L"", L"D:\\portable\\haku-control.exe", NORM, 1, NET_FW_PROFILE2_ALL);
    assert(sync_with(&fake, EXE, 1, &r) && !r.migrated && r.unclear == 1 && count(OLD_NAME) == 1);

    // the user's (or Windows') block rule: reported, kept, nothing else blocked by it
    nfk = 0;
    assert(sync_with(&fake, EXE, 1, &r));
    fk[nfk++] = rule(L"haku-control.exe", L"", EXE, L"*", 0, NET_FW_PROFILE2_PUBLIC);
    calls = 0;
    assert(sync_with(&fake, EXE, 1, &r) && r.blocks == 1 && !wcscmp(r.block, L"haku-control.exe") && calls == 0 && nfk == 2);
    fk[nfk - 1].enabled = 0;   // switched off: no longer a conflict
    assert(sync_with(&fake, EXE, 1, &r) && !r.blocks);

    // ours made stale (any network, by hand or an older build) and doubled (a run cut off): put right, one left
    nfk = 0;
    fk[nfk++] = rule(OUR_NAME, OUR_GROUP, EXE, NORM, 1, NET_FW_PROFILE2_ALL);
    fk[nfk++] = rule(OUR_NAME, OUR_GROUP, EXE, NORM, 1, NET_FW_PROFILE2_PRIVATE);
    assert(sync_with(&fake, EXE, 1, &r) && count(OUR_NAME) == 1 && has_current() == 1);
    nfk = 0;
    fk[nfk++] = rule(OUR_NAME, OUR_GROUP, EXE, NORM, 1, NET_FW_PROFILE2_ALL);
    assert(sync_with(&fake, EXE, 1, &r) && r.removed == 1 && r.added && nfk == 1 && fk[0].profiles == NET_FW_PROFILE2_PRIVATE);
    // a rule of our name that is not ours (no group): removing by name could hit it, so nothing is changed
    nfk = 0;
    fk[nfk++] = rule(OUR_NAME, L"", EXE, L"*", 1, NET_FW_PROFILE2_ALL);
    calls = 0;
    assert(!sync_with(&fake, EXE, 1, &r) && r.unclear == 1 && calls == 0 && nfk == 1);

    // uninstall: ours and the old rule go, the block rule and the others stay
    nfk = 0;
    assert(sync_with(&fake, EXE, 1, &r));
    fk[nfk++] = rule(OLD_NAME, L"", EXE, NORM, 1, 7);
    fk[nfk++] = rule(L"haku-control.exe", L"", EXE, L"*", 0, NET_FW_PROFILE2_PUBLIC);
    fk[nfk++] = rule(L"Steam", L"", L"C:\\Steam\\steam.exe", L"*", 1, NET_FW_PROFILE2_ALL);
    assert(sync_with(&fake, EXE, 0, &r) && r.migrated && r.blocks == 1 && nfk == 2 && !count(OUR_NAME) && !count(OLD_NAME));
    assert(sync_with(&fake, EXE, 0, &r) && nfk == 2);

    // no rights (the window started without them) or no answer: said so, nothing claimed
    nfk = 0; fail_add = E_ACCESSDENIED;
    assert(!sync_with(&fake, EXE, 1, &r) && r.err == E_ACCESSDENIED && !r.added && nfk == 0);
    fail_add = 0; fail_list = 1;
    assert(!sync_with(&fake, EXE, 1, &r) && r.err == E_ACCESSDENIED);
    fail_list = 0;
    printf("firewall: ok\n");
    return 0;
}
#endif
