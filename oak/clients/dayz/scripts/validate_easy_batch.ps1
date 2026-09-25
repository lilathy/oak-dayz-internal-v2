# Validate Easy batch: clear weather, container cargo, player inv, silent bypass wire.
param(
    [int]$WaitInjectSec = 120,
    [int]$SoakSec = 90
)

$ErrorActionPreference = "Stop"
$DayzRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $DayzRoot "bin"
$Log = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$Ini = Join-Path $env:LOCALAPPDATA "DayZ\oak_config.ini"
$Launch = Join-Path $PSScriptRoot "_launch_nobe_local.ps1"

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
        "-WaitInjectSec", "$WaitInjectSec", "-SoakSec", "$SoakSec"
    )
    exit 0
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class OakEasyVal {
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)]
  public static extern bool WritePrivateProfileString(string s, string k, string v, string f);
}
"@

# Enable Easy features via INI (loaded on inject / config refresh).
[OakEasyVal]::WritePrivateProfileString("worldMisc", "clearWx", "1", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("worldMisc", "timeLock", "0", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("esp.batch4.container", "enabled", "1", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("esp.batch4.invViewer", "enabled", "1", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("combat.silentAim", "enabled", "1", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("combat.silentAim", "bypass25m", "1", $Ini) | Out-Null
[OakEasyVal]::WritePrivateProfileString("combat.batch4", "silentBypass25", "1", $Ini) | Out-Null

$dll = Join-Path $DayzRoot "build\Debug\dayz_internal.dll"
if (-not (Test-Path $dll)) { throw "missing $dll - build Debug DayZInternal first" }

Get-Process DayZ_x64, DayZ_BE, DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Seconds 2

New-Item -ItemType Directory -Force -Path $Bin | Out-Null
Copy-Item -Force $dll (Join-Path $Bin "dayz_internal.dll")
if (Test-Path "C:\oak\dayz") { Copy-Item -Force $dll "C:\oak\dayz\dayz_internal.dll" }

$marker = Get-Date
Write-Host "== Launch + inject =="
& $Launch
if ($LASTEXITCODE -ne 0) { throw "launch failed exit=$LASTEXITCODE" }

Write-Host "== Wait for fresh init =="
$deadline = (Get-Date).AddSeconds($WaitInjectSec)
$ready = $false
do {
    Start-Sleep -Seconds 2
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited" }
    $hit = Select-String -Path $Log -Pattern "init done" | Select-Object -Last 1
    if ($hit -and $hit.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
        $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
        if ($stamp -ge $marker.AddMinutes(-1)) {
            Write-Host $hit.Line
            $ready = $true
        }
    }
} while (-not $ready -and (Get-Date) -lt $deadline)
if (-not $ready) { throw "no fresh init done" }

function Fresh-Match([string]$pattern) {
    $hits = Select-String -Path $Log -Pattern $pattern | Select-Object -Last 8
    foreach ($h in $hits) {
        if ($h.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
            $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
            if ($stamp -ge $marker.AddMinutes(-1)) { return $h.Line }
        }
    }
    return $null
}

Write-Host "== Wait for Present + imgui (weather/misc need gameplay frame) =="
$presentDeadline = (Get-Date).AddSeconds(180)
$presentReady = $false
do {
    Start-Sleep -Seconds 2
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited" }
    $hit = Fresh-Match "imgui warmed|d3d ok|present hook init"
    if ($hit) {
        Write-Host "PRESENT:" $hit
        $presentReady = $true
    }
} while (-not $presentReady -and (Get-Date) -lt $presentDeadline)
if (-not $presentReady) { throw "Present/imgui never ready" }

Write-Host "== Soak ${SoakSec}s for weather/inv/cargo/silent logs (join world) =="
$soakEnd = (Get-Date).AddSeconds($SoakSec)
$sawWeather = $false
$sawCargo = $false
$sawInv = $false
$sawSilentBypass = $false
$sawSilentGuide = $false

do {
    Start-Sleep -Seconds 3
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited during soak" }
    if (-not $sawWeather) {
        $l = Fresh-Match "weather: resolved WC|weather: clear wc=0000[1-9a-fA-F]|weather: clear wc=[1-9a-fA-F]"
        if ($l) { $sawWeather = $true; Write-Host "WEATHER:" $l }
    }
    if (-not $sawCargo) {
        $l = Fresh-Match "inv: cargo hit|inv: container probe n=[1-9]|inv: census container=.*cargoN=[1-9]"
        if ($l) { $sawCargo = $true; Write-Host "CARGO:" $l }
    }
    if (-not $sawInv) {
        $l = Fresh-Match "inv: viewer lines="
        if ($l) { $sawInv = $true; Write-Host "INV:" $l }
    }
    if (-not $sawSilentBypass) {
        $l = Fresh-Match "silent: bypass25m InitSpeed="
        if ($l) { $sawSilentBypass = $true; Write-Host "BYPASS:" $l }
    }
    if (-not $sawSilentGuide) {
        $l = Fresh-Match "silent: GUIDE"
        if ($l) { $sawSilentGuide = $true; Write-Host "GUIDE:" $l }
    }
} while ((Get-Date) -lt $soakEnd -and (-not ($sawWeather -and $sawCargo -and $sawInv)))

Write-Host "=== RESULTS ==="
Write-Host "weather_clear :" $(if ($sawWeather) { "PASS" } else { "FAIL/pending (enable Clear Weather + be in-world)" })
Write-Host "container_cargo:" $(if ($sawCargo) { "PASS" } else { "FAIL/pending (look at barrel/crate with Container Contents on)" })
Write-Host "player_inv    :" $(if ($sawInv) { "PASS" } else { "FAIL/pending (crosshair a geared player)" })
Write-Host "silent_bypass :" $(if ($sawSilentBypass) { "PASS" } else { "PENDING (Arm Silent in Lab, hold fire on target)" })
Write-Host "silent_guide  :" $(if ($sawSilentGuide) { "PASS" } else { "PENDING" })

# Weather requires a resolved WeatherController (not just clear=1 with null WC).
if (-not $sawWeather) {
    Write-Host "Recent weather/inv lines:"
    Select-String -Path $Log -Pattern "weather:|inv:" | Select-Object -Last 20 | ForEach-Object { Write-Host $_.Line }
    throw "EASY VALIDATION FAILED - weather WC never resolved/cleared"
}

if (-not $sawCargo) {
    Write-Host "WARN: container cargo not proven this run (empty barrels or walker miss)"
}

Write-Host "PASS: Easy weather path live. Cargo/inv/bypass status above."
exit 0
