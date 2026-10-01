@echo off
setlocal

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build

if exist "%BUILD_DIR%" (
    echo Removing build directory...
    rmdir /s /q "%BUILD_DIR%"
    echo Done - build artifacts cleaned.
) else (
    echo No build directory found.
)

echo.
echo The deployed plugins are kept:
echo   %%LOCALAPPDATA%%\Programs\Common\CLAP\madteasynth.clap
echo   C:\Program Files\Common Files\VST3\madteasynth.vst3
echo.
pause
