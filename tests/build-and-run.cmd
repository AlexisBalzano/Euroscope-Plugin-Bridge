@echo off
REM Builds and runs the M1 smoke test.
REM
REM Links the core translation units directly rather than loading the DLL: the
REM bridge imports EuroScopePlugInDll.dll, so LoadLibrary outside EuroScope
REM fails with ERROR_MOD_NOT_FOUND. This only links because Registry,
REM AircraftTable and ApiExports have no EuroScope dependency.
REM
REM Run from a "x86 Native Tools Command Prompt", or let this find vcvars.

setlocal
cd /d "%~dp0.."

if "%VSCMD_ARG_TGT_ARCH%"=="x86" goto :build

REM No FOR /F here: the literal parentheses in "Program Files (x86)" close the
REM FOR block during parsing, even when the path comes from a variable. Route
REM vswhere's output through a temp file instead.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere not found. Run from an x86 Native Tools Command Prompt.
  exit /b 1
)
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\esb_vspath.txt"
set /p VSPATH=<"%TEMP%\esb_vspath.txt"
del "%TEMP%\esb_vspath.txt" >nul 2>&1
if not defined VSPATH (
  echo Could not locate Visual Studio. Run from an x86 Native Tools Command Prompt.
  exit /b 1
)
REM vcvarsall.bat may print "'vswhere.exe' is not recognized" from its own
REM internals on some installs. It is noise -- the environment is still set up
REM correctly. Our own vswhere call above already succeeded, or we exited.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1

:build
if not exist build\obj\test mkdir build\obj\test

cl /nologo /EHsc /std:c++17 /W4 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Iinclude /Isrc ^
   tests\smoke.cpp src\Registry.cpp src\AircraftTable.cpp src\ApiExports.cpp ^
   src\BridgeInstance.cpp src\Log.cpp src\Json.cpp ^
   src\Config.cpp src\MqttCodec.cpp src\Relay.cpp src\Transport.cpp ^
   /Fe:build\smoke.exe /Fo:build\obj\test\ /link /SUBSYSTEM:CONSOLE
if errorlevel 1 exit /b 1

echo.
build\smoke.exe
exit /b %errorlevel%
