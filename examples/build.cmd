@echo off
REM Builds both example plugins as 32-bit EuroScope DLLs.
REM
REM They link EuroScopePlugInDll.lib exactly as any plugin does, and nothing
REM from the bridge: a client needs only esbridge.h and GetProcAddress.

setlocal
cd /d "%~dp0.."

if "%VSCMD_ARG_TGT_ARCH%"=="x86" goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere not found. Run from an x86 Native Tools Command Prompt.
  exit /b 1
)
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\esb_vspath.txt"
set /p VSPATH=<"%TEMP%\esb_vspath.txt"
del "%TEMP%\esb_vspath.txt" >nul 2>&1
if not defined VSPATH (
  echo Could not locate Visual Studio.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1

:build
if not exist build\examples mkdir build\examples

for %%P in (ExamplePublisher ExampleConsumer) do (
  cl /nologo /LD /EHsc /std:c++17 /W4 /MT /external:anglebrackets /external:W0 /Iinclude /Ilib ^
     examples\%%P.cpp ^
     /Fe:build\examples\%%P.dll /Fo:build\examples\ ^
     /link lib\EuroScopePlugInDll.lib
  if errorlevel 1 exit /b 1
)

echo.
echo Built build\examples\ExamplePublisher.dll and ExampleConsumer.dll
exit /b 0
