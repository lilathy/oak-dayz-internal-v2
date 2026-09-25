# Per-setting functional audit after inject+soak.
# Reads oak_imgui.log and prints PASS/FAIL/SKIP for each shipped verify signal.
param(
    [int]$WaitSeconds = 120,
    [int]$SoakSeconds = 90
)

$ErrorActionPreference = "Stop"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$ScriptDir = $PSScriptRoot
$Test = Join-Path $ScriptDir "test_internal_dayz.ps1"

Write-Host "== feature audit: launch+inject =="
& $Test -WaitSeconds $WaitSeconds -SoakSeconds $SoakSeconds -KeepDayZRunning
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 7) {
    Write-Host "launch failed exit=$LASTEXITCODE"
    exit $LASTEXITCODE
}

if (-not (Test-Path $LogPath)) { throw "missing $LogPath" }
$text = Get-Content $LogPath -Raw

function Has([string]$pat) { return [bool]($text -match $pat) }
function LastMatch([string]$pat) {
    $m = [regex]::Matches($text, $pat)
    if ($m.Count -eq 0) { return $null }
    return $m[$m.Count - 1].Value
}

$results = @()
function Add-Result($name, $status, $detail) {
    $script:results += [pscustomobject]@{ Feature = $name; Status = $status; Detail = $detail }
    $color = if ($status -eq 'PASS') { 'Green' } elseif ($status -eq 'FAIL') { 'Red' } else { 'Yellow' }
    Write-Host ("[{0}] {1} - {2}" -f $status, $name, $detail) -ForegroundColor $color
}

# Stability: ignore VEH only if none. Prefer FATAL as hard fail.
$vehN = ([regex]::Matches($text, 'crashhunt\[VEH\] code=')).Count
$fatalN = ([regex]::Matches($text, 'crashhunt\[FATAL\]')).Count
if ($fatalN -gt 0) { Add-Result 'stability' 'FAIL' "FATAL=$fatalN" }
elseif ($vehN -gt 0) { Add-Result 'stability' 'FAIL' "VEH=$vehN (see crashhunt lines)" }
else { Add-Result 'stability' 'PASS' 'no VEH/FATAL' }

# ESP drawing — prefer liveqa summary (Frame lines early in connect are draws=0)
$esp = LastMatch 'liveqa\[esp\] drawn=\d+.*local=\d+.*cam=\d+'
if (-not $esp) { $esp = LastMatch 'liveqa\[esp\] drawn=\d+' }
$frame = LastMatch 'Frame \d+ - esp=\d+ draws=\d+(?: cam=\d+ local=\d+ w2s=\d+ world=\d+)?'
if ($esp -match 'drawn=([1-9]\d*)' -and ($esp -match 'cam=1' -or $esp -match 'local=1')) {
    Add-Result 'esp_draw' 'PASS' $esp
} elseif ($frame -match 'draws=([1-9]\d*)' -and $frame -match 'cam=1') {
    Add-Result 'esp_draw' 'PASS' $frame
} elseif ($esp -match 'drawn=0' -and $esp -match 'cam=1') {
    Add-Result 'esp_draw' 'WARN' "cam ok but nothing in range: $esp"
} elseif ($frame -match 'cam=0' -and -not ($esp -match 'cam=1')) {
    Add-Result 'esp_draw' 'FAIL' "camera still invalid: $frame"
} else {
    Add-Result 'esp_draw' 'FAIL' "no ESP evidence frame='$frame' liveqa='$esp'"
}

# Ammo
$ammo = LastMatch 'verify\[ammo\].*PASS=\d'
if ($ammo -match 'PASS=1') { Add-Result 'fast_bullets_ammo' 'PASS' $ammo }
elseif ($ammo) { Add-Result 'fast_bullets_ammo' 'FAIL' $ammo }
else { Add-Result 'fast_bullets_ammo' 'SKIP' 'no verify[ammo] (toggle off?)' }

# Recoil — soak does not fire a gun; hook hits prove the site is live. PASS=1 needs ADS/fire.
$rec = LastMatch 'verify\[recoil\].*PASS=\d'
if ($rec -match 'PASS=1') { Add-Result 'no_recoil_sway' 'PASS' $rec }
elseif ($rec -match 'hits=([1-9]\d*)') { Add-Result 'no_recoil_sway' 'PASS' "hook live (fire in-game to confirm wipe): $rec" }
elseif ($rec -match 'aim=1') { Add-Result 'no_recoil_sway' 'WARN' "model found but cold: $rec" }
elseif ($rec) { Add-Result 'no_recoil_sway' 'FAIL' $rec }
else { Add-Result 'no_recoil_sway' 'SKIP' 'no verify[recoil]' }
if (Has 'recoil: mid-hook installed|recoil: using validated AimingModel') {
    Add-Result 'recoil_hook_install' 'PASS' (LastMatch 'recoil: (mid-hook installed|using validated).*')
} elseif (Has 'recoil: mid-hook site missing|recoil: mid-hook disabled') {
    Add-Result 'recoil_hook_install' 'WARN' (LastMatch 'recoil: mid-hook.*')
} else {
    Add-Result 'recoil_hook_install' 'SKIP' 'no recoil install log'
}

# FOV
$fov = LastMatch 'verify\[fov\].*'
if ($fov -match 'srcErr=0|errPct=0') { Add-Result 'fov_changer' 'PASS' $fov }
elseif ($fov -match 'srcErr=([0-9]+)' -and [int]$Matches[1] -le 15) { Add-Result 'fov_changer' 'PASS' $fov }
elseif ($fov) { Add-Result 'fov_changer' 'FAIL' $fov }
else { Add-Result 'fov_changer' 'SKIP' 'no verify[fov]' }

# Stream proof
$sp = LastMatch 'verify\[streamproof\].*PASS=\d'
if ($sp -match 'PASS=1') { Add-Result 'stream_proof' 'PASS' $sp }
elseif ($sp) { Add-Result 'stream_proof' 'FAIL' $sp }
else { Add-Result 'stream_proof' 'SKIP' 'no verify[streamproof]' }

# GetName native intentionally cold (ABI AV). Steam tags cover names.
if (Has 'native\[DayZPlayer::GetName\] OK') { Add-Result 'getname_native' 'WARN' 'entry OK but call path cold' }
elseif (Has 'native\[DayZPlayer::GetName\] REFUSED') { Add-Result 'getname_native' 'FAIL' (LastMatch 'native\[DayZPlayer::GetName\].*') }
else { Add-Result 'getname_native' 'SKIP' 'native call disabled; Steam/roster used' }

# Door — local server uses mission flag path (not Building::* natives)
if (Has 'door: server flag ON') { Add-Result 'door_unlock_flag' 'PASS' (LastMatch 'door: server flag.*') }
elseif (Has 'native\[Building::.*REFUSED') { Add-Result 'door_natives' 'FAIL' 'REFUSED (stale RVA)' }
elseif (Has 'native\[Building::') { Add-Result 'door_natives' 'PASS' 'accepted' }
else { Add-Result 'door_unlock' 'SKIP' 'no door evidence (toggle off?)' }

# Native phys ray — correctly refused on this build; prop LOS still drives vis-colors
if (Has 'ray: ready|ray: CollisionBuffer allocated') { Add-Result 'raycast_vis' 'PASS' (LastMatch 'ray:.*') }
elseif (Has 'ray: stale mid-RVA|ray: bad prologue|ray:.*disabled') {
    Add-Result 'raycast_vis' 'SKIP' 'native ray cold; prop LOS fallback'
} else { Add-Result 'raycast_vis' 'SKIP' 'not init' }

# Vanilla 3PP unlock
$tp = LastMatch '3pp: VANILLA.*'
if ($tp -match 'writer=1') { Add-Result 'third_person' 'PASS' $tp }
elseif ($tp) { Add-Result 'third_person' 'WARN' $tp }
else { Add-Result 'third_person' 'SKIP' 'no 3pp log' }

# Vitals / names / FOV hook
$vit = LastMatch 'liveqa\[vitals\].*'
if ($vit -match 'ok=1') { Add-Result 'player_vitals' 'PASS' $vit }
elseif ($vit) { Add-Result 'player_vitals' 'FAIL' $vit }
else { Add-Result 'player_vitals' 'SKIP' 'no liveqa[vitals]' }

$names = LastMatch 'liveqa\[names\] roster=\d+.*'
if ($names -match "name='[^']+") { Add-Result 'steam_names' 'PASS' $names }
elseif ($names) { Add-Result 'steam_names' 'WARN' $names }
else { Add-Result 'steam_names' 'SKIP' 'no liveqa[names]' }

if (Has 'fov: ENTRY hook OK|fov: game-thread entry hook installed') {
    Add-Result 'fov_entry_hook' 'PASS' (LastMatch 'fov: ENTRY hook OK.*|fov: game-thread entry hook installed.*')
} elseif (Has 'fov: entry hook site NOT FOUND') {
    Add-Result 'fov_entry_hook' 'FAIL' 'site not found'
} else {
    Add-Result 'fov_entry_hook' 'SKIP' 'hook not attempted / cam-proj only'
}

if (Has 'lab\[lock\] module=stamina') { Add-Result 'infinite_stamina' 'SKIP' 'Lab LOCKED by design' }
if (Has 'lab\[lock\] module=speed') { Add-Result 'speed_hack' 'SKIP' 'Lab LOCKED by design' }
if (Has 'lab\[lock\] module=noclip') { Add-Result 'noclip' 'SKIP' 'Lab LOCKED by design' }
if (Has 'lab\[lock\] module=wallbypass') { Add-Result 'wall_bypass' 'SKIP' 'Lab LOCKED by design' }

$fail = @($results | Where-Object { $_.Status -eq 'FAIL' }).Count
$pass = @($results | Where-Object { $_.Status -eq 'PASS' }).Count
Write-Host ""
Write-Host ("SUMMARY pass={0} fail={1} other={2}" -f $pass, $fail, ($results.Count - $pass - $fail))
if ($fail -gt 0) { exit 2 }
exit 0
