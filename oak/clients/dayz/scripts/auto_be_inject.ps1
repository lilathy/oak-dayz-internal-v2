# Autonomous BattlEye inject loop. Run elevated (self-elevates).
# Retries until DayZ stays alive with present hooked, or max attempts.
#
# SAFETY: disabled by default after a kernel BSOD (0x1E) during instrumentation
# inject. Pass -IUnderstandThisCanBsod to actually run.

param(
    [int]$MaxAttempts = 5,
    [switch]$IUnderstandThisCanBsod
)

$ErrorActionPreference = "Stop"
if (-not $IUnderstandThisCanBsod) {
    Write-Host "auto_be_inject.ps1 is DISABLED (two prior runs bugchecked the machine)." -ForegroundColor Red
    Write-Host "Prefer be_single_shot.ps1 with dual switches after reviewing inject.log." -ForegroundColor Yellow
    Write-Host "Re-enable only with: -IUnderstandThisCanBsod" -ForegroundColor Yellow
    exit 2
}
$Dest = "C:\oak\dayz"
$OakRoot = (Resolve-Path (Join-Path $PSScriptRoot ..\..\..)).Path
$DayzRoot = (Resolve-Path (Join-Path $PSScriptRoot ..)).Path
$Bin = Join-Path $OakRoot "bin"
$DriverSys = Join-Path $DayzRoot "driver\bin\oak.sys"
$DllBuild = Join-Path $DayzRoot "build\Release\dayz_internal.dll"
$LogImgui = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$Steam = "C:\Program Files (x86)\Steam\steam.exe"

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -MaxAttempts $MaxAttempts" -Wait
    exit $LASTEXITCODE
}

function Stop-DayZAll {
    Get-Process DayZ_x64, DayZ_BE, DayZLauncher, oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep 2
}

function Stage-Files {
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Copy-Item $DriverSys (Join-Path $Dest "oak.sys") -Force
    Copy-Item (Join-Path $Bin "oak_loader.exe") (Join-Path $Dest "oak_loader.exe") -Force
    $dll = if (Test-Path $DllBuild) { $DllBuild } else { Join-Path $Bin "dayz_internal.dll" }
    Copy-Item $dll (Join-Path $Dest "dayz_internal.dll") -Force
    Remove-Item (Join-Path $Dest "inject.log"), (Join-Path $Dest "dll_attach.flag"), $LogImgui -Force -EA SilentlyContinue
}

function Load-Driver {
    Set-Location $Dest
    $p = Start-Process -FilePath (Join-Path $Dest "oak_loader.exe") -WorkingDirectory $Dest -Wait -PassThru -WindowStyle Hidden
    $ok = (Test-Path (Join-Path $Dest "loader.log")) -and ((Get-Content (Join-Path $Dest "loader.log") -Raw) -match "result=ok")
    return @{ Exit = $p.ExitCode; Ok = $ok }
}

function Launch-DayZBE {
    if (-not (Get-Process steam -EA SilentlyContinue)) {
        Start-Process $Steam
        Start-Sleep 8
    }
    # DayZLauncher often sits on Play — launch BattlEye wrapper directly.
    $be = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_BE.exe"
    $dz = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
    Get-Process DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    if (Test-Path $be) {
        Start-Process $be -WorkingDirectory $dz
    } else {
        Start-Process $Steam -ArgumentList "-applaunch", "221100"
    }
}

function Wait-InjectResult([int]$TimeoutSec = 240) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $sawMap = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 3
        $inj = ""
        if (Test-Path (Join-Path $Dest "inject.log")) {
            $inj = Get-Content (Join-Path $Dest "inject.log") -Raw -EA SilentlyContinue
        }
        if ($inj -match "Hijack: DllMain OK|Hook: DllMain OK|RtlThread: DllMain OK|APC: DllMain OK|SpecialAPC: DllMain OK|DllMain should have run") { $sawMap = $true }
        if ($inj -match "died \d+ seconds after inject") {
            return @{ Status = "killed"; Log = $inj }
        }
        if ($inj -match "INJECTION COMPLETE") {
            # wait for imgui / beacon
            for ($i = 0; $i -lt 50; $i++) {
                Start-Sleep 2
                $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
                $flag = Test-Path (Join-Path $Dest "dll_attach.flag")
                $imgui = ""
                if (Test-Path $LogImgui) { $imgui = Get-Content $LogImgui -Raw -EA SilentlyContinue }
                if (-not $alive) { return @{ Status = "killed_after_complete"; Flag = $flag; Imgui = $imgui } }
                if ($imgui -match "present hooked") {
                    return @{ Status = "success"; Flag = $flag; Imgui = $imgui }
                }
                if ($flag -and $alive -and $i -gt 20 -and $imgui -match "BE detected|dll attach|main thread") {
                    # hooked delay still running
                    continue
                }
            }
            $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
            return @{
                Status = $(if ($alive) { "alive_no_hook" } else { "killed_late" })
                Flag = (Test-Path (Join-Path $Dest "dll_attach.flag"))
                Imgui = $(if (Test-Path $LogImgui) { Get-Content $LogImgui -Raw } else { "" })
            }
        }
        if ($inj -match "refusing duplicate map") {
            return @{ Status = "lock_held"; Log = $inj }
        }
    }
    return @{ Status = "timeout"; SawMap = $sawMap; Log = $(if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Tail 30 | Out-String } else { "" }) }
}

Write-Host "=== Autonomous BE inject loop (max $MaxAttempts) ===" -ForegroundColor Cyan

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    Write-Host "`n===== ATTEMPT $attempt / $MaxAttempts =====" -ForegroundColor Yellow
    Stop-DayZAll
    Stage-Files
    Write-Host "Loading driver..."
    $ld = Load-Driver
    Write-Host "loader exit=$($ld.Exit) ok=$($ld.Ok)"
    if (-not $ld.Ok) {
        Write-Host "Driver map failed - check Defender/blocklist" -ForegroundColor Red
        Start-Sleep 5
        continue
    }
    Write-Host "Launching DayZ via Steam (BattlEye)..."
    Launch-DayZBE
    $res = Wait-InjectResult 300
    Write-Host "Result: $($res.Status)" -ForegroundColor Cyan
    if ($res.Log) { Write-Host ($res.Log.Substring([Math]::Max(0, $res.Log.Length - 800))) }
    if ($res.Imgui) { Write-Host "imgui tail:"; ($res.Imgui -split "`n") | Select-Object -Last 15 | ForEach-Object { Write-Host $_ } }
    if (Test-Path (Join-Path $Dest "dll_attach.flag")) { Write-Host "dll_attach.flag: YES" -ForegroundColor Green }
    else { Write-Host "dll_attach.flag: NO" }

    if ($res.Status -eq "success") {
        Write-Host "SUCCESS: DayZ alive with Present hooked under BE" -ForegroundColor Green
        "success attempt=$attempt $(Get-Date -Format o)" | Set-Content (Join-Path $Dest "be_success.txt")
        exit 0
    }
    if ($res.Status -eq "alive_no_hook") {
        Write-Host "Partial: process alive, waiting more for hook..." -ForegroundColor Yellow
        Start-Sleep 45
        if ((Get-Process DayZ_x64 -EA SilentlyContinue) -and (Test-Path $LogImgui) -and ((Get-Content $LogImgui -Raw) -match "present hooked")) {
            Write-Host "SUCCESS after extra wait" -ForegroundColor Green
            exit 0
        }
    }
    Write-Host "Retrying..." -ForegroundColor Yellow
    Stop-DayZAll
    Start-Sleep 5
}

Write-Host "FAILED after $MaxAttempts attempts" -ForegroundColor Red
exit 1

