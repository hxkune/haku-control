// SPDX-License-Identifier: GPL-3.0-only
// This app usually runs with admin rights, and %TEMP% and %LOCALAPPDATA% can be written by any program of the user.
// So: a file this app is about to run with admin rights is kept in a folder only admins can touch (admin_tmp), and
// a program that lives in the user's folders is started without admin rights (run_as_user).
// For the app and its installer alike: needs only windows.h.
#pragma once
#include <windows.h>
#include <sddl.h>
#include <stdio.h>

static inline int tmp_elevated(void) {
    HANDLE t; TOKEN_ELEVATION e = { 0 }; DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n); CloseHandle(t);
    return e.TokenIsElevated != 0;
}

// A folder for a file that is checked and then run. With admin rights: a new one in Windows\Temp that only admins
// and the system can open (never one that was already there: anyone may have made it). Without: %TEMP%\tag,
// where a swap gains nothing this user does not have. 0: no folder, do not go on.
static inline int admin_tmp(const wchar_t *tag, wchar_t *out) {
    if (!tmp_elevated()) {
        GetTempPathW(MAX_PATH, out); wcscat_s(out, MAX_PATH, tag);
        return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa) }; wchar_t win[MAX_PATH]; int ok = 0;
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) return 0;
    for (DWORD i = 0, t = GetTickCount(); i < 8 && !ok; i++) {
        swprintf(out, MAX_PATH, L"%s\\Temp\\%s-%lu-%lu", win, tag, GetCurrentProcessId(), t + i);
        ok = CreateDirectoryW(out, &sa);
        if (!ok && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    LocalFree(sa.lpSecurityDescriptor);
    return ok;
}

// What earlier runs left: the files `files` in the folders tag* of %TEMP% and Windows\Temp, then the folders.
// Links to other folders are not followed (this may run with admin rights, in a folder anyone can write).
static inline void tmp_clean(const wchar_t *tag, const wchar_t *files) {
    wchar_t root[2][MAX_PATH], pat[MAX_PATH], d[MAX_PATH], f[MAX_PATH];
    GetTempPathW(MAX_PATH, root[0]);
    GetSystemWindowsDirectoryW(root[1], MAX_PATH); wcscat_s(root[1], MAX_PATH, L"\\Temp\\");
    for (int r = 0; r < 2; r++) {
        swprintf(pat, MAX_PATH, L"%s%s*", root[r], tag);
        WIN32_FIND_DATAW fd, ff; HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            swprintf(d, MAX_PATH, L"%s%s", root[r], fd.cFileName);
            swprintf(pat, MAX_PATH, L"%s\\%s", d, files);
            HANDLE g = FindFirstFileW(pat, &ff);
            if (g != INVALID_HANDLE_VALUE) {
                do { swprintf(f, MAX_PATH, L"%s\\%s", d, ff.cFileName); DeleteFileW(f); } while (FindNextFileW(g, &ff));
                FindClose(g);
            }
            RemoveDirectoryW(d);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// cmd started with this user's rights but no admin rights (the token Windows gives a program started normally)
static inline int limited_start(wchar_t *cmd, const wchar_t *dir, DWORD flags, PROCESS_INFORMATION *pi) {
    HANDLE t = NULL, r = NULL; PSID mid = NULL; int ok = 0;
    STARTUPINFOW si = { sizeof(si) };
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &t) &&
        CreateRestrictedToken(t, LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL, &r) && ConvertStringSidToSidW(L"S-1-16-8192", &mid)) {   // medium integrity
        TOKEN_MANDATORY_LABEL ml = { { mid, SE_GROUP_INTEGRITY } };
        ok = SetTokenInformation(r, TokenIntegrityLevel, &ml, sizeof(ml) + GetLengthSid(mid)) &&
             CreateProcessAsUserW(r, NULL, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, pi);
    }
    DWORD e = GetLastError();
    if (mid) LocalFree(mid);
    if (r) CloseHandle(r);
    if (t) CloseHandle(t);
    SetLastError(e);
    return ok;
}

// A program from the user's folders: never with admin rights. If they cannot be dropped, it is not started.
static inline int run_as_user(wchar_t *cmd, const wchar_t *dir, DWORD flags, PROCESS_INFORMATION *pi) {
    if (tmp_elevated()) return limited_start(cmd, dir, flags, pi);
    STARTUPINFOW si = { sizeof(si) };
    return CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL, dir, &si, pi);
}
