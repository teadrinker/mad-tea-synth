@echo off
setlocal

:: build.bat -- build this exported song into a standalone .exe.
::
:: GENERATED PROJECT. Re-exporting over this directory replaces this file.
::
:: Needs clang on PATH (LLVM). Two configurations:
::
::   build.bat          normal build, links the CRT
::   build.bat tiny     -nostdlib, no CRT -- a much smaller .exe
::
:: The tiny build is not a gimmick: it is what makes this template worth having
:: over just linking SDL. Nothing here needs the CRT -- the song is fixed-point
:: and the renderer, font cache included, works in static arenas.

cd /d "%~dp0"

:: The app's name -- the one thing about this project a .bat needs and cannot
:: read out of song_config.h. The exporter fills {{SLUG}} in when it writes this
:: file, so this is the project name slugged for use as a filename. Editing it
:: renames the .exe; a re-export replaces this file, so to make a rename stick
:: change the project name in the exporter instead.
set "PROJECT_NAME={{SLUG}}"

set "OUT=%PROJECT_NAME%.exe"
set "SRC=src/c/main.c src/c/vscreen.c src/c/song/song.c src/c/common/math_fixedp.c src/c/common/math_pure.c src/c/font/render_lowspec.c src/c/font/render_ctx.c"

:: No RS_PEBBLE_TIME2: render_lowspec blends through the screen's runtime layout
:: (GColor8 0bAARRGGBB until the song calls color_ramp_setup).
set "DEFS="
rem -include song_config.h on EVERY translation unit, not just the ones that
rem name it. vscreen.c only includes vscreen.h, so without this it would compile
rem against the DEFAULT VSCREEN_W/H and arena sizes while main.c compiled against
rem the configured ones -- two translation units disagreeing about the size of
rem the same static array, which is a silently wrong program rather than a link
rem error. It is also what makes the arena overrides reach vscreen.c at all.
set "INCS=-Isrc/c -include src/c/song_config.h"
set "LIBS=-luser32 -lgdi32 -lkernel32 -lwinmm"

if /i "%~1"=="tiny" goto tiny

echo ---- normal build ----
:: /entry:mainCRTStartup with /subsystem:windows: the app has no console, but
:: its entry point is plain main() rather than WinMain (see main.c -- one entry
:: point serves both configurations). Without this the CRT's windows-subsystem
:: startup looks for WinMain and the link fails.
clang -O2 %DEFS% %INCS% %SRC% -o "%OUT%" %LIBS% -Wl,/subsystem:windows,/entry:mainCRTStartup
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
goto done

:tiny
echo ---- tiny build (no CRT) ----
:: -nostdlib drops the CRT entirely; crt_stub_win32.c (carried in this project)
:: supplies mainCRTStartup, which calls main(), plus memset and _fltused.
clang -O2 -fno-asynchronous-unwind-tables -nostdlib -fuse-ld=lld ^
    %DEFS% -DTINY_RUNTIME_NO_MEMSET %INCS% ^
    %SRC% src/c/crt_stub_win32.c src/c/tiny_runtime.c ^
    -o "%OUT%" %LIBS% -Wl,/entry:mainCRTStartup,/subsystem:windows
if errorlevel 1 ( echo TINY BUILD FAILED & exit /b 1 )

:done
echo.
for %%F in ("%OUT%") do echo Built %%~fF  (%%~zF bytes)
echo Run it:  %OUT%
endlocal
