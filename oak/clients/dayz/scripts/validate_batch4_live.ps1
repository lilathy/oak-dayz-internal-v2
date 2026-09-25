# Measurement-based batch4 validation from oak_imgui.log.
# PASS criteria require verify[*] lines with PASS=1 — not menu/liveqa string echoes.
param(
    [int]$WaitSeconds = 90,
    [switch]$RequireFreshInject
)

$ErrorActionPreference = "Stop"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$OutPath = Join-Path $env:TEMP "oak_batch4_validation.txt"
$Bin = Join-Path (Split-Path $PSScriptRoot -Parent) "bin"
$Root = Split-Path $PSScriptRoot -Parent

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-Result([string]$line) {
    Write-Host $line
    Add-Content -Path $OutPath -Value $line
}

Remove-Item $OutPath -Force -ErrorAction SilentlyContinue

if ($RequireFreshInject) {
    if (-not (Test-Admin)) {
        Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
            "-WaitSeconds", "$WaitSeconds", "-RequireFreshInject"
        )
        if (Test-Path $OutPath) { Get-Content $OutPath }
        exit $LASTEXITCODE
    }
    Get-Process DayZ_x64, DayZ_BE, DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Seconds 3
    $dbg = Join-Path $Root "build\Debug\dayz_internal.dll"
    if (Test-Path $dbg) {
        Copy-Item -Force $dbg (Join-Path $Bin "dayz_internal.dll")
    }
    & (Join-Path $PSScriptRoot "_launch_nobe_local.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Result "FAIL launch exit=$LASTEXITCODE"; exit 1 }
}

if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) {
    Write-Result "FAIL DayZ_x64 not running"
    exit 2
}

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$hooked = $false
$crash = $false
$fovPass = $false
$stamHold = $false
$stamCycle = $false
$speedPass = $false
$fovLine = $null
$stamLine = $null
$stamCycleLine = $null
$speedLine = $null
$perfLine = $null
$vqHigh = $false

while ((Get-Date) -lt $deadline) {
    if (-not (Test-Path $LogPath)) { Start-Sleep -Seconds 2; continue }
    $lines = Get-Content $LogPath -Tail 800 -ErrorAction SilentlyContinue
    foreach ($l in $lines) {
        if ($l -match "present hooked|imgui ok") { $hooked = $true }
        if ($l -match "crashhunt\[VEH\]|crashhunt\[FATAL\]|crashhunt\[HANG\]") { $crash = $true }
        if ($l -match "verify\[fov\].*PASS=1") { $fovPass = $true; $fovLine = $l.Trim() }
        elseif ($l -match "verify\[fov\]") { $fovLine = $l.Trim() }
        if ($l -match "verify\[stam-cycle\].*PASS=1") { $stamCycle = $true; $stamCycleLine = $l.Trim() }
        elseif ($l -match "verify\[stam-cycle\]") { $stamCycleLine = $l.Trim() }
        if ($l -match "verify\[stam\].*PASS=1") { $stamHold = $true; $stamLine = $l.Trim() }
        elseif ($l -match "verify\[stam\]") { $stamLine = $l.Trim() }
        if ($l -match "verify\[speed\].*PASS=1") { $speedPass = $true; $speedLine = $l.Trim() }
        elseif ($l -match "verify\[speed\]") { $speedLine = $l.Trim() }
        if ($l -match "perf: memcache vq=(\d+)") {
            $perfLine = $l.Trim()
            if ([int64]$Matches[1] -gt 5000000) { $vqHigh = $true }
        }
    }
    if ($hooked -and $fovPass -and $stamCycle) { break }
    Start-Sleep -Seconds 2
}

Write-Result "=== Oak Batch4 measurement validation ==="
Write-Result ("time: {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Write-Result ("dayz_pid: {0}" -f (Get-Process DayZ_x64 -EA SilentlyContinue).Id)
Write-Result ("inject_hooked: {0}" -f $(if ($hooked) { "yes" } else { "no" }))
Write-Result ("CRASH/HANG: {0}" -f $(if ($crash) { "YES" } else { "no" }))
Write-Result ("FOV:      {0}  | {1}" -f $(if ($fovPass) { "PASS" } else { "FAIL" }), $fovLine)
Write-Result ("STAM-hold:{0}  | {1}" -f $(if ($stamHold) { "PASS" } else { "FAIL" }), $stamLine)
Write-Result ("STAM-cyc: {0}  | {1}" -f $(if ($stamCycle) { "PASS" } else { "FAIL" }), $stamCycleLine)
Write-Result ("SPEED:    {0}  | {1}" -f $(if ($speedPass) { "PASS" } else { "FAIL/pending (needs W held)" }), $speedLine)
Write-Result ("PERF:     {0}  | {1}" -f $(if ($vqHigh) { "HIGH_VQ" } else { "ok/unknown" }), $perfLine)

# FOV + stamina cycle are required for certainty. Speed needs player movement input.
$fail = (-not $hooked) -or $crash -or (-not $fovPass) -or (-not $stamCycle)
if ($fail) {
    Write-Result "RESULT: FAIL"
    exit 3
}
Write-Result "RESULT: PASS (fov+stam-cycle measured)"
exit 0
