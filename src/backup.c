// SPDX-License-Identifier: GPL-3.0-only
// A backup of the settings, to move them to another PC or keep them safe: one text file (.hakubackup) with
// settings.ini and every profile (profiles\N.ini). What only belongs to this PC stays out of it: device keys and
// tokens, passwords, e-mail addresses, PINs (keys named like that, as in the diagnostics) and the [pro] section
// (licence, trial). Restoring replaces the settings and profiles with the backup's, and keeps this PC's own secrets:
// [pro] as it is, a device's key when the backup has the same device (kind and address) in the same place, the
// other secrets (e.g. the MQTT password) as they are. Devices whose key is not on this PC are paired once again.
//   haku control backup 1
//   ; made ... (comments)
//   #file settings.ini
//   [general]
//   ...
//   #file profiles/3.ini
//   ...
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <commdlg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define BK_MAGIC "haku control backup 1"

// the last result, for the window (which words it): saved / restored (with counts) / bad_write / bad_open /
// bad_file / empty
static SRWLOCK lk = SRWLOCK_INIT;
static char res[16];
static int res_a, res_b;

static void say(const char *what, int a, int b) {
    AcquireSRWLockExclusive(&lk); snprintf(res, sizeof(res), "%s", what); res_a = a; res_b = b; ReleaseSRWLockExclusive(&lk);
    logf_("backup: %s %d %d", what, a, b);
}

// a value that belongs to this PC only (the diagnostics leave out the same ones); hotkeys only look like keys
static int secret(const char *sec, const char *k) {
    if (!_stricmp(sec, "pro")) return 1;
    char l[64]; snprintf(l, sizeof(l), "%s", k); _strlwr_s(l, sizeof(l));
    if (!strncmp(l, "hotkey", 6)) return 0;
    return strstr(l, "key") || strstr(l, "token") || strstr(l, "secret") || strstr(l, "pass") || strstr(l, "pin") || strstr(l, "mail");
}

static int pick_file(HWND owner, int save, wchar_t *file) {
#ifdef HAKU_DEV
    if (GetEnvironmentVariableW(L"HAKU_BACKUP_FILE", file, MAX_PATH)) return 1;   // test builds: no dialog
#endif
    OPENFILENAMEW of = { sizeof(of) };
    of.hwndOwner = owner; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrFilter = L"haku control backup (*.hakubackup)\0*.hakubackup\0All files\0*.*\0";
    of.lpstrDefExt = L"hakubackup";
    of.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
}

// ---------------------------------------------------------------- export
void backup_save(void *owner) {
    wchar_t file[MAX_PATH];
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(file, MAX_PATH, L"haku-control-backup-%04d-%02d-%02d.hakubackup", t.wYear, t.wMonth, t.wDay);
    if (!pick_file((HWND)owner, 1, file)) return;
    app_profiles_flush();   // the profile in use writes what changed in it to its file first
    FILE *f = _wfopen(file, L"wb");
    if (!f) { say("bad_write", 0, 0); return; }
    fprintf(f, BK_MAGIC "\r\n; made %04d-%02d-%02d by haku control " HAKU_VER_STR "\r\n"
            "; without device keys, passwords or the licence: they stay on each PC\r\n#file settings.ini\r\n",
            t.wYear, t.wMonth, t.wDay);
    static char secs[1024][64]; static cfg_item it[512];
    int ns = cfg_sections("", secs, 1024), lines = 0;
    for (int s = 0; s < ns; s++) {
        if (!_stricmp(secs[s], "pro")) continue;
        int n = cfg_items(secs[s], it, 512), head = 0;
        for (int i = 0; i < n; i++) {
            if (secret(secs[s], it[i].key)) continue;
            if (!head++) fprintf(f, "[%s]\r\n", secs[s]);
            fprintf(f, "%s=%s\r\n", it[i].key, it[i].val); lines++;
        }
    }
    int profiles = 0;
    for (int id = 1; id <= PROFILE_MAX; id++) {
        wchar_t p[MAX_PATH], n[40]; swprintf(n, 40, L"profiles\\%d.ini", id); app_data_path(n, p);
        FILE *pf = _wfopen(p, L"rb");
        if (!pf) continue;
        fprintf(f, "#file profiles/%d.ini\r\n", id);
        char line[1024];
        while (fgets(line, sizeof(line), pf)) {
            size_t l = strlen(line); while (l && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = 0;
            fprintf(f, "%s\r\n", line);
        }
        fclose(pf); profiles++;
    }
    int ok = !ferror(f);
    fclose(f);
    if (ok) say("saved", lines, profiles); else say("bad_write", 0, 0);
}

// ---------------------------------------------------------------- restore
typedef struct { char sec[64], key[64], val[256]; } entry;

void backup_load(void *owner) {
    wchar_t file[MAX_PATH] = L"";
    if (!pick_file((HWND)owner, 0, file)) return;
    FILE *f = _wfopen(file, L"rb");
    if (!f) { say("bad_open", 0, 0); return; }
    static entry e[8192]; static char prof[PROFILE_MAX + 1][32768]; static int plen[PROFILE_MAX + 1];
    int n = 0, cur = -1, have_settings = 0;   // cur: 0 = settings, 1..PROFILE_MAX = a profile
    memset(plen, 0, sizeof(plen));
    char line[1024], sec[64] = "";
    if (!fgets(line, sizeof(line), f) || strncmp(line, BK_MAGIC, strlen(BK_MAGIC))) { fclose(f); say("bad_file", 0, 0); return; }
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line); while (l && (line[l - 1] == '\r' || line[l - 1] == '\n')) line[--l] = 0;
        if (!strncmp(line, "#file ", 6)) {
            int id = 0;
            if (!strcmp(line + 6, "settings.ini")) { cur = 0; have_settings = 1; }
            else if (sscanf_s(line + 6, "profiles/%d.ini", &id) == 1 && id >= 1 && id <= PROFILE_MAX) cur = id;
            else cur = -1;
            continue;
        }
        if (cur > 0) {   // a profile's file, as it is
            if (plen[cur] + (int)l + 3 < (int)sizeof(prof[0])) { memcpy(prof[cur] + plen[cur], line, l); plen[cur] += (int)l; prof[cur][plen[cur]++] = '\r'; prof[cur][plen[cur]++] = '\n'; }
            continue;
        }
        if (cur != 0 || !line[0] || line[0] == ';') continue;
        if (line[0] == '[') { char *c = strchr(line, ']'); if (c) *c = 0; snprintf(sec, sizeof(sec), "%s", line + 1); continue; }
        char *eq = strchr(line, '=');
        if (!eq || !sec[0] || n >= 8192 || !_stricmp(sec, "pro")) continue;
        *eq = 0;
        if (secret(sec, line)) continue;   // (the name only: a value may say "keyboard")
        snprintf(e[n].sec, 64, "%s", sec); snprintf(e[n].key, 64, "%s", line); snprintf(e[n].val, 256, "%s", eq + 1); n++;
    }
    fclose(f);
    if (!have_settings || !n) { say("empty", 0, 0); return; }

    // this PC's secrets, to keep
    static entry keep[1024]; static char kind[1024][32], host[1024][64];
    static char secs[1024][64]; static cfg_item it[512];
    int nk = 0, ns = cfg_sections("", secs, 1024);
    for (int s = 0; s < ns; s++) {
        int m = cfg_items(secs[s], it, 512);
        for (int i = 0; i < m && nk < 1024; i++) {
            if (!secret(secs[s], it[i].key) || !it[i].val[0]) continue;
            snprintf(keep[nk].sec, 64, "%s", secs[s]); snprintf(keep[nk].key, 64, "%s", it[i].key); snprintf(keep[nk].val, 256, "%s", it[i].val);
            snprintf(kind[nk], 32, "%s", cfg_get(secs[s], "kind", "")); snprintf(host[nk], 64, "%s", cfg_get(secs[s], "host", ""));
            nk++;
        }
    }
    // the backup's settings in place of these
    for (int s = 0; s < ns; s++) cfg_remove_section(secs[s]);
    for (int i = 0; i < n; i++) cfg_set(e[i].sec, e[i].key, e[i].val);
    int kept = 0, lost = 0;
    for (int i = 0; i < nk; i++) {
        if (!_strnicmp(keep[i].sec, "dev.", 4)) {   // a device's key: only for the same device in the same place
            if (_stricmp(cfg_get(keep[i].sec, "kind", ""), kind[i]) || strcmp(cfg_get(keep[i].sec, "host", ""), host[i])) { lost++; continue; }
        }
        cfg_set(keep[i].sec, keep[i].key, keep[i].val); kept++;
    }
    cfg_save_if_dirty();
    // the profiles
    int np = 0;
    for (int id = 1; id <= PROFILE_MAX; id++) {
        wchar_t p[MAX_PATH], nm[40]; swprintf(nm, 40, L"profiles\\%d.ini", id); app_data_path(nm, p);
        if (!plen[id]) { DeleteFileW(p); continue; }
        wchar_t dir[MAX_PATH]; app_data_path(L"profiles", dir); CreateDirectoryW(dir, NULL);
        FILE *pf = _wfopen(p, L"wb");
        if (pf) { fwrite(prof[id], 1, plen[id], pf); fclose(pf); np++; }
    }
    app_reload_all();
    say("restored", n, np);
    logf_("backup: kept %d secrets of this PC, %d belonged to other devices", kept, lost);
}

// "backup":{"res":"saved","a":120,"b":3}
int backup_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "{\"res\":\"%s\",\"a\":%d,\"b\":%d}", res, res_a, res_b);
    ReleaseSRWLockShared(&lk);
    return n;
}
