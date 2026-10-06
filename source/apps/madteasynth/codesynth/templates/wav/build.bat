@echo off
setlocal

:: build.bat -- render this exported song to a .wav.
::
:: GENERATED PROJECT. Re-exporting over this directory replaces this file.
::
:: Needs clang on PATH (LLVM). Builds src/c/render_song.c -- which #includes the
:: generated song.c as a single translation unit -- and then RUNS it, so one
:: double-click takes you from source to audio.
::
:: The exporter runs this for you at export time. It is here so the render can
:: be repeated, or retimed, without going back to the plugin:
::
::   build.bat                 the whole song
::   build.bat 8               the first 8 seconds
::   build.bat 8 preview.wav   ...to a file of your choosing

cd /d "%~dp0"

:: The song's name -- the one thing about this project a .bat needs and cannot
:: read out of song_config.h. The exporter fills {{SLUG}} in when it writes this
:: file. Editing it renames the .exe and the .wav; a re-export replaces this
:: file, so to make a rename stick change the project name in the exporter.
set "PROJECT_NAME={{SLUG}}"

set "OUT_EXE=%PROJECT_NAME%_render.exe"
set "OUT_WAV=%PROJECT_NAME%.wav"

:: song.c is NOT in this list: render_song.c #includes it as <song.c>, an angle
:: include that resolves through -Isrc/c/song only -- so it is compiled as part
:: of that one translation unit, which is what makes song_SongState's
:: size-correct definition (SONG_CHANNEL_COUNT varies per export) visible
:: without duplicating the struct. vscreen.c and the font/ renderer are linked
:: because song.c DEFINES song_visual_render, whose body calls vscreen_*, even
:: though a .wav render never draws anything.
set "SRC=src/c/render_song.c src/c/vscreen.c src/c/common/math_fixedp.c src/c/common/math_pure.c src/c/font/render_lowspec.c src/c/font/render_ctx.c"

:: No RS_PEBBLE_TIME2, as in the win32 target, so the two builds compile the
:: shared sources the same way. Irrelevant to the audio.
set "DEFS="

rem -include song_config.h on EVERY translation unit, not just the ones that
rem name it: vscreen.c only includes vscreen.h, so without this it would compile
rem against the DEFAULT VSCREEN_W/H and arena sizes while render_song.c compiled
rem against the configured ones -- two translation units disagreeing about the
rem size of the same static array, which is a silently wrong program rather than
rem a link error. It is also what carries SONG_SAMPLE_RATE into render_song.c.
set "INCS=-Isrc/c -Isrc/c/song -include src/c/song_config.h"

:: No -lm: clang here targets MSVC, whose CRT carries the math functions and
:: which has no libm to link. (The in-tree render_song.bat uses gcc and does
:: pass it -- same source, different toolchain.)
echo ---- building %OUT_EXE% ----
clang -O2 -std=c11 %DEFS% %INCS% %SRC% -o "%OUT_EXE%"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

:: 0 seconds means "the whole song" -- spelled rather than omitted so the output
:: file can be named without also having to choose a duration.
set "ARG_SECONDS=%~1"
if "%ARG_SECONDS%"=="" set "ARG_SECONDS=0"
set "ARG_WAV=%~2"
if "%ARG_WAV%"=="" set "ARG_WAV=%OUT_WAV%"

:: Spelled with %~dp0 rather than as a bare name: the exporter runs this script
:: through a nested `cmd /c "..."`, and a quoted relative command there is not
:: resolved against the current directory the way it is at an interactive prompt.
echo ---- rendering %ARG_WAV% ----
"%~dp0%OUT_EXE%" %ARG_SECONDS% "%ARG_WAV%"
if errorlevel 1 ( echo RENDER FAILED & exit /b 1 )

echo.
for %%F in ("%ARG_WAV%") do echo Wrote %%~fF  (%%~zF bytes)
endlocal
