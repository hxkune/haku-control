@echo off
rem Builds haku control with MSVC (Visual Studio 2019 or newer, "Desktop development with C++").
rem Output goes to .\bin. Run from a plain cmd; the VS environment is found with vswhere.
setlocal
cd /d "%~dp0"
if defined VCINSTALLDIR goto have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
rem fallback: standard install folders (vswhere does not list every Build Tools install)
if not defined VSDIR for %%y in (2022 2019) do for %%e in (BuildTools Community Professional Enterprise) do (
  if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\%%y\%%e"
  if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e"
)
if not defined VSDIR echo Visual Studio with the C++ tools not found& exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs
if not exist bin mkdir bin
if not exist obj mkdir obj
rem icons + version info
rc /nologo /c65001 /fo obj\haku-control.res res\haku-control.rc || exit /b 1
cl /nologo /utf-8 /O2 /GS /W3 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /std:c++17 /EHsc /Foobj\ ^
   src\main.c src\config.c src\effects.c src\dev_msi.c src\dev_ene.c src\sensors.c src\dev_aidot.c src\dev_nanoleaf.c src\net.c ^
   src\ui_web.cpp ^
   /Fe:bin\haku-control.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   user32.lib shell32.lib gdi32.lib setupapi.lib hid.lib winmm.lib comctl32.lib comdlg32.lib ws2_32.lib bcrypt.lib iphlpapi.lib ^
   advapi32.lib third_party\webview2\WebView2LoaderStatic.lib obj\haku-control.res || exit /b 1
rem helper that switches the Windows Mobile Hotspot on (WinRT), started only when needed
cl /nologo /utf-8 /O2 /GS /W3 /MT /std:c++17 /EHsc /Foobj\ src\hotspot.cpp /Fe:bin\haku-control-hotspot.exe ^
   /link /SUBSYSTEM:CONSOLE || exit /b 1
rem the settings page (HTML/CSS/JS), loaded by the window from .\ui
if exist bin\ui rmdir /s /q bin\ui
xcopy /e /i /y /q ui bin\ui >nul
del /q bin\ui\mock.* 2>nul
rem PawnIO SMBus module (LGPL-2.1, see third_party\pawnio)
copy /y third_party\pawnio\SmbusPIIX4.bin bin\ >nul
echo OK
