# Setup / repair local DayZ dedicated server without BattlEye.
# 1) Ensures DayZ Server (Steam app 223350) is installed
# 2) Patches DayZServer_x64.exe (BE init + VAC checks)
# 3) Renames battleye folder so the DLL cannot load
# 4) Writes Oak serverDZ.cfg

$ErrorActionPreference = "Stop"
$Oak = Split-Path $PSScriptRoot -Parent
$ServerRoot = "C:\Program Files (x86)\Steam\steamapps\common\DayZServer"
$Exe = Join-Path $ServerRoot "DayZServer_x64.exe"
$Patcher = Join-Path $Oak "bin\OakDayZServerBEPatcher.exe"
$CfgSrc = Join-Path $Oak "server\serverDZ.cfg"

Write-Host "Oak local NoBE server setup" -ForegroundColor Cyan

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host "Installing DayZ Server via Steam (app 223350)..." -ForegroundColor Yellow
    Start-Process "steam://install/223350"
    for ($i = 0; $i -lt 120; $i++) {
        if (Test-Path -LiteralPath $Exe) { break }
        Start-Sleep -Seconds 5
        Write-Host " waiting... $($i*5)s"
    }
    if (-not (Test-Path -LiteralPath $Exe)) {
        throw "DayZ Server install not found. Install 'DayZ Server' from Steam Library -> Tools, then re-run."
    }
}

Get-Process DayZServer_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

Write-Host "Patching server binary..." -ForegroundColor Yellow
& $Patcher $Exe
if ($LASTEXITCODE -ne 0) { throw "Patch failed: $LASTEXITCODE" }

$be = Join-Path $ServerRoot "battleye"
if (Test-Path -LiteralPath $be) {
    $beOff = Join-Path $ServerRoot "battleye_disabled"
    if (Test-Path -LiteralPath $beOff) { Remove-Item -LiteralPath $beOff -Recurse -Force }
    Rename-Item -LiteralPath $be -NewName "battleye_disabled"
    Write-Host "battleye folder disabled"
}

Copy-Item -Force -LiteralPath $CfgSrc -Destination (Join-Path $ServerRoot "serverDZ.cfg")
New-Item -ItemType Directory -Force -Path (Join-Path $ServerRoot "oak_profiles") | Out-Null

Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Start:  powershell -File oak\scripts\start_local_server.ps1"
Write-Host "Join:   powershell -File oak\scripts\join_local_server.ps1"
