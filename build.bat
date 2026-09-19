@echo off
rem build.bat — no build system, just vcvars + cl. Sources are globbed so this
rem file never needs editing as the project grows.
setlocal

rem Locate Visual Studio via vswhere rather than hardcoding a path: the edition
rem (Community / Professional / Enterprise / BuildTools) and the install drive
rem both vary between machines.
rem
rem The for /f must stay on ONE line. Splitting it with ^ continuations breaks
rem the backtick-quoted command: cmd reports "'vswhere.exe' is not recognized"
rem and the loop silently yields nothing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] vswhere.exe not found at "%VSWHERE%"
    echo [build] Install Visual Studio 2022 with the "Desktop development with C++" workload.
    exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo [build] No Visual Studio installation with the C++ toolset was found.
    echo [build] Install the "Desktop development with C++" workload.
    exit /b 1
)

if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [build] vcvars64.bat missing under "%VSPATH%".
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build] Failed to initialise the MSVC environment from "%VSPATH%".
    exit /b 1
)

if not exist build mkdir build

rem /utf-8 is load-bearing: the sources are UTF-8 and the system codepage on a
rem Chinese Windows is GBK. Without it every Chinese string literal in the
rem binary is garbage. /MT gives a static CRT so the exe has no runtime
rem dependency.
cl /nologo /W4 /O1 /MT /GS- /GR- /utf-8 /DUNICODE /D_UNICODE /DNDEBUG ^
   /DWIN32_LEAN_AND_MEAN /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 ^
   /Fo:build\ /Fe:build\screenshot_tool.exe ^
   src\*.cpp ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO ^
   user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib ^
   windowscodecs.lib dwmapi.lib comdlg32.lib
if errorlevel 1 (
    echo [build] FAILED
    exit /b 1
)
echo [build] ok
