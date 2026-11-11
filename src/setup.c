// SPDX-License-Identifier: GPL-3.0-only
// haku-control-setup.exe: installs / updates / removes haku control.
//   haku-control-setup.exe              install or update (asks first)
//   haku-control-setup.exe /uninstall   remove (Apps & Features runs this copy from the install folder)
// The program files travel inside this exe as one RCDATA resource ("PAYLOAD", made by scripts\pack.ps1):
//   "HAKUPKG1", then per file: u16 name length, UTF-8 name (relative, backslashes), u32 size, bytes.
// Settings in %APPDATA%\haku-control are never touched by an update. An old "rgbfx" install is migrated.
// An install or update never writes into the installed folder: the new files go to "haku-control.new" beside it
// and are checked, the app is asked to quit, and the two folders are swapped by renaming (the installed one is kept
// as "haku-control.old" until the swap is done). A failure puts the previous version back; a run cut off midway is
// put right by the next run from the folder names (recover()). SETUP_TEST builds the self-test at the end instead.
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
#include "safetmp.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int lang;   // 0 English, 1 Russian, 2 French
#define TR(en, r, f) (lang == 1 ? (r) : lang == 2 ? (f) : (en))
#define APP_TITLE L"haku control"
#define UNINST_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\haku-control"

static wchar_t dest[MAX_PATH], dest_new[MAX_PATH], dest_old[MAX_PATH], data_dir[MAX_PATH];
// what failed last, for the message: the Windows error and the file or folder
static DWORD fail_err;
static wchar_t fail_path[MAX_PATH * 2];
static void failed(const wchar_t *path) {
    fail_err = GetLastError(); if (!fail_err) fail_err = ERROR_WRITE_FAULT;
    wcsncpy_s(fail_path, MAX_PATH * 2, path ? path : L"", _TRUNCATE);
}
static int move_tries = 20;   // x250 ms: an antivirus or the search indexer may hold a file for a moment
// the app's exit code when it could not confirm the fans were given back (main.c)
#define APP_FANS_LEFT 3
#ifdef SETUP_TEST
static int t_fail_write = -1;   // the number of the file whose write fails (-1: none)
static ULONGLONG t_free;        // free space to report (0: the real one)
#endif

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

// 1: the folder is gone (or was not there)
static int delete_tree(const wchar_t *dir) {
    if (!exists(dir)) return 1;
    wchar_t from[MAX_PATH + 2] = { 0 };
    wcsncpy_s(from, MAX_PATH, dir, _TRUNCATE);   // double-NUL terminated
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    return !exists(dir);
}

static int process_running(const wchar_t *name) {
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) if (!_wcsicmp(pe.szExeFile, name)) found = 1;
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- payload
// The package's files under to. 1: every file written, flushed and closed; 0: a damaged package (fail_err is
// ERROR_BAD_FORMAT) or a failed write (fail_err/fail_path say which). The package must end exactly after its last
// file and must hold haku-control.exe, so a cut-off one is not taken for a whole one.
static int unpack(const BYTE *p, size_t len, const wchar_t *to, DWORD *total) {
    const BYTE *e = p + len;
    int n = 0, has_exe = 0;
    *total = 0; fail_err = ERROR_BAD_FORMAT; fail_path[0] = 0;
    if (!p || len < 8 || memcmp(p, "HAKUPKG1", 8)) return 0;
    p += 8;
    while (p < e) {
        if (e - p < 6) return 0;
        unsigned short nl; memcpy(&nl, p, 2); p += 2;
        if (!nl || nl > 511 || (size_t)(e - p) < (size_t)nl + 4) return 0;
        char name8[512] = { 0 }; memcpy(name8, p, nl); p += nl;
        unsigned int size; memcpy(&size, p, 4); p += 4;
        if ((size_t)(e - p) < size || strlen(name8) != nl || strstr(name8, "..") || name8[0] == '\\' || name8[0] == '/' || strchr(name8, ':')) return 0;
        wchar_t name[512], full[MAX_PATH * 2];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name8, -1, name, 512)) return 0;
        if (!_wcsicmp(name, L"haku-control.exe")) has_exe = 1;
        swprintf(full, MAX_PATH * 2, L"%s\\%s", to, name);
        wchar_t dir[MAX_PATH * 2]; wcscpy_s(dir, MAX_PATH * 2, full);
        wchar_t *s = wcsrchr(dir, L'\\'); if (s) { *s = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
        HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { failed(full); return 0; }
        DWORD w = 0;
        BOOL ok = WriteFile(f, p, size, &w, NULL) && w == size;
#ifdef SETUP_TEST
        if (n == t_fail_write) { ok = FALSE; SetLastError(ERROR_DISK_FULL); }
#endif
        if (ok) ok = FlushFileBuffers(f);
        if (!ok) failed(full);
        if (!CloseHandle(f) && ok) { failed(full); ok = FALSE; }
        if (!ok) return 0;
        p += size; *total += size; n++;
    }
    if (!has_exe) { fail_err = ERROR_BAD_FORMAT; return 0; }
    return 1;
}

static const BYTE *payload(DWORD *len) {
    *len = 0;
    HRSRC r = FindResourceW(NULL, L"PAYLOAD", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return NULL;
    *len = SizeofResource(NULL, r);
    return LockResource(LoadResource(NULL, r));
}

static int extract(const wchar_t *to, DWORD *total) {
    DWORD len; const BYTE *p = payload(&len);
    return unpack(p, len, to, total);
}

// ---------------------------------------------------------------- staging and the swap
static ULONGLONG free_space(void) {
#ifdef SETUP_TEST
    if (t_free) return t_free;
#endif
    wchar_t parent[MAX_PATH]; wcscpy_s(parent, MAX_PATH, dest);
    wchar_t *s = wcsrchr(parent, L'\\'); if (s) *s = 0;   // Program Files: the program folder may not be there yet
    ULARGE_INTEGER a;
    return GetDiskFreeSpaceExW(parent, &a, NULL, NULL) ? a.QuadPart : ~0ULL;   // unknown: the writes will say
}

// A run cut off midway (power, a crash) leaves the folders as they were: the swap is two renames, so .old with no
// program folder means it stopped between them and the previous version goes back; both means the swap was done
// and only the old copy was left; a .new is an unfinished staging. 1: the program folder is a whole version again
// (or there was none); 0: the previous version could not be put back (fail_err says why)
static int recover(void) {
    if (exists(dest_old)) {
        if (!exists(dest)) { if (!MoveFileExW(dest_old, dest, 0)) { failed(dest_old); return 0; } }
        else delete_tree(dest_old);   // not needed for anything; tried again next time
    }
    delete_tree(dest_new);
    return 1;
}

// The new version into dest.new beside the installed one, which is not touched. 1: complete and checked
static int stage(const BYTE *pkg, DWORD len, const wchar_t *self, DWORD *bytes) {
    *bytes = 0;
    if (!delete_tree(dest_new)) { failed(dest_new); return 0; }
    // the files and this setup's copy, while the installed version is still there
    if (free_space() < (ULONGLONG)len * 2 + 64ull * 1024 * 1024) { fail_err = ERROR_DISK_FULL; wcscpy_s(fail_path, MAX_PATH * 2, dest); return 0; }
    int rc = SHCreateDirectoryExW(NULL, dest_new, NULL);
    if (rc != ERROR_SUCCESS) { SetLastError(rc); failed(dest_new); return 0; }
    int ok = unpack(pkg, len, dest_new, bytes);
    if (ok && self) {   // this setup stays in the program folder as the uninstaller
        wchar_t copy[MAX_PATH * 2]; swprintf(copy, MAX_PATH * 2, L"%s\\haku-control-setup.exe", dest_new);
        if (!CopyFileW(self, copy, FALSE)) { failed(copy); ok = 0; }
    }
    if (!ok) { DWORD e = fail_err; delete_tree(dest_new); fail_err = e; }
    return ok;
}

static int move_dir(const wchar_t *from, const wchar_t *to) {
    for (int t = 1; ; t++) {
        if (MoveFileExW(from, to, 0)) return 1;
        if (t >= move_tries) { failed(from); return 0; }
        Sleep(250);
    }
}

// dest.new becomes the program folder; the installed one is kept as dest.old meanwhile. 1: done (dest.old is left
// for the caller to delete); 0: nothing changed, the previous version is in place; -1: it could not be put back
// either (recover() tries again on the next run). fail_err/fail_path: the first failure
static int swap_in(void) {
    int had = exists(dest);
    if (had && !move_dir(dest, dest_old)) return 0;
    if (move_dir(dest_new, dest)) return 1;
    DWORD e = fail_err; wchar_t pth[MAX_PATH * 2]; wcscpy_s(pth, MAX_PATH * 2, fail_path);
    int back = !had || move_dir(dest_old, dest);
    fail_err = e; wcscpy_s(fail_path, MAX_PATH * 2, pth);
    return back ? 0 : -1;
}

// ---------------------------------------------------------------- PawnIO
// The driver memory (SMBus) lighting goes through. Its official signed setup travels in this exe ("PAWNIO") and is
// run silently when PawnIO is missing, so the memory lights up at once. 1: PawnIO is there.
static int pawnio_there(void) {
    wchar_t p[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\PawnIO\\PawnIOLib.dll", p, MAX_PATH);
    return exists(p);
}

// a setup carried in this exe (RCDATA res): written as name to a folder only admins can open, run with args, deleted
static void run_res(const wchar_t *res, const wchar_t *name, const wchar_t *args, DWORD ms) {
    HRSRC r = FindResourceW(NULL, res, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return;
    wchar_t d[MAX_PATH], f[MAX_PATH];
    if (!admin_tmp(L"haku-control-setup", d)) return;
    swprintf(f, MAX_PATH, L"%s\\%s", d, name);
    HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(d); return; }
    DWORD size = SizeofResource(NULL, r), w = 0;
    BOOL ok = WriteFile(h, LockResource(LoadResource(NULL, r)), size, &w, NULL) && w == size;
    CloseHandle(h);
    if (ok) run_wait(f, args, ms);
    DeleteFileW(f); RemoveDirectoryW(d);
}

static int pawnio_install(void) {
    if (!pawnio_there()) run_res(L"PAWNIO", L"PawnIO_setup.exe", L"-install -silent", 120000);
    return pawnio_there();
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
    wchar_t d[MAX_PATH], tmp[MAX_PATH];   // (the task runs what this file says with admin rights)
    if (!admin_tmp(L"haku-control-setup", d)) return 0;
    swprintf(tmp, MAX_PATH, L"%s\\haku-control-task.xml", d);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) { RemoveDirectoryW(d); return 0; }
    fwrite("\xFF\xFE", 1, 2, f); fwrite(xml, sizeof(wchar_t), wcslen(xml), f); fclose(f);
    wchar_t args[MAX_PATH + 64]; swprintf(args, MAX_PATH + 64, L"/Create /TN haku-control /XML \"%s\" /F", tmp);
    int rc = run_wait(L"schtasks.exe", args, 20000);
    DeleteFileW(tmp); RemoveDirectoryW(d);
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

// The settings window's runtime. Microsoft's small signed bootstrapper travels in this exe ("WEBVIEW2"); it downloads
// the Evergreen runtime and installs it silently for every user. Offline it fails, and the "done" message says so.
static void webview2_install(void) {
    if (!webview2_installed()) run_res(L"WEBVIEW2", L"MicrosoftEdgeWebview2Setup.exe", L"/silent /install", 300000);
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

// three answers: the first custom button (id 100), the second (101), or Cancel (IDCANCEL)
static int choose(const wchar_t *main, const wchar_t *text, const wchar_t *b1, const wchar_t *b2) {
    TASKDIALOG_BUTTON b[] = { { 100, b1 }, { 101, b2 } };
    TASKDIALOGCONFIG c = { sizeof(c) };
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = APP_TITLE L" " HAKU_VER_WSTR;
    c.pszMainIcon = TD_WARNING_ICON; c.pszMainInstruction = main; c.pszContent = text;
    c.cButtons = b2 ? 2 : 1; c.pButtons = b; c.nDefaultButton = 100;
    int btn = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &btn, NULL, NULL))) return IDCANCEL;
    return btn;
}

// what went wrong, in words, with the file; rolled: swap_in()'s answer (0: nothing changed, -1: not put back)
static void show_fail(int rolled) {
    wchar_t why[160], text[MAX_PATH * 2 + 512];
    switch (fail_err) {
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL:
        wcscpy_s(why, 160, TR(L"Not enough free space on the disk.", L"Недостаточно места на диске.", L"Espace disque insuffisant.")); break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION:
        wcscpy_s(why, 160, TR(L"A file or the folder is in use by another program, or access to it is denied.",
                              L"Файл или папка заняты другой программой, или к ним нет доступа.", L"Un fichier ou le dossier est utilisé par un autre programme, ou son accès est refusé.")); break;
    case ERROR_BAD_FORMAT:
        wcscpy_s(why, 160, TR(L"This setup file is damaged. Download it again.", L"Файл установки повреждён. Скачайте его заново.", L"Ce fichier d'installation est endommagé. Téléchargez-le à nouveau.")); break;
    default:
        swprintf(why, 160, TR(L"Windows error %lu.", L"Ошибка Windows %lu.", L"Erreur Windows %lu."), fail_err);
    }
    swprintf(text, MAX_PATH * 2 + 512, L"%s\n%s\n\n%s", why, fail_path,
             rolled < 0 ? TR(L"The previous version could not be put back yet. Run this setup again to repair it; your settings are not affected.",
                             L"Предыдущую версию пока не удалось вернуть. Запустите установку ещё раз, чтобы исправить это; настройки не затронуты.", L"La version précédente n'a pas encore pu être remise en place. Relancez cette installation pour la réparer ; vos réglages ne sont pas touchés.")
                        : TR(L"The installed version was not changed, and your settings are not affected.",
                             L"Установленная версия не изменилась, настройки не затронуты.", L"La version installée n'a pas changé et vos réglages ne sont pas touchés."));
    info(TR(L"Installation failed", L"Установка не удалась", L"L'installation a échoué"), text, TD_ERROR_ICON);
}

// Asks a running copy to quit (it gives the fans back and lets go of the lights the way the user chose) and waits.
// Nothing is closed by force unless the user says so: a forced close cannot give the fans back. 1: no copy runs
// any more, *fans_left: one said it could not confirm the fans were given back (or was closed by force); 0: the
// user chose to cancel (the copies still run)
static int stop_app(int *fans_left) {
    static const wchar_t *cls[][3] = { { L"haku-control", L"haku_control_quit", L"haku-control.exe" }, { L"rgbfx", L"rgbfx_quit", L"rgbfx.exe" } };
    HANDLE h[MAXIMUM_WAIT_OBJECTS]; DWORD n = 0;
    *fans_left = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { sizeof(pe) };
    for (BOOL ok = Process32FirstW(snap, &pe); ok && n < MAXIMUM_WAIT_OBJECTS; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, cls[0][2]) || !_wcsicmp(pe.szExeFile, cls[1][2])) {
            HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (p) h[n++] = p;
        }
    CloseHandle(snap);
    for (int i = 0; i < 2; i++) {
        HWND w = FindWindowW(cls[i][0], cls[i][0]);
        if (w) PostMessageW(w, RegisterWindowMessageW(cls[i][1]), 0, 0);
    }
    int forced = 0;
    // the app saves the settings, then gives the fans back before the slow network devices: well under 30 s
    while (n && WaitForMultipleObjects(n, h, TRUE, 30000) == WAIT_TIMEOUT) {
        int c = choose(TR(L"haku control is still closing", L"haku control ещё закрывается", L"haku control est encore en train de se fermer"),
                       TR(L"It may still be giving the fans and lights back, or it may be waiting for an answer in its window.\n\n"
                          L"If it is closed now, the fans stay at the speed they have until haku control starts again (it gives them back then) "
                          L"or the PC restarts.",
                          L"Возможно, она ещё возвращает управление вентиляторами и светом или ждёт ответа в своём окне.\n\n"
                          L"Если закрыть её сейчас, вентиляторы останутся на текущих оборотах, пока haku control не запустится снова "
                          L"(тогда она вернёт их) или пока ПК не перезагрузится.", L"Il rend peut-être encore les ventilateurs et les lumières, ou il attend une réponse dans sa fenêtre.\n\nS'il est fermé maintenant, les ventilateurs gardent leur vitesse jusqu'au prochain démarrage de haku control (il les rend alors) ou au redémarrage du PC."),
                       TR(L"Wait more", L"Подождать ещё", L"Attendre encore"), TR(L"Close it now", L"Закрыть сейчас", L"Fermer maintenant"));
        if (c == 100) continue;
        if (c != 101) { for (DWORD i = 0; i < n; i++) CloseHandle(h[i]); return 0; }
        for (DWORD i = 0; i < n; i++) TerminateProcess(h[i], 1);
        WaitForMultipleObjects(n, h, TRUE, 5000);
        forced = 1;
        break;
    }
    for (DWORD i = 0; i < n; i++) {
        DWORD code = 0;
        if (GetExitCodeProcess(h[i], &code) && code == APP_FANS_LEFT) *fans_left = 1;
        CloseHandle(h[i]);
    }
    if (forced) *fans_left = 1;
    // the logon task may still count the old one as running, and it ignores a new start while it does
    run_wait(L"schtasks.exe", L"/End /TN haku-control", 10000);
    Sleep(300);
    return 1;
}

// starts the installed program the way it runs at sign-in (through the task) when task, else directly. The task
// answers "done" even when it ignores the start (it still counted the old one as running), so it is checked that
// the program is really there, and started directly when it is not.
static void start_app(int task) {
    wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%s\\haku-control.exe", dest);
    int started = 0;
    if (task && run_wait(L"schtasks.exe", L"/Run /TN haku-control", 10000) == 0)
        for (int t = 0; t < 80 && !(started = process_running(L"haku-control.exe")); t++) Sleep(100);
    if (!started) {
        STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dest, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
}

// ---------------------------------------------------------------- install / uninstall
// quiet: started by the app itself for an update (/update): no first question and no "done" message, autostart as it
// was; a problem is still shown, and the app is still never closed by force without asking
static int install(int quiet, int quiet_autostart) {
    wchar_t exe[MAX_PATH]; swprintf(exe, MAX_PATH, L"%s\\haku-control.exe", dest);
    int update = exists(exe);
    BOOL autostart = quiet ? quiet_autostart : TRUE;
    wchar_t footer[512] = L"";
    if (!pawnio_there())
        wcscpy_s(footer, 512, TR(L"Also installs <a href=\"https://pawnio.eu\">PawnIO</a>, the free driver memory (RAM) lighting needs.",
                                 L"Также установится <a href=\"https://pawnio.eu\">PawnIO</a> — бесплатный драйвер, нужный для подсветки памяти.", L"Installe aussi <a href=\"https://pawnio.eu\">PawnIO</a>, le pilote gratuit nécessaire à l'éclairage de la mémoire (RAM)."));
    if (!webview2_installed())
        wcscat_s(footer, 512, TR(L" Microsoft's WebView2 Runtime for the settings window is downloaded and installed too.",
                                 L" Также скачается и установится WebView2 Runtime от Microsoft для окна настроек.", L" Le WebView2 Runtime de Microsoft, pour la fenêtre des réglages, est aussi téléchargé et installé."));
    if (!quiet && !ask(update ? TR(L"Update haku control", L"Обновить haku control", L"Mettre à jour haku control") : TR(L"Install haku control", L"Установить haku control", L"Installer haku control"),
             TR(L"One lighting scene for your PC and your room.\n\nInstalls to Program Files and starts at sign-in with administrator rights "
                L"(needed for the motherboard and memory lighting). Your settings stay in %APPDATA%\\haku-control.",
                L"Одна сцена света для ПК и комнаты.\n\nУстанавливается в Program Files и запускается при входе с правами администратора "
                L"(нужны для подсветки платы и памяти). Настройки хранятся в %APPDATA%\\haku-control.", L"Une seule scène lumineuse pour votre PC et votre pièce.\n\nS'installe dans Program Files et démarre à l'ouverture de session avec les droits d'administrateur (nécessaires pour l'éclairage de la carte mère et de la mémoire). Vos réglages restent dans %APPDATA%\\haku-control."),
             update ? TR(L"Update", L"Обновить", L"Mettre à jour") : TR(L"Install", L"Установить", L"Installer"),
             TR(L"Start with Windows", L"Запускать вместе с Windows", L"Démarrer avec Windows"), &autostart, footer[0] ? footer : NULL))
        return 1;

    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    DWORD len = 0, bytes = 0;
    const BYTE *pkg = payload(&len);
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    // a run cut off earlier is put right first; then the new files are written and checked while the app still runs
    if (!recover()) { SetCursor(old); show_fail(-1); return 2; }
    update = exists(exe);
    if (!stage(pkg, len, self, &bytes)) { SetCursor(old); show_fail(0); return 2; }

    int fans_left = 0;
    if (!stop_app(&fans_left)) { delete_tree(dest_new); SetCursor(old); return 1; }
    if (fans_left && update &&
        choose(TR(L"The fans may not have been given back", L"Вентиляторы могли не вернуться под управление платы", L"Les ventilateurs n'ont peut-être pas été rendus"),
               TR(L"haku control closed without confirming that the fans went back to the motherboard; they keep the speed they had. "
                  L"The new version gives them back as soon as it starts.\n\nCancel starts the installed version again, which gives them back too.",
                  L"haku control закрылась, не подтвердив, что вентиляторы вернулись под управление материнской платы; они остались на прежних оборотах. "
                  L"Новая версия вернёт их сразу после запуска.\n\n«Отмена» снова запустит установленную версию — она тоже их вернёт.", L"haku control s'est fermé sans confirmer que les ventilateurs sont revenus à la carte mère ; ils gardent leur vitesse. La nouvelle version les rend dès son démarrage.\n\nAnnuler relance la version installée, qui les rend aussi."),
               update ? TR(L"Update anyway", L"Всё равно обновить", L"Mettre à jour quand même") : TR(L"Install anyway", L"Всё равно установить", L"Installer quand même"), NULL) != 100) {
        delete_tree(dest_new); start_app(0); SetCursor(old); return 1;
    }
    SHCreateDirectoryExW(NULL, data_dir, NULL);
    migrate();
    int sw = swap_in();
    if (sw != 1) {
        delete_tree(dest_new);
        if (sw == 0 && update) start_app(0);   // the previous version, as it was
        SetCursor(old); show_fail(sw);
        return 2;
    }
    delete_tree(dest_old);   // if something still holds it, recover() deletes it on the next run
    // before the app starts, so it finds the memory and opens its window; on an update the app started itself too,
    // so nothing has to be installed by hand
    pawnio_install();
    webview2_install();
    int task = register_task(autostart);
    make_shortcut();
    firewall_rule(1);
    register_uninstall(bytes);
    SetCursor(old);

    start_app(task && autostart);
    // its window opens (with what is new), as when it is opened from the Start menu; after an update the app
    // started itself too, so it is plain that the new version is there
    if (webview2_installed()) {
        HWND h = NULL;
        for (int t = 0; t < 100 && !(h = FindWindowW(L"haku-control", L"haku-control")); t++) Sleep(100);
        if (h) {
            Sleep(500);   // let it finish starting
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(h, RegisterWindowMessageW(L"haku_control_show"), 0, 0);
            return 0;
        }
    }
    if (quiet) return 0;   // no "done" message after an update the app started itself
    info(update ? TR(L"haku control is updated", L"haku control обновлён", L"haku control est à jour") : TR(L"haku control is installed", L"haku control установлен", L"haku control est installé"),
         webview2_installed()
             ? TR(L"It is running in the notification area (tray). Click its icon to open the window; it is also in the Start menu.",
                  L"Программа работает в области уведомлений (трее). Нажмите на значок, чтобы открыть окно; она также есть в меню «Пуск».", L"Il fonctionne dans la zone de notification. Cliquez sur son icône pour ouvrir la fenêtre ; il est aussi dans le menu Démarrer.")
             : TR(L"The settings window needs the Microsoft Edge <a href=\"https://developer.microsoft.com/microsoft-edge/webview2/\">WebView2 Runtime</a>. The lights already work.",
                  L"Для окна настроек нужен <a href=\"https://developer.microsoft.com/microsoft-edge/webview2/\">WebView2 Runtime</a> от Microsoft. Свет уже работает.", L"La fenêtre des réglages a besoin du <a href=\"https://developer.microsoft.com/microsoft-edge/webview2/\">WebView2 Runtime</a> de Microsoft Edge. Les lumières fonctionnent déjà."),
         TD_INFORMATION_ICON);
    return 0;
}

static int uninstall(void) {
    BOOL purge = FALSE;
    if (!ask(TR(L"Remove haku control?", L"Удалить haku control?", L"Supprimer haku control ?"),
             TR(L"The app is closed first, so the lights do what you chose for exit (off by default).",
                L"Сначала программа закроется, и свет сделает то, что выбрано для выхода (по умолчанию погаснет).", L"L'application est d'abord fermée ; les lumières font ce que vous avez choisi pour la fermeture (éteintes par défaut)."),
             TR(L"Remove", L"Удалить", L"Supprimer"), TR(L"Also delete settings and device keys", L"Удалить также настройки и ключи устройств", L"Supprimer aussi les réglages et les clés des appareils"), &purge, NULL))
        return 1;
    int fans_left = 0;
    if (!stop_app(&fans_left)) return 1;
    run_wait(L"schtasks.exe", L"/Delete /TN haku-control /F", 10000);
    wchar_t lnk[MAX_PATH]; shortcut_path(lnk); DeleteFileW(lnk);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, UNINST_KEY);
    firewall_rule(0);
    if (purge) {
        delete_tree(data_dir);
        wchar_t l[MAX_PATH]; ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\haku-control", l, MAX_PATH); delete_tree(l);
    }
    delete_tree(dest_new); delete_tree(dest_old);
    wchar_t orgb[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\haku-control-openrgb", orgb, MAX_PATH); delete_tree(orgb);   // the OpenRGB haku set up
    // this exe lives in the folder: remove the folder once we have exited
    wchar_t cmd[MAX_PATH * 2 + 128], sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, MAX_PATH * 2 + 128, L"\"%s\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%s\"", sys, dest);
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    wchar_t text[512];
    swprintf(text, 512, L"%s%s", purge ? L"" : TR(L"Your settings are kept in %APPDATA%\\haku-control.", L"Настройки сохранены в %APPDATA%\\haku-control.", L"Vos réglages sont conservés dans %APPDATA%\\haku-control."),
             fans_left ? TR(L"\n\nThe fans may still be at the speed haku control set: restart the PC to give them back to the motherboard.",
                            L"\n\nВентиляторы могут остаться на оборотах, заданных haku control: перезагрузите ПК, чтобы вернуть их материнской плате.", L"\n\nLes ventilateurs sont peut-être encore à la vitesse réglée par haku control : redémarrez le PC pour les rendre à la carte mère.") : L"");
    info(TR(L"haku control was removed", L"haku control удалён", L"haku control a été supprimé"), text, fans_left ? TD_WARNING_ICON : TD_INFORMATION_ICON);
    return 0;
}

static void set_paths(const wchar_t *program) {
    wcscpy_s(dest, MAX_PATH, program);
    swprintf(dest_new, MAX_PATH, L"%s.new", dest);
    swprintf(dest_old, MAX_PATH, L"%s.old", dest);
    // the working folder comes from whoever started this: the app's update starts it from the program folder
    // (its shortcut's "Start in"), and a process's working folder cannot be renamed (sharing violation)
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); SetCurrentDirectoryW(sys);
}

#ifndef SETUP_TEST
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)show;
    int l = PRIMARYLANGID(GetUserDefaultUILanguage());
    lang = l == LANG_RUSSIAN ? 1 : l == LANG_FRENCH ? 2 : 0;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);
    wchar_t program[MAX_PATH]; ExpandEnvironmentStringsW(L"%ProgramFiles%\\haku-control", program, MAX_PATH);
    set_paths(program);
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

#else
// ---------------------------------------------------------------- self-test (tools\sim\run-tests.cmd)
// Runs the file part of an install (recover, stage, swap) on folders under %TEMP%, with a write failing, a file held
// open, too little space and every point a run can be cut off at. No admin rights, no Program Files, no app.
static int bad;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s (error %lu)\n", __LINE__, m, fail_err); bad = 1; } } while (0)
static BYTE pkg[4096];
static DWORD pkg_len;
static wchar_t base[MAX_PATH], settings[MAX_PATH];

static void add(BYTE *b, DWORD *n, const char *name, const char *data) {
    unsigned short nl = (unsigned short)strlen(name); unsigned int sz = (unsigned int)strlen(data);
    memcpy(b + *n, &nl, 2); *n += 2; memcpy(b + *n, name, nl); *n += nl;
    memcpy(b + *n, &sz, 4); *n += 4; memcpy(b + *n, data, sz); *n += sz;
}
static void put(const wchar_t *dir, const wchar_t *name, const char *s) {
    wchar_t f[MAX_PATH * 2]; swprintf(f, MAX_PATH * 2, L"%s\\%s", dir, name);
    wchar_t d[MAX_PATH * 2]; wcscpy_s(d, MAX_PATH * 2, f); *wcsrchr(d, L'\\') = 0; SHCreateDirectoryExW(NULL, d, NULL);
    FILE *o = _wfopen(f, L"wb"); if (o) { fputs(s, o); fclose(o); }
}
static int has(const wchar_t *dir, const wchar_t *name, const char *s) {
    wchar_t f[MAX_PATH * 2]; swprintf(f, MAX_PATH * 2, L"%s\\%s", dir, name);
    char buf[64] = { 0 }; FILE *i = _wfopen(f, L"rb"); if (!i) return 0;
    fread(buf, 1, 63, i); fclose(i);
    return !strcmp(buf, s);
}
static int file_in(const wchar_t *dir, const wchar_t *name) { wchar_t f[MAX_PATH * 2]; swprintf(f, MAX_PATH * 2, L"%s\\%s", dir, name); return exists(f); }
// the installed version: an older exe, a page, and a file the new version no longer has
static void installed(void) {
    delete_tree(dest); delete_tree(dest_new); delete_tree(dest_old);
    put(dest, L"haku-control.exe", "old exe"); put(dest, L"ui\\app.js", "old ui"); put(dest, L"gone.txt", "only in the old");
}
static int is_old(void) { return has(dest, L"haku-control.exe", "old exe") && has(dest, L"ui\\app.js", "old ui") && file_in(dest, L"gone.txt"); }
static int is_new(void) { return has(dest, L"haku-control.exe", "new exe") && has(dest, L"ui\\app.js", "new ui") && !file_in(dest, L"gone.txt"); }
static int clean(void) { return !exists(dest_new) && !exists(dest_old); }
static HANDLE hold(const wchar_t *dir, const wchar_t *name) {
    wchar_t f[MAX_PATH * 2]; swprintf(f, MAX_PATH * 2, L"%s\\%s", dir, name);
    return CreateFileW(f, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);   // no sharing: as a running exe or a scanner
}
static int update_files(void) {   // what install() does to the files, without the dialogs and the app
    DWORD bytes;
    if (!recover() || !stage(pkg, pkg_len, NULL, &bytes)) return 0;
    int sw = swap_in();
    if (sw != 1) { delete_tree(dest_new); return sw; }
    delete_tree(dest_old);
    return 1;
}

int wmain(void) {
    GetTempPathW(MAX_PATH, base);
    swprintf(base + wcslen(base), MAX_PATH - wcslen(base), L"haku-setup-test-%lu", GetCurrentProcessId());
    wchar_t program[MAX_PATH]; swprintf(program, MAX_PATH, L"%s\\haku-control", base);
    set_paths(program);
    swprintf(data_dir, MAX_PATH, L"%s\\data", base);
    swprintf(settings, MAX_PATH, L"%s\\data", base);
    put(data_dir, L"settings.ini", "[general]\r\nmine=1\r\n");
    move_tries = 3;
    memcpy(pkg, "HAKUPKG1", 8); pkg_len = 8;
    add(pkg, &pkg_len, "haku-control.exe", "new exe");
    add(pkg, &pkg_len, "ui\\app.js", "new ui");
    DWORD bytes;

    // a damaged package is refused whole, before anything is written over
    BYTE b[256]; DWORD n;
    memcpy(b, pkg, pkg_len); b[pkg_len] = 0; wchar_t t[MAX_PATH]; swprintf(t, MAX_PATH, L"%s\\x", base);
    CHECK(unpack(pkg, pkg_len, t, &bytes) && bytes == 13, "a good package was refused");
    CHECK(!unpack(b, pkg_len + 1, t, &bytes) && fail_err == ERROR_BAD_FORMAT, "a trailing byte was taken");
    CHECK(!unpack(pkg, pkg_len - 1, t, &bytes) && fail_err == ERROR_BAD_FORMAT, "a cut-off package was taken");
    n = 8; memcpy(b, "HAKUPKG1", 8); add(b, &n, "ui\\app.js", "x");
    CHECK(!unpack(b, n, t, &bytes) && fail_err == ERROR_BAD_FORMAT, "a package without the program was taken");
    n = 8; add(b, &n, "haku-control.exe", "x"); add(b, &n, "..\\evil.txt", "x");
    CHECK(!unpack(b, n, t, &bytes), "a name leaving the folder was taken");
    n = 8; add(b, &n, "C:\\evil.txt", "x"); add(b, &n, "haku-control.exe", "x");
    CHECK(!unpack(b, n, t, &bytes), "an absolute name was taken");
    delete_tree(t);

    // a clean install
    delete_tree(dest);
    CHECK(update_files() == 1 && has(dest, L"haku-control.exe", "new exe") && clean(), "clean install");
    // an update replaces the whole folder: a file the new version dropped is gone, the settings stay
    installed();
    CHECK(update_files() == 1 && is_new() && clean(), "update");

    // one write fails while staging: the installed version is untouched and nothing is left over
    installed(); t_fail_write = 1;
    CHECK(!stage(pkg, pkg_len, NULL, &bytes) && fail_err == ERROR_DISK_FULL && wcsstr(fail_path, L"app.js"), "a failed write was not reported");
    CHECK(is_old() && clean(), "a failed write touched the installed version");
    t_fail_write = -1;
    // too little space: refused before writing
    t_free = 1024;
    CHECK(!stage(pkg, pkg_len, NULL, &bytes) && fail_err == ERROR_DISK_FULL && is_old() && clean(), "too little space");
    t_free = 0;

    // a file of the installed version held open (a running exe, a scanner): the swap fails, nothing changes,
    // then the same update again succeeds once it is let go
    installed();
    HANDLE f = hold(dest, L"ui\\app.js");
    CHECK(f != INVALID_HANDLE_VALUE, "could not hold the file");
    int sw = update_files();
    CHECK(fail_err == ERROR_ACCESS_DENIED || fail_err == ERROR_SHARING_VIOLATION, "a file in use is not said as such");
    CloseHandle(f);   // before reading it back
    CHECK(sw == 0 && is_old() && clean(), "a file in use: the previous version is not intact");
    CHECK(update_files() == 1 && is_new() && clean(), "running again after a failure");
    // started from inside the program folder (the app's update, its shortcut's "Start in"): the swap still works
    installed();
    CHECK(SetCurrentDirectoryW(dest), "could not enter the program folder");
    set_paths(dest);
    CHECK(update_files() == 1 && is_new() && clean(), "started from inside the program folder");
    // the second rename fails (the new folder held): the previous version is put back
    installed();
    CHECK(recover() && stage(pkg, pkg_len, NULL, &bytes), "staging");
    f = hold(dest_new, L"ui\\app.js");
    sw = swap_in();
    CloseHandle(f);
    CHECK(sw == 0 && is_old() && !exists(dest_old), "a failed second rename: the previous version is not back");
    delete_tree(dest_new);

    // cut off while staging: the half-written .new is dropped, the installed version stays
    installed(); put(dest_new, L"haku-control.exe", "half");
    CHECK(recover() && is_old() && clean(), "cut off while staging");
    // cut off between the two renames: only .old and .new are there; the previous version goes back
    installed(); CHECK(stage(pkg, pkg_len, NULL, &bytes), "staging"); MoveFileExW(dest, dest_old, 0);
    CHECK(!exists(dest) && recover() && is_old() && clean(), "cut off between the renames");
    // cut off after the swap: the new version stays, the old copy goes
    installed(); CHECK(stage(pkg, pkg_len, NULL, &bytes), "staging"); CHECK(swap_in() == 1, "swap");
    CHECK(recover() && is_new() && clean(), "cut off after the swap");
    // and the next run goes on as normal
    CHECK(update_files() == 1 && is_new() && clean(), "an update after a recovered run");

    CHECK(has(settings, L"settings.ini", "[general]\r\nmine=1\r\n"), "the settings were touched");
    delete_tree(base);
    printf(bad ? "setup: SOME CHECKS FAILED\n" : "setup: all checks passed\n");
    return bad;
}
#endif
