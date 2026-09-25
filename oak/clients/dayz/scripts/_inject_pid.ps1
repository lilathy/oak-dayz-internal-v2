# Inject dayz_internal into a process (usermode overlay loader).
# Same-user DayZ does NOT need UAC elevation.
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [string]$Dll = ""
)
$ErrorActionPreference = "Stop"
$Bin = Join-Path (Split-Path $PSScriptRoot -Parent) "bin"
if (-not $Dll) { $Dll = Join-Path $Bin "dayz_internal.dll" }
$loader = Join-Path $Bin "OakImGuiOverlayLoader.exe"
if (-not (Test-Path $loader)) { throw "Missing $loader" }
if (-not (Test-Path $Dll)) { throw "Missing $Dll" }
if (-not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
    throw "Process $ProcessId not running"
}

Write-Host "[inject] pid=$ProcessId dll=$Dll"
$proc = Start-Process -FilePath $loader -ArgumentList "`"$Dll`"", "$ProcessId" -WorkingDirectory $Bin -PassThru -Wait -NoNewWindow
Write-Host "[inject] loader exit=$($proc.ExitCode)"
exit $proc.ExitCode
