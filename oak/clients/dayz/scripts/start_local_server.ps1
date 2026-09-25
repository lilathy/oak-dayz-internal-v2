param(
    [int]$Port = 2302,
    [string]$ServerRoot = "C:\Program Files (x86)\Steam\steamapps\common\DayZServer"
)

$ErrorActionPreference = "Stop"
$Oak = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $Oak "bin"
$CfgSrc = Join-Path $Oak "server\serverDZ.cfg"
$Patcher = Join-Path $Bin "OakDayZServerBEPatcher.exe"
$Exe = Join-Path $ServerRoot "DayZServer_x64.exe"
$Profiles = Join-Path $ServerRoot "oak_profiles"

function Write-Info([string]$m) { Write-Host "[OakServer] $m" -ForegroundColor Cyan }

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host "DayZ Server not found at $ServerRoot" -ForegroundColor Red
    Write-Host "Run oak\scripts\setup_local_server.ps1 first (or install DayZ Server from Steam Tools)." -ForegroundColor Yellow
    Start-Process "steam://install/223350"
    exit 1
}

Get-Process DayZServer_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$needsPatch = $true
try {
    $bytes = [IO.File]::ReadAllBytes($Exe)
    if ([Text.Encoding]::ASCII.GetString($bytes).Contains("OakNoBE")) { $needsPatch = $false }
} catch {}

if ($needsPatch) {
    Write-Info "Patching BattlEye out of DayZServer_x64.exe..."
    & $Patcher $Exe
    if ($LASTEXITCODE -ne 0) { throw "BE patcher failed ($LASTEXITCODE)" }
}

$be = Join-Path $ServerRoot "battleye"
if (Test-Path -LiteralPath $be) {
    $beOff = Join-Path $ServerRoot "battleye_disabled"
    if (Test-Path -LiteralPath $beOff) { Remove-Item -LiteralPath $beOff -Recurse -Force }
    Rename-Item -LiteralPath $be -NewName "battleye_disabled"
    Write-Info "Disabled battleye folder"
}

New-Item -ItemType Directory -Force -Path $Profiles | Out-Null
Copy-Item -Force -LiteralPath $CfgSrc -Destination (Join-Path $ServerRoot "serverDZ.cfg")
$MissionInitSrc = Join-Path $Oak "server\mpmissions\dayzOffline.chernarusplus\init.c"
$MissionInitDst = Join-Path $ServerRoot "mpmissions\dayzOffline.chernarusplus\init.c"
if (Test-Path -LiteralPath $MissionInitSrc) {
    New-Item -ItemType Directory -Force -Path (Split-Path $MissionInitDst) | Out-Null
    Copy-Item -Force -LiteralPath $MissionInitSrc -Destination $MissionInitDst
    Write-Info "Mission init.c deployed (kits + Lockpick + pre-lock at last pos, no TP)"
}
Write-Info "Config ready: Oak Local NoBE"

# DayZ -profiles breaks on unquoted paths with spaces. Use 8.3 short path.
$ProfilesArg = $Profiles
try {
    $fso = New-Object -ComObject Scripting.FileSystemObject
    $ProfilesArg = $fso.GetFolder($Profiles).ShortPath
    Write-Info "profiles short path: $ProfilesArg"
} catch {
    Write-Info "profiles short path unavailable; using quoted long path"
}

# Do NOT pass -BEpath. Use arg array so paths with spaces work.
$argList = @(
    "-config=serverDZ.cfg",
    "-port=$Port",
    "-profiles=$ProfilesArg",
    "-dologs",
    "-adminlog",
    "-netlog",
    "-freezecheck",
    "-cpuCount=4"
)
Write-Info "Starting DayZServer_x64 on port $Port (BattlEye patched out)..."
Write-Info ("Args: " + ($argList -join ' '))
Start-Process -FilePath $Exe -ArgumentList $argList -WorkingDirectory $ServerRoot

Start-Sleep -Seconds 12
$p = Get-Process DayZServer_x64 -ErrorAction SilentlyContinue
if (-not $p) {
    Write-Host "Server exited early. Check logs in: $Profiles" -ForegroundColor Red
    Get-ChildItem -LiteralPath $Profiles -Filter "*.RPT" -Recurse -EA SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 |
        ForEach-Object {
            Write-Host "---- $($_.Name) (tail) ----"
            Get-Content -LiteralPath $_.FullName -Tail 40
        }
    exit 2
}

Write-Info "Server running PID=$($p.Id) title='$($p.MainWindowTitle)'"
Write-Info "Join: powershell -File oak\scripts\join_local_server.ps1"
Write-Info "Or: DayZ_x64.exe -connect=127.0.0.1 -port=$Port -noBattlEye"
