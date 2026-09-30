// SPDX-License-Identifier: GPL-3.0-only
// Tiny INI reader/writer. Keeps the file as lines so edits preserve user comments.
// Comments start with ';' (anywhere) or '#' at the start of a line ('#' inside values is a colour).
#include "common.h"
#include <stdlib.h>
#include <ctype.h>

#define MAX_LINES 4096   // presets with device colours take a few dozen lines each
#define LINE_LEN  256

static char     lines[MAX_LINES][LINE_LEN];
static int      nlines;
static wchar_t  path_w[MAX_PATH];
static FILETIME last_write;
static __declspec(thread) char ret_buf[8][LINE_LEN];   // per-thread return slots
static __declspec(thread) int ret_slot;
static SRWLOCK   lock = SRWLOCK_INIT;

static FILETIME file_time(void) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME z = { 0 };
    return GetFileAttributesExW(path_w, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

const wchar_t *cfg_path(void) { return path_w; }

void cfg_load(const wchar_t *p) {
    AcquireSRWLockExclusive(&lock);
    if (p) wcsncpy_s(path_w, MAX_PATH, p, _TRUNCATE);
    nlines = 0;
    FILE *f = _wfopen(path_w, L"rb");
    if (!f) { ReleaseSRWLockExclusive(&lock); return; }
    char buf[LINE_LEN];
    while (nlines < MAX_LINES && fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        // strip UTF-8 BOM on the first line
        char *s = buf;
        if (nlines == 0 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        strcpy_s(lines[nlines++], LINE_LEN, s);
    }
    fclose(f);
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

// Finds key in section. Returns line index or -1; *sec_end gets the index after the section's last line.
static int find(const char *section, const char *key, char *val, int *sec_end) {
    int in = 0, found = -1;
    if (sec_end) *sec_end = -1;
    for (int i = 0; i < nlines; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s == '#' || *s == 0) continue;
        char *c = strchr(s, ';'); if (c) *c = 0;
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

static void save(void) {
    FILE *f = _wfopen(path_w, L"wb");
    if (!f) return;
    for (int i = 0; i < nlines; i++) fprintf(f, "%s\r\n", lines[i]);
    fclose(f);
    last_write = file_time();
}

static int dirty;

// Changes the value in memory only; cfg_save_if_dirty() writes the file.
void cfg_set(const char *section, const char *key, const char *value) {
    AcquireSRWLockExclusive(&lock);
    int end;
    int i = find(section, key, NULL, &end);
    if (i >= 0) {
        // keep indentation-free "key=value" and any trailing comment
        const char *comment = strchr(lines[i], ';');
        char nl[LINE_LEN];
        if (comment) snprintf(nl, LINE_LEN, "%s=%s   %s", key, value, comment);
        else snprintf(nl, LINE_LEN, "%s=%s", key, value);
        strcpy_s(lines[i], LINE_LEN, nl);
    } else if (nlines + 2 < MAX_LINES) {
        if (end < 0) {
            snprintf(lines[nlines++], LINE_LEN, "[%s]", section);
            end = nlines;
        }
        memmove(lines[end + 1], lines[end], (size_t)(nlines - end) * LINE_LEN);
        snprintf(lines[end], LINE_LEN, "%s=%s", key, value);
        nlines++;
    }
    dirty = 1;
    ReleaseSRWLockExclusive(&lock);
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
        char *c = strchr(s, ';'); if (c) *c = 0;
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

void cfg_save_if_dirty(void) {
    AcquireSRWLockExclusive(&lock);
    if (dirty) { save(); dirty = 0; }
    ReleaseSRWLockExclusive(&lock);
}

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
        else if (c < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    if (n < cap - 1) out[n++] = '"';
    out[n] = 0;
    return n;
}

// Whole config as {"section":{"key":"value",...},...}; comments are skipped.
int cfg_json(char *out, int cap) {
    int n = 0, open = 0, first_key = 1;
    out[n++] = '{';
    AcquireSRWLockShared(&lock);
    for (int i = 0; i < nlines && n < cap - 600; i++) {
        char tmp[LINE_LEN]; strcpy_s(tmp, LINE_LEN, lines[i]);
        char *s = trim(tmp);
        if (*s == '#' || *s == 0) continue;
        char *c = strchr(s, ';'); if (c) *c = 0;
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
        // pairing secrets ([dev.N] key=) stay in the file
        n = json_put_str(out, cap, n, !_stricmp(k, "key") && trim(eq + 1)[0] ? "(set)" : trim(eq + 1));
    }
    ReleaseSRWLockShared(&lock);
    if (open) out[n++] = '}';
    out[n++] = '}';
    out[n] = 0;
    return n;
}
