# Stage C:\oak and prepare BattlEye kernel inject path.
# Run PowerShell as Administrator.

$ErrorActionPreference = "Stop"
$OakRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $OakRoot "bin"
$Dest = "C:\oak"

Write-Host "=== Oak BE setup ===" -ForegroundColor Cyan

# 1) Disable vulnerable driver blocklist (iqvw64e / kdmapper)
$blPath = "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config"
if (-not (Test-Path $blPath)) {
    New-Item -Path $blPath -Force | Out-Null
}
$cur = (Get-ItemProperty -Path $blPath -Name VulnerableDriverBlocklistEnable -ErrorAction SilentlyContinue).VulnerableDriverBlocklistEnable
Write-Host "VulnerableDriverBlocklistEnable was: $cur"
Set-ItemProperty -Path $blPath -Name VulnerableDriverBlocklistEnable -Value 0 -Type DWord -Force
Write-Host "VulnerableDriverBlocklistEnable set to 0" -ForegroundColor Yellow
Write-Host "REBOOT REQUIRED before oak_loader can load iqvw64e.sys" -ForegroundColor Red

# 2) Stage binaries
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$need = @(
    @{ Src = "oak_loader.exe"; Req = "oak_loader.exe" },
    @{ Src = "oak.sys"; Req = "oak.sys" },
    @{ Src = "dayz_internal.dll"; Dst = "dayz_internal.dll" }
)
foreach ($n in $need) {
    $from = Join-Path $Bin $n.Src
    if (-not (Test-Path $from)) {
        # oak.sys may live under driver\bin
        if ($n.Src -eq "oak.sys") {
            $from = Join-Path $OakRoot "driver\bin\oak.sys"
        }
    }
    if (-not (Test-Path $from)) { throw "Missing: $($n.Src) (build first)" }
    Copy-Item $from (Join-Path $Dest $n.Dst) -Force
    Write-Host "  staged $($n.Dst)" -ForegroundColor Green
}

Write-Host ""
Write-Host "After reboot, as Admin:" -ForegroundColor Cyan
Write-Host "  cd C:\oak"
Write-Host "  .\oak_loader.exe"
Write-Host "Then start DayZ normally (WITH BattlEye / Steam)."
Write-Host "INSERT opens menu. Log: %LOCALAPPDATA%\DayZ\oak_imgui.log"
Write-Host "DbgView (Capture Kernel) shows [oak] messages."
