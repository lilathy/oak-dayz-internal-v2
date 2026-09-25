# Launch DayZ (no BattlEye) into local server + usermode inject. Keeps game running.
param(
    [switch]$SkipInject
)

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($SkipInject) { $argList += "-SkipInject" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList ($argList -join " ")
    exit 0
}

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")

$Bin = Join-Path (Split-Path $PSScriptRoot -Parent) "bin"
$DayZDir = Get-DayZDir
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"

Get-Process DayZ_x64, DayZ_BE, DayZLauncher -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

$null = Wait-SteamReady -Log { param($m) Write-Host $m }
Ensure-DayZSteamAppId -DayZDir $DayZDir -Log { param($m) Write-Host $m }

Write-Host "Launching DayZ no-BE via Steam (fixes 'Steam is not running' false error)"
Start-DayZNoBattlEye -Log { param($m) Write-Host $m }

$null = Wait-DayZProcess -TimeoutSeconds 180 -Log { param($m) Write-Host $m }
$dayz = Wait-DayZWindow -TimeoutSeconds 180 -Log { param($m) Write-Host $m }

# Let D3D / main menu finish loading before we patch DXGI Present.
Write-Host "Waiting 40s for DayZ D3D to settle before inject..."
for ($i = 40; $i -gt 0; $i--) {
    if (-not (Get-Process -Id $dayz.Id -ErrorAction SilentlyContinue)) {
        Write-Host "FAIL: DayZ exited before inject window ($i s left)"
        exit 5
    }
    Start-Sleep -Seconds 1
}

$dayz = Get-Process -Id $dayz.Id -ErrorAction SilentlyContinue
if (-not $dayz) {
    Write-Host "FAIL: DayZ exited before inject"
    exit 5
}
if ($SkipInject) {
    Write-Host "DayZ launched via Steam (no inject). Press Play in launcher if needed."
    exit 0
}

$loader = Join-Path $Bin "OakImGuiOverlayLoader.exe"
$dll = Join-Path $Bin "dayz_internal.dll"
if (-not (Test-Path $loader) -or -not (Test-Path $dll)) {
    throw "Missing loader or dll in bin - build Debug OakImGuiOverlayLoader + DayZInternal first"
}

Write-Host "Injecting pid=$($dayz.Id)..."
$proc = Start-Process -FilePath $loader -ArgumentList "`"$dll`"", $dayz.Id `
    -WorkingDirectory $Bin -PassThru -Wait -NoNewWindow
Write-Host "loader exit=$($proc.ExitCode)"
if ($proc.ExitCode -ne 0) { exit 3 }

$deadline = (Get-Date).AddSeconds(120)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process DayZ_x64 -ErrorAction SilentlyContinue)) {
        Write-Host "FAIL: DayZ exited during init"
        if (Test-Path $LogPath) { Get-Content $LogPath -Tail 30 }
        exit 5
    }
    if (Test-Path $LogPath) {
        $text = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
        if ($text -match "present hooked" -and $text -match "imgui ok") {
            $ready = $true
            break
        }
        if ($text -match "protection authorization unavailable|no game window|CreateSwapChain failed") {
            Write-Host "FAIL: init error in log"
            Get-Content $LogPath -Tail 30
            exit 4
        }
    }
    Start-Sleep -Seconds 2
}

Write-Host "--- oak_imgui.log (tail) ---"
if (Test-Path $LogPath) { Get-Content $LogPath -Tail 30 } else { Write-Host "(missing)" }

if ($ready) {
    Write-Host "READY - press K in-game for Oak menu. DayZ left running."
    exit 0
}
Write-Host "WARN: inject ran but overlay markers not seen yet - check log, press K after ~10s"
exit 0
