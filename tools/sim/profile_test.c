// SPDX-License-Identifier: GPL-3.0-only
// Checks for profiles' files (src/profile.c over src/config.c) in a folder of its own, no app, no devices. Failures of
// the disk are made with config.c's fault points (file_fail), not by changing a folder's rights.
//   build.cmd's environment, then: cl /nologo /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
//   tools\sim\profile_test.c src\profile.c src\config.c /Fe:%TEMP%\profile_test.exe && %TEMP%\profile_test.exe
#include "../../src/common.h"
#include <stdarg.h>
#include <stdlib.h>

static wchar_t dir[MAX_PATH];
void app_data_path(const wchar_t *name, wchar_t *out) { swprintf(out, MAX_PATH, L"%s%s", dir, name); }
void logf_(const char *fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

// as main.c's profile_key, cut down: the effect, brightness, a device's on/off and [flow]
static int keep(const char *sec, const char *key) {
    if (!_stricmp(sec, "general")) return !_stricmp(key, "effect") || !_stricmp(key, "brightness");
    if (!_strnicmp(sec, "dev.", 4)) return !_stricmp(key, "enabled");
    if (!_strnicmp(sec, "group.", 6)) return 1;
    return !_stricmp(sec, "flow");
}

static void put(const wchar_t *name, const char *text) {
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"wb"); fputs(text, f); fclose(f);
}
static char *get(const wchar_t *name) {
    static char b[8192]; wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"rb"); if (!f) return "";
    size_t n = fread(b, 1, sizeof(b) - 1, f); b[n] = 0; fclose(f); return b;
}
static int there(const wchar_t *name) { wchar_t p[MAX_PATH]; app_data_path(name, p); return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int err(void) { char b[64]; prof_json(b, sizeof(b)); return atoi(strstr(b, "\"err\":") + 6); }
static void start(const char *ini) {
    put(L"settings.ini", ini);
    wchar_t p[MAX_PATH]; app_data_path(L"settings.ini", p); cfg_load(p);
}
static void reload(void) { cfg_load(NULL); }

static const char *BASE =
    "[general]\r\neffect=flow\r\nbrightness=80\r\nprofile=1\r\n[profile.1]\r\nname=Day\r\n[profile.2]\r\nname=Night\r\n"
    "[dev.1]\r\nkind=wled\r\nenabled=1\r\n[flow]\r\nspeed=5\r\n";
static const char *NIGHT = "; haku control profile 2 v2: its setup\r\n[general]\r\neffect=static\r\nbrightness=20\r\n[dev.1]\r\nenabled=0\r\n; end\r\n";

int main(void) {
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-profile-test\\");
    CreateDirectoryW(dir, NULL);
    wchar_t pd[MAX_PATH]; app_data_path(L"profiles", pd); CreateDirectoryW(pd, NULL);
    prof_keep = keep;
    for (int i = 1; i <= 5; i++) { wchar_t p[MAX_PATH], n[40]; swprintf(n, 40, L"profiles\\%d.ini", i); app_data_path(n, p); DeleteFileW(p); }

    // ---- writing: whole or not at all
    start(BASE); put(L"profiles\\1.ini", "; haku control profile 1 v2: old\r\n[general]\r\neffect=old\r\n; end\r\n");
    CHECK(prof_write(1) && strstr(get(L"profiles\\1.ini"), "effect=flow") && strstr(get(L"profiles\\1.ini"), "; end"), "a profile is written with its end mark");
    put(L"profiles\\1.ini", "; haku control profile 1 v2: old\r\n[general]\r\neffect=old\r\n; end\r\n");
    for (int k = 1; k <= 3; k++) {
        char what[96]; file_fail = k;
        snprintf(what, sizeof(what), "fault %d (%s): not written, the old file whole, no .new left", k, k == 1 ? "create" : k == 2 ? "write" : "replace");
        CHECK(!prof_write(1) && strstr(get(L"profiles\\1.ini"), "effect=old") && !there(L"profiles\\1.ini.new"), what);
    }
    file_fail = 0;

    // ---- a new profile: no name without its file
    start(BASE);
    file_fail = 1;
    CHECK(prof_save(0, "Film") == 0 && err() == PE_WRITE, "save fails when its file can't be written");
    CHECK(!prof_exists(3) && cfg_geti("general", "profile", 0) == 1, "no profile 3 shows, the one in use stays");
    file_fail = 0;
    CHECK(prof_save(0, "Film") == 3 && prof_exists(3) && there(L"profiles\\3.ini") && cfg_geti("general", "profile", 0) == 3, "retried, it is saved");
    reload();
    CHECK(prof_exists(3) && cfg_geti("general", "profile", 0) == 3, "after a restart it is there");
    // the settings can't be written after the profile's file: kept in memory, said, written with the next change
    start(BASE);
    {
        CHECK(prof_write(3), "profile 3's file made");
        cfg_set("profile.3", "name", "Film"); file_fail = 3; prof_saved(3); file_fail = 0;
        CHECK(err() == PE_CFG, "settings that can't be written are reported, not shown as saved");
    }

    // ---- switching
    start(BASE); put(L"profiles\\2.ini", NIGHT);
    cfg_set("flow", "speed", "9");   // a change in the one in use, not yet in its file
    file_fail = 2;
    CHECK(!prof_switch(2) && err() == PE_LEAVE, "switch refused when the one in use can't keep its changes");
    CHECK(cfg_geti("general", "profile", 0) == 1 && cfg_geti("flow", "speed", 0) == 9 && !strcmp(cfg_get("general", "effect", ""), "flow"), "the live setup and its change are kept");
    file_fail = 0;
    CHECK(prof_switch(2) && cfg_geti("general", "profile", 0) == 2 && !strcmp(cfg_get("general", "effect", ""), "static") && cfg_geti("dev.1", "enabled", 1) == 0, "retried, the switch is done");
    CHECK(strstr(get(L"profiles\\1.ini"), "speed=9") != NULL, "the one left has its change in its file");
    CHECK(cfg_geti("flow", "speed", 0) == 0 && !strcmp(cfg_get("dev.1", "kind", ""), "wled"), "what profile 2 lacks is back to default, other keys untouched");

    // ---- damaged files: read whole, refused before anything changes
    const char *bad[] = { "", "\r\n\r\n", "garbage\r\n[general]\r\neffect=x\r\n",
                          "; haku control profile 2 v2: cut\r\n[general]\r\neffect=static\r\n",        // no end: cut short
                          "; haku control profile 2 v2: x\r\n[general]\r\nnot a key\r\n; end\r\n",
                          "; haku control profile 2 v2: x\r\n; end\r\n[general]\r\neffect=static\r\n" };
    for (int i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        start(BASE); put(L"profiles\\2.ini", bad[i]);
        char what[64]; snprintf(what, sizeof(what), "damaged file %d refused, live setup untouched", i);
        CHECK(!prof_switch(2) && err() == PE_BAD && cfg_geti("general", "profile", 0) == 1 && !strcmp(cfg_get("general", "effect", ""), "flow") && cfg_geti("flow", "speed", 0) == 5, what);
    }
    start(BASE); { wchar_t p2[MAX_PATH]; app_data_path(L"profiles\\2.ini", p2); DeleteFileW(p2); }
    CHECK(!prof_switch(2) && err() == PE_MISSING && cfg_geti("general", "profile", 0) == 1, "missing file refused");
    // a file from before v2 (no end mark, an unknown key, a line of junk): read as it always was
    start(BASE); put(L"profiles\\2.ini", "; haku control profile 2: its setup, written when another profile takes over\r\n[general]\r\neffect=static\r\nfuture=1\r\njunk\r\n[flow]\r\nspeed=3\r\n");
    CHECK(prof_switch(2) && !strcmp(cfg_get("general", "effect", ""), "static") && cfg_geti("flow", "speed", 0) == 3, "an old profile file still loads");

    // ---- a copy: never a stale one
    start(BASE); put(L"profiles\\1.ini", "; haku control profile 1 v2: stale\r\n[general]\r\neffect=stale\r\n; end\r\n");
    cfg_set("flow", "speed", "7");
    file_fail = 3;
    CHECK(prof_dup(1, "Copy") == 0 && err() == PE_WRITE && !prof_exists(3), "copy of the one in use refused when it can't be written first");
    file_fail = 0;
    CHECK(prof_dup(1, "Copy") == 3 && strstr(get(L"profiles\\3.ini"), "speed=7") && !strstr(get(L"profiles\\3.ini"), "stale"), "the copy has the live setup");
    start(BASE); put(L"profiles\\2.ini", "; haku control profile 2 v2: cut\r\n[general]\r\n");
    CHECK(prof_dup(2, "Copy") == 0 && err() == PE_BAD && !prof_exists(3), "a damaged profile is not copied");

    // ---- delete and the start
    start(BASE); put(L"profiles\\2.ini", NIGHT);
    file_fail = 1; prof_delete(2); file_fail = 0;
    CHECK(err() == PE_CFG && there(L"profiles\\2.ini"), "delete whose settings can't be written keeps the file");
    reload();
    CHECK(prof_exists(2), "after a restart it is still whole (name and file)");
    prof_delete(2);
    CHECK(!prof_exists(2) && !there(L"profiles\\2.ini"), "delete: name and file gone");
    start("[general]\r\nprofile=5\r\n[profile.1]\r\nname=Day\r\n");
    prof_check();
    CHECK(cfg_geti("general", "profile", 0) == 0, "at start a profile in use that is gone is cleared");

    // ---- the settings' own save goes through the same fault points
    start(BASE);
    cfg_set("flow", "speed", "1"); file_fail = 2;
    CHECK(!cfg_save_if_dirty() && strstr(get(L"settings.ini"), "speed=5"), "settings not saved on a fault, the old file whole");
    file_fail = 0;
    CHECK(cfg_save_if_dirty() && strstr(get(L"settings.ini"), "speed=1"), "and saved on the next try");

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
