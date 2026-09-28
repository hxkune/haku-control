@echo off
rem Builds haku control with MSVC (Visual Studio 2019 or newer, "Desktop development with C++").
rem   build.cmd       release build into .\bin
rem   build.cmd dev   test build into .\bin-dev: own settings folder (%APPDATA%\haku-control-dev), runs without
rem                   admin rights and never touches the motherboard / memory, so it can run next to the real app.
rem Run from a plain cmd; the VS environment is found with vswhere.
setlocal
cd /d "%~dp0"
set "OUT=bin" & set "OBJ=obj" & set "DEFS=" & set "UAC=requireAdministrator"
if /i "%~1"=="dev" set "OUT=bin-dev" & set "OBJ=obj-dev" & set "DEFS=/DHAKU_DEV" & set "UAC=asInvoker"
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
if not exist %OUT% mkdir %OUT%
if not exist %OBJ% mkdir %OBJ%
rem icons + version info
rc /nologo /c65001 /fo %OBJ%\haku-control.res res\haku-control.rc || exit /b 1
cl /nologo /utf-8 /O2 /GS /W3 /MT /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE %DEFS% /std:c++17 /EHsc /Fo%OBJ%\ ^
   src\main.c src\config.c src\effects.c src\dev_msi.c src\dev_ene.c src\sensors.c src\dev_aidot.c src\dev_nanoleaf.c src\net.c ^
   src\netutil.c src\devices.c src\drv_wled.c src\drv_openrgb.c src\drv_govee.c src\drv_lifx.c src\drv_yeelight.c src\drv_hue.c ^
   src\ui_web.cpp src\audio.cpp ^
   /Fe:%OUT%\haku-control.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='%UAC%' uiAccess='false'" ^
   user32.lib shell32.lib gdi32.lib setupapi.lib hid.lib winmm.lib comctl32.lib comdlg32.lib ws2_32.lib bcrypt.lib iphlpapi.lib ^
   advapi32.lib ole32.lib third_party\webview2\WebView2LoaderStatic.lib %OBJ%\haku-control.res || exit /b 1
rem helper that switches the Windows Mobile Hotspot on (WinRT), started only when needed
cl /nologo /utf-8 /O2 /GS /W3 /MT /std:c++17 /EHsc /Fo%OBJ%\ src\hotspot.cpp /Fe:%OUT%\haku-control-hotspot.exe ^
   /link /SUBSYSTEM:CONSOLE || exit /b 1
rem the settings page (HTML/CSS/JS), loaded by the window from .\ui
if exist %OUT%\ui rmdir /s /q %OUT%\ui
xcopy /e /i /y /q ui %OUT%\ui >nul
del /q %OUT%\ui\mock.* 2>nul
rem PawnIO SMBus module (LGPL-2.1, see third_party\pawnio)
copy /y third_party\pawnio\SmbusPIIX4.bin %OUT%\ >nul
echo OK
