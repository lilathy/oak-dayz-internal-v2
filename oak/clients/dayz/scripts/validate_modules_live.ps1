# DayZ Batch4 module validation (memory + feature gates)
# Pass = verify[*] PASS=1 in oak_imgui.log after enabling features.
param(
  [switch]$CheckOnly,
  [switch]$PatchConfig
)

$ErrorActionPreference = 'Stop'
$log = Join-Path $env:LOCALAPPDATA 'DayZ\oak_imgui.log'
$cfg = Join-Path $env:LOCALAPPDATA 'DayZ\oak_config.ini'

function Set-Ini([string]$path, [string]$key, [string]$val) {
  if (-not (Test-Path $path)) { return }
  $c = Get-Content $path -Raw
  if ($c -match "(?m)^$key=") {
    $c = [regex]::Replace($c, "(?m)^$key=.*$", "$key=$val")
  } else {
    $c = $c.TrimEnd() + "`r`n$key=$val`r`n"
  }
  Set-Content -Path $path -Value $c -NoNewline
}

if ($PatchConfig) {
  Set-Ini $cfg 'fovChanger' '1'
  Set-Ini $cfg 'horizontalFov' '120'
  Set-Ini $cfg 'thirdPerson' '1'
  Set-Ini $cfg 'streamProof' '1'
  Set-Ini $cfg 'fastBullets' '1'
  Set-Ini $cfg 'noDispersion' '1'
  Set-Ini $cfg 'perfectBallistics' '1'
  Set-Ini $cfg 'infStamina' '0'
  Set-Ini $cfg 'speedHack' '0'
  Set-Ini $cfg 'noclip' '0'
  Set-Ini $cfg 'warp' '0'
  Write-Host "Config patched for validation."
}

if (-not $CheckOnly) {
  Write-Host "Next: _launch_nobe_local.ps1 then: validate_modules_live.ps1 -CheckOnly"
  exit 0
}

if (-not (Test-Path $log)) { throw "no log: $log" }
$tail = Get-Content $log -Tail 500
$keys = @(
  @{ Name = 'FOV memory'; Pat = 'verify\[fov\].*PASS=1' },
  @{ Name = 'Ammo mods'; Pat = 'verify\[ammo\].*PASS=1' },
  @{ Name = 'Third person'; Pat = 'verify\[thirdperson\].*PASS=1' },
  @{ Name = 'Stream proof'; Pat = 'verify\[streamproof\].*PASS=1' },
  @{ Name = 'Stamina (expect fail)'; Pat = 'verify\[stam\].*PASS=1' },
  @{ Name = 'Speed (expect fail)'; Pat = 'verify\[speed\].*PASS=1' },
  @{ Name = 'MB snap'; Pat = 'verify\[mb-snap\].*PASS=1' },
  @{ Name = 'Recoil (expect miss)'; Pat = 'verify\[recoil\].*PASS=1' }
)
Write-Host "=== Module gate scan (last 500 log lines) ==="
foreach ($k in $keys) {
  $m = $tail | Select-String -Pattern $k.Pat | Select-Object -Last 1
  if ($m) { Write-Host ("PASS-LINE {0}: {1}" -f $k.Name, $m.Line) }
  else { Write-Host ("NO-HIT    {0}" -f $k.Name) }
}
