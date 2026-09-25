param(
    [string]$HostAddress = "127.0.0.1",
    [int]$Port = 2302,
    [string]$DayZDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $DayZDir "DayZ_x64.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "DayZ client not found: $exe" }

# Stop existing client so we can relaunch into local server
Get-Process DayZ_x64, DayZ_BE -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host "Launching DayZ client -> $HostAddress`:$Port (no BattlEye)" -ForegroundColor Cyan
Start-Process -FilePath $exe `
    -ArgumentList "-connect=$HostAddress","-port=$Port","-noBattlEye" `
    -WorkingDirectory $DayZDir
