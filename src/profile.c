// SPDX-License-Identifier: GPL-3.0-only
// Profiles' files and their bookkeeping, apart from main.c so tools/sim/profile_test.c can run them over config.c.
// [profile.N] (N = 1..PROFILE_MAX) holds a profile's name and hotkey, its setup is in profiles\N.ini (sections and keys
// as in settings.ini, only those prof_keep says belong to a profile). [general] profile=N is the one in use: its setup
// IS the live settings, written to its file when another profile takes over.
//
// A file is written whole or not at all (file_begin/file_commit), and read whole and checked before anything of the
// live setup changes. Since v2 a file ends with "; end": one cut short is refused, not taken as "everything default".
// Files from before (no v2 in the first line) are read as they always were.
#include "common.h"
#include <stdlib.h>

prof_keep_fn prof_keep;

static int err, err_id, err_n;
static int fail(int code, int id) {
    err = code; err_id = id; err_n++;
    logf_("profile %d: %s", id, code == PE_WRITE ? "its file can't be written" : code == PE_LEAVE ? "the one in use can't be written, not switched" :
          code == PE_MISSING ? "its file is missing" : code == PE_BAD ? "its file is damaged, not used" : code == PE_FULL ? "all are taken" :
          "the settings can't be written yet");
    return 0;
}
int prof_json(char *out, int cap) { return snprintf(out, cap, "{\"err\":%d,\"id\":%d,\"n\":%d}", err, err_id, err_n); }

static void prof_file(int id, wchar_t *out) {
    wchar_t n[40]; swprintf(n, 40, L"profiles\\%d.ini", id);
    app_data_path(n, out);
}
static void prof_dir(void) { wchar_t p[MAX_PATH]; app_data_path(L"profiles", p); CreateDirectoryW(p, NULL); }

int prof_exists(int id) {
    char sec[24]; snprintf(sec, sizeof(sec), "profile.%d", id);
    return id >= 1 && id <= PROFILE_MAX && cfg_get(sec, "name", NULL) != NULL;
}
static int prof_free(void) {
    for (int i = 1; i <= PROFILE_MAX; i++) if (!prof_exists(i)) return i;
    return 0;
}

// the live setup -> profiles\N.ini; 0: not written (the file is as it was)
int prof_write(int id) {
    static char secs[512][64]; static cfg_item it[512];
    wchar_t p[MAX_PATH], tmp[MAX_PATH]; prof_dir(); prof_file(id, p);
    FILE *f = file_begin(p, tmp);
    if (!f) return 0;
    fprintf(f, "; haku control profile %d v2: its setup, written when another profile takes over\r\n", id);
    int ns = cfg_sections("", secs, 512);
    for (int s = 0; s < ns; s++) {
        int n = cfg_items(secs[s], it, 512), head = 0;
        for (int i = 0; i < n; i++) {
            if (!it[i].val[0] || !prof_keep(secs[s], it[i].key)) continue;
            if (!head++) fprintf(f, "[%s]\r\n", secs[s]);
            fprintf(f, "%s=%s\r\n", it[i].key, it[i].val);
        }
    }
    fprintf(f, "; end\r\n");
    return file_commit(f, tmp, p);
}

#define FILE_CAP (2 << 20)   // a profile is a few KB; more is not one of ours
static char *slurp(const wchar_t *p, size_t *len) {
    FILE *f = _wfopen(p, L"rb");
    if (!f) return NULL;
    char *b = malloc(FILE_CAP + 1);
    size_t n = b ? fread(b, 1, FILE_CAP + 1, f) : 0;
    fclose(f);
    if (!b) return NULL;
    if (n > FILE_CAP) n = 0;   // too big: read as damaged (empty)
    b[n] = 0; *len = n;
    return b;
}

// a profile file's text -> its entries (only the keys of a profile's setup); PROF_BAD when it isn't one whole file
int prof_parse(char *b, size_t len, prof_entry *e, int max) {
    int n = 0, v2 = -1, ended = 0; char sec[64] = "";
    if (len >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b += 3;
    for (char *s = b, *next; *s; s = next) {
        next = s + strcspn(s, "\n"); if (*next) *next++ = 0;
        char *end = s + strlen(s);
        while (end > s && (end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        if (!*s) continue;
        if (v2 < 0) {   // the first line says what it is
            if (strncmp(s, "; haku control profile", 22)) return PROF_BAD;
            v2 = strstr(s, " v2:") != NULL;
            continue;
        }
        if (ended) return PROF_BAD;   // nothing after the end
        if (*s == ';') { if (v2 && !strcmp(s, "; end")) ended = 1; continue; }
        if (*s == '[') {
            char *c = strchr(s, ']');
            if (!c || c - s - 1 >= 64) { if (v2) return PROF_BAD; sec[0] = 0; continue; }
            *c = 0; snprintf(sec, sizeof(sec), "%s", s + 1); continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !sec[0] || eq - s >= 64 || strlen(eq + 1) >= 256) { if (v2) return PROF_BAD; continue; }
        *eq = 0;
        if (!prof_keep(sec, s)) continue;
        if (n >= max) return PROF_BAD;
        snprintf(e[n].sec, 64, "%s", sec); snprintf(e[n].key, 64, "%s", s); snprintf(e[n].val, 256, "%s", eq + 1);
        n++;
    }
    if (v2 < 0 || (v2 && !ended)) return PROF_BAD;   // empty, or cut short
    return n;
}

// profiles\N.ini -> the live setup, what the file doesn't have back to its default. Nothing changes unless the
// whole file is good: 1 done, PROF_MISSING, PROF_BAD
int prof_load(int id) {
    static prof_entry e[4096]; static char secs[512][64]; static cfg_item it[512];
    wchar_t p[MAX_PATH]; prof_file(id, p);
    size_t len; char *b = slurp(p, &len);
    if (!b) return PROF_MISSING;
    int n = prof_parse(b, len, e, 4096);
    free(b);
    if (n < 0) return PROF_BAD;
    // the preview's groups are replaced whole; everything else of the setup is emptied, then filled from the file
    int ns = cfg_sections("group.", secs, 512);
    for (int s = 0; s < ns; s++) cfg_remove_section(secs[s]);
    ns = cfg_sections("", secs, 512);
    for (int s = 0; s < ns; s++) {
        int m = cfg_items(secs[s], it, 512);
        for (int i = 0; i < m; i++) if (it[i].val[0] && prof_keep(secs[s], it[i].key)) cfg_set(secs[s], it[i].key, "");
    }
    for (int i = 0; i < n; i++) cfg_set(e[i].sec, e[i].key, e[i].val);
    return 1;
}

// the one in use writes its changes to its file, then profile id's setup becomes the live one. 0: nothing changed
// (the error is in prof_json): the one in use can't keep its changes, or id's file is missing or damaged
int prof_switch(int id) {
    if (!prof_exists(id)) return 0;
    int cur = cfg_geti("general", "profile", 0);
    if (cur != id && prof_exists(cur) && !prof_write(cur)) return fail(PE_LEAVE, cur);   // switching would lose them
    int r = prof_load(id);
    if (r != 1) return fail(r == PROF_MISSING ? PE_MISSING : PE_BAD, id);
    char v[8]; snprintf(v, sizeof(v), "%d", id);
    cfg_set("general", "profile", v);
    return 1;
}

// the settings with the change could not be written: the change is kept in memory and written with the next one
void prof_saved(int id) { if (!cfg_save_if_dirty()) fail(PE_CFG, id); }

// [profile.N] name: what was given (else the one it has, else "Profile N"), without what would break the file
static const char *prof_name(const char *sec, int id, const char *name) {
    char nm[80]; snprintf(nm, sizeof(nm), "%s", name && *name ? name : cfg_get(sec, "name", ""));
    for (char *p = nm; *p; p++) if (*p == ';' || *p == '[' || *p == ']' || *p == '=' || (unsigned char)*p < 0x20) *p = ' ';
    if (!nm[0]) snprintf(nm, sizeof(nm), "Profile %d", id);
    cfg_set(sec, "name", nm);
    return cfg_get(sec, "name", "");
}

// id 0 (or one not there): a new profile with the setup as it is now, which becomes the one in use (the profile used
// before keeps what it had when it was last left); else rewrites profile id with the setup as it is now.
// The file first: a profile is named only once its file is there. Returns its number, 0: none (see prof_json)
int prof_save(int id, const char *name) {
    if (!prof_exists(id) && !(id = prof_free())) return fail(PE_FULL, 0);
    if (!prof_write(id)) return fail(PE_WRITE, id);
    char sec[24], v[8]; snprintf(sec, sizeof(sec), "profile.%d", id); snprintf(v, sizeof(v), "%d", id);
    const char *nm = prof_name(sec, id, name);
    cfg_set("general", "profile", v);
    prof_saved(id);
    logf_("profile %d saved: %s", id, nm);
    return id;
}

// a new profile with what profile src has (the one in use: as it is now), not its hotkey, programs or time.
// src's file is checked whole before it is copied. Returns its number, 0: none (see prof_json)
int prof_dup(int src, const char *name) {
    static cfg_item it[64]; static prof_entry e[4096];
    int id = prof_free();
    if (!prof_exists(src)) return 0;
    if (!id) return fail(PE_FULL, 0);
    if (src == cfg_geti("general", "profile", 0) && !prof_write(src)) return fail(PE_WRITE, src);   // not a stale copy
    wchar_t from[MAX_PATH], to[MAX_PATH], tmp[MAX_PATH]; prof_file(src, from); prof_file(id, to);
    size_t len; char *b = slurp(from, &len);
    if (!b) return fail(PE_MISSING, src);
    char *chk = malloc(len + 1);
    int good = chk && (memcpy(chk, b, len + 1), prof_parse(chk, len, e, 4096) >= 0);
    free(chk);
    if (!good) { free(b); return fail(PE_BAD, src); }
    FILE *f = file_begin(to, tmp);
    int ok = f && fwrite(b, 1, len, f) == len;
    if (f) ok = file_commit(f, tmp, to) && ok;
    free(b);
    if (!ok) return fail(PE_WRITE, id);
    char ss[24], sec[24]; snprintf(ss, sizeof(ss), "profile.%d", src); snprintf(sec, sizeof(sec), "profile.%d", id);
    static const char *own[] = { "name", "hotkey", "apps", "at", "days", "at_off", "pin" };
    int n = cfg_items(ss, it, 64);
    for (int i = 0; i < n; i++) {
        int skip = 0;
        for (int k = 0; k < (int)(sizeof(own) / sizeof(own[0])); k++) skip |= !_stricmp(it[i].key, own[k]);
        if (!skip) cfg_set(sec, it[i].key, it[i].val);
    }
    prof_name(sec, id, name);
    prof_saved(id);
    logf_("profile %d: a copy of %d", id, src);
    return id;
}

// its file goes only once the settings without it are written: a restart never finds a profile without its file
void prof_delete(int id) {
    char sec[24]; snprintf(sec, sizeof(sec), "profile.%d", id);
    cfg_remove_section(sec);
    if (cfg_geti("general", "profile", 0) == id) cfg_set("general", "profile", "");   // the setup stays as it is
    if (!cfg_save_if_dirty()) { fail(PE_CFG, id); return; }
    wchar_t p[MAX_PATH]; prof_file(id, p); DeleteFileW(p);
}

// at start: [general] profile names one that is gone (its [profile.N] removed by hand, a restore): none is in use
void prof_check(void) {
    int cur = cfg_geti("general", "profile", 0);
    if (cur && !prof_exists(cur)) { cfg_set("general", "profile", ""); cfg_save_if_dirty(); }
}
