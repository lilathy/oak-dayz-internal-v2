@echo off
cd /d C:\oak\dayz
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting Administrator for BE kernel inject...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\oak\dayz\LaunchDayZSafe.ps1" -Steam -Inject -InitSeconds 45
if errorlevel 1 (
  echo LAUNCH FAILED - see %LOCALAPPDATA%\DayZ\oak_nvidia_launch.log
  pause
  exit /b 1
)
