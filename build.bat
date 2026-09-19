@echo off
rem build.bat — no build system, just vcvars + cl. Sources are globbed so this
rem file never needs editing as the project grows.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [build] failed to initialise the MSVC environment
    exit /b 1
)
if not exist build mkdir build

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
