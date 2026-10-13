@echo off
REM Quick build script for Windows 10. Usage:
REM   build.bat            REM CLI + GUI
REM   build.bat --no-gui   REM CLI only (skips the raylib/raygui fetch)
REM Requires CMake and a C++17 compiler (Visual Studio 2019/2022 "Desktop development with C++"
REM workload) on PATH. Run from a "Developer Command Prompt for VS" if cmake can't find a compiler.
setlocal enabledelayedexpansion

cd /d "%~dp0"

set BUILD_GUI=ON
if /I "%~1"=="--no-gui" set BUILD_GUI=OFF
if not "%~1"=="" if /I not "%~1"=="--no-gui" (
    echo Unknown argument: %~1 ^(expected --no-gui^)
    exit /b 1
)

if not exist build mkdir build
cd build

cmake -DSOLARORBZ_BUILD_GUI=%BUILD_GUI% ..
if errorlevel 1 goto :error

cmake --build . --config Release -j
if errorlevel 1 goto :error

echo.
echo Build complete. Binaries (exact path depends on your CMake generator):
echo   build\solarorbz_cli.exe        (or build\Release\solarorbz_cli.exe for Visual Studio)
if /I "%BUILD_GUI%"=="ON" (
    echo   build\solarorbz_gui.exe        (or build\Release\solarorbz_gui.exe for Visual Studio)
)
goto :eof

:error
echo.
echo Build failed - see errors above.
exit /b 1
