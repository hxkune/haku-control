// SPDX-License-Identifier: GPL-3.0-only
// A backup of the settings, to move them to another PC or keep them safe: one text file (.hakubackup) with
// settings.ini and every profile (profiles\N.ini). What only belongs to this PC stays out of it: device keys and
// tokens, passwords, e-mail addresses, PINs (keys named like that, as in the diagnostics) and the [pro] section
// (licence, trial). Restoring replaces the settings and profiles with the backup's, and keeps this PC's own secrets:
// [pro] as it is, a device's key when the backup has the same device (kind and address) in the same place, the
// other secrets (e.g. the MQTT password) as they are. Devices whose key is not on this PC are paired once again.
//   haku control backup 2
//   ; made ... (comments)
//   #file settings.ini
//   [general]
//   ...
//   #file profiles/3.ini
//   ...
//   #end <lines> <fnv-1a of them>
// The end line makes a file cut short or changed on the way show: one without it, or whose lines do not add up, is
// refused whole. Version 1 (before 0.4.64) has no end line and is still read; a v1 file cut short between its parts
// cannot be told from a whole one (a profile cut short in its v2 format can).
#include "common.h"
#include "devices.h"
#include "../res/version.h"
#include <commdlg.h>
#include <errno.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define BK_MAGIC1 "haku control backup 1"
#define BK_MAGIC2 "haku control backup 2"

// tests only: a restore stops there as if the app were killed (1 the journal written, 2 one profile swapped, 3 all
// swapped, 4 the settings in); 0 in the app
int backup_stop_at;

// the last result, for the window (which words it): saved / restored (with counts) / bad_write / bad_open /
// bad_file / bad_prof / full / empty
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
    char l[64], ls[64]; snprintf(l, sizeof(l), "%s", k); _strlwr_s(l, sizeof(l)); snprintf(ls, sizeof(ls), "%s", sec); _strlwr_s(ls, sizeof(ls));
    if (cfg_pc_only(ls, l)) return 1;   // a program this PC starts, where its updates come from: never taken from a file
    if (!strncmp(l, "hotkey", 6)) return 0;
    if (!strcmp(l, "pin") && !_strnicmp(sec, "profile.", 8)) return 0;   // a profile pinned to the top, not a PIN
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

static void prof_path(int id, const wchar_t *ext, wchar_t *out) {
    wchar_t n[40]; swprintf(n, 40, L"profiles\\%d.ini%s", id, ext); app_data_path(n, out);
}
static int there(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int gone(const wchar_t *p) { return DeleteFileW(p) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND; }

// FNV-1a over a line and its end
static unsigned fnv(unsigned h, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    h ^= '\n'; return h * 16777619u;
}
static void cut_eol(char *s) { size_t l = strlen(s); while (l && (s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = 0; }
// a line longer than the buffer (none we write is): the file is not one of ours
static int too_long(const char *line, size_t cap, FILE *f) { size_t l = strlen(line); return l == cap - 1 && line[l - 1] != '\n' && !feof(f); }

// a profile written in the v2 format ends with "; end"; one cut short is not whole (older ones have no end to check)
static int prof_whole(const char *t, int len) {
    const char *nl = memchr(t, '\n', len);
    int first = nl ? (int)(nl - t) : len;
    char head[128]; snprintf(head, sizeof(head), "%.*s", first < 127 ? first : 127, t);
    if (strncmp(head, "; haku control profile", 22) || !strstr(head, " v2:")) return 1;
    while (len && (t[len - 1] == '\r' || t[len - 1] == '\n' || t[len - 1] == ' ')) len--;
    return len >= 5 && !strncmp(t + len - 5, "; end", 5) && (len == 5 || t[len - 6] == '\n');
}

// ---------------------------------------------------------------- export
typedef struct { FILE *f; unsigned h; int n, bad; } sink;
static void out(sink *k, const char *fmt, ...) {
    char b[1100]; va_list a; va_start(a, fmt); int l = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (l < 0 || l >= (int)sizeof(b)) { k->bad = 1; return; }
    fprintf(k->f, "%s\r\n", b); k->h = fnv(k->h, b, (size_t)l); k->n++;
}

// Written beside the chosen file and put in its place only when all of it is on disk: a backup that fails part way
// leaves the one that was there as it was, and says so instead of "saved".
void backup_save(void *owner) {
    wchar_t file[MAX_PATH], tmp[MAX_PATH];
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(file, MAX_PATH, L"haku-control-backup-%04d-%02d-%02d.hakubackup", t.wYear, t.wMonth, t.wDay);
    if (!pick_file((HWND)owner, 1, file)) return;
    if (!app_profiles_flush()) { say("bad_prof", 0, 0); return; }   // the profile in use would go in stale
    sink k = { file_begin(file, tmp), 2166136261u, 0, 0 };
    if (!k.f) { say("bad_write", 0, 0); return; }
    fprintf(k.f, BK_MAGIC2 "\r\n");
    out(&k, "; made %04d-%02d-%02d by haku control " HAKU_VER_STR, t.wYear, t.wMonth, t.wDay);
    out(&k, "; without device keys, passwords or the licence: they stay on each PC");
    out(&k, "#file settings.ini");
    static char secs[1024][64]; static cfg_item it[512];
    int ns = cfg_sections("", secs, 1024), lines = 0;
    for (int s = 0; s < ns; s++) {
        if (!_stricmp(secs[s], "pro")) continue;
        int n = cfg_items(secs[s], it, 512), head = 0;
        for (int i = 0; i < n; i++) {
            if (secret(secs[s], it[i].key)) continue;
            if (!head++) out(&k, "[%s]", secs[s]);
            out(&k, "%s=%s", it[i].key, it[i].val); lines++;
        }
    }
    // the profiles there are ([profile.N] named); a file of one deleted is left behind
    int profiles = 0, bad_prof = 0;
    for (int id = 1; id <= PROFILE_MAX && !bad_prof; id++) {
        char sec[24]; snprintf(sec, sizeof(sec), "profile.%d", id);
        if (!cfg_get(sec, "name", NULL)) continue;
        wchar_t p[MAX_PATH]; prof_path(id, L"", p);
        FILE *pf = _wfopen(p, L"rb");
        if (!pf) { if (errno != ENOENT) bad_prof = 1; continue; }   // never written yet: the backup has its name only
        out(&k, "#file profiles/%d.ini", id);
        char line[1024]; static char body[32768]; int len = 0;
        while (fgets(line, sizeof(line), pf)) {
            if (too_long(line, sizeof(line), pf)) { bad_prof = 1; break; }
            cut_eol(line);
            out(&k, "%s", line);
            size_t l = strlen(line);
            if (len + (int)l + 2 < (int)sizeof(body)) { memcpy(body + len, line, l); len += (int)l; body[len++] = '\n'; }
        }
        if (ferror(pf) || !prof_whole(body, len)) bad_prof = 1;   // damaged on disk: a restore would refuse it
        fclose(pf);
        profiles++;
    }
    if (bad_prof || k.bad) { fclose(k.f); DeleteFileW(tmp); say(bad_prof ? "bad_prof" : "bad_write", 0, 0); return; }
    fprintf(k.f, "#end %d %08x\r\n", k.n, k.h);
    if (!file_commit(k.f, tmp, file)) { say("bad_write", 0, 0); return; }
    say("saved", lines, profiles);
}

// ---------------------------------------------------------------- reading a backup
typedef struct { char sec[64], key[64], val[256]; } entry;
static entry e[8192];
static int ne;
static char prof[PROFILE_MAX + 1][32768];
static int plen[PROFILE_MAX + 1];
static unsigned char has_file[PROFILE_MAX + 1];

static int named(int id) {
    char sec[24]; snprintf(sec, sizeof(sec), "profile.%d", id);
    for (int i = 0; i < ne; i++) if (!_stricmp(e[i].sec, sec) && !_stricmp(e[i].key, "name")) return 1;
    return 0;
}

// NULL: read whole and checked; else what is wrong: bad_open, bad_file (cut short, changed, not a backup), empty
static const char *read_backup(const wchar_t *file) {
    FILE *f = _wfopen(file, L"rb");
    if (!f) return "bad_open";
    ne = 0; memset(plen, 0, sizeof(plen)); memset(has_file, 0, sizeof(has_file));
    int ver = 0, cur = -1, have_settings = 0, bad = 0, ended = 0, nl = 0;
    unsigned h = 2166136261u;
    char line[1024], sec[64] = "";
    if (fgets(line, sizeof(line), f)) { cut_eol(line); ver = !strcmp(line, BK_MAGIC2) ? 2 : !strcmp(line, BK_MAGIC1) ? 1 : 0; }
    if (!ver) { fclose(f); return "bad_file"; }
    while (!bad && fgets(line, sizeof(line), f)) {
        if (too_long(line, sizeof(line), f)) { bad = 1; break; }
        cut_eol(line);
        size_t l = strlen(line);
        if (ended) { if (l) bad = 1; continue; }   // nothing after the end
        if (ver == 2 && !strncmp(line, "#end ", 5)) {
            int cnt = -1; unsigned hh = 0;
            ended = sscanf_s(line + 5, "%d %x", &cnt, &hh) == 2 && cnt == nl && hh == h;
            if (!ended) bad = 1;
            continue;
        }
        nl++; h = fnv(h, line, l);
        if (!strncmp(line, "#file ", 6)) {
            int id = 0; char want[32];
            if (sscanf_s(line + 6, "profiles/%d.ini", &id) == 1) snprintf(want, sizeof(want), "profiles/%d.ini", id);
            if (!strcmp(line + 6, "settings.ini")) { bad |= have_settings; cur = 0; have_settings = 1; }
            else if (id >= 1 && id <= PROFILE_MAX && !strcmp(line + 6, want)) { bad |= has_file[id]; cur = id; has_file[id] = 1; }
            else bad = 1;   // a part twice, or one no backup has
            continue;
        }
        if (cur > 0) {   // a profile's file, as it is
            if (plen[cur] + (int)l + 3 >= (int)sizeof(prof[0])) { bad = 1; break; }
            memcpy(prof[cur] + plen[cur], line, l); plen[cur] += (int)l; prof[cur][plen[cur]++] = '\r'; prof[cur][plen[cur]++] = '\n';
            continue;
        }
        if (cur < 0) { if (ver == 2 && l && line[0] != ';') bad = 1; continue; }   // before the first part: comments only
        if (!l || line[0] == ';') continue;
        if (line[0] == '[') {
            char *c = strchr(line, ']');
            if (!c || c - line - 1 >= (int)sizeof(sec)) { bad = 1; break; }
            *c = 0; snprintf(sec, sizeof(sec), "%s", line + 1); continue;
        }
        char *eq = strchr(line, '=');
        if (!eq || !sec[0] || eq - line >= 64 || strlen(eq + 1) >= 256) { bad = 1; break; }   // not what a backup holds
        if (!_stricmp(sec, "pro")) continue;
        *eq = 0;
        if (secret(sec, line)) continue;   // (the name only: a value may say "keyboard")
        if (ne >= 8192) { bad = 1; break; }
        snprintf(e[ne].sec, 64, "%s", sec); snprintf(e[ne].key, 64, "%s", line); snprintf(e[ne].val, 256, "%s", eq + 1); ne++;
    }
    if (ferror(f)) bad = 1;
    fclose(f);
    if (ver == 2 && !ended) bad = 1;   // cut short: the end line never came
    for (int id = 1; id <= PROFILE_MAX; id++) if (has_file[id] && !prof_whole(prof[id], plen[id])) bad = 1;
    // a v2 backup has a file only for a named profile; v1 ones may have left-over files, taken as they are
    if (ver == 2) for (int id = 1; id <= PROFILE_MAX; id++) if (has_file[id] && !named(id)) bad = 1;
    if (bad) return "bad_file";
    if (!have_settings || !ne) return "empty";
    return NULL;
}

// ---------------------------------------------------------------- restore
// restore.journal, written before the first profile moves: "haku restore 1", "<id> <had_old> <has_new>" for each
// profile the swap touches, "; end". With it and settings.ini.restore on disk, a restore was cut off before its
// settings went in: the next start takes it back. With it alone, the settings went in: the next start finishes it.
static void journal_path(wchar_t *p) { app_data_path(L"restore.journal", p); }
static void restore_path(wchar_t *p) { swprintf(p, MAX_PATH, L"%s.restore", cfg_path()); }

// where each profile is in the swap: 1 = the old file set aside as N.ini.old, 2 = the backup's in place as N.ini
static unsigned char st[PROFILE_MAX + 1];

// every profile back as it was before the swap; 0: one could not be (it stays in N.ini.old, the journal with it)
static int profiles_back(void) {
    int ok = 1;
    for (int id = 1; id <= PROFILE_MAX; id++) {
        wchar_t p[MAX_PATH], po[MAX_PATH]; prof_path(id, L"", p); prof_path(id, L".old", po);
        if ((st[id] & 2) && !(st[id] & 1) && !gone(p)) ok = 0;
        if ((st[id] & 1) && !MoveFileExW(po, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            logf_("backup: profile %d could not be put back (error %lu), it is in %d.ini.old", id, GetLastError(), id);
            ok = 0;
        }
        st[id] = 0;
    }
    return ok;
}

static int stage(int id) {
    wchar_t p[MAX_PATH]; prof_path(id, L".new", p);
    FILE *pf = _wfopen(p, L"wb");
    if (!pf) return 0;
    int ok = fwrite(prof[id], 1, plen[id], pf) == (size_t)plen[id] && !fflush(pf) && !_commit(_fileno(pf));
    if (fclose(pf)) ok = 0;
    return ok;
}

// Nothing changes unless all of it can: the file is read and checked whole first (bad_file), the new settings are
// built in memory and refused whole when they do not fit (full), then everything is written beside what is in use
// (N.ini.new, settings.ini.restore) and a journal says what is about to move. The old profiles step aside (not
// deleted), the backup's take their place, and the settings go in last with one rename: that rename is the moment
// the restore happened. A failure before it puts every file back (bad_write); a crash before it is taken back at
// the next start, a crash after it is finished then (backup_recover).
void backup_load(void *owner) {
    wchar_t file[MAX_PATH] = L"";
    if (!pick_file((HWND)owner, 0, file)) return;
    const char *why = read_backup(file);
    if (why) { say(why, 0, 0); return; }
    wchar_t rp[MAX_PATH], jp[MAX_PATH]; restore_path(rp); journal_path(jp);
    backup_recover();   // one cut off before is put right first: its N.ini.old may be the only copy
    if (there(jp) || !cfg_save_if_dirty()) { say("bad_write", 0, 0); return; }   // memory is the file from here on
    memset(st, 0, sizeof(st));

    // this PC's secrets, to keep
    static entry keep[1024]; static char kind[1024][32], host[1024][64];
    static char secs[1024][64]; static cfg_item it[512];
    int nk = 0, kept = 0, lost = 0, ns = cfg_sections("", secs, 1024);
    for (int s = 0; s < ns; s++) {
        int m = cfg_items(secs[s], it, 512);
        for (int i = 0; i < m && nk < 1024; i++) {
            if (!secret(secs[s], it[i].key) || !it[i].val[0]) continue;
            snprintf(keep[nk].sec, 64, "%s", secs[s]); snprintf(keep[nk].key, 64, "%s", it[i].key); snprintf(keep[nk].val, 256, "%s", it[i].val);
            snprintf(kind[nk], 32, "%s", cfg_get(secs[s], "kind", "")); snprintf(host[nk], 64, "%s", cfg_get(secs[s], "host", ""));
            nk++;
        }
    }
    // the backup's settings in place of these, in memory only long enough to write them beside settings.ini
    cfg_group_begin();
    for (int s = 0; s < ns; s++) cfg_remove_section(secs[s]);
    for (int i = 0; i < ne; i++) cfg_set(e[i].sec, e[i].key, e[i].val);
    for (int i = 0; i < nk; i++) {
        if (!_strnicmp(keep[i].sec, "dev.", 4)) {   // a device's key: only for the same device in the same place
            if (_stricmp(cfg_get(keep[i].sec, "kind", ""), kind[i]) || strcmp(cfg_get(keep[i].sec, "host", ""), host[i])) { lost++; continue; }
        }
        cfg_set(keep[i].sec, keep[i].key, keep[i].val); kept++;
    }
    if (!cfg_group_end()) { say("full", 0, 0); return; }   // taken back whole: memory is as it was
    int ok = cfg_write_to(rp);
    // shortcut: for these few ms another thread's save would write the backup's settings early; a restore is rare
    cfg_load(NULL);   // memory is the old file again until the rename

    // the profiles beside the ones in use
    int np = 0;
    wchar_t dir[MAX_PATH]; app_data_path(L"profiles", dir); CreateDirectoryW(dir, NULL);
    for (int id = 1; id <= PROFILE_MAX && ok; id++) if (has_file[id]) { ok = stage(id); np++; }
    // what the swap will touch, and nothing left from a restore of an older version in the way
    char jl[PROFILE_MAX + 1][16]; int nj = 0;
    for (int id = 1; id <= PROFILE_MAX && ok; id++) {
        wchar_t p[MAX_PATH], po[MAX_PATH]; prof_path(id, L"", p); prof_path(id, L".old", po);
        if (!gone(po)) { ok = 0; break; }
        int had = there(p);
        if (had || has_file[id]) snprintf(jl[nj++], 16, "%d %d %d", id, had, has_file[id]);
    }
    if (ok) {
        wchar_t tmp[MAX_PATH]; FILE *jf = file_begin(jp, tmp);
        if (!jf) ok = 0;
        else {
            fprintf(jf, "haku restore 1\r\n");
            for (int i = 0; i < nj; i++) fprintf(jf, "%s\r\n", jl[i]);
            fprintf(jf, "; end\r\n");
            ok = file_commit(jf, tmp, jp);
        }
    }
    if (ok && backup_stop_at == 1) return;

    // the swap: an old profile steps aside (one another program holds open cannot, and nothing is changed yet
    // for it), the backup's takes its place; the ones the backup has not only step aside
    for (int id = 1; id <= PROFILE_MAX && ok; id++) {
        wchar_t p[MAX_PATH], po[MAX_PATH], pn[MAX_PATH]; prof_path(id, L"", p); prof_path(id, L".old", po); prof_path(id, L".new", pn);
        if (there(p)) {
            if (!MoveFileExW(p, po, MOVEFILE_WRITE_THROUGH)) { logf_("backup: profile %d is held (error %lu)", id, GetLastError()); ok = 0; break; }
            st[id] |= 1;
        }
        if (has_file[id]) {
            if (!MoveFileExW(pn, p, MOVEFILE_WRITE_THROUGH)) { ok = 0; break; }
            st[id] |= 2;
        }
        if (st[id] && backup_stop_at == 2) return;
    }
    if (ok && backup_stop_at == 3) return;
    // the settings in: from here the restore has happened
    if (ok && !MoveFileExW(rp, cfg_path(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ok = 0;
    if (!ok) {
        int back = profiles_back();
        for (int id = 1; id <= PROFILE_MAX; id++) { wchar_t p[MAX_PATH]; prof_path(id, L".new", p); DeleteFileW(p); }
        if (back) { DeleteFileW(jp); DeleteFileW(rp); }   // else both stay: the next start puts the rest back
        say("bad_write", 0, 0);
        return;
    }
    if (backup_stop_at == 4) return;
    cfg_load(NULL);
    // all of it in: the old profiles are not needed any more, the journal goes last
    for (int id = 1; id <= PROFILE_MAX; id++) { wchar_t po[MAX_PATH]; prof_path(id, L".old", po); if (st[id] & 1) DeleteFileW(po); st[id] = 0; }
    DeleteFileW(jp);
    app_reload_all();
    say("restored", ne, np);
    logf_("backup: kept %d secrets of this PC, %d belonged to other devices", kept, lost);
}

// At start, after cfg_load: a restore cut off part way (the app killed, the PC off) is taken back or finished, by
// what is on disk. The journal is deleted only once every file is where it belongs.
void backup_recover(void) {
    wchar_t jp[MAX_PATH], rp[MAX_PATH]; journal_path(jp); restore_path(rp);
    FILE *jf = _wfopen(jp, L"rb");
    if (!jf) {
        // no restore under way: a settings file staged before its journal was written is not needed; a profile set
        // aside by an older version's restore with nothing in its place goes back
        DeleteFileW(rp);
        for (int id = 1; id <= PROFILE_MAX; id++) {
            wchar_t p[MAX_PATH], po[MAX_PATH], pn[MAX_PATH]; prof_path(id, L"", p); prof_path(id, L".old", po); prof_path(id, L".new", pn);
            DeleteFileW(pn);
            if (there(po) && !there(p)) MoveFileExW(po, p, MOVEFILE_WRITE_THROUGH);
        }
        return;
    }
    char line[64]; int whole = 0, ok = 1, n = 0, ids[PROFILE_MAX + 1][3];
    if (fgets(line, sizeof(line), jf)) { cut_eol(line); whole = !strcmp(line, "haku restore 1") ? 1 : 0; }
    while (whole == 1 && fgets(line, sizeof(line), jf)) {
        cut_eol(line);
        if (!strcmp(line, "; end")) { whole = 2; break; }
        if (n > PROFILE_MAX || sscanf_s(line, "%d %d %d", &ids[n][0], &ids[n][1], &ids[n][2]) != 3 || ids[n][0] < 1 || ids[n][0] > PROFILE_MAX) { whole = 0; break; }
        n++;
    }
    fclose(jf);
    if (whole != 2) {   // file_commit writes it whole or not at all: this is not ours, leave the files to the user
        logf_("backup: restore.journal is not readable, the profiles' files are left as they are");
        return;
    }
    int committed = !there(rp);
    for (int i = 0; i < n; i++) {
        int id = ids[i][0], had = ids[i][1], has = ids[i][2];
        wchar_t p[MAX_PATH], po[MAX_PATH], pn[MAX_PATH]; prof_path(id, L"", p); prof_path(id, L".old", po); prof_path(id, L".new", pn);
        if (committed) { if (!gone(po) || !gone(pn)) ok = 0; continue; }
        // taken back: the old file where it was, the backup's gone
        if (there(po)) { if (!MoveFileExW(po, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ok = 0; }
        else if (!had && has && !there(pn) && !gone(p)) ok = 0;   // the backup's came in where there was none
        if (!gone(pn)) ok = 0;
    }
    if (!ok) { logf_("backup: a restore cut off could not be %s yet (error %lu)", committed ? "finished" : "taken back", GetLastError()); return; }
    if (!committed) DeleteFileW(rp);
    DeleteFileW(jp);
    logf_("backup: a restore cut off was %s", committed ? "finished" : "taken back");
}

// "backup":{"res":"saved","a":120,"b":3}
int backup_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    int n = jcat(out, cap, 0, "{\"res\":\"%s\",\"a\":%d,\"b\":%d}", res, res_a, res_b);
    ReleaseSRWLockShared(&lk);
    return n;
}
