// SPDX-License-Identifier: GPL-3.0-only
// Checks for the settings backup (src/backup.c over src/config.c) in a folder of its own, no app, no devices:
//   build.cmd's environment, then: cl /nologo /utf-8 /DHAKU_DEV /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
//   tools\sim\backup_test.c src\backup.c src\config.c comdlg32.lib /Fe:%TEMP%\backup_test.exe && %TEMP%\backup_test.exe
#include "../../src/common.h"
#include <stdarg.h>
#include <stdlib.h>

static wchar_t dir[MAX_PATH];
static int flush_ok = 1;
void app_data_path(const wchar_t *name, wchar_t *out) { swprintf(out, MAX_PATH, L"%s%s", dir, name); }
int app_profiles_flush(void) { return flush_ok; }
void app_reload_all(void) { }
void logf_(const char *fmt, ...) { (void)fmt; }
int backup_json(char *out, int cap);
extern int backup_stop_at;

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static void put(const wchar_t *name, const char *text) {
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"wb"); fputs(text, f); fclose(f);
}
static char *get(const wchar_t *name) {
    static char b[4][1 << 18]; static int k; char *o = b[k++ & 3];
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"rb"); if (!f) { o[0] = 0; return o; }
    size_t n = fread(o, 1, sizeof(b[0]) - 1, f); o[n] = 0; fclose(f); return o;
}
// open as another program would that does not let it be moved or deleted (no FILE_SHARE_DELETE)
static HANDLE hold(const wchar_t *name) {
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    return CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}
static int there(const wchar_t *name) { wchar_t p[MAX_PATH]; app_data_path(name, p); return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static const char *result(void) { static char b[128]; backup_json(b, sizeof(b)); return b; }
static void use_file(const wchar_t *name) { wchar_t p[MAX_PATH]; app_data_path(name, p); SetEnvironmentVariableW(L"HAKU_BACKUP_FILE", p); }
static int name_is(const char *v) { return !strcmp(cfg_get("general", "name", ""), v); }
static int nothing_beside(void) {
    return !there(L"profiles\\1.ini.old") && !there(L"profiles\\2.ini.old") && !there(L"profiles\\3.ini.old") &&
           !there(L"profiles\\1.ini.new") && !there(L"profiles\\2.ini.new") && !there(L"settings.ini.restore") && !there(L"restore.journal");
}

// a v2 backup of body (lines with \r\n) with its end line, as backup_save writes it
static void put_v2(const wchar_t *name, const char *body) {
    static char t[1 << 18]; unsigned h = 2166136261u; int n = 0;
    for (const char *s = body; *s; ) {
        const char *e = strstr(s, "\r\n"); size_t l = e ? (size_t)(e - s) : strlen(s);
        for (size_t i = 0; i < l; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
        h ^= '\n'; h *= 16777619u; n++;
        s += l + (e ? 2 : 0);
    }
    snprintf(t, sizeof(t), "haku control backup 2\r\n%s#end %d %08x\r\n", body, n, h);
    put(name, t);
}
// the first len bytes of a file into another
static void put_cut(const wchar_t *name, const char *text, size_t len) {
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"wb"); fwrite(text, 1, len, f); fclose(f);
}

static const char *SETTINGS =
    "[general]\r\nprofile=2\r\nname=desk\r\n[profile.2]\r\nname=Night\r\npin=1\r\nat=22:00\r\n"
    "[dev.1]\r\nkind=wled\r\nhost=10.0.0.5\r\nkey=SECRET-KEY\r\n[pro]\r\nlicence=SECRET-LIC\r\n";

int main(void) {
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-backup-test\\");
    CreateDirectoryW(dir, NULL);
    wchar_t pd[MAX_PATH]; app_data_path(L"profiles", pd); CreateDirectoryW(pd, NULL);
    const wchar_t *stale[] = { L"profiles\\1.ini", L"profiles\\3.ini", L"profiles\\1.ini.old", L"profiles\\2.ini.old", L"profiles\\3.ini.old",
                               L"profiles\\2.ini.new", L"settings.ini.restore", L"restore.journal", L"o.hakubackup" };
    for (int i = 0; i < (int)(sizeof(stale) / sizeof(*stale)); i++) { wchar_t p[MAX_PATH]; app_data_path(stale[i], p); DeleteFileW(p); }
    put(L"profiles\\2.ini", "[flow]\r\nspeed=4\r\n");
    put(L"settings.ini", SETTINGS);
    wchar_t ini[MAX_PATH]; app_data_path(L"settings.ini", ini); cfg_load(ini);

    // export: the pinned profile goes along, the secrets do not, and the end line closes it
    use_file(L"a.hakubackup"); backup_save(NULL);
    char *bk = get(L"a.hakubackup");
    CHECK(strstr(result(), "saved"), "export saved");
    CHECK(!strncmp(bk, "haku control backup 2\r\n", 23), "export is version 2");
    CHECK(strstr(bk, "\r\n#end ") != NULL, "export ends with its end line");
    CHECK(strstr(bk, "[profile.2]\r\nname=Night\r\npin=1"), "a pinned profile keeps pin=1 in the backup");
    CHECK(strstr(bk, "#file profiles/2.ini\r\n[flow]\r\nspeed=4"), "the named profile's file is in it");
    CHECK(!strstr(bk, "SECRET"), "no device key or licence in the backup");
    static char keep[1 << 18]; strcpy_s(keep, sizeof(keep), bk);   // this backup, for the checks below
    put(L"k.hakubackup", keep);

    // export over a backup already there: a failure at any step leaves it as it was and never says saved
    put(L"o.hakubackup", "the old backup\r\n");
    use_file(L"o.hakubackup");
    for (file_fail = 1; file_fail <= 3; file_fail++) {
        char what[96];
        backup_save(NULL);
        snprintf(what, sizeof(what), "export fault %d: reported, not saved", file_fail); CHECK(strstr(result(), "bad_write"), what);
        snprintf(what, sizeof(what), "export fault %d: the old backup is kept", file_fail); CHECK(!strcmp(get(L"o.hakubackup"), "the old backup\r\n"), what);
        snprintf(what, sizeof(what), "export fault %d: no temp file left", file_fail); CHECK(!there(L"o.hakubackup.new"), what);
    }
    file_fail = 0;
    flush_ok = 0; backup_save(NULL); flush_ok = 1;
    CHECK(strstr(result(), "bad_prof"), "the profile in use not written: export refused");
    CHECK(!strcmp(get(L"o.hakubackup"), "the old backup\r\n"), "the profile in use not written: old backup kept");
    put(L"profiles\\2.ini", "; haku control profile 2 v2: its setup\r\n[flow]\r\nspeed=4\r\n");   // v2, cut short on disk
    backup_save(NULL);
    CHECK(strstr(result(), "bad_prof"), "a profile cut short on disk: export refused");
    CHECK(!strcmp(get(L"o.hakubackup"), "the old backup\r\n"), "a profile cut short on disk: old backup kept");
    put(L"profiles\\2.ini", "[flow]\r\nspeed=4\r\n");
    backup_save(NULL);
    CHECK(strstr(result(), "saved") && !strcmp(get(L"o.hakubackup"), keep), "export over an old backup replaces it whole");

    // restore on a PC where the profile is not pinned: it comes back pinned, this PC's key stays
    use_file(L"a.hakubackup");
    cfg_set("profile.2", "pin", "0"); cfg_set("general", "name", "other"); cfg_save_if_dirty();
    backup_load(NULL);
    CHECK(strstr(result(), "restored"), "restore says restored");
    CHECK(!strcmp(cfg_get("profile.2", "pin", ""), "1"), "the pin is back after restore");
    CHECK(!strcmp(cfg_get("dev.1", "key", ""), "SECRET-KEY"), "this PC's device key is kept");
    CHECK(!strcmp(cfg_get("pro", "licence", ""), "SECRET-LIC"), "this PC's licence is kept");
    // a backup made by hand elsewhere cannot choose a program this PC starts: the line is left out, this PC's stays
    put(L"e.hakubackup", "haku control backup 1\r\n#file settings.ini\r\n[general]\r\nprofile=2\r\nname=desk\r\n[profile.2]\r\nname=Night\r\npin=1\r\n"
                         "[openrgb]\r\npath=C:\\evil.exe\r\nport=6743\r\n[mood]\r\nOllama_Path=C:\\evil2.exe\r\n[General]\r\nupdate_repo=x/y\r\n");
    use_file(L"e.hakubackup");
    cfg_set("openrgb", "path", "C:\\mine.exe"); cfg_save_if_dirty();
    backup_load(NULL);
    CHECK(strstr(result(), "restored") && !strcmp(cfg_get("openrgb", "port", ""), "6743"), "the hand-made backup is restored");
    CHECK(!strcmp(cfg_get("openrgb", "path", ""), "C:\\mine.exe"), "a program path in a backup is not taken, this PC's stays");
    CHECK(!cfg_get("mood", "ollama_path", NULL) && !cfg_get("general", "update_repo", NULL), "nor the AI program or the update source, in any letter case");
    use_file(L"a.hakubackup"); backup_load(NULL);
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=4") != NULL, "the profile file is restored");
    CHECK(strstr(get(L"settings.ini"), "pin=1") != NULL && nothing_beside(), "restore: on disk, nothing left beside");

    // a profile that cannot be written: nothing changes, and it says so
    cfg_set("general", "name", "mine"); cfg_save_if_dirty();
    put(L"profiles\\2.ini", "[flow]\r\nspeed=9\r\n");
    wchar_t blk[MAX_PATH]; app_data_path(L"profiles\\2.ini.new", blk); CreateDirectoryW(blk, NULL);   // in the way
    backup_load(NULL);
    CHECK(strstr(result(), "bad_write"), "a failed profile write is reported");
    CHECK(name_is("mine"), "settings in memory untouched");
    CHECK(strstr(get(L"settings.ini"), "name=mine") != NULL, "settings on disk untouched");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "the old profile file untouched");
    RemoveDirectoryW(blk);

    // the settings cannot be put in (another program holds settings.ini): the profiles go back, nothing is left over
    HANDLE held = hold(L"settings.ini");
    backup_load(NULL);
    CloseHandle(held);
    CHECK(strstr(result(), "bad_write"), "a failed settings write is reported");
    CHECK(name_is("mine") && strstr(get(L"settings.ini"), "name=mine") != NULL, "settings back to the old ones");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "old profile kept when the settings fail");
    CHECK(nothing_beside(), "settings held: no leftover new profile, staged settings or journal");

    // the profile in use is held open by another program (no delete sharing): the swap fails, all stays as it was
    held = hold(L"profiles\\2.ini");
    backup_load(NULL);
    CloseHandle(held);
    CHECK(strstr(result(), "bad_write"), "a held profile is reported");
    CHECK(name_is("mine"), "held profile: settings in memory untouched");
    CHECK(strstr(get(L"settings.ini"), "name=mine") != NULL, "held profile: settings on disk untouched");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "held profile: its file untouched");
    CHECK(nothing_beside(), "held profile: nothing left beside it");

    // several profiles (a version 1 backup, with files for profiles it has no name for): the first is swapped in,
    // the second is held; the first goes back to the old one
    put(L"two.hakubackup", "haku control backup 1\r\n#file settings.ini\r\n[general]\r\nname=desk\r\n"
        "#file profiles/1.ini\r\n[flow]\r\nspeed=1\r\n#file profiles/2.ini\r\n[flow]\r\nspeed=2\r\n");
    put(L"profiles\\1.ini", "[flow]\r\nspeed=7\r\n");
    use_file(L"two.hakubackup");
    held = hold(L"profiles\\2.ini");
    backup_load(NULL);
    CloseHandle(held);
    CHECK(strstr(result(), "bad_write"), "second profile held: reported");
    CHECK(strstr(get(L"profiles\\1.ini"), "speed=7") != NULL, "second profile held: the first is back to the old one");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "second profile held: it is untouched");
    CHECK(name_is("mine"), "second profile held: settings untouched");
    CHECK(nothing_beside(), "second profile held: nothing left beside the first");

    // a profile the backup has not, held open: it cannot be removed, so nothing changes
    put(L"profiles\\3.ini", "[flow]\r\nspeed=3\r\n");
    held = hold(L"profiles\\3.ini");
    backup_load(NULL);
    CloseHandle(held);
    CHECK(strstr(result(), "bad_write"), "a held extra profile is reported");
    CHECK(strstr(get(L"profiles\\1.ini"), "speed=7") != NULL && strstr(get(L"profiles\\2.ini"), "speed=9") != NULL,
          "held extra profile: the others back to the old ones");
    CHECK(strstr(get(L"profiles\\3.ini"), "speed=3") != NULL, "held extra profile: it is still there");

    // nothing held: all of it goes in, the extra profile is gone and no old file is left (a v1 backup is still read)
    backup_load(NULL);
    CHECK(strstr(result(), "restored"), "version 1 backup: restored");
    CHECK(name_is("desk"), "version 1 backup: settings in");
    CHECK(strstr(get(L"profiles\\1.ini"), "speed=1") != NULL && strstr(get(L"profiles\\2.ini"), "speed=2") != NULL, "version 1 backup: profiles in");
    CHECK(!there(L"profiles\\3.ini") && nothing_beside(), "version 1 backup: extra and old files gone");
    cfg_set("general", "name", "mine"); cfg_save_if_dirty();
    put(L"profiles\\2.ini", "[flow]\r\nspeed=9\r\n");

    // a backup cut short or changed anywhere is refused whole, and nothing changes
    static char before[1 << 18]; strcpy_s(before, sizeof(before), get(L"settings.ini"));
    const char *fp = strstr(keep, "#file profiles/2.ini"), *end = strstr(keep, "#end ");
    struct { const char *what; size_t len; } cut[] = {
        { "cut inside the settings", (size_t)(strstr(keep, "[dev.1]") - keep) },
        { "cut after the settings, before the profiles", (size_t)(fp - keep) },
        { "cut inside a profile", (size_t)(strstr(fp, "speed") - keep) },
        { "cut before the end line", (size_t)(end - keep) },
        { "cut inside the end line", (size_t)(end - keep) + 8 },
    };
    for (int i = 0; i < (int)(sizeof(cut) / sizeof(*cut)); i++) {
        put_cut(L"cut.hakubackup", keep, cut[i].len);
        use_file(L"cut.hakubackup"); backup_load(NULL);
        CHECK(strstr(result(), "bad_file") && name_is("mine") && !strcmp(get(L"settings.ini"), before) &&
              strstr(get(L"profiles\\2.ini"), "speed=9") && nothing_beside(), cut[i].what);
    }
    static char t[1 << 18];
    strcpy_s(t, sizeof(t), keep); *strstr(t, "Night") = 'n'; put(L"cut.hakubackup", t);
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "one byte changed: refused");
    snprintf(t, sizeof(t), "%sname=extra\r\n", keep); put(L"cut.hakubackup", t);
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a line after the end: refused");
    put_v2(L"cut.hakubackup", "#file settings.ini\r\n[general]\r\nname=x\r\n#file profiles/4.ini\r\n[flow]\r\nspeed=4\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a profile file without its profile: refused");
    put_v2(L"cut.hakubackup", "#file settings.ini\r\n[general]\r\nname=x\r\n#file settings.ini\r\n[general]\r\nname=y\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a part twice: refused");
    put_v2(L"cut.hakubackup", "#file settings.ini\r\n[general]\r\nname=x\r\n#file notes.txt\r\nhello\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a part no backup has: refused");
    put_v2(L"cut.hakubackup", "#file settings.ini\r\n[general]\r\nname=x\r\n[profile.2]\r\nname=N\r\n"
           "#file profiles/2.ini\r\n; haku control profile 2 v2: its setup\r\n[flow]\r\nspeed=5\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a v2 profile without its end: refused");
    put(L"cut.hakubackup", "haku control backup 1\r\n#file settings.ini\r\n[general]\r\nname=x\r\nthis is not a setting\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "bad_file") && name_is("mine"), "a damaged version 1 backup: refused");
    put_v2(L"cut.hakubackup", "#file settings.ini\r\n[general]\r\nname=x\r\n[profile.5]\r\nname=Later\r\n");
    backup_load(NULL);
    CHECK(strstr(result(), "restored") && name_is("x"), "a named profile with no file yet: restored");
    CHECK(!there(L"profiles\\2.ini"), "a profile the backup has not: its file removed");
    cfg_set("general", "name", "mine"); cfg_save_if_dirty();
    put(L"profiles\\2.ini", "[flow]\r\nspeed=9\r\n");
    strcpy_s(before, sizeof(before), get(L"settings.ini"));

    // more settings than the settings file can hold: refused whole before any file is touched
    {
        static char big[1 << 18]; int n = snprintf(big, sizeof(big), "#file settings.ini\r\n[general]\r\nname=huge\r\n");
        for (int i = 0; i < 5000; i++) n += snprintf(big + n, sizeof(big) - n, "[s%d]\r\nk=%d\r\n", i, i);
        put_v2(L"cut.hakubackup", big);
        backup_load(NULL);
        CHECK(strstr(result(), "full"), "over the settings' capacity: reported as full");
        CHECK(name_is("mine") && !strcmp(get(L"settings.ini"), before) && strstr(get(L"profiles\\2.ini"), "speed=9") && nothing_beside(),
              "over the settings' capacity: nothing changed");
    }

    // the app killed part way through a restore: the next start takes it back (before the settings went in) or
    // finishes it (after); never half of each
    use_file(L"k.hakubackup");
    for (int s = 1; s <= 4; s++) {
        put(L"profiles\\3.ini", "[flow]\r\nspeed=3\r\n");   // one the backup has not
        backup_stop_at = s; backup_load(NULL); backup_stop_at = 0;
        cfg_load(ini); backup_recover();   // the next start
        char what[96];
        if (s < 4) {
            snprintf(what, sizeof(what), "killed at step %d: taken back, settings as before", s);
            CHECK(name_is("mine") && !strcmp(get(L"settings.ini"), before), what);
            snprintf(what, sizeof(what), "killed at step %d: profiles as before", s);
            CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") && strstr(get(L"profiles\\3.ini"), "speed=3"), what);
        } else {
            CHECK(name_is("desk") && !strcmp(cfg_get("profile.2", "name", ""), "Night"), "killed after the settings went in: finished, settings in");
            CHECK(strstr(get(L"profiles\\2.ini"), "speed=4") && !there(L"profiles\\3.ini"), "killed after the settings went in: profiles in");
        }
        snprintf(what, sizeof(what), "killed at step %d: nothing left beside", s);
        CHECK(nothing_beside(), what);
    }
    // a profile set aside by an older version's restore, with nothing in its place: put back at start
    {
        wchar_t p[MAX_PATH], po[MAX_PATH]; app_data_path(L"profiles\\2.ini", p); app_data_path(L"profiles\\2.ini.old", po);
        MoveFileExW(p, po, 0);
        backup_recover();
        CHECK(strstr(get(L"profiles\\2.ini"), "speed=4") && !there(L"profiles\\2.ini.old"), "an older version's cut-off swap: profile put back");
    }

    printf(fails ? "backup: %d FAILED\n" : "backup: all ok\n", fails);
    return fails != 0;
}
