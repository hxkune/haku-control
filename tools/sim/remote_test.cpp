// SPDX-License-Identifier: GPL-3.0-only
// Checks for what a phone or Home Assistant may send (remote_allowed in src/ui_web.cpp), no window, no app:
//   build.cmd's environment, then: cl /nologo /utf-8 /std:c++17 /EHsc /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
//   tools\sim\remote_test.cpp /Fe:%TEMP%\remote_test.exe /link /FORCE:UNRESOLVED && %TEMP%\remote_test.exe
// The rest of the app is not linked in (hence /FORCE:UNRESOLVED): only remote_allowed and its field reader run here.
#include "../../src/ui_web.cpp"
#include <stdio.h>

static int fails;
static void check(bool c, const char *what) { printf(c ? "ok   %s\n" : "FAIL %s\n", what); fails += !c; }

int main() {
    check(remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"2\",\"name\":\"Night copy\"}"), "a copy of profile 2 is let through");
    check(remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"16\",\"name\":\"\xd0\x9d\xd0\xbe\xd1\x87\xd1\x8c (\xd0\xba\xd0\xbe\xd0\xbf\xd0\xb8\xd1\x8f)\"}"), "the last profile and a Russian name");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"0\",\"name\":\"x\"}"), "profile 0 refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"17\",\"name\":\"x\"}"), "a profile past the last refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"1a\",\"name\":\"x\"}"), "an id that is not a number refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"-1\",\"name\":\"x\"}"), "a negative id refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"name\":\"x\"}"), "no id refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"1\",\"name\":\"\"}"), "an empty name refused");
    check(!remote_allowed("{\"cmd\":\"profile_dup\",\"id\":\"1\",\"name\":\"a\\nb\"}"), "a line break in the name refused");
    check(!remote_allowed(std::string("{\"cmd\":\"profile_dup\",\"id\":\"1\",\"name\":\"") + std::string(161, 'a') + "\"}"), "a name past 160 bytes refused");
    // what was refused before stays refused
    check(!remote_allowed("{\"cmd\":\"set\",\"s\":\"remote\",\"k\":\"token\",\"v\":\"x\"}"), "remote settings still refused");
    check(!remote_allowed("{\"cmd\":\"set\",\"s\":\"openrgb\",\"k\":\"path\",\"v\":\"x\"}"), "a program path still refused");
    check(!remote_allowed("{\"cmd\":\"quit\"}"), "quit still refused");
    check(!remote_allowed("{\"cmd\":\"backup_load\"}"), "restore still refused outside test builds");
    printf(fails ? "remote_allowed: %d FAILED\n" : "remote_allowed: all ok\n", fails);
    return fails != 0;
}
