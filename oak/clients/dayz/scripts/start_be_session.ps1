# Stage latest builds to C:\oak, load oak.sys, then launch DayZ WITH BattlEye.
# Run from an elevated PowerShell (or this script will re-launch itself elevated).

param(
    [switch]$SkipLaunch,
    [switch]$SkipLoader
)

$ErrorActionPreference = "Stop"
$OakRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $OakRoot "bin"
$Dest = "C:\oak"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host "Re-launching elevated..." -ForegroundColor Yellow
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($SkipLaunch) { $arg += " -SkipLaunch" }
    if ($SkipLoader) { $arg += " -SkipLoader" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $arg -Wait
    exit $LASTEXITCODE
}

Write-Host "=== Oak BattlEye session ===" -ForegroundColor Cyan

$bl = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config" -Name VulnerableDriverBlocklistEnable -EA SilentlyContinue).VulnerableDriverBlocklistEnable
Write-Host "VulnerableDriverBlocklistEnable=$bl"
if ($bl -ne 0) {
    Write-Host "ERROR: set VulnerableDriverBlocklistEnable=0 and reboot first" -ForegroundColor Red
    exit 1
}

# Stop NoBE / leftover clients so the driver attaches to a fresh BE session
Get-Process DayZ_x64, DayZ_BE, DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep 2

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# Prefer newest dayz_internal from Release build, else bin
$dllCandidates = @(
    (Join-Path $OakRoot "build\Release\dayz_internal.dll"),
    (Join-Path $Bin "dayz_internal.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) { throw "dayz_internal.dll not found - build first" }

$sys = Join-Path $Bin "oak.sys"
if (-not (Test-Path $sys)) { $sys = Join-Path $OakRoot "driver\bin\oak.sys" }
if (-not (Test-Path $sys)) { throw "oak.sys missing - run build_driver_wdk.ps1" }

$loader = Join-Path $Bin "oak_loader.exe"
if (-not (Test-Path $loader)) { throw "oak_loader.exe missing" }

Copy-Item $dll (Join-Path $Dest "dayz_internal.dll") -Force
Copy-Item $sys (Join-Path $Dest "oak.sys") -Force
Copy-Item $loader (Join-Path $Dest "oak_loader.exe") -Force
Write-Host "Staged:"
Get-ChildItem $Dest\dayz_internal.dll,$Dest\oak.sys,$Dest\oak_loader.exe | Format-Table Name,Length,LastWriteTime

if (Test-Path (Join-Path $Dest "inject.log")) {
    Remove-Item (Join-Path $Dest "inject.log") -Force
}
if (Test-Path $LogPath) { Remove-Item $LogPath -Force -EA SilentlyContinue }

if (-not $SkipLoader) {
    Write-Host "Loading driver (oak_loader)..." -ForegroundColor Cyan
    Set-Location $Dest
    $p = Start-Process -FilePath ".\oak_loader.exe" -WorkingDirectory $Dest -Wait -PassThru -NoNewWindow
    Write-Host "oak_loader exit=$($p.ExitCode)"
    if (Test-Path (Join-Path $Dest "loader.log")) {
        Get-Content (Join-Path $Dest "loader.log")
    }
    if ($p.ExitCode -ne 0) {
        Write-Host "Driver load FAILED. Check Defender quarantine / blocklist / HVCI." -ForegroundColor Red
        exit $p.ExitCode
    }
    Write-Host "DRIVER LOADED - waiting for DayZ_x64.exe" -ForegroundColor Green
}

if (-not $SkipLaunch) {
    # Prefer Steam protocol so BattlEye starts (not -noBattlEye)
    $steam = "C:\Program Files (x86)\Steam\steam.exe"
    if (-not (Test-Path $steam)) { throw "Steam not found" }
    Write-Host "Launching DayZ via Steam (BattlEye ON)..." -ForegroundColor Cyan
    Start-Process $steam -ArgumentList "-applaunch","221100"
}

Write-Host ""
Write-Host "Timeline:" -ForegroundColor Yellow
Write-Host "  ~45s  driver waits after DayZ appears, then maps DLL"
Write-Host "  +40s  cheat delays Present hook while BEClient is present"
Write-Host "  then INSERT for menu"
Write-Host "Logs:"
Write-Host "  C:\oak\inject.log"
Write-Host "  $LogPath"
Write-Host ""
Write-Host "Polling inject.log for 180s..."
$deadline = (Get-Date).AddSeconds(180)
$sawComplete = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep 5
    if (Test-Path (Join-Path $Dest "inject.log")) {
        $tail = Get-Content (Join-Path $Dest "inject.log") -Tail 8 -EA SilentlyContinue
        if ($tail) {
            Write-Host "--- inject ---"
            $tail | ForEach-Object { Write-Host $_ }
        }
        $all = Get-Content (Join-Path $Dest "inject.log") -Raw -EA SilentlyContinue
        if ($all -match "INJECTION COMPLETE") { $sawComplete = $true; break }
        if ($all -match "died .* after inject") {
            Write-Host "BE killed the process after inject." -ForegroundColor Red
            exit 2
        }
    }
    $dz = Get-Process DayZ_x64 -EA SilentlyContinue
    if (-not $dz) { Write-Host "(waiting for DayZ_x64...)" }
}
if ($sawComplete) {
    Write-Host "Injection reported COMPLETE. Waiting for oak_imgui.log..." -ForegroundColor Green
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep 2
        if (Test-Path $LogPath) {
            Get-Content $LogPath -Tail 25
            if ((Get-Content $LogPath -Raw) -match "present hooked|imgui ok|BE detected") {
                Write-Host "RESULT: PASS - cheat alive under BE path" -ForegroundColor Green
                exit 0
            }
        }
        if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) {
            Write-Host "RESULT: FAIL - DayZ exited after inject" -ForegroundColor Red
            exit 3
        }
    }
    Write-Host "RESULT: inject OK but no Present hook yet (still delaying or stuck)" -ForegroundColor Yellow
    exit 0
}
Write-Host "RESULT: timed out waiting for inject.log COMPLETE" -ForegroundColor Yellow
exit 4
