# Hunt module crashes - cycles every Oak module in-process and attributes VEH/FATAL/exit
# to the active crashmatrix step.
#
# Usage (admin):
#   powershell -File .\hunt_module_crashes.ps1
#   powershell -File .\hunt_module_crashes.ps1 -AttachOnly   # DayZ already running+injected
param(
    [switch]$AttachOnly,
    [int]$WaitSeconds = 180,
    [int]$MaxSeconds = 420,
    [int]$ReadyTimeoutSec = 120
)

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath)
    if ($AttachOnly) { $argList += "-AttachOnly" }
    $argList += @("-WaitSeconds", "$WaitSeconds", "-MaxSeconds", "$MaxSeconds", "-ReadyTimeoutSec", "$ReadyTimeoutSec")
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait
    exit $LASTEXITCODE
}

$ErrorActionPreference = "Stop"
$ScriptDir = $PSScriptRoot
$FlagDir = "C:\oak\dayz"
$Flag = Join-Path $FlagDir "oak_crash_matrix.flag"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$CrashPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_crash.log"
$ReportDir = Join-Path $env:LOCALAPPDATA "DayZ\oak_sessions"
New-Item -ItemType Directory -Force -Path $FlagDir, $ReportDir | Out-Null

$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$Report = Join-Path $ReportDir ("crash_matrix_" + $stamp + ".txt")

function Write-Rep([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m
    Add-Content -Path $Report -Value $line
    Write-Host $line
}

function Get-LogTail([int]$n = 400) {
    if (-not (Test-Path $LogPath)) { return @() }
    return Get-Content $LogPath -Tail $n -ErrorAction SilentlyContinue
}

function Test-CrashMarkers([string[]]$lines) {
    $pat = 'crashhunt\[VEH\]|crashhunt\[FATAL\]|crashhunt\[HANG\]|crashmatrix\[FAULT\]'
    return @($lines | Select-String -Pattern $pat)
}

Write-Rep "report: $Report"
Write-Rep "log: $LogPath"

if (-not $AttachOnly) {
    Write-Rep "launch+inject via test_internal_dayz.ps1"
    & (Join-Path $ScriptDir "test_internal_dayz.ps1") -WaitSeconds $WaitSeconds -SoakSeconds 15 -KeepDayZRunning
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 7) {
        Write-Rep "FAIL: launch/inject exit=$LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

$dayz = Get-Process DayZ_x64 -EA SilentlyContinue
if (-not $dayz) {
    Write-Rep "FAIL: DayZ_x64 not running"
    exit 2
}
Write-Rep "DayZ pid=$($dayz.Id)"

# Wait until local+camera exist so matrix can start
$readyDeadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
$ready = $false
while ((Get-Date) -lt $readyDeadline) {
    if (-not (Get-Process -Id $dayz.Id -EA SilentlyContinue)) {
        Write-Rep "FAIL: DayZ died before matrix ready"
        exit 3
    }
    $tail = Get-LogTail 80
    if ($tail -match 'liveqa\[alive\].*local=1.*cam=1' -or $tail -match 'liveqa\[esp\].*local=1.*cam=1') {
        $ready = $true
        break
    }
    Start-Sleep -Seconds 2
}
if (-not $ready) {
    Write-Rep "WARN: never saw local+cam in log - arming matrix anyway (will wait in-process)"
}

# Clear previous flag, arm matrix
Remove-Item $Flag -Force -EA SilentlyContinue
Add-Content -Path $LogPath -Value ("`n===== crashmatrix harness start {0} =====`n" -f $stamp)
New-Item -ItemType File -Path $Flag -Force | Out-Null
Write-Rep "armed flag $Flag"

$deadline = (Get-Date).AddSeconds($MaxSeconds)
$lastStep = "none"
$faults = New-Object System.Collections.Generic.List[string]
$completed = $false
$crashed = $false

while ((Get-Date) -lt $deadline) {
    $proc = Get-Process -Id $dayz.Id -EA SilentlyContinue
    if (-not $proc) {
        $crashed = $true
        Write-Rep "CRASH: DayZ_x64 exited during matrix (lastStep=$lastStep)"
        break
    }

    $tail = Get-LogTail 120
    $arm = $tail | Select-String -Pattern 'crashmatrix\[arm\] i=\d+/\d+ id=(\S+)' | Select-Object -Last 1
    if ($arm -and $arm.Matches.Count -gt 0) {
        $lastStep = $arm.Matches[0].Groups[1].Value
    }
    $hold = $tail | Select-String -Pattern 'crashmatrix\[hold\] .* id=(\S+)' | Select-Object -Last 1
    if ($hold -and $hold.Matches.Count -gt 0) {
        $lastStep = $hold.Matches[0].Groups[1].Value
    }

    $marks = Test-CrashMarkers $tail
    foreach ($m in $marks) {
        $t = $m.Line.Trim()
        if (-not $faults.Contains($t)) {
            $faults.Add($t)
            Write-Rep "MARKER: $t"
        }
    }

    if ($tail -match 'crashmatrix\[COMPLETE\]') {
        $completed = $true
        Write-Rep "matrix COMPLETE"
        break
    }

    Start-Sleep -Seconds 1
}

# Disarm
Remove-Item $Flag -Force -EA SilentlyContinue
Write-Rep "flag cleared"

# Final scan of full log since banner
$all = @()
if (Test-Path $LogPath) {
    $raw = Get-Content $LogPath -Raw
    $idx = $raw.LastIndexOf("===== crashmatrix harness start $stamp =====")
    $slice = if ($idx -ge 0) { $raw.Substring($idx) } else { $raw }
    $all = $slice -split "`r?`n"
}

$arms = @($all | Select-String -Pattern 'crashmatrix\[arm\]')
$passes = @($all | Select-String -Pattern 'crashmatrix\[pass\]')
$faultLines = @($all | Select-String -Pattern 'crashmatrix\[FAULT\]|crashhunt\[VEH\]|crashhunt\[FATAL\]|crashhunt\[HANG\]')
$alive = [bool](Get-Process DayZ_x64 -EA SilentlyContinue)

Write-Rep ""
Write-Rep "===== SUMMARY ====="
Write-Rep ("steps_armed={0} steps_passed={1} fault_lines={2} completed={3} process_alive={4} lastStep={5}" -f `
    $arms.Count, $passes.Count, $faultLines.Count, $completed, $alive, $lastStep)

if ($faultLines.Count -gt 0) {
    Write-Rep "--- faults / VEH ---"
    $faultLines | ForEach-Object { Write-Rep $_.Line.Trim() }
}

# Attribute: last arm before first VEH/FATAL
$firstVeh = $all | Select-String -Pattern 'crashhunt\[VEH\]|crashhunt\[FATAL\]' | Select-Object -First 1
if ($firstVeh) {
    $before = $all[0..([Math]::Max(0, $firstVeh.LineNumber - 2))]
    $culprit = $before | Select-String -Pattern 'crashmatrix\[arm\] i=\d+/\d+ id=(\S+)' | Select-Object -Last 1
    if ($culprit) {
        Write-Rep ("LIKELY_CULPRIT: {0}" -f $culprit.Matches[0].Groups[1].Value)
    } else {
        Write-Rep "LIKELY_CULPRIT: unknown (no arm line before first VEH)"
    }
}

if ($crashed) {
    Write-Rep "RESULT: FAIL - process died (see LIKELY_CULPRIT / lastStep)"
    exit 10
}
if ($faultLines.Count -gt 0) {
    Write-Rep "RESULT: WARN - survived but VEH/FAULT markers present"
    exit 11
}
if (-not $completed) {
    Write-Rep "RESULT: FAIL - timed out before COMPLETE"
    exit 12
}
Write-Rep "RESULT: PASS - full matrix, no VEH/FATAL"
exit 0
