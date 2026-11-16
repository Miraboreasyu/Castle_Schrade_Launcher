@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
  if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo Run from an x64 Native Tools Command Prompt, or install Visual Studio C++ Build Tools.
    exit /b 1
  )
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
  call "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
  if errorlevel 1 exit /b 1
)
cl /nologo /LD /O2 /EHsc /W4 /std:c++17 /utf-8 src\auto_quest_start.cpp src\hook.cpp /Fe:auto_quest_start.dll
if errorlevel 1 exit /b 1
exit /b 0
