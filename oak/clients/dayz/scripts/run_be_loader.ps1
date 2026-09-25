# Load oak.sys via kdmapper then you start DayZ WITH BattlEye.
# Requires: Admin, VulnerableDriverBlocklistEnable=0 (reboot after changing it),
#           C:\oak\{oak_loader.exe,oak.sys,dayz_internal.dll}
#
# Order:
#   1) Run this FIRST (driver waits for DayZ)
#   2) Start DayZ via Steam normally (BattlEye ON) - not -noBattlEye
#   3) Wait ~90s in menu/world, then INSERT
# Or use: .\start_be_session.ps1

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run as Administrator"
}

$bl = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config" -Name VulnerableDriverBlocklistEnable -EA SilentlyContinue).VulnerableDriverBlocklistEnable
if ($bl -ne 0) {
    Write-Host "WARNING: VulnerableDriverBlocklistEnable=$bl (need 0 + reboot)" -ForegroundColor Red
}

$Dest = "C:\oak"
foreach ($f in @("oak_loader.exe", "oak.sys", "dayz_internal.dll")) {
    if (-not (Test-Path (Join-Path $Dest $f))) {
        throw "Missing $Dest\$f - run setup_be_path.ps1 first"
    }
}

Get-Process oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Remove-Item (Join-Path $Dest "inject.log") -Force -EA SilentlyContinue
Remove-Item (Join-Path $Dest "dll_attach.flag") -Force -EA SilentlyContinue

Write-Host "Loading oak.sys via oak_loader (ONCE)..." -ForegroundColor Cyan
Set-Location $Dest
& .\oak_loader.exe
Write-Host ""
Write-Host "If DRIVER LOADED: start DayZ via Steam (BattlEye ON)." -ForegroundColor Green
Write-Host "Wait ~90s after DayZ_x64 appears, then INSERT for menu."
Write-Host "Logs: C:\oak\inject.log  and  %LOCALAPPDATA%\DayZ\oak_imgui.log"
Write-Host "Beacon: C:\oak\dll_attach.flag (proves DllMain ran)"
