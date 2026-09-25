# Scan oak_imgui.log / oak_crash.log for crash-hunt markers.
param(
    [string]$LogPath = (Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"),
    [string]$CrashPath = (Join-Path $env:LOCALAPPDATA "DayZ\oak_crash.log")
)

$ErrorActionPreference = "Stop"
$patterns = @(
    "liveqa\[crashgate\]",
    "crashhunt\[VEH\]",
    "crashhunt\[FATAL\]",
    "crashhunt\[HANG\]",
    "crashstack\[",
    "EXCEPTION in entity loop",
    "present init EXCEPTION",
    "warmup EXCEPTION",
    "imgui init failed"
)

function Scan-File([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "missing: $path"
        return @()
    }
    Write-Host "`n== $path =="
    $hits = Select-String -Path $path -Pattern ($patterns -join "|") -ErrorAction SilentlyContinue
    if (-not $hits) {
        Write-Host "  (no crash markers)"
        return @()
    }
    $hits | ForEach-Object { Write-Host ("  L{0}: {1}" -f $_.LineNumber, $_.Line.Trim()) }
    return $hits
}

$all = @()
$all += Scan-File $LogPath
$all += Scan-File $CrashPath

# Summarize crashgate tags
if (Test-Path -LiteralPath $LogPath) {
    $gates = Select-String -Path $LogPath -Pattern "liveqa\[crashgate\] (\S+)" -AllMatches -ErrorAction SilentlyContinue
    if ($gates) {
        Write-Host "`n== crashgate tag counts =="
        $gates.Matches | ForEach-Object { $_.Groups[1].Value } |
            Group-Object | Sort-Object Count -Descending |
            ForEach-Object { Write-Host ("  {0,-20} {1}" -f $_.Name, $_.Count) }
    }
    $summary = Select-String -Path $LogPath -Pattern "crashhunt\[summary\]" | Select-Object -Last 1
    if ($summary) { Write-Host "`nlast summary: $($summary.Line.Trim())" }
}

if ($all.Count -eq 0) {
    Write-Host "`nPASS: no crash markers in logs"
    exit 0
}
Write-Host "`nWARN: $($all.Count) crash marker(s) found"
exit 1
