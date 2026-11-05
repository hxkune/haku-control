@echo off
rem The checks that need no hardware: the C tests in tools\sim against simulators and fake servers, and the phone
rem page's test in node. Nothing here opens a real device, the motherboard, the memory or a fan.
rem   tools\sim\run-tests.cmd        from a plain cmd (the VS environment is found as build.cmd finds it)
rem Exits with 1 when any check fails; the UI tests in the browser (ui\index.html?mock^&test) are run apart.
setlocal
cd /d "%~dp0\..\.."
if defined VCINSTALLDIR goto have_vs
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "delims=" %%i in ('vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul') do set "VSDIR=%%i"
if not defined VSDIR for %%y in (2022 2019) do for %%e in (BuildTools Community Professional Enterprise) do (
  if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\%%y\%%e"
  if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\%%y\%%e"
)
if not defined VSDIR echo Visual Studio with the C++ tools not found& exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_vs
set "T=%TEMP%\haku-tests" & set "BAD="
if not exist "%T%" mkdir "%T%"
set "CF=/nologo /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Fo:%T%\ /Isrc"

call :one xfade    tools\sim\xfade_test.c
call :one tryx     tools\sim\tryx_test.c
call :one effects  /O2 tools\sim\effects_test.c
call :one profile  /W3 tools\sim\profile_test.c src\profile.c src\config.c
call :one backup   /DHAKU_DEV tools\sim\backup_test.c src\backup.c src\config.c comdlg32.lib
rem the remote check takes only the allowlist from ui_web.cpp; the rest of the program is left unlinked on purpose
call :one remote   /std:c++17 /EHsc tools\sim\remote_test.cpp /link /FORCE:UNRESOLVED
rem the fan, iCUE LINK and Lian Li parts test themselves against simulated devices (no USB or Super I/O is opened)
call :one fans     /DFANS_TEST src\fans.c setupapi.lib hid.lib
call :one link     /DLINK_TEST src\link.c
call :one lianli   /DLL_TEST src\lianli.c
call :one sio      /DSIO_TEST src\sio.c
call :one corsairlcd /DCORSAIR_LCD_TEST src\drv_corsairlcd.c

echo === orgb (against the fake OpenRGB server)
del /q "%T%\orgb_test.exe" 2>nul
cl %CF% tools\sim\orgb_test.c src\netutil.c ws2_32.lib winhttp.lib bcrypt.lib wintrust.lib crypt32.lib /Fe:%T%\orgb_test.exe >"%T%\orgb.log" 2>&1 || (type "%T%\orgb.log" & set "BAD=1" & goto node)
start "fake openrgb" /b python tools\sim\fake_devices.py openrgb --orgb-test --orgb-port=6799
rem the server needs a moment to listen
ping -n 3 127.0.0.1 >nul
"%T%\orgb_test.exe" || set "BAD=1"
taskkill /f /fi "WINDOWTITLE eq fake openrgb*" >nul 2>&1
for /f "tokens=5" %%p in ('netstat -ano ^| findstr ":6799 " ^| findstr LISTENING') do taskkill /f /pid %%p >nul 2>&1

:node
echo === phone page (node)
node ui\mock.remote.test.js || set "BAD=1"

if defined BAD (echo SOME CHECKS FAILED& exit /b 1)
echo ALL CHECKS PASSED
exit /b 0

:one
set "N=%~1"
shift
set "A="
:args
if "%~1"=="" goto run
set "A=%A% %1"
shift
goto args
:run
echo === %N%
del /q "%T%\%N%_test.exe" 2>nul
cl %CF% /Fe:%T%\%N%_test.exe %A% >"%T%\%N%.log" 2>&1
if not exist "%T%\%N%_test.exe" (type "%T%\%N%.log" & set "BAD=1" & exit /b 0)
"%T%\%N%_test.exe" || set "BAD=1"
exit /b 0
