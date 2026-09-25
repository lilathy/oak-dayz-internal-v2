# Steam DayZ + NVIDIA Freestyle
# Keeps overlay ALIVE at launch (required for "supported game" / Alt+F3).
# Only clears saved DayZ filter auto-apply (that is what crashes nvppex at D3D init).
#
# Steam Launch Options:
#   C:/oak/dayz/steam_wrap.cmd %command%

[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$SteamWrap,
    [switch]$SetSteamLaunchOptions,
    [switch]$ClearSteamLaunchOptions,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CommandArgs
)

$ErrorActionPreference = "Continue"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_nvidia_wrap.log"
function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $m
    try { Add-Content -LiteralPath $LogPath -Value $line } catch {}
    Write-Host $line
}

$overlayExe = "C:\Program Files\NVIDIA Corporation\NVIDIA App\CEF\NVIDIA Overlay.exe"
$wrapCmd = "C:\oak\dayz\steam_wrap.cmd"
$share = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\ShareSettings.json"
$idb = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\CefCache\Default\IndexedDB\https_nvfile_0.indexeddb.leveldb"
$ls = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\CefCache\Default\Local Storage\leveldb"

function Stop-OverlayOnly {
    Get-Process "NVIDIA Overlay" -ErrorAction SilentlyContinue | ForEach-Object {
        try { Stop-Process -Id $_.Id -Force } catch {}
    }
}

function Start-Overlay {
    try { Start-Service NvContainerLocalSystem -ErrorAction SilentlyContinue } catch {}
    Start-Sleep -Seconds 1
    if (-not (Get-Process "NVIDIA Overlay" -ErrorAction SilentlyContinue)) {
        if (Test-Path -LiteralPath $overlayExe) {
            Start-Process -FilePath $overlayExe | Out-Null
            Log "Overlay started"
        }
    }
    # Give Freestyle time to come up before DayZ creates D3D
    Start-Sleep -Seconds 3
}

function Clear-DayZFilterAutoApply {
    # Stop overlay so LevelDB unlocks, wipe Freestyle slot storage, then bring overlay back
    # BEFORE DayZ starts so hooks attach and Freestyle sees a supported game.
    Stop-OverlayOnly
    Start-Sleep -Milliseconds 600
    foreach ($dir in @($idb, $ls)) {
        if (Test-Path -LiteralPath $dir) {
            Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue |
                Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
            Log ("cleared filter cache: " + (Split-Path $dir -Leaf))
        }
    }
    if (Test-Path -LiteralPath $share) {
        try {
            $j = Get-Content -LiteralPath $share -Raw | ConvertFrom-Json
            if ($null -ne $j.settings.video) {
                $j.settings.video.irEnabled = $false
                $utf8 = New-Object System.Text.UTF8Encoding $false
                [System.IO.File]::WriteAllText($share, ($j | ConvertTo-Json -Depth 12), $utf8)
                Log "Instant Replay OFF"
            }
        } catch { Log ("ShareSettings: " + $_.Exception.Message) }
    }
}

function Set-SteamLaunchOptions([bool]$enable) {
    $opt = if ($enable) { '"' + $wrapCmd + '" %command%' } else { "" }
    $userdata = "C:\Program Files (x86)\Steam\userdata"
    $optLine = "						`"LaunchOptions`"		`"C:/oak/dayz/steam_wrap.cmd %command%`"`r`n"
    $n = 0
    foreach ($dir in @(Get-ChildItem $userdata -Directory -ErrorAction SilentlyContinue)) {
        $lc = Join-Path $dir.FullName "config\localconfig.vdf"
        if (-not (Test-Path -LiteralPath $lc)) { continue }
        $raw = [IO.File]::ReadAllText($lc)
        if ($raw -notmatch '"221100"') { continue }
        $raw = [regex]::Replace($raw, '(?m)^\s*"LaunchOptions"\s*".*\r?\n', '')
        if ($enable) {
            $appsIdx = $raw.IndexOf('"apps"')
            if ($appsIdx -lt 0) { continue }
            $pos = $raw.IndexOf('"221100"', $appsIdx)
            if ($pos -lt 0) { continue }
            $brace = $raw.IndexOf('{', $pos)
            $insertAt = $brace + 1
            if ($insertAt -lt $raw.Length -and $raw[$insertAt] -eq "`r") { $insertAt++ }
            if ($insertAt -lt $raw.Length -and $raw[$insertAt] -eq "`n") { $insertAt++ }
            $raw = $raw.Insert($insertAt, $optLine)
        }
        [IO.File]::WriteAllText($lc, $raw)
        Log ("Steam options enable=$enable userdata=" + $dir.Name)
        $n++
    }
    if ($n -lt 1) { throw "No Steam accounts patched." }
}

if ($SetSteamLaunchOptions) { Set-SteamLaunchOptions $true; Log "Exit Steam fully, reopen, Play DayZ."; return }
if ($ClearSteamLaunchOptions) { Set-SteamLaunchOptions $false; return }
if (-not $SteamWrap) { Log "Need -SteamWrap from Steam launch options."; return }

$gameArgs = @($CommandArgs)
if ($gameArgs.Count -lt 1) { $gameArgs = @($args) }
Log ("SteamWrap args=" + ($gameArgs -join " | "))
if ($gameArgs.Count -lt 1) { Log "ERROR no args"; exit 1 }

# 1) Clear saved filters (crash cause)
Clear-DayZFilterAutoApply
# 2) Start overlay AGAIN so DayZ launches as a detected supported game
Start-Overlay
Log "Overlay ready - launching DayZ with Freestyle hookable (filters OFF until you enable)"

$exe = $gameArgs[0].Trim('"')
$rest = @()
if ($gameArgs.Count -gt 1) { $rest = $gameArgs[1..($gameArgs.Count - 1)] }
if (-not (Test-Path -LiteralPath $exe)) { Log ("missing " + $exe); exit 1 }

$workDir = Split-Path -Parent $exe
try {
    if ($rest.Count -gt 0) {
        $proc = Start-Process -FilePath $exe -ArgumentList $rest -WorkingDirectory $workDir -PassThru
    } else {
        $proc = Start-Process -FilePath $exe -WorkingDirectory $workDir -PassThru
    }
    Log ("started pid=" + $proc.Id)
} catch {
    Log ("start failed: " + $_.Exception.Message)
    exit 1
}

# Wait for DayZ_x64; do NOT kill NVIDIA (that breaks Freestyle detection)
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    if (Get-Process DayZ_x64 -ErrorAction SilentlyContinue) { break }
    if ($proc.HasExited -and -not (Get-Process DayZ_BE, DayZLauncher, DayZ_x64 -ErrorAction SilentlyContinue)) {
        Log "launcher exited before DayZ_x64"
        exit 2
    }
    Start-Sleep -Seconds 1
}

$dz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dz) { Log "DayZ_x64 never appeared"; exit 3 }

# Give Present/hook a moment, then nudge overlay restart so Freestyle binds to live session
Start-Sleep -Seconds 8
if (-not (Get-Process DayZ_x64 -ErrorAction SilentlyContinue)) {
    Log "DayZ died during early init (likely filter still applied). Clear filters in NVIDIA App and retry."
    exit 4
}

Stop-OverlayOnly
Start-Sleep -Seconds 1
Start-Overlay
Log ("DayZ pid=" + $dz.Id + " - FOCUS the DayZ window, wait 3s, then Alt+F3")
Log "If filters say unsupported: Alt+Tab away and back to DayZ, then Alt+Z -> Game Filter"
exit 0
