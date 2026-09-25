# Dual-client local NoBE validation (real MP).
# Client A = host Steam (yggeik70 / hjfgb47629 Autologin).
# Client B = Sandboxie OakDayZ2:
#   1) Prefer second Steam login (Mister_Hacker_999 / oyqso656877)
#   2) Fallback: DayZ in sandbox sharing host Steam (same account) if B never appears
# Meet pad: mission init.c teleports both near (4560, 10240).
param(
    [string]$AccountA = "hjfgb47629",
    [string]$AccountB = "oyqso656877",
    [string]$SandboxName = "OakDayZNet",
    [int]$JoinWaitSec = 150,
    [int]$ValidateSec = 90,
    [switch]$SameSteamFallback
)

$ErrorActionPreference = "Stop"
$Scripts = $PSScriptRoot
$Oak = Split-Path $Scripts -Parent
. (Join-Path $Scripts "_dayz_steam_launch.ps1")

$RunLog = Join-Path $env:LOCALAPPDATA "DayZ\oak_sessions\dual_validate_run.log"
New-Item -ItemType Directory -Force -Path (Split-Path $RunLog) | Out-Null

# Do NOT elevate the whole script (UAC consent blocks unattended runs).
# Only _inject_pid.ps1 elevates for the loader.
try { Stop-Transcript | Out-Null } catch {}
Start-Transcript -Path $RunLog -Force | Out-Null

function Write-Step([string]$m) { Write-Host "[DUAL] $m" -ForegroundColor Cyan }
function Write-Ok([string]$m) { Write-Host "[PASS] $m" -ForegroundColor Green }
function Write-Bad([string]$m) { Write-Host "[FAIL] $m" -ForegroundColor Red }

function Invoke-OakInject([int]$ProcessId) {
    $inj = Join-Path $Scripts "_inject_pid.ps1"
    Write-Step "Inject pid=$ProcessId (may prompt UAC once for loader)..."
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $inj -ProcessId $ProcessId -Dll (Join-Path $Oak "bin\dayz_internal.dll")
    Write-Step "Inject exit=$LASTEXITCODE"
    return $LASTEXITCODE
}

$Steam = Get-SteamExe
$DayZDir = Get-DayZDir
$StartExe = @(
    "C:\Program Files\Sandboxie-Plus\Start.exe",
    "C:\Program Files\Sandboxie\Start.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
$SbieIni = "C:\Program Files\Sandboxie-Plus\SbieIni.exe"

if (-not $StartExe) {
    Write-Bad "Sandboxie Start.exe not found. Install Sandboxie-Plus first."
    Stop-Transcript | Out-Null
    exit 2
}
Write-Step "Sandboxie: $StartExe"

# Ensure box exists
if (Test-Path $SbieIni) {
    & $SbieIni set $SandboxName Enabled y | Out-Null
    & $SbieIni set $SandboxName FileRootPath "%USERPROFILE%\Sandbox\%SANDBOX%" | Out-Null
    & $SbieIni set $SandboxName OpenPipePath "\Device\NamedPipe\Steam*" | Out-Null
    & $SbieIni set $SandboxName OpenFilePath "C:\Program Files (x86)\Steam" | Out-Null
}

# Stage DLL
$dllSrc = Join-Path $Oak "build\Debug\dayz_internal.dll"
$dllBin = Join-Path $Oak "bin\dayz_internal.dll"
if (-not (Test-Path $dllSrc)) { throw "Missing $dllSrc - build Debug DayZInternal first" }
Copy-Item -Force $dllSrc $dllBin
New-Item -ItemType Directory -Force -Path "C:\oak\dayz" | Out-Null
Copy-Item -Force $dllSrc "C:\oak\dayz\dayz_internal.dll"
# Dual auto-exercise flag for DLL liveqa
Set-Content -Path "C:\oak\dayz\liveqa_dual.flag" -Value ("armed " + (Get-Date -Format o)) -Encoding ASCII
$loader = Join-Path $Oak "bin\OakImGuiOverlayLoader.exe"
if (-not (Test-Path $loader)) { throw "Missing loader $loader" }

# Force config keys for bars/3PP/recoil/names (loaded on inject)
$cfg = Join-Path $env:LOCALAPPDATA "DayZ\oak_config.ini"
if (-not (Test-Path $cfg)) { New-Item -ItemType File -Path $cfg -Force | Out-Null }
if (-not ("Win32.NativeIni" -as [type])) {
    Add-Type -Namespace Win32 -Name NativeIni -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet=System.Runtime.InteropServices.CharSet.Ansi)]
public static extern bool WritePrivateProfileString(string s, string k, string v, string f);
"@
}
function Set-Ini([string]$sec, [string]$key, [string]$val) {
    [Win32.NativeIni]::WritePrivateProfileString($sec, $key, $val, $cfg) | Out-Null
}
Set-Ini "esp" "healthBars" "1"
Set-Ini "esp" "enabled" "1"
Set-Ini "esp" "players" "1"
Set-Ini "misc" "steamNames" "1"
Set-Ini "worldMisc" "thirdPerson" "1"
Set-Ini "combat.recoil" "noRecoil" "1"
Set-Ini "combat.recoil" "noSway" "1"
Set-Ini "combat.batch4" "noRecoil" "1"

# Fresh log
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
if (Test-Path $LogPath) {
    Move-Item $LogPath ($LogPath + ".prev_" + (Get-Date -Format "HHmmss")) -Force -ErrorAction SilentlyContinue
}

Get-Process DayZ_x64, DayZLauncher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Step "Starting local NoBE server (deploys meet-pad init.c)..."
& (Join-Path $Scripts "start_local_server.ps1")
Start-Sleep -Seconds 8
if (-not (Get-Process DayZServer_x64 -ErrorAction SilentlyContinue)) {
    Write-Bad "DayZServer_x64 did not stay up"
    Stop-Transcript | Out-Null
    exit 3
}
Write-Ok "Server up"

# --- Client A (host Steam session) ---
Write-Step "Launch Client A on host Steam (expect account $AccountA)..."
Ensure-DayZSteamAppId -DayZDir $DayZDir
$exe = Join-Path $DayZDir "DayZ_x64.exe"
Start-Process -FilePath $exe -ArgumentList @("-noBattlEye","-connect=127.0.0.1","-port=2302") -WorkingDirectory $DayZDir
$deadline = (Get-Date).AddSeconds($JoinWaitSec)
$procA = $null
while ((Get-Date) -lt $deadline) {
    $all = @(Get-Process DayZ_x64 -ErrorAction SilentlyContinue)
    if ($all.Count -ge 1) { $procA = $all | Sort-Object StartTime | Select-Object -First 1; break }
    Start-Sleep -Seconds 2
}
if (-not $procA) { Write-Bad "Client A never started"; Stop-Transcript | Out-Null; exit 4 }
Write-Ok "Client A pid=$($procA.Id)"
Write-Step "Waiting 45s for Client A D3D before inject..."
Start-Sleep -Seconds 45
$null = Invoke-OakInject -ProcessId $procA.Id

# --- Client B ---
Write-Step "Starting sandboxed Steam for AccountB=$AccountB ..."
& $StartExe /box:$SandboxName $Steam -login $AccountB | Out-Null
Start-Sleep -Seconds 30

Write-Step "Launching DayZ inside sandbox $SandboxName (second Steam login path)..."
# Sandboxie isolates loopback: 127.0.0.1 inside the box does NOT reach host DayZServer.
# Prefer a real host IPv4 (Wi-Fi / Ethernet / VPN) so sandboxed DayZ gets UDP game traffic.
$connectHost = "127.0.0.1"
$ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Sort-Object -Property @{Expression={
        if ($_.InterfaceAlias -match "Mullvad|VPN") { 0 }
        elseif ($_.InterfaceAlias -match "Wi-Fi|Ethernet") { 1 }
        else { 2 }
    }} |
    Select-Object -ExpandProperty IPAddress -Unique)
# Prefer Mullvad/VPN first - Sandboxie+loopback fails; Mullvad host IP proven to give Client B UDP.
if ($ips -and $ips.Count -gt 0) { $connectHost = [string]$ips[0] }
Write-Step "Client B connect host=$connectHost (not loopback - required for Sandboxie UDP)"
& $StartExe /box:$SandboxName $exe -noBattlEye -connect=$connectHost -port=2302
# Verify Client B actually bound a UDP game port (proof of real net join)
Start-Sleep -Seconds 8
$deadline = (Get-Date).AddSeconds($JoinWaitSec)
$procB = $null
while ((Get-Date) -lt $deadline) {
    $all = @(Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Sort-Object StartTime)
    if ($all.Count -ge 2) {
        $procB = $all | Select-Object -Last 1
        if ($procB.Id -ne $procA.Id) { break }
    }
    Start-Sleep -Seconds 3
}

if (-not $procB -or $procB.Id -eq $procA.Id) {
    Write-Step "Second Steam login path did not yield Client B - trying same-Steam sandbox DayZ fallback..."
    & $StartExe /box:$SandboxName $exe -noBattlEye -connect=$connectHost -port=2302
    $deadline2 = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline2) {
        $all = @(Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Sort-Object StartTime)
        if ($all.Count -ge 2) {
            $procB = $all | Select-Object -Last 1
            if ($procB.Id -ne $procA.Id) { break }
        }
        Start-Sleep -Seconds 5
    }
}

if (-not $procB -or $procB.Id -eq $procA.Id) {
    Write-Bad "Still only one DayZ_x64 - cannot dual-validate without Client B"
    Write-Host "If Sandboxie Steam is waiting for password for $AccountB, complete login in box $SandboxName and re-run."
    Stop-Transcript | Out-Null
    exit 5
}
Write-Ok "Client B pid=$($procB.Id)"
# Require UDP bind - otherwise B is a phantom offline world and MP checks are meaningless
$udpDeadline = (Get-Date).AddSeconds(60)
$hasUdp = $false
while ((Get-Date) -lt $udpDeadline) {
    $udp = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $procB.Id })
    if ($udp.Count -gt 0) { $hasUdp = $true; Write-Ok ("Client B UDP ports=" + ($udp.LocalPort -join ",")); break }
    Start-Sleep -Seconds 3
}
if (-not $hasUdp) {
    Write-Bad "Client B has no UDP endpoints - Sandboxie did not join the game server. MP name checks will fail."
}
Write-Step "Waiting 45s for Client B D3D before inject..."
Start-Sleep -Seconds 45
$null = Invoke-OakInject -ProcessId $procB.Id

Write-Step "Soak ${ValidateSec}s at meet pad - nudging mouse for freecam look proof..."
$endSoak = (Get-Date).AddSeconds($ValidateSec)
while ((Get-Date) -lt $endSoak) {
    # Relative mouse move so freecam cursor-delta / raw path can accumulate
    try {
        $sig = '[DllImport("user32.dll")] public static extern void mouse_event(int f,int dx,int dy,int d,int e);'
        $t = Add-Type -MemberDefinition $sig -Name U32Mouse -Namespace Native -PassThru -ErrorAction SilentlyContinue
        if (-not $t) { $t = [Native.U32Mouse] }
        [Native.U32Mouse]::mouse_event(0x0001, 40, 15, 0, 0)
        Start-Sleep -Milliseconds 200
        [Native.U32Mouse]::mouse_event(0x0001, -25, -10, 0, 0)
    } catch {}
    Start-Sleep -Seconds 2
}

# --- Validate logs ---
if (-not (Test-Path $LogPath)) {
    Write-Bad "No oak_imgui.log"
    Stop-Transcript | Out-Null
    exit 6
}
$log = Get-Content $LogPath -Raw
$results = @()

function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Ok "$name - $detail"; $script:results += [pscustomobject]@{Test=$name; Pass=$true; Detail=$detail} }
    else { Write-Bad "$name - $detail"; $script:results += [pscustomobject]@{Test=$name; Pass=$false; Detail=$detail} }
}

Check "dll_attach" ($log -match "dll attach") "inject saw attach"
Check "imgui_ok" ($log -match "imgui ok") "menu ready"
Check "present_hooked" ($log -match "present hooked") "Present live"
Check "alive" ($log -match "liveqa\[alive\].*local=1") "local+cam resolved"
Check "dual_flag" ($log -match "liveqa\[dual\]") "dual auto-exercise active"

$nameLines = Select-String -Path $LogPath -Pattern "liveqa\[names\]" | Select-Object -Last 8
$namesOk = $false
$namesDetail = "no liveqa[names] lines"
if ($nameLines) {
    $last = ($nameLines | Where-Object { $_.Line -match "roster=" } | Select-Object -Last 1)
    if ($last) {
        $namesDetail = $last.Line
        if ($last.Line -match "remotes=([1-9]\d*)" -and $last.Line -notmatch "need_mp=1") { $namesOk = $true }
    }
}
# Also accept ESP seeing another player anytime this session (roster can lag on NoBE).
if (-not $namesOk -and ($log -match "liveqa\[esp\].*p=[1-9]")) {
    $espMp = Select-String -Path $LogPath -Pattern "liveqa\[esp\].*p=[1-9]" | Select-Object -Last 1
    $namesOk = $true
    $namesDetail = "ESP remote player visible: " + $espMp.Line
}
# Sandbox Client B log (separate LocalAppData)
$blog = Get-ChildItem "$env:USERPROFILE\Sandbox" -Recurse -Filter "oak_imgui.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($blog) {
    $bNames = Select-String -Path $blog.FullName -Pattern "liveqa\[names\].*local='[^?].*'" | Select-Object -Last 1
    $aNames = Select-String -Path $LogPath -Pattern "liveqa\[names\].*local='[^?].*'" | Select-Object -Last 1
    if ($bNames -and $aNames -and $bNames.Line -match "local='([^']+)'" ) {
        $bn = $Matches[1]
        if ($aNames.Line -match "local='([^']+)'") {
            $an = $Matches[1]
            if ($an -and $bn -and $an -ne $bn) {
                $namesOk = $true
                $namesDetail = "two personas A='$an' B='$bn' | " + $namesDetail
            }
        }
    }
}
Check "steam_names_mp" $namesOk $namesDetail

$vitLines = Select-String -Path $LogPath -Pattern "liveqa\[vitals\]" | Select-Object -Last 8
$vitOk = $false
$vitDetail = "no vitals lines"
foreach ($vl in $vitLines) {
    $vitDetail = $vl.Line
    if ($vl.Line -match "ok=1" -and $vl.Line -match "hp=(\d+)") {
        $hp = [int]$Matches[1]
        if ($hp -ge 1 -and $hp -le 100) { $vitOk = $true; break }
    }
}
Check "vitals_sane" $vitOk $vitDetail

Check "soft_3pp_path" ($log -match "3pp: soft-cam translation override" -or $log -match "3pp: LOCKED soft-cam") "soft 3PP armed (no VT nop)"
Check "recoil_hook" ($log -match "recoil:.*hook installed" -or $log -match "verify\[recoil\].*PASS=1" -or $log -match "AimingModel") "recoil path exercised"
Check "freecam_on" ($log -match "freecam: on" -or $log -match "liveqa\[freecam\]: AUTO ON") "freecam auto soak"
$lookOk = $false
$lookDetail = "no freecam-look lines"
$lookPaths = @($LogPath)
if ($blog) { $lookPaths += $blog.FullName }
foreach ($lp in $lookPaths) {
    $lookLines = Select-String -Path $lp -Pattern "liveqa\[freecam-look\]" | Select-Object -Last 8
    foreach ($ll in $lookLines) {
        $lookDetail = $ll.Line
        if ($ll.Line -match "yawx100=([1-9]\d*)" -or $ll.Line -match "peakDx=([1-9]\d*)" -or $ll.Line -match "peakDy=([1-9]\d*)" -or $ll.Line -match "frames=([1-9]\d*)") {
            $lookOk = $true; break
        }
    }
    if ($lookOk) { break }
}
Check "freecam_look" $lookOk $lookDetail

$crash = ($log -match "crashhunt\[HANG\]" -or $log -match "EXCEPTION_EXECUTE" -or $log -match "fatal=1")
Check "no_fatal_crash" (-not $crash) "no hang/exception markers"

$alive = @(Get-Process DayZ_x64 -ErrorAction SilentlyContinue)
Check "two_clients_alive" ($alive.Count -ge 2) ("DayZ_x64 count=" + $alive.Count)

# Meet pad: LocalPlayer Pos near 4560,10240 (mission Print may not hit an RPT on this server layout)
$meetOk = $false
$meetDetail = "no meet pos in imgui logs"
$posHits = @()
foreach ($lp in $lookPaths) {
    $posHits += @(Select-String -Path $lp -Pattern "LocalPlayer Pos: X=45\d\d .*Z=102\d\d" | Select-Object -Last 2)
}
if ($posHits.Count -gt 0) {
    $meetOk = $true
    $meetDetail = ($posHits | ForEach-Object { $_.Line } | Select-Object -Last 3) -join " | "
}
if (-not $meetOk) {
    $rpt = Get-ChildItem "C:\Program Files (x86)\Steam\steamapps\common\DayZServer\oak_profiles" -Filter "*.RPT" -Recurse -EA SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($rpt) {
        $meetHits = Select-String -Path $rpt.FullName -Pattern "\[OAK\] meet" | Select-Object -Last 5
        if ($meetHits) {
            $meetOk = $true
            $meetDetail = ($meetHits | ForEach-Object { $_.Line }) -join " | "
        }
    }
}
Check "meet_teleport" $meetOk $meetDetail

$fail = @($results | Where-Object { -not $_.Pass }).Count
Write-Host ""
Write-Host "==== DUAL VALIDATION SUMMARY: $fail failures / $($results.Count) checks ====" -ForegroundColor Yellow
$results | Format-Table -AutoSize
$out = Join-Path $env:LOCALAPPDATA "DayZ\oak_sessions\dual_validate_$(Get-Date -Format 'yyyy-MM-dd_HH-mm-ss').txt"
$results | Format-Table -AutoSize | Out-String | Set-Content $out
Write-Host "Wrote $out"
Write-Host "Clients left running for manual visual check."
Remove-Item -Force "C:\oak\dayz\liveqa_dual.flag" -ErrorAction SilentlyContinue
Write-Host "Cleared liveqa_dual.flag (menu control restored)."
Stop-Transcript | Out-Null
exit $(if ($fail -gt 0) { 1 } else { 0 })
