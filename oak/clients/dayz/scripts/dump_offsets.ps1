# Oak DayZ — one-shot offset dumper
# Prefer this over ad-hoc RVA hunting after every Steam update.
param(
    [switch]$Full,
    [switch]$Snapshot,
    [switch]$LiveValidate,
    [switch]$Apply,
    [switch]$Learn,
    [switch]$WriteHpp,
    [switch]$AllowWarnWrite,
    [string]$Exe = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe",
    [string]$OldExe = "C:\oak\dayz\DayZ_x64_OLD.exe"
)
$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "dump_all_offsets.py"
$pyArgs = @($script)
if ($Full) { $pyArgs += "--full" }
if ($Snapshot) { $pyArgs += "--snapshot" }
if ($LiveValidate) { $pyArgs += "--live-validate" }
if ($Apply) { $pyArgs += "--apply" }
if ($Learn) { $pyArgs += "--learn" }
if ($WriteHpp) { $pyArgs += "--write-hpp" }
if ($AllowWarnWrite) { $pyArgs += "--allow-warn-write" }
$pyArgs += @("--new-exe", $Exe)
if (Test-Path $OldExe) { $pyArgs += @("--old-exe", $OldExe) }

# Default: full dump if nothing specified
if (-not ($Full -or $Snapshot -or $LiveValidate -or $Apply -or $Learn)) {
    $pyArgs += @("--full", "--allow-warn-write")
}

Write-Host "python $($pyArgs -join ' ')"
python @pyArgs
exit $LASTEXITCODE
