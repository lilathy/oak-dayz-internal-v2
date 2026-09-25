param(
    [int]$WaitSeconds = 90
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")
$OakRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $OakRoot "bin"
$DayZDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$SessionDir = Join-Path $env:LOCALAPPDATA "DayZ\oak_sessions"
New-Item -ItemType Directory -Force -Path $Bin, $SessionDir | Out-Null

$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$SessionLog = Join-Path $SessionDir ("session_" + $stamp + ".log")

function Write-Session([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg
    Add-Content -Path $SessionLog -Value $line
    Write-Host $line
}

Write-Session "session log: $SessionLog"
Write-Session "imgui log: $LogPath"

if (Test-Path $LogPath) {
    $prev = Join-Path $SessionDir ("oak_imgui_prev_" + $stamp + ".log")
    Move-Item $LogPath $prev -Force
}

try {
    Get-Process DayZ_x64, DayZ_BE, DayZLauncher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    Start-DayZNoBattlEye -Log { param($m) Write-Session $m }
    $null = Wait-DayZProcess -TimeoutSeconds 180 -Log { param($m) Write-Session $m }
    $null = Wait-DayZWindow -TimeoutSeconds 180 -Log { param($m) Write-Session $m }
} catch {
    Write-Session "FAIL: $($_.Exception.Message)"
    exit 1
}

Start-Sleep -Seconds 8

$ready = $true

Start-Sleep -Seconds 5

$loader = Join-Path $Bin "OakImGuiOverlayLoader.exe"
$overlay = Join-Path $Bin "OakImGuiOverlay.dll"
if (-not (Test-Path $loader) -or -not (Test-Path $overlay)) {
    Write-Session "FAIL: missing loader or overlay DLL in bin"
    exit 2
}

Write-Session "loading UI-only overlay..."
$proc = Start-Process -FilePath $loader -ArgumentList $overlay -WorkingDirectory $Bin -PassThru -Wait -NoNewWindow
Write-Session ("loader exit={0}" -f $proc.ExitCode)
if ($proc.ExitCode -ne 0) {
    Write-Session "FAIL: overlay loader failed"
    exit 3
}

Write-Session ("waiting {0}s for Present/ImGui log lines..." -f $WaitSeconds)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$pass = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path $LogPath) {
        $text = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
        if ($null -eq $text) { $text = "" }
        if (($text -match "imgui ok") -and ($text -match "present#")) {
            $pass = $true
            break
        }
        if (($text -match "imgui init FAILED") -or ($text -match "GetDevice failed")) {
            Write-Session "FAIL: imgui init error in log"
            Get-Content $LogPath | ForEach-Object { Write-Session ("LOG " + $_) }
            exit 4
        }
    }
    Start-Sleep -Seconds 2
}

Write-Session "oak_imgui.log contents:"
if (Test-Path $LogPath) {
    Get-Content $LogPath | ForEach-Object { Write-Session ("LOG " + $_) }
} else {
    Write-Session "LOG missing"
}

$rpt = Get-ChildItem (Join-Path $env:LOCALAPPDATA "DayZ") -Filter "*.RPT" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($rpt) {
    $rptCopy = Join-Path $SessionDir ("dayz_" + $stamp + ".RPT")
    Copy-Item $rpt.FullName $rptCopy -Force
    Write-Session ("copied RPT to {0} size={1}" -f $rptCopy, $rpt.Length)
}

if ($pass) {
    Write-Session "RESULT: PASS - ImGui initialized and presenting in DayZ"
    exit 0
}

Write-Session "RESULT: FAIL - no imgui ok + present# in log within timeout"
exit 5
