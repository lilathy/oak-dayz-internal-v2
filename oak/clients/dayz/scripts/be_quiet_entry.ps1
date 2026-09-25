# BattlEye soak with quiet-first kernel entry (PeekMessage hook / hijack / APC, RtlThread last).
# Stages builds, starts oak_loader WITHOUT -Wait (fixes loader/DayZ deadlock), launches DayZ_BE.

param(
    [int]$SoakSeconds = 90,
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -SoakSeconds $SoakSeconds$(if($NoLaunch){' -NoLaunch'})" -Wait
    exit $LASTEXITCODE
}

$Dest = "C:\oak"
# This script lives in <repo>\oak\scripts\ → project root is parent of scripts
$OakRoot = Split-Path $PSScriptRoot -Parent
$DriverSys = Join-Path $OakRoot "driver\bin\oak.sys"
$Bin = Join-Path $OakRoot "bin"
$DllBuild = Join-Path $OakRoot "build\Release\dayz_internal.dll"
$LogImgui = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$BeExe = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_BE.exe"
$DzDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
$Steam = "C:\Program Files (x86)\Steam\steam.exe"

if (-not (Test-Path $DriverSys)) {
    $DriverSys = Join-Path $Bin "oak.sys"
}
if (-not (Test-Path $DllBuild)) {
    $DllBuild = Join-Path $Bin "dayz_internal.dll"
}

Write-Host "=== BE quiet-entry soak ===" -ForegroundColor Cyan

Get-Process DayZ_x64, DayZ_BE, DayZLauncher, oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep 3

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item $DriverSys (Join-Path $Dest "oak.sys") -Force
Copy-Item (Join-Path $Bin "oak_loader.exe") (Join-Path $Dest "oak_loader.exe") -Force
$dll = if (Test-Path $DllBuild) { $DllBuild } else { Join-Path $Bin "dayz_internal.dll" }
Copy-Item $dll (Join-Path $Dest "dayz_internal.dll") -Force
Remove-Item (Join-Path $Dest "inject.log"), (Join-Path $Dest "dll_attach.flag"), (Join-Path $Dest "imgui_init.flag"), $LogImgui -Force -EA SilentlyContinue

Write-Host "Starting oak_loader (no wait)..."
Start-Process -FilePath (Join-Path $Dest "oak_loader.exe") -WorkingDirectory $Dest -WindowStyle Hidden
Start-Sleep 4
if (Test-Path (Join-Path $Dest "loader.log")) {
    Get-Content (Join-Path $Dest "loader.log") -Tail 8
}
if (Test-Path (Join-Path $Dest "inject.log")) {
    $inj0 = Get-Content (Join-Path $Dest "inject.log") -Raw
    if ($inj0 -match "refusing duplicate") {
        Write-Host "Inject lock still held — abort. Reboot or wait for prior driver thread." -ForegroundColor Red
        exit 3
    }
}

if (-not $NoLaunch) {
    if (-not (Get-Process steam -EA SilentlyContinue)) {
        Write-Host "Starting Steam..."
        Start-Process $Steam
        Start-Sleep 10
    }
    Write-Host 'Launching DayZ_BE.exe with BattlEye ON...'
    Get-Process DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    if (Test-Path $BeExe) {
        Start-Process $BeExe -WorkingDirectory $DzDir
    } else {
        Start-Process $Steam -ArgumentList "-applaunch", "221100"
    }
}

$deadline = (Get-Date).AddSeconds(360)
$lastLen = 0
$mapped = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep 4
    $inj = if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Raw } else { "" }
    if ($inj.Length -gt $lastLen) {
        ($inj.Substring($lastLen)) -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { Write-Host $_ }
        $lastLen = $inj.Length
    }
    $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
    $flag = Test-Path (Join-Path $Dest "dll_attach.flag")

    if ($inj -match "SUCCESS via |DllMain OK|INJECTION COMPLETE|RtlThread: DllMain OK|Hook: DllMain OK") {
        $mapped = $true
        Write-Host "MAP/EXEC signal — soaking ${SoakSeconds}s..." -ForegroundColor Green
        break
    }
    if ($inj -match "ALL paths failed|Mapping failed|died \d+ seconds after inject") {
        Write-Host "FAIL early" -ForegroundColor Red
        Get-Content (Join-Path $Dest "inject.log") -Tail 40
        exit 1
    }
    if (-not $alive -and $inj -match "Mapping complete|SUCCESS via |INJECTION COMPLETE") {
        Write-Host "DayZ died before map complete" -ForegroundColor Red
        exit 1
    }
}

if (-not $mapped) {
    Write-Host "TIMEOUT waiting for inject" -ForegroundColor Red
    if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Tail 40 }
    exit 4
}

$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $SoakSeconds) {
    Start-Sleep 5
    $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
    $flag = Test-Path (Join-Path $Dest "dll_attach.flag")
    $imgui = if (Test-Path $LogImgui) { Get-Content $LogImgui -Raw } else { "" }
    $secs = [int](((Get-Date) - $t0).TotalSeconds)
    Write-Host ("[{0}s] alive={1} flag={2} present={3}" -f $secs, $alive, $flag, ($imgui -match "present hooked|present#"))
    if (-not $alive) {
        Write-Host "DayZ died during soak" -ForegroundColor Red
        Get-Content (Join-Path $Dest "inject.log") -Tail 20 -EA SilentlyContinue
        Get-Content $LogImgui -Tail 20 -EA SilentlyContinue
        exit 1
    }
}

$imgui = if (Test-Path $LogImgui) { Get-Content $LogImgui -Raw } else { "" }
Write-Host "=== RESULT ===" -ForegroundColor Cyan
Write-Host "alive=$([bool](Get-Process DayZ_x64 -EA SilentlyContinue)) flag=$(Test-Path (Join-Path $Dest 'dll_attach.flag'))"
Write-Host "--- inject (exec path) ---"
Select-String -Path (Join-Path $Dest "inject.log") -Pattern "Exec\[|SUCCESS via|Hook:|Hijack:|APC:|SpecialAPC|RtlThread|INJECTION COMPLETE|died" -EA SilentlyContinue | ForEach-Object { $_.Line }
Write-Host "--- imgui tail ---"
Get-Content $LogImgui -Tail 25 -EA SilentlyContinue

if ($imgui -match "present hooked|present#") {
    Write-Host "BE QUIET ENTRY: Present alive" -ForegroundColor Green
    exit 0
}
if (Test-Path (Join-Path $Dest "dll_attach.flag")) {
    Write-Host "BE QUIET ENTRY: beacon ok, Present not confirmed yet" -ForegroundColor Yellow
    exit 0
}
Write-Host "BE QUIET ENTRY: unclear" -ForegroundColor Yellow
exit 5
