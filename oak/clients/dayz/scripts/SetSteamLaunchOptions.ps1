# Set DayZ Steam Launch Options while Steam is fully quit.
# VDF backslashes are escapes — use forward slashes, no nested quotes.
param(
    [string]$UserdataId = "786099022",
    [string]$Value = "C:/oak/dayz/steam_wrap.cmd %command%"
)

$ErrorActionPreference = "Stop"
if (Get-Process steam,steamwebhelper -EA SilentlyContinue) {
    Write-Error "Steam is running. Fully Exit Steam first (Steam → Exit), then re-run."
    exit 2
}

$vdf = "C:\Program Files (x86)\Steam\userdata\$UserdataId\config\localconfig.vdf"
if (-not (Test-Path $vdf)) { throw "missing $vdf" }

$raw = [IO.File]::ReadAllText($vdf)
$rx = '(?ms)("221100"\s*\{.*?)"LaunchOptions"\s*"([^"]*)"'
if ($raw -notmatch $rx) { throw "221100 LaunchOptions not found in $vdf" }

$raw2 = [regex]::Replace($raw, $rx, { param($m) $m.Groups[1].Value + '"LaunchOptions"' + "`t`t" + '"' + $Value + '"' }, 1)
$utf8 = New-Object System.Text.UTF8Encoding $false
[IO.File]::WriteAllText($vdf, $raw2, $utf8)
Write-Host "Set 221100 LaunchOptions = $Value"
Write-Host "file: $vdf"
exit 0
