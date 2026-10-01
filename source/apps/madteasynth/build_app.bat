@echo off
setlocal

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
call "%SCRIPT_DIR%find_cmake.bat"
if errorlevel 1 (
    pause
    exit /b 1
)

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configuring project...
    "%CMAKE%" -DIPLUG2_DIR="%~dp0iplug2" -B "%BUILD_DIR%" -S "%SCRIPT_DIR%"
    if errorlevel 1 (
        echo Configuration failed.
        pause
        exit /b 1
    )
)

echo Building APP target (Release)...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target madteasynth-app
if errorlevel 1 (
    echo Build failed.
    pause
    exit /b 1
)

echo.
echo Done! Standalone EXE at:
echo   %BUILD_DIR%\out\madteasynth.exe
echo.
pause
