@echo off
net stop NvContainerLocalSystem /y >nul 2>&1
taskkill /F /IM "NVIDIA Overlay.exe" >nul 2>&1
taskkill /F /IM nvcontainer.exe >nul 2>&1
exit /b 0
