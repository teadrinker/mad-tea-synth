@echo off
setlocal

:: build_vst.bat -- one command, from a fresh copy of the tree to a built VST3.
::
::   build_vst.bat [args passed through to get_iplug2.bat]
::
:: Checks that iPlug2 is present AND correct, provisions it if not, then builds.
:: Everything past this script is the existing pair:
::
::   apps\madteasynth\get_iplug2.bat   iPlug2 + its SDKs + the local patches
::   apps\madteasynth\build_vst3.bat   CMake configure + MSBuild + deploy
::
:: It runs the VERIFY step rather than just testing whether apps\madteasynth\
:: iplug2 exists as a directory. That distinction is the whole point: the way
:: this goes wrong in practice is not an absent iplug2, it is a PRESENT one that
:: is missing the VST3/CLAP SDKs, or the local patches, or both -- see
:: get_iplug2.bat's header. An existence check passes on exactly the tree that
:: cannot build.
::
:: Offline, or want to reuse an iPlug2 you already have? Arguments are forwarded:
::
::   build_vst.bat --source C:\path\to\iplug2

set "HERE=%~dp0"
set "PLUG=%HERE%apps\madteasynth"

if not exist "%PLUG%\get_iplug2.bat" (
    echo ERROR: %PLUG%\get_iplug2.bat not found.
    echo        Run this from the root of the tree, not from a subdirectory.
    exit /b 1
)

call "%PLUG%\get_iplug2.bat" --verify-only >nul 2>&1
if errorlevel 1 (
    echo iPlug2 is missing or incomplete -- provisioning it first.
    echo.
    call "%PLUG%\get_iplug2.bat" %*
    if errorlevel 1 (
        echo.
        echo Could not provision iPlug2. Nothing was built.
        exit /b 1
    )
    echo.
) else (
    echo iPlug2 OK.
    echo.
)

:: A configure that failed earlier leaves build\CMakeCache.txt behind, and
:: build_vst3.bat only configures when that file is ABSENT -- so it would skip
:: to MSBuild and report "MSB1009: Project file does not exist" forever. If the
:: cache exists but no project was generated, that is exactly what happened.
if exist "%PLUG%\build\CMakeCache.txt" (
    if not exist "%PLUG%\build\madteasynth-vst3.vcxproj" (
        echo Stale CMake cache from a failed configure -- clearing it.
        rmdir /s /q "%PLUG%\build"
        echo.
    )
)

call "%PLUG%\build_vst3.bat"
exit /b %errorlevel%
