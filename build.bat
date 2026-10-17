@echo off
rem LilyInEM build entry: build.bat [clean] [test] [core]
rem   core  = engine-independent core library + unit tests only (fast, no UE4SS)
chcp 65001 >nul
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
if not defined LIE_BUILD_CONFIG set "LIE_BUILD_CONFIG=Game__Shipping__Win64"

set "DO_CLEAN=0"
set "DO_TEST=0"
set "CORE_ONLY=OFF"
for %%A in (%*) do (
    if /I "%%~A"=="clean" set "DO_CLEAN=1"
    if /I "%%~A"=="test" set "DO_TEST=1"
    if /I "%%~A"=="core" set "CORE_ONLY=ON"
)
set "BUILD_DIR=%ROOT%\build"
if "%CORE_ONLY%"=="ON" set "BUILD_DIR=%ROOT%\build\core"

for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\devconfig.ps1" -Get ue4ssSource`) do set "LIE_UE4SS_DIR=%%I"
if not defined LIE_FETCH_CACHE (
    for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\devconfig.ps1" -Get fetchCache`) do set "LIE_FETCH_CACHE=%%I"
)
if defined LIE_FETCH_CACHE set "CARGO_NET_OFFLINE=true"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo [ERROR] vswhere.exe not found && exit /b 1
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%I"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44 >nul
if errorlevel 1 echo [ERROR] vcvars64 failed && exit /b 1

if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"

echo [configure] %BUILD_DIR% (core-only=%CORE_ONLY%)
cmake -G Ninja -DCMAKE_BUILD_TYPE=%LIE_BUILD_CONFIG% -DLIE_CORE_ONLY=%CORE_ONLY% -B "%BUILD_DIR%" -S "%ROOT%" >"%ROOT%\build\configure.log" 2>&1
if errorlevel 1 type "%ROOT%\build\configure.log" && exit /b 1

if not defined LIE_TARGETS (
    if "%CORE_ONLY%"=="ON" (set "LIE_TARGETS=lie_tests") else (set "LIE_TARGETS=LiliesGuest MagnoliaHost lie_tests")
)
echo [build] %LIE_TARGETS%
cmake --build "%BUILD_DIR%" --target %LIE_TARGETS% -j 8
if errorlevel 1 exit /b 1

if "%DO_TEST%"=="1" (
    ctest --test-dir "%BUILD_DIR%" --output-on-failure
    if errorlevel 1 exit /b 1
)
echo [done] %BUILD_DIR%
endlocal
exit /b 0
