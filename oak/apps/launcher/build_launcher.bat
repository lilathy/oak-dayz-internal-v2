@echo off
REM Build OakLauncher.exe with MSVC (Developer Command Prompt or vsdevcmd)
setlocal
cd /d "%~dp0"
where cl >nul 2>&1
if errorlevel 1 (
  echo Run from "x64 Native Tools Command Prompt for VS" or call vsdevcmd
  exit /b 1
)
if not exist "..\bin" mkdir "..\bin"
cl /nologo /O2 /EHsc /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS OakLauncher.cpp /Fe:..\bin\OakLauncher.exe /link shell32.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
copy /Y ..\bin\OakLauncher.exe C:\oak\OakLauncher.exe >nul 2>&1
echo Built ..\bin\OakLauncher.exe
exit /b 0
