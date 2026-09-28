// haku-control-open.exe: what the Start menu shortcut runs. It needs no admin rights, so opening the window
// never shows a UAC prompt: if haku control is running it just asks it to show its window, otherwise it
// starts the elevated logon task (which may run with highest privileges without a prompt) and then asks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    UINT msg = RegisterWindowMessageW(L"haku_control_show");
    HWND h = FindWindowW(L"haku-control", L"haku-control");
    if (!h) {
        SHELLEXECUTEINFOW si = { sizeof(si) };
        si.fMask = SEE_MASK_NOCLOSEPROCESS;
        si.lpFile = L"schtasks.exe";
        si.lpParameters = L"/Run /TN haku-control";
        si.nShow = SW_HIDE;
        if (ShellExecuteExW(&si) && si.hProcess) { WaitForSingleObject(si.hProcess, 5000); CloseHandle(si.hProcess); }
        for (int i = 0; i < 40 && !(h = FindWindowW(L"haku-control", L"haku-control")); i++) Sleep(100);
        if (!h) {
            // start with Windows is off (the task is disabled): start it directly, Windows asks for admin rights
            wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
            wchar_t *s = wcsrchr(exe, L'\\'); if (s) wcscpy_s(s + 1, MAX_PATH - (s + 1 - exe), L"haku-control.exe");
            if ((INT_PTR)ShellExecuteW(NULL, L"runas", exe, L"--settings", NULL, SW_SHOWNORMAL) <= 32)
                MessageBoxW(NULL, L"haku control could not be started. Reinstall it, or start it from the install folder.",
                            L"haku control", MB_ICONWARNING);
            return 0;   // it opens its window itself
        }
        Sleep(300);   // let it finish starting
    }
    AllowSetForegroundWindow(ASFW_ANY);
    PostMessageW(h, msg, 0, 0);
    return 0;
}
