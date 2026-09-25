# Validate kernel inject WITHOUT BattlEye (no BSOD-risk techniques).
# Loads oak.sys, launches DayZ_x64 -noBattlEye, waits for Done/imgui.

param([switch]$Force)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Wait
    exit $LASTEXITCODE
}

$Dest = "C:\oak\dayz"
$OakRoot = (Resolve-Path (Join-Path $PSScriptRoot ..\..\..)).Path
$DayzRoot = (Resolve-Path (Join-Path $PSScriptRoot ..)).Path
$DriverSys = Join-Path $DayzRoot "driver\bin\oak.sys"
$Bin = Join-Path $OakRoot "bin"
$DllBuild = Join-Path $DayzRoot "build\Release\dayz_internal.dll"
$LogImgui = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$Dz = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"

Write-Host "=== NoBE kernel inject validation ===" -ForegroundColor Cyan
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
$ok = (Test-Path (Join-Path $Dest "loader.log")) -and ((Get-Content (Join-Path $Dest "loader.log") -Raw) -match "result=ok")
Write-Host "loader ok=$ok exit=$($p.ExitCode)"
if (-not $ok) { exit 1 }

Write-Host "Launching DayZ via Steam (-applaunch 221100 -noBattlEye)..."
Start-DayZNoBattlEye -Log { param($m) Write-Host $m }

$deadline = (Get-Date).AddSeconds(240)
$last = ""
while ((Get-Date) -lt $deadline) {
    Start-Sleep 3
    $inj = if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Raw } else { "" }
    if ($inj.Length -gt $last.Length) {
        ($inj.Substring($last.Length)) -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { Write-Host $_ }
        $last = $inj
    }
    $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
    if ($inj -match "Hook: DllMain OK|RtlThread: DllMain OK|INJECTION COMPLETE") {
        Start-Sleep 8
        $alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
        $flag = Test-Path (Join-Path $Dest "dll_attach.flag")
        $imgui = if (Test-Path $LogImgui) { Get-Content $LogImgui -Raw } else { "" }
        Write-Host "alive=$alive flag=$flag"
        if ($imgui) { Write-Host "imgui:"; ($imgui -split "`n") | Select-Object -Last 12 | ForEach-Object { Write-Host $_ } }
        if ($alive -and ($imgui -match "present hooked|dll attach|main thread|BE detected" -or $flag)) {
            Write-Host "NoBE KERNEL PATH OK" -ForegroundColor Green
            exit 0
        }
        if ($alive) { Write-Host "Injected-ish, still alive (partial)"; exit 0 }
        Write-Host "Died after inject"; exit 1
    }
    if ($inj -match "Mapping failed|RtlThread: timeout|Hook: timeout|Hook: exception|refusing") {
        Write-Host "FAIL"; exit 1
    }
    if (-not $alive -and $inj -match "Found") {
        Write-Host "DayZ exited early"; exit 1
    }
}
Write-Host "TIMEOUT"
if (Test-Path (Join-Path $Dest "inject.log")) { Get-Content (Join-Path $Dest "inject.log") -Tail 40 }
exit 4
