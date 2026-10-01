@echo off
setlocal

cd /d "%~dp0"

:: Filled in by the exporter, exactly as in build.bat -- keep the two the same
:: or this runs a cart build.bat never wrote.
set "PROJECT_NAME={{SLUG}}"

if "%UW8_TOOLS%"=="" set "UW8_TOOLS=%~dp0"

"%UW8_TOOLS%\uw8.exe" run "%PROJECT_NAME%.uw8" --fullscreen
pause

endlocal
