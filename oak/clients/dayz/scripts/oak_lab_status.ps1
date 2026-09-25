# Oak Lab — quick status from live log
# Usage: powershell -File oak/clients/dayz/scripts/oak_lab_status.ps1

$log = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
if (-not (Test-Path $log)) {
    Write-Host "No log at $log"
    exit 1
}

Write-Host "=== Oak Lab (last matches) ==="
Write-Host "log: $log"
Write-Host ""

$patterns = @(
    "lab: init",
    "lab\[status\]",
    "lab\[claim\]",
    "lab\[arm\]",
    "lab\[arm-deny\]",
    "lab\[arm-force\]",
    "lab\[disarm\]",
    "lab\[safe\]",
    "lab\[lock\]",
    "lab\[probe\]",
    "lab\[hold\]",
    "lab\[soak\]",
    "lab\[confirm\]",
    "lab\[contract\]",
    "lab\[ledger\]",
    "lab\[write\]",
    "lab\[write-deny\]",
    "crashhunt\[VEH\]",
    "crashhunt\[FATAL\]"
)

$rx = ($patterns -join "|")
Get-Content -Path $log -Tail 4000 | Select-String -Pattern $rx | Select-Object -Last 60
