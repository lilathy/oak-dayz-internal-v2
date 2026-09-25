@echo off
setlocal EnableExtensions
title Oak DayZ

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0StartOakDayZ.ps1"
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" (
  echo.
  echo Oak launch failed with exit code %EC%.
  pause
)
exit /b %EC%
