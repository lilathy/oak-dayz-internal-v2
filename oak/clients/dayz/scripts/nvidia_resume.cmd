@echo off
net start NvContainerLocalSystem >nul 2>&1
timeout /t 2 /nobreak >nul
tasklist /FI "IMAGENAME eq NVIDIA Overlay.exe" | find /I "NVIDIA Overlay.exe" >nul
if errorlevel 1 start "" "C:\Program Files\NVIDIA Corporation\NVIDIA App\CEF\NVIDIA Overlay.exe"
exit /b 0
