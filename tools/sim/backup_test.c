// SPDX-License-Identifier: GPL-3.0-only
// Checks for the settings backup (src/backup.c over src/config.c) in a folder of its own, no app, no devices:
//   build.cmd's environment, then: cl /nologo /utf-8 /DHAKU_DEV /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
//   tools\sim\backup_test.c src\backup.c src\config.c comdlg32.lib /Fe:%TEMP%\backup_test.exe && %TEMP%\backup_test.exe
#include "../../src/common.h"
#include <stdarg.h>
#include <stdlib.h>

static wchar_t dir[MAX_PATH];
void app_data_path(const wchar_t *name, wchar_t *out) { swprintf(out, MAX_PATH, L"%s%s", dir, name); }
void app_profiles_flush(void) { }
void app_reload_all(void) { }
void logf_(const char *fmt, ...) { (void)fmt; }
int backup_json(char *out, int cap);

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static void put(const wchar_t *name, const char *text) {
    wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"wb"); fputs(text, f); fclose(f);
}
static char *get(const wchar_t *name) {
    static char b[8192]; wchar_t p[MAX_PATH]; app_data_path(name, p);
    FILE *f = _wfopen(p, L"rb"); if (!f) return "";
    size_t n = fread(b, 1, sizeof(b) - 1, f); b[n] = 0; fclose(f); return b;
}
static const char *result(void) { static char b[128]; backup_json(b, sizeof(b)); return b; }
static void use_file(const wchar_t *name) { wchar_t p[MAX_PATH]; app_data_path(name, p); SetEnvironmentVariableW(L"HAKU_BACKUP_FILE", p); }

static const char *SETTINGS =
    "[general]\r\nprofile=2\r\nname=desk\r\n[profile.2]\r\nname=Night\r\npin=1\r\nat=22:00\r\n"
    "[dev.1]\r\nkind=wled\r\nhost=10.0.0.5\r\nkey=SECRET-KEY\r\n[pro]\r\nlicence=SECRET-LIC\r\n";

int main(void) {
    GetTempPathW(MAX_PATH, dir); wcscat_s(dir, MAX_PATH, L"haku-backup-test\\");
    CreateDirectoryW(dir, NULL);
    wchar_t pd[MAX_PATH]; app_data_path(L"profiles", pd); CreateDirectoryW(pd, NULL);
    put(L"profiles\\2.ini", "[flow]\r\nspeed=4\r\n");
    put(L"settings.ini", SETTINGS);
    wchar_t ini[MAX_PATH]; app_data_path(L"settings.ini", ini); cfg_load(ini);

    // export: the pinned profile goes along, the secrets do not
    use_file(L"a.hakubackup"); backup_save(NULL);
    char *bk = get(L"a.hakubackup");
    CHECK(strstr(result(), "saved"), "export saved");
    CHECK(strstr(bk, "[profile.2]\r\nname=Night\r\npin=1"), "a pinned profile keeps pin=1 in the backup");
    CHECK(!strstr(bk, "SECRET"), "no device key or licence in the backup");

    // restore on a PC where the profile is not pinned: it comes back pinned, this PC's key stays
    cfg_set("profile.2", "pin", "0"); cfg_set("general", "name", "other"); cfg_save_if_dirty();
    backup_load(NULL);
    CHECK(strstr(result(), "restored"), "restore says restored");
    CHECK(!strcmp(cfg_get("profile.2", "pin", ""), "1"), "the pin is back after restore");
    CHECK(!strcmp(cfg_get("dev.1", "key", ""), "SECRET-KEY"), "this PC's device key is kept");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=4") != NULL, "the profile file is restored");

    // a profile that cannot be written: nothing changes, and it says so
    cfg_set("general", "name", "mine"); cfg_save_if_dirty();
    put(L"profiles\\2.ini", "[flow]\r\nspeed=9\r\n");
    wchar_t blk[MAX_PATH]; app_data_path(L"profiles\\2.ini.new", blk); CreateDirectoryW(blk, NULL);   // in the way
    backup_load(NULL);
    CHECK(strstr(result(), "bad_write"), "a failed profile write is reported");
    CHECK(!strcmp(cfg_get("general", "name", ""), "mine"), "settings in memory untouched");
    CHECK(strstr(get(L"settings.ini"), "name=mine") != NULL, "settings on disk untouched");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "the old profile file untouched");
    RemoveDirectoryW(blk);

    // the settings file cannot be written: back to what was there, the new profiles thrown away
    app_data_path(L"settings.ini.new", blk); CreateDirectoryW(blk, NULL);
    backup_load(NULL);
    CHECK(strstr(result(), "bad_write"), "a failed settings write is reported");
    CHECK(!strcmp(cfg_get("general", "name", ""), "mine"), "settings back to the old ones");
    CHECK(strstr(get(L"profiles\\2.ini"), "speed=9") != NULL, "old profile kept when the settings fail");
    CHECK(!*get(L"profiles\\2.ini.new"), "no leftover new profile");
    RemoveDirectoryW(blk);

    // a damaged file (a line that is not key=value) changes nothing
    put(L"bad.hakubackup", "haku control backup 1\r\n#file settings.ini\r\n[general]\r\nname=x\r\nthis is not a setting\r\n");
    use_file(L"bad.hakubackup"); backup_load(NULL);
    CHECK(strstr(result(), "bad_file"), "a damaged backup is refused");
    CHECK(!strcmp(cfg_get("general", "name", ""), "mine"), "and nothing changed");

    printf(fails ? "backup: %d FAILED\n" : "backup: all ok\n", fails);
    return fails != 0;
}
