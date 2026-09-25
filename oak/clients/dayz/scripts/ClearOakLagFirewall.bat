@echo off
:: Lag switch used to add persistent Windows Firewall block rules for DayZ.
:: If DayZ crashed mid-hold, those rules stayed and blocked every server.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting Administrator to clear OakLag firewall rules...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
echo Clearing OakLagCut firewall rules...
netsh advfirewall firewall delete rule name=OakLagCut >nul 2>&1
netsh advfirewall firewall delete rule name=OakLagCutTcp >nul 2>&1
echo Done. DayZ outbound traffic is no longer blocked by OakLag.
pause
