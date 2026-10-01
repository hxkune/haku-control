// SPDX-License-Identifier: GPL-3.0-only
// haku-control-setup.exe: installs / updates / removes haku control.
//   haku-control-setup.exe              install or update (asks first)
//   haku-control-setup.exe /uninstall   remove (Apps & Features runs this copy from the install folder)
// The program files travel inside this exe as one RCDATA resource ("PAYLOAD", made by scripts\pack.ps1):
//   "HAKUPKG1", then per file: u16 name length, UTF-8 name (relative, backslashes), u32 size, bytes.
// Settings in %APPDATA%\haku-control are never touched by an update. An old "rgbfx" install is migrated.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#define SECURITY_WIN32
#include <security.h>
#include <stdio.h>
#include "../res/version.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int ru;
#define TR(en, r) (ru ? (r) : (en))
#define APP_TITLE L"haku control"
#define UNINST_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\haku-control"

static wchar_t dest[MAX_PATH], data_dir[MAX_PATH];

// ---------------------------------------------------------------- small helpers
static int run_wait(const wchar_t *exe, const wchar_t *args, DWORD ms) {
    wchar_t cmd[2048]; swprintf(cmd, 2048, L"\"%s\" %s", exe, args);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

// Inbound rule for the phone page and for lights answering a network scan: only from private addresses (the home
// network, also when it has several subnets, and Tailscale), only for haku-control.exe. The app itself also refuses
// anything else, and repairs this rule when it starts (remote.c).
static void firewall_rule(int add) {
    run_wait(L"netsh.exe", L"advfirewall firewall delete rule name=\"haku control\"", 10000);
    if (!add) return;
    wchar_t args[MAX_PATH + 256];
    swprintf(args, MAX_PATH + 256, L"advfirewall firewall add rule name=\"haku control\" dir=in action=allow enable=yes profile=any "
             L"program=\"%s\\haku-control.exe\" remoteip=LocalSubnet,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,100.64.0.0/10", dest);
    run_wait(L"netsh.exe", args, 10000);
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void delete_tree(const wchar_t *dir) {
    wchar_t from[MAX_PATH + 2] = { 0 };
    wcsncpy_s(from, MAX_PATH, dir, _TRUNCATE);   // double-NUL terminated
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static int process_running(const wchar_t *name) {
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) if (!_wcsicmp(pe.szExeFile, name)) found = 1;
    CloseHandle(snap);
    return found;
}

static void kill_process(const wchar_t *name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, name)) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        }
    CloseHandle(snap);
}

// Asks a running copy to quit (it lets go of the lights the way the user chose), then waits for it.
static void stop_app(void) {
    static const wchar_t *cls[][3] = { { L"haku-control", L"haku_control_quit", L"haku-control.exe" }, { L"rgbfx", L"rgbfx_quit", L"rgbfx.exe" } };
    for (int i = 0; i < 2; i++) {
        HWND h = FindWindowW(cls[i][0], cls[i][0]);
        if (h) PostMessageW(h, RegisterWindowMessageW(cls[i][1]), 0, 0);
    }
    for (int t = 0; t < 100 && (process_running(L"haku-control.exe") || process_running(L"rgbfx.exe")); t++) Sleep(100);
    kill_process(L"haku-control.exe"); kill_process(L"rgbfx.exe");
    // the logon task may still count the old one as running, and it ignores a new start while it does
    run_wait(L"schtasks.exe", L"/End /TN haku-control", 10000);
    Sleep(300);
}

// ---------------------------------------------------------------- payload
static int extract(const wchar_t *to, DWORD *total) {
    HRSRC r = FindResourceW(NULL, L"PAYLOAD", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return 0;
    const BYTE *p = LockResource(LoadResource(NULL, r)), *e = p + SizeofResource(NULL, r);
    if (e - p < 8 || memcmp(p, "HAKUPKG1", 8)) return 0;
    p += 8;
    *total = 0;
    while (p + 6 <= e) {
        unsigned short nl; memcpy(&nl, p, 2); p += 2;
        if (p + nl + 4 > e) return 0;
        char name8[512] = { 0 }; memcpy(name8, p, nl < 511 ? nl : 511); p += nl;
        unsigned int size; memcpy(&size, p, 4); p += 4;
        if (p + size > e || strstr(name8, "..") || name8[0] == '\\' || strchr(name8, ':')) return 0;
        wchar_t name[512], full[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, name8, -1, name, 512);
        swprintf(full, MAX_PATH * 2, L"%s\\%s", to, name);
        wchar_t dir[MAX_PATH * 2]; wcscpy_s(dir, MAX_PATH * 2, full);
        wchar_t *s = wcsrchr(dir, L'\\'); if (s) { *s = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
        HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) return 0;
        DWORD w = 0; BOOL ok = WriteFile(f, p, size, &w, NULL); CloseHandle(f);
        if (!ok || w != size) return 0;
        p += size; *total += size;
    }
    return 1;
}

// ---------------------------------------------------------------- migration from the old "rgbfx" build
static void migrate(void) {
    wchar_t old[MAX_PATH], oldData[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\rgbfx", old, MAX_PATH);
    ExpandEnvironmentStringsW(L"%APPDATA%\\rgbfx", oldData, MAX_PATH);
    swprintf(a, MAX_PATH, L"%s\\rgbfx.ini", old); swprintf(b, MAX_PATH, L"%s\\settings.ini", data_dir);
    if (exists(a) && !exists(b)) {
        FILE *f = _wfopen(a, L"rb");
        if (f) {
            static char buf[256 * 1024]; size_t n = fread(buf, 1, sizeof(buf) - 256, f); fclose(f); buf[n] = 0;
            if (!strstr(buf, "[hotspot]")) strcat_s(buf, sizeof(buf), "\r\n[hotspot]\r\nauto=1\r\n");   // rgbfx kept it on
            FILE *o = _wfopen(b, L"wb"); if (o) { fwrite(buf, 1, strlen(buf), o); fclose(o); }
        }
    }
    static const wchar_t *files[] = { L"aidot.json", L"nanoleaf.json" };
    for (int i = 0; i < 2; i++) {
        swprintf(a, MAX_PATH, L"%s\\%s", oldData, files[i]); swprintf(b, MAX_PATH, L"%s\\%s", data_dir, files[i]);
        if (exists(a) && !exists(b)) CopyFileW(a, b, TRUE);
    }
    swprintf(a, MAX_PATH, L"%s\\msi_backup.bin", old); swprintf(b, MAX_PATH, L"%s\\msi_backup.bin", data_dir);
    if (exists(a) && !exists(b)) CopyFileW(a, b, TRUE);
    run_wait(L"schtasks.exe", L"/Delete /TN rgbfx /F", 10000);
    swprintf(a, MAX_PATH, L"%s\\rgbfx.exe", old);
    if (exists(a)) delete_tree(old);
}

// ---------------------------------------------------------------- logon task, shortcut, uninstall entry
static int register_task(int enabled) {
    wchar_t user[256]; ULONG ul = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &ul)) return 0;
    wchar_t xml[4096];
    swprintf(xml, 4096,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>haku control - PC and room lighting</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId><Delay>PT5S</Delay></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><StartWhenAvailable>true</StartWhenAvailable>"
        L"<Enabled>%s</Enabled></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"%s\\haku-control.exe\"</Command><WorkingDirectory>%s</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n", user, user, enabled ? L"true" : L"false", dest, dest);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); wcscat_s(tmp, MAX_PATH, L"haku-control-task.xml");
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return 0;
    fwrite("\xFF\xFE", 1, 2, f); fwrite(xml, sizeof(wchar_t), wcslen(xml), f); fclose(f);
    wchar_t args[MAX_PATH + 64]; swprintf(args, MAX_PATH + 64, L"/Create /TN haku-control /XML \"%s\" /F", tmp);
    int rc = run_wait(L"schtasks.exe", args, 20000);
    DeleteFileW(tmp);
    return rc == 0;
}

static void shortcut_path(wchar_t *p) {
    wchar_t *progs = NULL;
    SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &progs);
    swprintf(p, MAX_PATH, L"%s\\haku control.lnk", progs ? progs : L"");
    CoTaskMemFree(progs);
}

static void make_shortcut(void) {
    IShellLinkW *sl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    wchar_t target[MAX_PATH], icon[MAX_PATH], lnk[MAX_PATH];
    swprintf(target, MAX_PATH, L"%s\\haku-control-open.exe", dest);
    swprintf(icon, MAX_PATH, L"%s\\haku-control.exe", dest);
    IShellLinkW_SetPath(sl, target);
    IShellLinkW_SetWorkingDirectory(sl, dest);
    IShellLinkW_SetIconLocation(sl, icon, 0);
    IShellLinkW_SetDescription(sl, L"PC and room lighting");
    IPersistFile *pf = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        shortcut_path(lnk);
        IPersistFile_Save(pf, lnk, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(sl);
}

static void set_str(HKEY k, const wchar_t *name, const wchar_t *v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)(wcslen(v) + 1) * 2); }
static void set_dw(HKEY k, const wchar_t *name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, 4); }

static void register_uninstall(DWORD bytes) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UNINST_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    wchar_t v[MAX_PATH + 32];
    set_str(k, L"DisplayName", APP_TITLE);
    set_str(k, L"DisplayVersion", HAKU_VER_WSTR);
    set_str(k, L"Publisher", L"haku");
    set_str(k, L"InstallLocation", dest);
    swprintf(v, MAX_PATH + 32, L"%s\\haku-control.exe,0", dest); set_str(k, L"DisplayIcon", v);
    swprintf(v, MAX_PATH + 32, L"\"%s\\haku-control-setup.exe\" /uninstall", dest); set_str(k, L"UninstallString", v);
    set_dw(k, L"EstimatedSize", bytes / 1024 + 64);
    set_dw(k, L"NoModify", 1); set_dw(k, L"NoRepair", 1);
    RegCloseKey(k);
}

static int webview2_installed(void) {
    static const wchar_t *keys[] = {
        L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}",
        L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" };
    wchar_t pv[64]; DWORD n;
    for (int i = 0; i < 2; i++) {
        n = sizeof(pv);
        if (!RegGetValueW(HKEY_LOCAL_MACHINE, keys[i], L"pv", RRF_RT_REG_SZ, NULL, pv, &n) && wcscmp(pv, L"0.0.0.0")) return 1;
        n = sizeof(pv);
        if (!RegGetValueW(HKEY_CURRENT_USER, keys[i], L"pv", RRF_RT_REG_SZ, NULL, pv, &n) && wcscmp(pv, L"0.0.0.0")) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- dialogs
static HRESULT CALLBACK td_cb(HWND h, UINT n, WPARAM w, LPARAM l, LONG_PTR ref) {
    (void)h; (void)w; (void)ref;
    if (n == TDN_HYPERLINK_CLICKED) ShellExecuteW(NULL, L"open", (LPCWSTR)l, NULL, NULL, SW_SHOWNORMAL);
    return S_OK;
}

static int ask(const wchar_t *main, const wchar_t *text, const wchar_t *yes, const wchar_t *check, BOOL *checked, const wchar_t *footer) {
    TASKDIALOG_BUTTON b[] = { { IDOK, yes } };
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | (checked && *checked ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = APP_TITLE L" " HAKU_VER_WSTR;
    c.hInstance = GetModuleHandleW(NULL);
    c.pszMainIcon = MAKEINTRESOURCEW(1);
    c.pszMainInstruction = main; c.pszContent = text;
    c.cButtons = 1; c.pButtons = b; c.nDefaultButton = IDOK;
    c.pszVerificationText = check;
    if (footer) { c.pszFooter = footer; c.pszFooterIcon = TD_INFORMATION_ICON; }
    c.pfCallback = td_cb;
    int btn = 0;
    TaskDialogIndirect(&c, &btn, NULL, checked);
    return btn == IDOK;
}

static void info(const wchar_t *main, const wchar_t *text, PCWSTR icon) {
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    c.pszWindowTitle = APP_TITLE;
    c.pszMainIcon = icon; c.pszMainInstruction = main; c.pszContent = text;
    c.pfCallback = td_cb;
    TaskDialogIndirect(&c, NULL, NULL, NULL);
}

// ---------------------------------------------------------------- install / uninstall
// quiet: started by the app itself for an update (/update): no questions, no "done" message, autostart as it was
static int install(int quiet, int quiet_autostart) {
    wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%s\\haku-control.exe", dest);
    int update = exists(exe);
    BOOL autostart = quiet ? quiet_autostart : TRUE;
    wchar_t footer[512] = L"";
    wchar_t pawn[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", pawn, MAX_PATH);
    if (!exists(pawn))
        wcscpy_s(footer, 512, TR(L"Memory (RAM) lighting needs <a href=\"https://pawnio.eu\">PawnIO</a>. Everything else works without it.",
                                 L"Для подсветки памяти нужен <a href=\"https://pawnio.eu\">PawnIO</a>. Всё остальное работает без него."));
    if (!quiet && !ask(update ? TR(L"Update haku control", L"Обновить haku control") : TR(L"Install haku control", L"Установить haku control"),
             TR(L"One lighting scene for your PC and your room.\n\nInstalls to Program Files and starts at sign-in with administrator rights "
                L"(needed for the motherboard and memory lighting). Your settings stay in %APPDATA%\\haku-control.",
                L"Одна сцена света для ПК и комнаты.\n\nУстанавливается в Program Files и запускается при входе с правами администратора "
                L"(нужны для подсветки платы и памяти). Настройки хранятся в %APPDATA%\\haku-control."),
             update ? TR(L"Update", L"Обновить") : TR(L"Install", L"Установить"),
             TR(L"Start with Windows", L"Запускать вместе с Windows"), &autostart, footer[0] ? footer : NULL))
        return 1;

    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    stop_app();
    SHCreateDirectoryExW(NULL, data_dir, NULL);
    migrate();
    SHCreateDirectoryExW(NULL, dest, NULL);
    wchar_t ui[MAX_PATH]; swprintf(ui, MAX_PATH, L"%s\\ui", dest);
    delete_tree(ui);
    DWORD bytes = 0;
    if (!extract(dest, &bytes)) {
        SetCursor(old);
        info(TR(L"Installation failed", L"Установка не удалась"), TR(L"Could not write the program files. Is the folder in use?", L"Не удалось записать файлы программы. Папка занята?"), TD_ERROR_ICON);
        return 2;
    }
    // this setup stays in the install folder as the uninstaller
    wchar_t self[MAX_PATH], copy[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(copy, MAX_PATH, L"%s\\haku-control-setup.exe", dest);
    if (_wcsicmp(self, copy)) CopyFileW(self, copy, FALSE);
    int task = register_task(autostart);
    make_shortcut();
    firewall_rule(1);
    register_uninstall(bytes);
    SetCursor(old);

    // start it the way it runs at sign-in (through the task), or directly if autostart is off. The task answers
    // "done" even when it ignores the start (it still counted the old one as running), so it is checked that the
    // program is really there, and started directly when it is not.
    int started = 0;
    if (task && autostart && run_wait(L"schtasks.exe", L"/Run /TN haku-control", 10000) == 0)
        for (int t = 0; t < 80 && !(started = process_running(L"haku-control.exe")); t++) Sleep(100);
    if (!started) {
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dest, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
    if (quiet) return 0;
    info(update ? TR(L"haku control is updated", L"haku control обновлён") : TR(L"haku control is installed", L"haku control установлен"),
         webview2_installed()
             ? TR(L"It is running in the notification area (tray). Click its icon to open the window; it is also in the Start menu.",
                  L"Программа работает в области уведомлений (трее). Нажмите на значок, чтобы открыть окно; она также есть в меню «Пуск».")
             : TR(L"The settings window needs the Microsoft Edge <a href=\"https://developer.microsoft.com/microsoft-edge/webview2/\">WebView2 Runtime</a>. The lights already work.",
                  L"Для окна настроек нужен <a href=\"https://developer.microsoft.com/microsoft-edge/webview2/\">WebView2 Runtime</a> от Microsoft. Свет уже работает."),
         TD_INFORMATION_ICON);
    return 0;
}

static int uninstall(void) {
    BOOL purge = FALSE;
    if (!ask(TR(L"Remove haku control?", L"Удалить haku control?"),
             TR(L"The app is closed first, so the lights do what you chose for exit (off by default).",
                L"Сначала программа закроется, и свет сделает то, что выбрано для выхода (по умолчанию погаснет)."),
             TR(L"Remove", L"Удалить"), TR(L"Also delete settings and device keys", L"Удалить также настройки и ключи устройств"), &purge, NULL))
        return 1;
    stop_app();
    run_wait(L"schtasks.exe", L"/Delete /TN haku-control /F", 10000);
    wchar_t lnk[MAX_PATH]; shortcut_path(lnk); DeleteFileW(lnk);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, UNINST_KEY);
    firewall_rule(0);
    if (purge) {
        delete_tree(data_dir);
        wchar_t l[MAX_PATH]; ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\haku-control", l, MAX_PATH); delete_tree(l);
    }
    // this exe lives in the folder: remove the folder once we have exited
    wchar_t cmd[MAX_PATH * 2 + 128], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, MAX_PATH * 2 + 128, L"\"%s\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%s\"", sys, dest);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    info(TR(L"haku control was removed", L"haku control удалён"), purge ? L"" : TR(L"Your settings are kept in %APPDATA%\\haku-control.", L"Настройки сохранены в %APPDATA%\\haku-control."), TD_INFORMATION_ICON);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)show;
    ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\haku-control", dest, MAX_PATH);
    ExpandEnvironmentStringsW(L"%APPDATA%\\haku-control", data_dir, MAX_PATH);
    // /extract <folder>: just unpack the program files (portable use, tests); no admin rights needed
    const wchar_t *ex = wcsstr(cmd, L"/extract ");
    if (ex) {
        wchar_t to[MAX_PATH]; wcsncpy_s(to, MAX_PATH, ex + 9, _TRUNCATE);
        wchar_t *q = to; while (*q == L' ' || *q == L'"') q++;
        wchar_t *e = q + wcslen(q); while (e > q && (e[-1] == L' ' || e[-1] == L'"')) *--e = 0;
        SHCreateDirectoryExW(NULL, q, NULL);
        DWORD bytes; int ok = extract(q, &bytes);
        CoUninitialize();
        return ok ? 0 : 2;
    }
    // install / uninstall need admin rights: ask for them by starting this exe again elevated
    if (!IsUserAnAdmin()) {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
        SHELLEXECUTEINFOW si = { sizeof(si) };
        si.fMask = SEE_MASK_NOCLOSEPROCESS; si.lpVerb = L"runas"; si.lpFile = self; si.lpParameters = cmd; si.nShow = SW_SHOWNORMAL;
        DWORD code = 1;
        if (ShellExecuteExW(&si) && si.hProcess) { WaitForSingleObject(si.hProcess, INFINITE); GetExitCodeProcess(si.hProcess, &code); CloseHandle(si.hProcess); }
        CoUninitialize();
        return (int)code;
    }
    const wchar_t *as = wcsstr(cmd, L"/autostart=");
    int rc = wcsstr(cmd, L"/uninstall") ? uninstall() : install(wcsstr(cmd, L"/update") != NULL, as ? as[11] != L'0' : 1);
    CoUninitialize();
    return rc;
}
