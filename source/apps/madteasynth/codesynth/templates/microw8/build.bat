@echo off
setlocal

:: build.bat -- build this exported song into a microw8 cart.
::
:: GENERATED PROJECT. Re-exporting over this directory replaces this file.
::
::   build.bat          -> <name>.wasm + <name>.uw8
::   build.bat nopack   -> <name>.wasm only (skips the pack step)
::
:: <name> is the project name chosen at export time; the exporter substitutes it
:: into the set below.
::
:: Needs clang (with the wasm32 target, which stock LLVM has) plus microw8's
:: own uw8.exe and wasm-opt.exe. Put those two next to this file, or set
:: UW8_TOOLS to the directory holding them.
::
:: ---------------------------------------------------------------------------
:: THE STATIC-MEMORY BUDGET -- the one number to watch
::
:: A cart gets 256 KB TOTAL. The platform owns everything below 0x14000 (the
:: 320x240 framebuffer at 0x78, the palette, the font); C statics and globals
:: start at 0x14000, the stack sits directly above them growing down, and what
:: is left above that is free.
::
:: Overrun it and the cart does NOT fail to build or to pack. It fails to LOAD,
:: with a backtrace into the loader that names no function of yours:
::
::     Load error: error while executing at wasm backtrace:
::         0:  0x210 - <unknown>!<wasm function 4>
::         1:   0x9d - <unknown>!<wasm function 0>
::
:: Measured on this song: 62,416 bytes of statics loads and runs; an earlier
:: build at 139,216 did not. An unrelated uw8 project reports the same failure
:: appearing around 110-120 KB, so treat somewhere under ~110 KB as the budget.
::
:: Note that running the raw .wasm does NOT enforce this -- a raw module runs
:: happily at sizes a packed cart refuses -- so testing only the .wasm hides the
:: problem. Run the .uw8.
::
:: The single biggest saving is already applied: song_config.h sets
:: VSCREEN_FRAMEBUFFER_ADDR to 0x78, so the renderer draws straight into the
:: platform's framebuffer. Without it vscreen declares its own 320x240 buffer
:: and the cart pays 76,800 bytes for a second copy of a screen it already has
:: -- which by itself was the difference between the two numbers above. Do not
:: remove it.
::
:: If a bigger song busts the budget, the next levers are
:: VSCREEN_GLYPH_SCRATCH_BYTES and VSCREEN_IMAGE_ARENA_BYTES in song_config.h,
:: then dropping SONG_VISUALS for an audio-only cart.
:: ---------------------------------------------------------------------------

cd /d "%~dp0"

:: The cart's name -- the one thing about this project a .bat needs and cannot
:: read out of song_config.h. The exporter fills {{SLUG}} in when it writes this
:: file, so this is the project name slugged for use as a filename. Editing it
:: renames the cart; a re-export replaces this file, so to make a rename stick
:: change the project name in the exporter instead.
set "PROJECT_NAME={{SLUG}}"
set "WASM=%PROJECT_NAME%.wasm"
set "CART=%PROJECT_NAME%.uw8"

if "%UW8_TOOLS%"=="" set "UW8_TOOLS=%~dp0"
if not exist "%UW8_TOOLS%\wasm-opt.exe" (
    echo ERROR: wasm-opt.exe not found in "%UW8_TOOLS%".
    echo Set UW8_TOOLS to the directory holding uw8.exe and wasm-opt.exe.
    exit /b 1
)

set "SRC=src/c/cart.c src/c/vscreen.c src/c/tiny_runtime.c src/c/song/song.c src/c/common/math_fixedp.c src/c/common/math_pure.c src/c/font/render_lowspec_lowmem.c src/c/font/render_ctx.c"

:: --initial-memory=262144 is microw8's whole address space: 256 KB, of which
:: the 320x240 framebuffer is 76,800 and the palette and font take the rest
:: below 0x14000. --global-base=81920 puts our data just above them. Those
:: numbers are the platform's, not ours -- do not tune them.
::
:: RS_PEBBLE_TIME2=0 overrides render_lowspec_lowmem.c's Pebble default, so the
:: renderer blends through the screen's runtime layout (GColor8 until the song
:: calls color_ramp_setup).
::
:: Turning OFF bulk-memory is NOT optional with a current clang (21 enables it
:: by default). With it on, memset/memcpy lower to memory.fill/memory.copy and
:: the linker emits a DataCountSection alongside them -- which uw8 pack rejects
:: outright ("Unsupported section: DataCountSection"), at the very LAST step,
:: after clang and wasm-opt have both reported success. microw8 predates the
:: proposal and its loader wants baseline MVP wasm.
::
:: It has to be spelled as -target-feature, not -mno-bulk-memory: the -m form is
:: accepted and silently does nothing here, and bulk-memory-opt is a SEPARATE
:: feature since LLVM 19 -- leaving it on puts memory.fill back on its own.
::
:: -include song_config.h on EVERY translation unit, not just the ones that name
:: it. vscreen.c includes only vscreen.h, so without this it would compile
:: against the DEFAULT VSCREEN_W/H and arena sizes while cart.c compiled against
:: the configured ones -- two translation units disagreeing about the size of
:: the same static array, which is a silently wrong program rather than a link
:: error. It is also what makes the arena overrides reach vscreen.c at all.
::
:: No --export-all: cart.c names its two exports with export_name attributes
:: instead. Besides being what microw8 actually needs, it lets dead-code
:: elimination work -- with --export-all every function is reachable by
:: definition, and the cart was ~20% bigger.
echo ---- compiling %WASM% ----
clang -O2 --target=wasm32 ^
    -Wno-incompatible-library-redeclaration ^
    --no-standard-libraries ^
    -ffast-math ^
    -Xclang -target-feature -Xclang -bulk-memory ^
    -Xclang -target-feature -Xclang -bulk-memory-opt ^
    -Xclang -target-feature -Xclang +nontrapping-fptoint ^
    -DRS_PEBBLE_TIME2=0 ^
    -Isrc/c -include src/c/song_config.h ^
    -Wl,--no-entry,--import-memory,--initial-memory=262144,--global-base=81920,-zstack-size=4096 ^
    -o "%WASM%" %SRC%
if errorlevel 1 ( echo COMPILE FAILED & exit /b 1 )

echo ---- wasm-opt ----
"%UW8_TOOLS%\wasm-opt.exe" -Oz --fast-math --strip-producers -o "%WASM%" "%WASM%"
if errorlevel 1 ( echo WASM-OPT FAILED & exit /b 1 )

if /i "%~1"=="nopack" goto done

:: Compression level. The reference carts this pipeline came from are a few KB,
:: where -l 9 is instant; a song cart is ~200 KB and -l 9 on that takes tens of
:: MINUTES (measured at ~100 bytes/sec). Level 1 packs in seconds.
if "%UW8_PACK_LEVEL%"=="" set "UW8_PACK_LEVEL=1"
echo ---- packing (level %UW8_PACK_LEVEL%) ----
"%UW8_TOOLS%\uw8.exe" pack -l %UW8_PACK_LEVEL% "%WASM%" "%CART%"
if errorlevel 1 ( echo PACK FAILED & exit /b 1 )
for %%F in ("%CART%") do echo Built %%~fF  (%%~zF bytes)

:done
echo.
for %%F in ("%WASM%") do echo Built %%~fF  (%%~zF bytes)
if exist "%CART%" (
    echo Run it:  "%UW8_TOOLS%\uw8.exe" run "%CART%"
) else (
    echo Run it:  "%UW8_TOOLS%\uw8.exe" run "%WASM%"
)
endlocal
