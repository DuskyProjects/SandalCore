@echo off
REM SPDX-License-Identifier: AGPL-3.0-only
REM Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
REM GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

setlocal

:: ============================================================
::  build_eossdk_proxy.bat — build + deploy EOSSDK stub DLL
::
::  Compiles src/eossdk_proxy.cpp into EOSSDK-Win64-Shipping.dll
::  (a stub replacement for the real Epic Online Services SDK)
::  and deploys it into the game's Win64 directory, renaming
::  the real DLL to EOSSDK_real.dll.
::
::  Override the game directory if yours differs:
::      set GAMEDIR=<AOC_CLIENT_DIR>\AOC\Binaries\Win64
::      scripts\build_eossdk_proxy.bat
:: ============================================================

:: ── Paths derived from script location ──────────────────────
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
set "SRC=%REPO_ROOT%\src\eossdk_proxy.cpp"
set "BUILD_DIR=%REPO_ROOT%\build\eossdk_proxy"
set "OUT=%BUILD_DIR%\EOSSDK-Win64-Shipping.dll"

:: ── Game directory: override via env, else default ──────────
if not defined GAMEDIR (
    if exist "C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game\AOC\Binaries\Win64\EOSSDK-Win64-Shipping.dll" (
        set "GAMEDIR=C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game\AOC\Binaries\Win64"
    ) else if exist "C:\Ashes of Creation\Game\AOC\Binaries\Win64\EOSSDK-Win64-Shipping.dll" (
        set "GAMEDIR=C:\Ashes of Creation\Game\AOC\Binaries\Win64"
    ) else (
        set "GAMEDIR=C:\Ashes of Creation\Game\AOC\Binaries\Win64"
    )
)

if not exist "%SRC%" (
    echo ERROR: source file not found: %SRC%
    pause
    exit /b 1
)

:: ── Find vcvars64.bat via vswhere ───────────────────────────
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Install Visual Studio 2022.
    pause
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo ERROR: Visual Studio with C++ toolset not found.
    pause
    exit /b 1
)
set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

echo [1/3] Setting up MSVC x64 environment ...
call "%VCVARS%" >nul 2>&1

echo [2/3] Compiling proxy DLL to %OUT% ...
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"
cl /LD /O2 /nologo "%SRC%" /link /OUT:"%OUT%" /MACHINE:X64 /NOLOGO
if errorlevel 1 (
    echo COMPILE FAILED
    pause
    exit /b 1
)
echo Compiled OK: %OUT%

echo [3/3] Deploying to %GAMEDIR% ...
set "REAL=%GAMEDIR%\EOSSDK_real.dll"
set "PROXY=%GAMEDIR%\EOSSDK-Win64-Shipping.dll"

if not exist "%GAMEDIR%" (
    echo ERROR: Game directory not found: %GAMEDIR%
    echo Set GAMEDIR env var to override.
    pause
    exit /b 1
)

if not exist "%REAL%" (
    echo   Renaming original DLL to EOSSDK_real.dll ...
    if not exist "%PROXY%" (
        echo ERROR: Original EOSSDK DLL not found:
        echo   "%PROXY%"
        pause
        exit /b 1
    )
    ren "%GAMEDIR%\EOSSDK-Win64-Shipping.dll" EOSSDK_real.dll
    if errorlevel 1 (
        echo ERROR: Could not rename - is the game running? Close it first.
        pause
        exit /b 1
    )
) else (
    echo   EOSSDK_real.dll already exists, skipping rename.
)

echo   Copying proxy DLL ...
copy /y "%OUT%" "%PROXY%"
if errorlevel 1 (
    echo ERROR: Could not copy proxy DLL.
    pause
    exit /b 1
)

echo.
echo SUCCESS - Proxy deployed.
echo   Real DLL : "%REAL%"
echo   Proxy DLL: "%PROXY%"
echo.
echo To REVERT: delete EOSSDK-Win64-Shipping.dll, rename EOSSDK_real.dll back.
echo.
pause
endlocal
