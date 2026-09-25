# Oak Engine Script Search
param(
    [Parameter(Mandatory = $true)]
    [string]$Class,
    [string]$Root = ""
)

if (-not $Root) {
    $Root = Join-Path $PSScriptRoot "..\docs\engine\DayZ-Scripts\scripts"
}

if (-not (Test-Path $Root)) {
    Write-Error "Scripts not found at $Root - clone Bohemia DayZ-Script-Diff-Experimental first."
    exit 1
}

Write-Host "Searching $Root for $Class ..."
$rx = [regex]::Escape($Class)
Get-ChildItem -Path $Root -Recurse -Include *.c,*.cpp -File |
    Select-String -Pattern $rx |
    Select-Object -First 40 Path, LineNumber, Line
