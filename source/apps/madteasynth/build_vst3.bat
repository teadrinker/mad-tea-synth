@echo off
setlocal

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
call "%SCRIPT_DIR%find_cmake.bat"
if errorlevel 1 exit /b 1

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configuring project...
    "%CMAKE%" -DIPLUG2_DIR="%~dp0iplug2" -B "%BUILD_DIR%" -S "%SCRIPT_DIR%"
    if errorlevel 1 (
        echo Configuration failed.
        exit /b 1
    )
)

echo Building VST3 target (Release)...
"%CMAKE%" --build "%BUILD_DIR%" --config Release --target madteasynth-vst3
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo.
echo Done! VST3 plugin written to:
echo   C:\Program Files\Common Files\VST3\madteasynth.vst3
echo.
