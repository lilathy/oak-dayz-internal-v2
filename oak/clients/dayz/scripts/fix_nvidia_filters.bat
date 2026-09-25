@echo off
echo.
echo Steam DayZ + NVIDIA filters setup
echo ---------------------------------
echo 1) This sets Steam Launch Options for DayZ.
echo 2) FULLY exit Steam (tray too), reopen Steam.
echo 3) Hit Play on DayZ.
echo 4) When in menu: click DayZ window, Alt+F3 for filters.
echo.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0fix_nvidia_filters.ps1" -SetSteamLaunchOptions
echo.
pause
