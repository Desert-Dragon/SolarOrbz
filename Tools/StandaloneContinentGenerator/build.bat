@echo off
REM Quick build script for Windows 10. Usage:
REM   build.bat            REM CLI + GUI
REM   build.bat --no-gui   REM CLI only (skips the raylib/raygui fetch)
REM Requires CMake and a C++17 compiler (Visual Studio 2019/2022 with the "Desktop development
REM with C++" workload installed). This window stays open and pauses at the end (success or
REM failure) even when double-clicked from Explorer, so you can actually read the output.
setlocal enabledelayedexpansion

cd /d "%~dp0"

set BUILD_GUI=ON
if /I "%~1"=="--no-gui" set BUILD_GUI=OFF
if not "%~1"=="" if /I not "%~1"=="--no-gui" (
    echo Unknown argument: %~1 ^(expected --no-gui^)
    goto :pause_and_fail
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo CMake was not found on PATH.
    echo Install it from https://cmake.org/download/ - during setup, choose
    echo "Add CMake to the system PATH for all users" - then re-run this script.
    goto :pause_and_fail
)

if not exist build mkdir build
cd build

echo.
echo === Configuring ^(cmake ..^) ===
cmake -DSOLARORBZ_BUILD_GUI=%BUILD_GUI% ..
if errorlevel 1 (
    echo.
    echo Configure failed - see the CMake error above.
    echo The most common cause on Windows: no C++ compiler found. Install Visual Studio 2019/2022
    echo ^(the free Community edition is fine^) with the "Desktop development with C++" workload,
    echo then re-run this script.
    cd ..
    goto :pause_and_fail
)

echo.
echo === Building ^(cmake --build .^) ===
cmake --build . --config Release -j
if errorlevel 1 (
    echo.
    echo Build failed - see the compiler errors above.
    cd ..
    goto :pause_and_fail
)

cd ..
echo.
echo ================================================================
echo Build complete. Binaries ^(exact path depends on your CMake generator -
echo Visual Studio puts them under build\Release\^):
echo   build\solarorbz_cli.exe        (or build\Release\solarorbz_cli.exe)
if /I "%BUILD_GUI%"=="ON" (
    echo   build\solarorbz_gui.exe        (or build\Release\solarorbz_gui.exe)
)
echo ================================================================
pause
exit /b 0

:pause_and_fail
echo.
echo Build did not complete.
pause
exit /b 1
