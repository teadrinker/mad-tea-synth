@echo off
setlocal

:: build_and_install.bat -- build this exported song and install it to a watch.
::
:: GENERATED PROJECT. Re-exporting over this directory replaces this file.
::
:: Needs the Pebble SDK in WSL (Ubuntu) and the watch reachable from the phone's
:: Developer Connection. The phone's IP is the whole of pebble_phone_ip.txt next
:: to this file -- the export seeds it, and it is the one file here a re-export
:: does not overwrite, so a corrected address stays corrected. Comment the line
:: out with a leading `#`, or delete the file, to build without installing.
::
:: Environment knobs, all optional:
::   SONG_NO_VISUALS=1  audio only -- drops the whole drawing half
::   SONG_NO_LTO=1      A/B a suspected LTO miscompile (will likely bust the cap)
::   SONG_CPU=m33       ARMv8-M ISA instead of the SDK's cortex-m3
::   SONG_DEFINES=...   ad-hoc defines, e.g. PEBBLE_SYNC_DEBUG=1

set PHONE_IP=
if exist "%~dp0pebble_phone_ip.txt" (
    set /p PHONE_IP=<"%~dp0pebble_phone_ip.txt"
)
:: A `#` first character means the line is commented out, so treat it as no IP
:: at all and build without installing. Two details, both load-bearing: this
:: sits OUTSIDE the block above, because %PHONE_IP% inside it would still hold
:: the value from before the `set /p` (a parenthesised block is expanded once,
:: as a whole); and the test is `if defined` rather than
:: `if not "%PHONE_IP%"==""`, because the latter pastes the line's text into the
:: command line, where a `<` in a comment would be read as a redirection before
:: the quotes get a say.
if defined PHONE_IP if "%PHONE_IP:~0,1%"=="#" set PHONE_IP=

:: WSL sees this directory through /mnt/<drive>.
for %%I in ("%~dp0.") do set "PROJ_WIN=%%~fI"
set "PROJ_WSL=%PROJ_WIN::=%"
set "PROJ_WSL=/mnt/%PROJ_WSL:\=/%"
set "PROJ_WSL=%PROJ_WSL:/C/=/c/%"

echo Building in %PROJ_WSL% ...
wsl.exe -- bash -c "export PATH=\"$HOME/.local/share/uv/tools/pebble-tool/bin:$PATH\" && cd '%PROJ_WSL%' && pebble clean && pebble build"
if %errorlevel% neq 0 ( echo BUILD FAILED & exit /b 1 )

:: ---- the 64 KB virtual-size cap ----------------------------------------
::
:: text+data+bss must come in under 65,535 bytes -- the cap is a uint16 field in
:: the app header that inject_metadata.py writes. The build above already fails
:: if it is exceeded, but it fails deep inside the SDK with a message about
:: "App image size", so print the number here either way: it is the one figure
:: that decides whether a song fits, and knowing the headroom is what tells you
:: whether the next change will.
echo.
echo ---- size against the 65,535-byte virtual-size cap ----
wsl.exe -- bash -c "SZ=$(ls $HOME/.local/share/pebble-sdk/SDKs/*/toolchain/arm-none-eabi/bin/arm-none-eabi-size 2>/dev/null | head -1); [ -n \"$SZ\" ] && cd '%PROJ_WSL%' && $SZ build/emery/pebble-app.elf | tail -1"
echo.
echo   Over the cap? Try, in order:
echo     set SONG_CPU=m33          ^(different ISA, usually a little smaller^)
echo     set SONG_NO_VISUALS=1     ^(audio only -- always fits, but blank screen^)
echo.

:: ---- the LTO app-header trap -------------------------------------------
::
:: LTO drops __pbl_app_info unless the wscript holds it with -u, and when that
:: happens the build still reports success while producing a .pbw the firmware
:: cannot load. The first six bytes of pebble-app.bin must read PBLAPP.
for /f %%m in ('wsl.exe -- bash -c "head -c6 '%PROJ_WSL%'/build/emery/pebble-app.bin"') do set "APPMAGIC=%%m"
if not "%APPMAGIC%"=="PBLAPP" (
    echo ERROR: pebble-app.bin does not start with PBLAPP ^(got "%APPMAGIC%"^).
    echo The app header was dropped -- see the LTO note in wscript. NOT installing.
    exit /b 1
)

if "%PHONE_IP%"=="" (
    echo Built, but not installed: put the phone's IP on the first line of
    echo pebble_phone_ip.txt, uncommented.
    exit /b 0
)

echo Installing to %PHONE_IP% ...
wsl.exe -- bash -c "export PATH=\"$HOME/.local/share/uv/tools/pebble-tool/bin:$PATH\" && cd '%PROJ_WSL%' && pebble install --phone %PHONE_IP% build/*.pbw"
if %errorlevel% neq 0 ( echo INSTALL FAILED & exit /b 1 )

echo Done.
endlocal
