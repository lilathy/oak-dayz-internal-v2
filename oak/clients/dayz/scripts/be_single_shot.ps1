# Single cautious BattlEye inject attempt.
# DISABLED after two host BSODs (instrumentation + SpecialAPC/SMAP).
# Pass BOTH switches to run.

param(
    [switch]$IUnderstandThisCanBsod,
    [switch]$IAcceptProcessKillOnlyPath
)

$ErrorActionPreference = "Stop"
if (-not $IUnderstandThisCanBsod -or -not $IAcceptProcessKillOnlyPath) {
    Write-Host @"
be_single_shot.ps1 is LOCKED after two BSODs on this PC.
Current driver path uses RtlCreateUserThread only (may kill DayZ under BE, should not bugcheck).
To run: -IUnderstandThisCanBsod -IAcceptProcessKillOnlyPath
"@ -ForegroundColor Red
    exit 2
}

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -IUnderstandThisCanBsod -IAcceptProcessKillOnlyPath" -Wait
    exit $LASTEXITCODE
}

$Dest = "C:\oak\dayz"
$OakRoot = (Resolve-Path (Join-Path $PSScriptRoot ..\..\..)).Path
$DayzRoot = (Resolve-Path (Join-Path $PSScriptRoot ..)).Path
$DriverSys = Join-Path $DayzRoot "driver\bin\oak.sys"
$Bin = Join-Path $OakRoot "bin"
$DllBuild = Join-Path $DayzRoot "build\Release\dayz_internal.dll"
$LogImgui = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$BeExe = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_BE.exe"
$DzDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"

Write-Host "=== BE single-shot (Special User APC path) ===" -ForegroundColor Cyan

Get-Process DayZ_x64, DayZ_BE, DayZLauncher, oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep 2

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item $DriverSys (Join-Path $Dest "oak.sys") -Force
Copy-Item (Join-Path $Bin "oak_loader.exe") (Join-Path $Dest "oak_loader.exe") -Force
$dll = if (Test-Path $DllBuild) { $DllBuild } else { Join-Path $Bin "dayz_internal.dll" }
Copy-Item $dll (Join-Path $Dest "dayz_internal.dll") -Force
Remove-Item (Join-Path $Dest "inject.log"), (Join-Path $Dest "dll_attach.flag"), $LogImgui -Force -EA SilentlyContinue

Write-Host "Loading driver..."
Set-Location $Dest
$p = Start-Process -FilePath (Join-Path $Dest "oak_loader.exe") -WorkingDirectory $Dest -Wait -PassThru -WindowStyle Hidden
$loaderOk = (Test-Path (Join-Path $Dest "loader.log")) -and ((Get-Content (Join-Path $Dest "loader.log") -Raw) -match "result=ok")
Write-Host "loader exit=$($p.ExitCode) ok=$loaderOk"
if (-not $loaderOk) { Write-Host "Driver map failed"; exit 1 }

Write-Host "Launching DayZ_BE.exe..."
Get-Process DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Process $BeExe -WorkingDirectory $DzDir

$deadline = (Get-Date).AddSeconds(300)
$lastTail = ""
while ((Get-Date) -lt $deadline) {
    Start-Sleep 3
    $inj = if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Raw -EA SilentlyContinue } else { "" }
    if ($inj.Length -gt $lastTail.Length) {
        ($inj.Substring($lastTail.Length)) -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { Write-Host $_ }
        $lastTail = $inj
    }
    if ($inj -match "died \d+ seconds after inject|ERROR: Mapping failed|All DllMain exec paths failed") {
        Write-Host "FAILED (see inject.log)" -ForegroundColor Red
        Get-Content (Join-Path $Dest "inject.log") -Tail 25
        exit 1
    }
    if ($inj -match "SpecialAPC: DllMain OK|INJECTION COMPLETE") {
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep 2
            $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
            $flag = Test-Path (Join-Path $Dest "dll_attach.flag")
            $imgui = if (Test-Path $LogImgui) { Get-Content $LogImgui -Raw -EA SilentlyContinue } else { "" }
            if (-not $alive) { Write-Host "DayZ died after map"; exit 1 }
            if ($imgui -match "present hooked") {
                Write-Host "SUCCESS: alive + present hooked under BE" -ForegroundColor Green
                "success $(Get-Date -Format o)" | Set-Content (Join-Path $Dest "be_success.txt")
                exit 0
            }
            if ($flag) { Write-Host "dll_attach.flag present, waiting for present hook..." }
        }
        Write-Host "Mapped but no present hook yet; DayZ still alive=$(Test-Path (Get-Process DayZ_x64 -EA SilentlyContinue))"
        exit 3
    }
}

Write-Host "TIMEOUT waiting for inject" -ForegroundColor Yellow
if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Tail 30 }
exit 4
