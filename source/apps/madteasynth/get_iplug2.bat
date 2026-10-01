@echo off
setlocal EnableDelayedExpansion

:: get_iplug2.bat -- provision a BUILDABLE iPlug2 next to this script.
::
::   get_iplug2.bat [options]
::
::     --source <path>  copy the base tree from an existing iPlug2 checkout
::                      instead of cloning. Much faster, and the only option
::                      offline. In this repo: apps\madteasynth\iplug2 itself.
::     --ref <ref>      branch/tag/sha to clone. Default %IPLUG2_REF% below.
::     --sdks           re-download the VST3/CLAP SDKs even if present.
::     --verify-only    check an existing tree, change nothing. Exit 1 if the
::                      tree would not build.
::
:: This script lives in apps\madteasynth\ so that it TRAVELS WITH THE COPY that
:: copy_madteasynth_deps.bat makes -- that copy leaves iplug2 behind, so the
:: thing that puts iPlug2 back has to be inside the copied tree, not in the
:: source repo the copy was made from. iplug2 always lands next to this script;
:: there is no destination argument, because there is only ever one right place
:: for it.
::
:: WHAT A PLAIN COPY OF "SOME iPLUG2" GETS WRONG
::
:: 1. THE SDKs ARE NOT IN AN iPLUG2 CHECKOUT.
::    Dependencies\IPlug\VST3_SDK, CLAP_SDK and CLAP_HELPERS ship as
::    placeholder directories holding one README.md. They are NOT submodules --
::    there is no .gitmodules. iPlug2 fills them with
::    Dependencies\IPlug\download-*.sh, which git clone the SDKs and then
::    "rm -rf .git*", which is why even a filled-in tree carries no provenance,
::    and why copying a tree that was never filled in silently hands you the
::    placeholders. CMake then fails with
::      Cannot find source file: .../VST3_SDK/base/source/baseiids.cpp
::    plus a wall of "Imported target iPlug2::VST3 includes non-existent path".
::    (VST2_SDK and AAX_SDK stay empty on purpose -- those formats are not
::    built, and CMake just logs "SDK not found" and skips them.)
::
:: 2. iPLUG2 IS PATCHED HERE.
::    Five files differ from upstream. They are tracked in iplug2_patches\
::    beside this script -- iplug2\ itself is gitignored, so without that
::    directory the patches would live on one disk only. Missing them, the
::    build stops at
::      error C3668: 'SteepSynthControl::OnDropAt': method with override
::      specifier 'override' did not override any base class methods
::    which reads as a bug in madteasynth's own code. See
::    iplug2_patches\README.md for what each one does.
::
:: Both failures compound with the stale-cache trap: a failed CMake configure
:: still leaves build\CMakeCache.txt behind, and build_vst3.bat only configures
:: when that file is ABSENT -- so the next run skips straight to MSBuild and
:: reports "MSB1009: Project file does not exist". build_vst.bat in the tree
:: root runs the verify step before building, which is what stops that loop.
::
:: PINNED VERSIONS
::
:: The SDK tags below are the versions this tree was built against, and the
:: verify step asserts them by reading the version out of the headers -- so a
:: moved tag cannot drift past unnoticed. iPlug2 itself has NO pinned revision:
:: the checkout this repo vendors had its .git removed before it got here, so
:: there is no commit id to pin to and --ref defaults to a branch. If a clone
:: builds, put its sha in IPLUG2_REF below and it stops being a moving target.

set "IPLUG2_URL=https://github.com/iPlug2/iPlug2.git"
set "IPLUG2_REF=master"

:: v3.8.0_build_33 was here and does not exist upstream -- Steinberg publishes
:: one tag per SDK release and 3.8.0's is build_66. The vendored tree carries no
:: build number to check it against (the headers stop at "VST 3.8.0"), so the
:: wrong number sat here until the clone path was first exercised. If this fails
:: again, `git ls-remote --tags https://github.com/steinbergmedia/vst3sdk.git`
:: lists what actually exists; anything that still says VST 3.8.0 passes verify.
set "VST3_URL=https://github.com/steinbergmedia/vst3sdk.git"
set "VST3_TAG=v3.8.0_build_66"
set "VST3_EXPECT=VST 3.8.0"

set "CLAP_URL=https://github.com/free-audio/clap.git"
set "CLAP_TAG=1.2.9"

set "HELPERS_URL=https://github.com/free-audio/clap-helpers.git"
set "HELPERS_TAG=main"

:: ---------------------------------------------------------------- arguments

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

set "IPLUG2=%HERE%\iplug2"
set "PATCHES=%HERE%\iplug2_patches"
set "DEPS=%IPLUG2%\Dependencies\IPlug"

:: No quoted entries in here: `set "VAR=..."` ends the value at the LAST quote
:: on the line, so an embedded "New folder" would silently reshape the whole
:: option string. Nothing needing quotes is worth excluding.
set "RCOPTS=/E /NFL /NDL /NJH /NJS /NP /XD .git build .vs __pycache__ /XF *.obj *.pdb *.ilk *.exp *.lib"

set "SOURCE="
set "FORCE_SDKS=0"
set "VERIFY_ONLY=0"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--source"      ( set "SOURCE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--ref"         ( set "IPLUG2_REF=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--sdks"        ( set "FORCE_SDKS=1" & shift & goto :parse )
if /i "%~1"=="--verify-only" ( set "VERIFY_ONLY=1" & shift & goto :parse )
if /i "%~1"=="--help"        goto :usage
if /i "%~1"=="-h"            goto :usage
echo ERROR: unexpected argument "%~1"
goto :usage

:parsed
echo Target: %IPLUG2%
echo.
if "%VERIFY_ONLY%"=="1" goto :verify

:: ------------------------------------------------------------ 1. base tree

if exist "%IPLUG2%\iPlug2.cmake" (
    echo [1/3] base tree: already present ^(delete it to re-provision^)
    goto :sdks
)

:: NB: every comment in this file sits OUTSIDE its ( ) block, and has to. A ::
:: line inside a parenthesised block is not a comment -- cmd tries to run it and
:: prints "The system cannot find the drive specified." per line, to stderr,
:: while the block itself works perfectly. Which is a convincing thing to hand
:: someone who is already debugging a build. Use `rem` if it must go inside.
::
:: .git is excluded from the copy because upstream's own download scripts strip
:: it anyway, and a half-repo here would only be misleading.
if not "%SOURCE%"=="" (
    if not exist "%SOURCE%\iPlug2.cmake" (
        echo ERROR: --source "%SOURCE%" is not an iPlug2 tree ^(no iPlug2.cmake^).
        exit /b 1
    )
    echo [1/3] base tree: copying from %SOURCE%
    robocopy "%SOURCE%" "%IPLUG2%" %RCOPTS% >nul
    if errorlevel 8 (
        echo ROBOCOPY FAILED ^(exit code %errorlevel%^)
        exit /b 1
    )
    goto :sdks
)

call :needgit
if errorlevel 1 exit /b 1
echo [1/3] base tree: cloning %IPLUG2_URL% @ %IPLUG2_REF%
git clone --depth=1 --branch "%IPLUG2_REF%" "%IPLUG2_URL%" "%IPLUG2%"
if errorlevel 1 (
    echo ERROR: clone failed. Offline? Pass --source ^<path to an iPlug2 tree^>.
    exit /b 1
)
echo       cloned a floating ref; if this builds, pin this sha in IPLUG2_REF:
git -C "%IPLUG2%" rev-parse HEAD

:: ------------------------------------------------------------- 2. SDKs

:sdks
set "NEED_SDKS=0"
if not exist "%DEPS%\VST3_SDK\base\source\baseiids.cpp"                  set "NEED_SDKS=1"
if not exist "%DEPS%\CLAP_SDK\include\clap\clap.h"                       set "NEED_SDKS=1"
if not exist "%DEPS%\CLAP_HELPERS\include\clap\helpers\checking-level.hh" set "NEED_SDKS=1"
if "%FORCE_SDKS%"=="1" set "NEED_SDKS=1"

if "%NEED_SDKS%"=="0" (
    echo [2/3] SDKs: already present
    goto :patches
)

call :needgit
if errorlevel 1 (
    echo        ...and the VST3/CLAP SDKs are missing, which needs git.
    exit /b 1
)

echo [2/3] SDKs: downloading ^(the step a plain file copy misses^)
call :getsdk "%DEPS%\VST3_SDK"     "%VST3_URL%"    "%VST3_TAG%"    vst3
if errorlevel 1 exit /b 1
call :getsdk "%DEPS%\CLAP_SDK"     "%CLAP_URL%"    "%CLAP_TAG%"    plain
if errorlevel 1 exit /b 1
call :getsdk "%DEPS%\CLAP_HELPERS" "%HELPERS_URL%" "%HELPERS_TAG%" plain
if errorlevel 1 exit /b 1

:: ---------------------------------------------------------- 3. local patches

:patches
if not exist "%PATCHES%\IGraphics\IControl.h" (
    echo ERROR: patch set not found at %PATCHES%
    exit /b 1
)
echo [3/3] patches: overlaying iplug2_patches
robocopy "%PATCHES%" "%IPLUG2%" /E /NFL /NDL /NJH /NJS /NP /XF README.md >nul
if errorlevel 8 (
    echo ROBOCOPY FAILED ^(exit code %errorlevel%^)
    exit /b 1
)

:: ------------------------------------------------------------------ verify

:verify
echo.
echo Verifying...
set "FAIL=0"

call :needfile "%IPLUG2%\iPlug2.cmake"                                   "iPlug2 base tree"
call :needfile "%DEPS%\VST3_SDK\base\source\baseiids.cpp"                "VST3 SDK"
call :needfile "%DEPS%\CLAP_SDK\include\clap\clap.h"                     "CLAP SDK"
call :needfile "%DEPS%\CLAP_HELPERS\include\clap\helpers\checking-level.hh" "CLAP helpers"
call :needfile "%IPLUG2%\Scripts\cmake\DeployCopy.cmake"                 "patch: DeployCopy.cmake"

:: The patches are whole-file overrides, so grep a sentinel out of each rather
:: than trusting that the copy happened.
call :needtext "%IPLUG2%\IGraphics\IControl.h"    "OnDropAt" "patch: IControl.h OnDropAt"
call :needtext "%IPLUG2%\IGraphics\IGraphics.cpp" "OnDropAt" "patch: IGraphics.cpp OnDropAt"
call :needtext "%IPLUG2%\IGraphics\Platforms\IGraphicsWin.cpp" "(float) GET_WHEEL_DELTA_WPARAM" "patch: IGraphicsWin.cpp wheel delta"
call :needtext "%IPLUG2%\Scripts\cmake\Deploy.cmake" "DeployCopy.cmake" "patch: Deploy.cmake indirection"

:: Assert the SDK VERSIONS, not just their presence: the CLAP tag is a branch
:: name upstream can move, and a mismatched VST3 SDK compiles for a while and
:: then fails somewhere that looks unrelated.
call :needtext "%DEPS%\VST3_SDK\pluginterfaces\vst\vsttypes.h" "%VST3_EXPECT%" "VST3 SDK is %VST3_EXPECT%"
call :needtext "%DEPS%\CLAP_SDK\include\clap\version.h" "CLAP_VERSION_MAJOR 1"   "CLAP major 1"
call :needtext "%DEPS%\CLAP_SDK\include\clap\version.h" "CLAP_VERSION_MINOR 2"   "CLAP minor 2"
call :needtext "%DEPS%\CLAP_SDK\include\clap\version.h" "CLAP_VERSION_REVISION 9" "CLAP revision 9"

if not "%FAIL%"=="0" (
    echo.
    echo FAILED: %FAIL% check^(s^) did not pass. This tree will not build.
    if "%VERIFY_ONLY%"=="1" echo        Run get_iplug2.bat with no arguments to fix it.
    exit /b 1
)

echo.
echo OK -- iPlug2 is ready at %IPLUG2%
endlocal
exit /b 0

:: ----------------------------------------------------------------- helpers

:getsdk
:: %1 = target dir, %2 = url, %3 = tag, %4 = "vst3" for the extra submodules
echo       %~3  ^<-  %~2
if exist "%~1" rmdir /s /q "%~1"
git clone --depth=1 --branch "%~3" --single-branch "%~2" "%~1"
if errorlevel 1 (
    echo ERROR: clone of %~2 @ %~3 failed. If the tag moved or was renamed,
    echo        update the pin at the top of this script.
    exit /b 1
)
:: Mirrors Dependencies\IPlug\download-vst3-sdk.sh, minus vstgui4 -- iPlug2 does
:: not use it and it is a large part of the download. (Comment outside the block
:: on purpose -- see the note by the base-tree copy.)
if /i "%~4"=="vst3" (
    for %%M in (pluginterfaces base public.sdk cmake) do (
        git -C "%~1" submodule update --init --depth=1 %%M
        if errorlevel 1 (
            echo ERROR: VST3 submodule %%M failed.
            exit /b 1
        )
    )
    if exist "%~1\public.sdk\samples" rmdir /s /q "%~1\public.sdk\samples"
)
:: Same as upstream's scripts: drop the git metadata, so the result is a plain
:: source tree and nothing mistakes it for a repo to update.
if exist "%~1\.git" rmdir /s /q "%~1\.git"
del /q "%~1\.gitmodules" >nul 2>&1
exit /b 0

:needgit
where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: git is not on PATH.
    exit /b 1
)
exit /b 0

:needfile
if not exist "%~1" (
    echo   MISSING  %~2
    set /a FAIL+=1
) else (
    echo   ok       %~2
)
exit /b 0

:needtext
if not exist "%~1" (
    echo   MISSING  %~3  ^(no such file^)
    set /a FAIL+=1
    exit /b 0
)
findstr /c:"%~2" "%~1" >nul 2>&1
if errorlevel 1 (
    echo   MISSING  %~3
    set /a FAIL+=1
) else (
    echo   ok       %~3
)
exit /b 0

:usage
echo.
echo   get_iplug2.bat [--source ^<path^>] [--ref ^<ref^>] [--sdks] [--verify-only]
echo.
echo   Provisions a buildable iPlug2 at:
echo     %IPLUG2%
echo   base tree, the VST3/CLAP SDKs an iPlug2 checkout does not carry, and the
echo   local patches from iplug2_patches\. Verifies all three.
echo.
endlocal
exit /b 1
