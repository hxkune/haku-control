// SPDX-License-Identifier: GPL-3.0-only
// Tiny INI reader/writer. Keeps the file as lines so edits preserve user comments.
// Comments start with ';' at the start of a line or after a blank, or '#' at the start of a line ('#' inside
// values is a colour, ';' inside one is part of it: a Wi-Fi password, a name).
#include "common.h"
#include <stdlib.h>
#include <ctype.h>
#include <io.h>

#define MAX_LINES 4096   // presets with device colours take a few dozen lines each
#define LINE_LEN  256

static char     lines[MAX_LINES][LINE_LEN];
static int      nlines;
static wchar_t  path_w[MAX_PATH];
static FILETIME last_write;
static __declspec(thread) char ret_buf[8][LINE_LEN];   // per-thread return slots
static __declspec(thread) int ret_slot;
static SRWLOCK   lock = SRWLOCK_INIT;
static int      dirty, save_bad;      // save_bad: the last write failed and its changes are still only in memory
static int      load_bad;             // the file did not fit (a line too long, too many lines): writing would cut it for good
static volatile LONG dropped;         // changes refused since start (no line left, or a value too long for one)
static __declspec(thread) int my_dropped;   // the same, by this thread: what a group checks

static FILETIME file_time(void) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME z = { 0 };
    return GetFileAttributesExW(path_w, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

const wchar_t *cfg_path(void) { return path_w; }

void cfg_load(const wchar_t *p) {
    AcquireSRWLockExclusive(&lock);
    if (p) wcsncpy_s(path_w, MAX_PATH, p, _TRUNCATE);
    nlines = 0; dirty = save_bad = load_bad = 0;   // memory is the file again
    FILE *f = _wfopen(path_w, L"rb");
    if (!f) { ReleaseSRWLockExclusive(&lock); return; }
    char buf[LINE_LEN];
    while (nlines < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        if (n == LINE_LEN - 1 && buf[n - 1] != '\n') load_bad = 1;   // the rest of this line would read as a line of its own
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        // strip UTF-8 BOM on the first line
        char *s = buf;
        if (nlines == 0 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        strcpy_s(lines[nlines++], LINE_LEN, s);
    }
    if (nlines == MAX_LINES && fgets(buf, sizeof(buf), f)) load_bad = 1;
    fclose(f);
    if (load_bad) logf_("settings: the file has a line past %d bytes or more than %d lines; it is left as it is, changes are not saved", LINE_LEN - 2, MAX_LINES);
    last_write = file_time();
    ReleaseSRWLockExclusive(&lock);
}

int cfg_changed_on_disk(void) {
    FILETIME t = file_time();
    return CompareFileTime(&t, &last_write) != 0;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// where a line's comment starts: a ';' first on the line or after a blank. NULL: none
static char *comment_at(char *s) {
    for (char *c = s; (c = strchr(c, ';')) != NULL; c++) if (c == s || isspace((unsigned char)c[-1])) return c;
    return NULL;
}

// Finds key in section. Returns line index or -1; *sec_end gets the index after the section's last line.
static int find(const char *section, const char *key, char *val, int *sec_end) {
    int in = 0, found = -1;
    if (sec_end) *sec_end = -1;
    for (int i = 0; i < nlines; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s == '#' || *s == 0) continue;
        char *c = comment_at(s); if (c) *c = 0;
        s = trim(s);
        if (*s == '[') {
            char *e = strchr(s, ']'); if (e) *e = 0;
            in = _stricmp(trim(s + 1), section) == 0;
            if (in && sec_end) *sec_end = i + 1;
            continue;
        }
        if (!in) continue;
        if (sec_end) *sec_end = i + 1;
        char *eq = strchr(s, '='); if (!eq) continue;
        *eq = 0;
        if (_stricmp(trim(s), key) == 0 && found < 0) {
            found = i;
            if (val) strcpy_s(val, LINE_LEN, trim(eq + 1));
        }
    }
    return found;
}

const char *cfg_get(const char *section, const char *key, const char *def) {
    char *out = ret_buf[ret_slot++ & 7];
    AcquireSRWLockShared(&lock);
    int ok = find(section, key, out, NULL) >= 0 && out[0];
    ReleaseSRWLockShared(&lock);
    return ok ? out : def;
}

// Several keys of one section read together: a change written with cfg_set_many is seen whole or not at all.
// vals[i] is "" for a key that is not there (unset, as cfg_get).
void cfg_get_many(const char *section, const char *const *keys, int n, char (*vals)[LINE_LEN]) {
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < n; i++) if (find(section, keys[i], vals[i], NULL) < 0) vals[i][0] = 0;
    ReleaseSRWLockShared(&lock);
}

float cfg_getf(const char *section, const char *key, float def) {
    const char *v = cfg_get(section, key, NULL);
    return v ? (float)atof(v) : def;
}

int cfg_geti(const char *section, const char *key, int def) {
    const char *v = cfg_get(section, key, NULL);
    return v ? atoi(v) : def;
}

// "#RRGGBB, #RRGGBB ..." -> colours 0..1. Returns the count.
int cfg_palette(const char *section, rgbf *out, int max) {
    const char *v = cfg_get(section, "palette", NULL);
    if (!v) return 0;
    int n = 0;
    for (const char *p = v; *p && n < max; p++) {
        if (*p != '#') continue;
        unsigned int c;
        if (sscanf_s(p + 1, "%6x", &c) == 1) {
            out[n].r = ((c >> 16) & 255) / 255.0f;
            out[n].g = ((c >> 8) & 255) / 255.0f;
            out[n].b = (c & 255) / 255.0f;
            n++;
        }
    }
    return n;
}

// a file written whole or not at all: the new one goes beside it as path.new and only then replaces it (the old one
// is never deleted first), so a full disk or a failing drive part way through leaves the old file as it was.
// file_fail: a test's fault point (1 the new file can't be made, 2 writing it fails, 3 replacing fails), 0 in the app
int file_fail;
FILE *file_begin(const wchar_t *path, wchar_t *tmp) {
    swprintf(tmp, MAX_PATH, L"%s.new", path);
    return file_fail == 1 ? NULL : _wfopen(tmp, L"wb");
}
int file_commit(FILE *f, const wchar_t *tmp, const wchar_t *path) {
    int ok = !ferror(f) && file_fail != 2 && !fflush(f) && !_commit(_fileno(f));   // on the disk before it replaces the old one
    if (fclose(f)) ok = 0;
    if (!ok || file_fail == 3 || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp); return 0; }
    return 1;
}

static int write_lines(const wchar_t *p) {
    wchar_t tmp[MAX_PATH];
    FILE *f = file_begin(p, tmp);
    if (!f) return 0;
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\r\n", lines[i]);
    return file_commit(f, tmp, p);
}

// 0: not saved
static int save(void) {
    if (load_bad || !write_lines(path_w)) return 0;
    last_write = file_time();
    return 1;
}

// the settings in memory, whole, to another file (a restore stages them there); 0: not written
int cfg_write_to(const wchar_t *p) {
    AcquireSRWLockShared(&lock);
    int ok = write_lines(p);
    ReleaseSRWLockShared(&lock);
    return ok;
}

static void drop(void) { InterlockedIncrement(&dropped); my_dropped++; }
// lines a new key of section needs (its own, and the section's header when there is none yet)
static int need(const char *section, const char *key) {
    int end;
    return find(section, key, NULL, &end) >= 0 ? 0 : end < 0 ? 2 : 1;
}
// a value that fits its line whole: one cut short would mean something else (a colour list, a curve, a name)
static int fits(const char *key, const char *value) { return strlen(key) + 1 + strlen(value) < LINE_LEN; }

// CFG_OK, or the change is refused and nothing of it is kept: CFG_FULL no line left for a new key, CFG_LONG too long
static int set_locked(const char *section, const char *key, const char *value) {
    if (!fits(key, value)) { drop(); return CFG_LONG; }
    // one line per key: a line break in a value (a name typed on a phone) would add lines of its own to the file
    char clean[LINE_LEN]; strcpy_s(clean, LINE_LEN, value);
    for (char *p = clean; *p; p++) if ((unsigned char)*p < 0x20 || (*p == ';' && p > clean && p[-1] == ' ')) *p = ' ';   // " ;" would start a comment
    value = clean;
    int end;
    int i = find(section, key, NULL, &end);
    char nl[LINE_LEN];
    if (i >= 0) {
        // keep indentation-free "key=value" and any trailing comment that still fits
        const char *comment = comment_at(lines[i]);
        if (!comment || snprintf(nl, LINE_LEN, "%s=%s   %s", key, value, comment) >= LINE_LEN) snprintf(nl, LINE_LEN, "%s=%s", key, value);
        if (strcmp(lines[i], nl)) { strcpy_s(lines[i], LINE_LEN, nl); dirty = 1; }
        return CFG_OK;
    }
    if (nlines + (end < 0 ? 2 : 1) > MAX_LINES) { drop(); return CFG_FULL; }
    if (end < 0) {
        snprintf(lines[nlines++], LINE_LEN, "[%s]", section);
        end = nlines;
    }
    memmove(lines[end + 1], lines[end], (size_t)(nlines - end) * LINE_LEN);
    snprintf(lines[end], LINE_LEN, "%s=%s", key, value);
    nlines++;
    dirty = 1;
    return CFG_OK;
}

// Changes the value in memory only; cfg_save_if_dirty() writes the file. CFG_OK, or CFG_FULL / CFG_LONG: not changed
int cfg_set(const char *section, const char *key, const char *value) {
    AcquireSRWLockExclusive(&lock);
    int r = set_locked(section, key, value);
    ReleaseSRWLockExclusive(&lock);
    return r;
}

int cfg_dropped(void) { return (int)dropped; }

// Many cfg_set as one change (a profile, a preset, a restore): cfg_group_end() 1 when every change this thread made
// since cfg_group_begin() went in, else the settings in memory go back to what they were at cfg_group_begin().
// shortcut: one group at a time; a change another thread makes during a group that fails is taken back with it
static char snap[MAX_LINES][LINE_LEN];
static int snap_n, snap_dirty, snap_dropped;
void cfg_group_begin(void) {
    AcquireSRWLockShared(&lock);
    memcpy(snap, lines, (size_t)nlines * LINE_LEN); snap_n = nlines; snap_dirty = dirty; snap_dropped = my_dropped;
    ReleaseSRWLockShared(&lock);
}
int cfg_group_end(void) {
    AcquireSRWLockExclusive(&lock);
    int ok = my_dropped == snap_dropped;
    if (!ok) { memcpy(lines, snap, (size_t)snap_n * LINE_LEN); nlines = snap_n; dirty = snap_dirty; }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

// [section] key as a comma list with item first, the same item (any case) not twice, at most max items (and what fits
// one line); memory only
int cfg_list_front(const char *section, const char *key, const char *item, int max) {
    char out[LINE_LEN]; int lim = LINE_LEN - (int)strlen(key) - 1, n = snprintf(out, sizeof(out), "%s", item), c = 1;
    size_t w0 = strlen(item);
    if (!w0 || strchr(item, ',')) return CFG_OK;   // nothing to put in front
    if (n >= lim) { drop(); return CFG_LONG; }
    char cur[LINE_LEN];
    AcquireSRWLockExclusive(&lock);
    const char *p = find(section, key, cur, NULL) >= 0 ? cur : "";
    for (; *p && c < max; ) {
        const char *e = strchr(p, ','); size_t w = e ? (size_t)(e - p) : strlen(p);
        if (w && !(w == w0 && !_strnicmp(p, item, w)) && n + (int)w + 2 < lim) { n += snprintf(out + n, sizeof(out) - n, ",%.*s", (int)w, p); c++; }
        p += w + (e ? 1 : 0);
    }
    int r = set_locked(section, key, out);
    ReleaseSRWLockExclusive(&lock);
    return r;
}

// kv: key, value, key, value...; all of them land under one lock, so cfg_get_many never sees part of them; all or
// none: CFG_OK, or CFG_FULL / CFG_LONG and nothing changed
int cfg_set_many(const char *section, const char *const *kv, int n) {
    AcquireSRWLockExclusive(&lock);
    int more = 0, r = CFG_OK, head = 0;
    for (int i = 0; i + 1 < n * 2; i += 2) {
        if (!fits(kv[i], kv[i + 1])) r = CFG_LONG;
        int k = need(section, kv[i]), seen = 0;
        for (int j = 0; j < i; j += 2) if (!_stricmp(kv[j], kv[i])) seen = 1;   // the same key twice takes one line
        if (k == 2) head = 1;
        if (k && !seen) more++;
    }
    if (r == CFG_OK && nlines + more + head > MAX_LINES) r = CFG_FULL;
    if (r == CFG_OK) for (int i = 0; i + 1 < n * 2; i += 2) set_locked(section, kv[i], kv[i + 1]);
    else drop();
    ReleaseSRWLockExclusive(&lock);
    return r;
}

// Deletes a whole [section] (header, keys and its comments up to the next section).
void cfg_remove_section(const char *section) {
    AcquireSRWLockExclusive(&lock);
    int start = -1, end = nlines;
    for (int i = 0; i < nlines; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s != '[') continue;
        char *e = strchr(s, ']'); if (e) *e = 0;
        if (start < 0 && _stricmp(trim(s + 1), section) == 0) start = i;
        else if (start >= 0) { end = i; break; }
    }
    if (start >= 0) {
        while (end > start + 1 && !trim(lines[end - 1])[0]) end--;   // keep the blank line before the next section
        memmove(lines[start], lines[end], (size_t)(nlines - end) * LINE_LEN);
        nlines -= end - start;
        dirty = 1;
    }
    ReleaseSRWLockExclusive(&lock);
}

// Keys and values of one section, in file order; copies, so the caller may change the config meanwhile.
int cfg_items(const char *section, cfg_item *out, int max) {
    int n = 0, in = 0;
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < nlines && n < max; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s == '#' || *s == 0) continue;
        char *c = comment_at(s); if (c) *c = 0;
        s = trim(s);
        if (*s == '[') { char *e = strchr(s, ']'); if (e) *e = 0; in = _stricmp(trim(s + 1), section) == 0; continue; }
        char *eq = strchr(s, '=');
        if (!in || !eq) continue;
        *eq = 0;
        snprintf(out[n].key, sizeof(out[n].key), "%s", trim(s));
        snprintf(out[n].val, sizeof(out[n].val), "%s", trim(eq + 1));
        n++;
    }
    ReleaseSRWLockShared(&lock);
    return n;
}

// Names of the sections that start with prefix (case-insensitive), in file order.
int cfg_sections(const char *prefix, char (*out)[64], int max) {
    int n = 0, pl = (int)strlen(prefix);
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < nlines && n < max; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s != '[') continue;
        char *e = strchr(s, ']'); if (e) *e = 0;
        s = trim(s + 1);
        if (_strnicmp(s, prefix, pl)) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (!_stricmp(out[k], s)) dup = 1;
        if (!dup) snprintf(out[n++], 64, "%s", s);
    }
    ReleaseSRWLockShared(&lock);
    return n;
}

int cfg_save_if_dirty(void) {
    AcquireSRWLockExclusive(&lock);
    int ok = 1;
    if (dirty && (ok = save())) dirty = 0;   // not saved: tried again on the next change
    save_bad = dirty && !ok;
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

int cfg_unsaved(void) { return save_bad && dirty; }

void cfg_set_and_save(const char *section, const char *key, const char *value) {
    cfg_set(section, key, value);
    cfg_save_if_dirty();
}

// Appends a JSON string literal (with escaping) to out.
static int json_put_str(char *out, int cap, int n, const char *s) {
    if (n < cap - 1) out[n++] = '"';
    for (; *s && n < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n = jcat(out, cap, n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    if (n < cap - 1) out[n++] = '"';
    out[n] = 0;
    return n;
}

// Whole config as {"section":{"key":"value",...},...}; comments are skipped.
int cfg_json(char *out, int cap) {
    int n = 0, open = 0, first_key = 1;
    if (cap < 8) { if (cap > 0) *out = 0; return 0; }
    out[n++] = '{';
    AcquireSRWLockShared(&lock);
    // 1600: one line at its longest (every byte a \u00XX) with its punctuation, so the single bytes below always fit
    for (int i = 0; i < nlines && n < cap - 1600; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s == '#' || *s == 0) continue;
        char *c = comment_at(s); if (c) *c = 0;
        s = trim(s);
        if (*s == '[') {
            char *e = strchr(s, ']'); if (e) *e = 0;
            if (open) out[n++] = '}';
            if (open) out[n++] = ',';
            n = json_put_str(out, cap, n, trim(s + 1));
            out[n++] = ':'; out[n++] = '{';
            open = 1; first_key = 1;
            continue;
        }
        char *eq = strchr(s, '=');
        if (!open || !eq) continue;
        *eq = 0;
        if (!first_key) out[n++] = ',';
        first_key = 0;
        char *k = trim(s);
        n = json_put_str(out, cap, n, k);
        out[n++] = ':';
        // secrets ([dev.N] / [pro] key=, [mqtt] password=) stay in the file: the page (and a paired phone) sees only that one is set
        n = json_put_str(out, cap, n, (!_stricmp(k, "key") || !_stricmp(k, "password")) && trim(eq + 1)[0] ? "(set)" : trim(eq + 1));
    }
    ReleaseSRWLockShared(&lock);
    if (open) out[n++] = '}';
    out[n++] = '}';
    out[n] = 0;
    return n;
}

#ifdef CFG_TEST   // cl /DCFG_TEST src\config.c: a source and its curve written together are never read half changed
#include <assert.h>
void logf_(const char *fmt, ...) { (void)fmt; }
static volatile LONG stop;
static DWORD WINAPI writer(void *p) {
    (void)p;
    static const char *const a[] = { "fan1_curve", "40:30,90:100", "fan1_src", "cpu" }, *const b[] = { "fan1_curve", "25:30,40:100", "fan1_src", "t1" };
    for (int i = 0; !stop; i++) cfg_set_many("fans", i & 1 ? b : a, 2);
    return 0;
}
int main(void) {
    cfg_set("fans", "fan1", "curve"); cfg_set("other", "x", "1");
    const char *const k[] = { "fan1_src", "fan1_curve", "fan1_none" }; char v[3][LINE_LEN];
    cfg_get_many("fans", k, 3, v); assert(!v[0][0] && !v[2][0]);
    HANDLE w = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    int mixed = 0, seen[2] = { 0 };
    for (int i = 0; i < 200000; i++) {
        cfg_get_many("fans", k, 3, v);
        if (!v[0][0]) continue;
        int cpu = !strcmp(v[0], "cpu");
        mixed += cpu ? strcmp(v[1], "40:30,90:100") != 0 : strcmp(v[1], "25:30,40:100") != 0;
        seen[cpu]++;
    }
    stop = 1; WaitForSingleObject(w, INFINITE);
    assert(!mixed && seen[0] && seen[1] && !v[2][0]);

    // limits: a refused change is said and leaves the settings as they were, never half of it
    nlines = 0; dirty = 0;
    char key[32], big[300];
    assert(cfg_set("a", "k", "1") == CFG_OK && dirty);
    dirty = 0; assert(cfg_set("a", "k", "1") == CFG_OK && !dirty);   // the same value: nothing to write
    memset(big, 'x', 260); big[260] = 0;
    int d = cfg_dropped();
    assert(cfg_set("a", "k", big) == CFG_LONG && !strcmp(cfg_get("a", "k", ""), "1") && cfg_dropped() == d + 1);
    strcpy_s(lines[0 + 1], LINE_LEN, "k=1   ; a note");   // a comment that no longer fits goes, the value stays whole
    memset(big, 'y', 240); big[240] = 0;
    assert(cfg_set("a", "k", big) == CFG_OK && !strcmp(cfg_get("a", "k", ""), big));
    while (nlines < MAX_LINES - 2) { snprintf(lines[nlines], LINE_LEN, "k%d=v", nlines); nlines++; }
    sprintf_s(key, sizeof key, "k%d", MAX_LINES - 3); assert(!strcmp(cfg_get("a", key, ""), "v"));
    const char *const three[] = { "n1", "1", "n2", "2", "n3", "3" };
    int before = nlines; d = cfg_dropped();
    assert(cfg_set_many("a", three, 3) == CFG_FULL && nlines == before && !cfg_get("a", "n1", NULL) && cfg_dropped() == d + 1);
    assert(cfg_set_many("a", three, 2) == CFG_OK && nlines == MAX_LINES);
    assert(cfg_set("a", "n3", "3") == CFG_FULL && cfg_set("b", "x", "1") == CFG_FULL && cfg_set("a", "n1", "9") == CFG_OK);
    nlines -= 3;   // room for two keys of a new section (its header and one key) and one more
    cfg_group_begin();
    cfg_set("a", "g1", "1"); cfg_set("c", "g2", "2"); cfg_set("c", "g3", "3"); cfg_set("c", "g4", "4");
    assert(!cfg_group_end() && nlines == MAX_LINES - 3 && !cfg_get("a", "g1", NULL) && !cfg_get("c", "g2", NULL));
    cfg_group_begin(); cfg_set("c", "g2", "2"); cfg_set("c", "g3", "3");
    assert(cfg_group_end() && nlines == MAX_LINES && !strcmp(cfg_get("c", "g3", ""), "3"));

    // the save on exit (main.c): nothing changed writes nothing; a failed write keeps the change and says so; again works
    wchar_t dir[MAX_PATH], f[MAX_PATH];
    GetTempPathW(MAX_PATH, dir); swprintf(dir + wcslen(dir), 64, L"haku-cfg-test-%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL); swprintf(f, MAX_PATH, L"%s\\settings.ini", dir);
    nlines = 0; dirty = save_bad = 0; wcscpy_s(path_w, MAX_PATH, f);
    assert(cfg_save_if_dirty() == 1 && GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES);   // no change, no file
    cfg_set("general", "brightness", "0.4");
    assert(RemoveDirectoryW(dir));   // the folder gone: the write fails
    assert(cfg_save_if_dirty() == 0 && cfg_unsaved() && dirty && !strcmp(cfg_get("general", "brightness", ""), "0.4"));
    CreateDirectoryW(dir, NULL);   // the retry
    assert(cfg_save_if_dirty() == 1 && !cfg_unsaved() && !dirty && GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES);
    FILETIME t1 = file_time(), t2; Sleep(20);
    assert(cfg_save_if_dirty() == 1); t2 = file_time(); assert(!CompareFileTime(&t1, &t2));   // saved, then nothing new
    cfg_load(NULL); assert(!strcmp(cfg_get("general", "brightness", ""), "0.4"));   // what is on disk
    // a ';' inside a value is the value's; after a blank it starts a comment, so a value never keeps one there
    cfg_set("wifi", "pass", "a;b"); assert(!strcmp(cfg_get("wifi", "pass", ""), "a;b"));
    cfg_set("wifi", "pass", "c;d"); assert(!strcmp(cfg_get("wifi", "pass", ""), "c;d"));   // not kept as a comment of the old line
    cfg_set("wifi", "name", "a ;b"); assert(!strcmp(cfg_get("wifi", "name", ""), "a  b"));
    strcpy_s(lines[nlines++], LINE_LEN, "note=1   ; said"); assert(!strcmp(cfg_get("wifi", "note", ""), "1"));
    cfg_item it[8]; assert(cfg_items("wifi", it, 8) == 3 && !strcmp(it[0].val, "c;d"));
    // a file that does not fit is not written back cut: the change stays in memory and is said
    assert(cfg_save_if_dirty() == 1);
    FILE *lf = _wfopen(f, L"ab"); for (int i = 0; i < 300; i++) fputc('z', lf); fputs("\r\n", lf); fclose(lf);
    cfg_load(NULL); assert(load_bad);
    cfg_set("general", "brightness", "0.9"); assert(cfg_save_if_dirty() == 0 && cfg_unsaved());
    cfg_load(NULL); assert(strcmp(cfg_get("general", "brightness", ""), "0.9"));   // the file was not touched
    DeleteFileW(f); RemoveDirectoryW(dir);
    puts("config: ok");
    return 0;
}
#endif
