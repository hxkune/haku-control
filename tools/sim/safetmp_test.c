// src/safetmp.h: the folder for a file that is checked and then run, the clean-up of such folders, and a program
// started without admin rights. Run without admin rights it checks the plain branch and forces the limited start;
// run with them it also checks that the folder is a new one in Windows\Temp and that the child has no admin rights.
#include "../../src/safetmp.h"

static int bad;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); bad = 1; } } while (0)
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

int main(void) {
    int adm = tmp_elevated();
    printf("safetmp: running %s admin rights\n", adm ? "with" : "without");
    wchar_t d[MAX_PATH], d2[MAX_PATH], f[MAX_PATH], keep[MAX_PATH], win[MAX_PATH];
    CHECK(admin_tmp(L"haku-safetmp-test", d) && exists(d), "a folder is made");
    if (adm) {
        GetSystemWindowsDirectoryW(win, MAX_PATH); wcscat_s(win, MAX_PATH, L"\\Temp\\");
        CHECK(!_wcsnicmp(d, win, wcslen(win)), "with admin rights it is in Windows\\Temp");
        CHECK(admin_tmp(L"haku-safetmp-test", d2) && wcscmp(d, d2), "and never one that is already there");
    }
    swprintf(f, MAX_PATH, L"%s\\a.tmp", d); swprintf(keep, MAX_PATH, L"%s\\b.keep", d);
    FILE *o = _wfopen(f, L"wb"); if (o) fclose(o);
    o = _wfopen(keep, L"wb"); if (o) fclose(o);
    tmp_clean(L"haku-safetmp-test", L"*.tmp");
    CHECK(!exists(f) && exists(keep), "the clean-up deletes only the files it is told to");
    DeleteFileW(keep); tmp_clean(L"haku-safetmp-test", L"*.tmp");
    CHECK(!exists(d), "and removes the emptied folder");

    wchar_t cmd[64] = L"cmd.exe /c exit 7"; PROCESS_INFORMATION pi; DWORD code = 0, n; HANDLE t; TOKEN_ELEVATION e = { 1 };
    int ok = limited_start(cmd, NULL, CREATE_NO_WINDOW | CREATE_SUSPENDED, &pi);
    CHECK(ok, "a program starts with the limited token");
    if (ok) {
        if (OpenProcessToken(pi.hProcess, TOKEN_QUERY, &t)) { GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n); CloseHandle(t); }
        CHECK(!e.TokenIsElevated, "it has no admin rights");
        ResumeThread(pi.hThread); DWORD w = WaitForSingleObject(pi.hProcess, 10000); GetExitCodeProcess(pi.hProcess, &code);
        if (code != 7) printf("     wait %lu, exit code 0x%08lX
", w, code);
        CHECK(code == 7, "it runs and ends with its own code");
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    puts(bad ? "safetmp: FAILED" : "safetmp: ok");
    return bad;
}
