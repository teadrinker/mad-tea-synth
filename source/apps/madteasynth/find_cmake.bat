:: Sets CMAKE and CMAKE_GENERATOR for the calling script (no setlocal here, on
:: purpose). Values the caller already set win. Otherwise the Visual Studio with
:: the C++ tools is picked through vswhere, VS 2022 first, then the newest, and
:: its bundled CMake is used ahead of cmake.exe on PATH, since an older CMake
:: may not know that VS's generator. CMake reads CMAKE_GENERATOR and
:: CMAKE_GENERATOR_INSTANCE itself when configure is given no -G.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
set "VSVER="
set "VSMAJOR="
if not exist "%VSWHERE%" goto :cmake
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -sort -property installationPath`) do if not defined VSDIR set "VSDIR=%%i"
if defined VSDIR set "VSMAJOR=17"
if defined VSDIR goto :generator
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -sort -property installationPath`) do if not defined VSDIR set "VSDIR=%%i"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -prerelease -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -sort -property installationVersion`) do if not defined VSVER set "VSVER=%%i"
if not defined VSDIR goto :cmake
for /f "delims=." %%i in ("%VSVER%") do set "VSMAJOR=%%i"

:generator
if defined CMAKE_GENERATOR goto :cmake
if "%VSMAJOR%"=="16" set "CMAKE_GENERATOR=Visual Studio 16 2019"
if "%VSMAJOR%"=="17" set "CMAKE_GENERATOR=Visual Studio 17 2022"
if "%VSMAJOR%"=="18" set "CMAKE_GENERATOR=Visual Studio 18 2026"
if defined CMAKE_GENERATOR set "CMAKE_GENERATOR_INSTANCE=%VSDIR%"

:cmake
if not defined CMAKE goto :bundled
set "CMAKE=%CMAKE:"=%"
goto :check

:bundled
if not defined VSDIR goto :path
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if exist "%CMAKE%" goto :check
set "CMAKE="

:path
for %%i in (cmake.exe) do set "CMAKE=%%~$PATH:i"
if defined CMAKE goto :check
if not exist "%VSWHERE%" goto :missing
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -prerelease -sort -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do if not defined CMAKE set "CMAKE=%%i"
if not defined CMAKE goto :missing

:check
if exist "%CMAKE%" exit /b 0
echo ERROR: CMAKE is set to "%CMAKE%", which does not exist.
exit /b 1

:missing
echo ERROR: cmake.exe not found. Put it on PATH, set CMAKE=C:\path\to\cmake.exe,
echo        or install the "C++ CMake tools for Windows" Visual Studio component.
exit /b 1
