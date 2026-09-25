param(
    [int]$WaitSeconds = 180,
    [int]$SoakSeconds = 60,
    [switch]$KeepDayZRunning
)

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    $argList = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
        "-WaitSeconds", $WaitSeconds, "-SoakSeconds", $SoakSeconds
    )
    if ($KeepDayZRunning) { $argList += "-KeepDayZRunning" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList ($argList -join " ") -Wait
    exit $LASTEXITCODE
}

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")
$OakRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $OakRoot "bin"
$DayZDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$SessionDir = Join-Path $env:LOCALAPPDATA "DayZ\oak_sessions"
New-Item -ItemType Directory -Force -Path $Bin, $SessionDir | Out-Null

$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$SessionLog = Join-Path $SessionDir ("internal_session_" + $stamp + ".log")

function Write-Session([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg
    Add-Content -Path $SessionLog -Value $line
    Write-Host $line
}

Write-Session "session log: $SessionLog"
Write-Session "imgui/internal log: $LogPath"

if (Test-Path $LogPath) {
    $prev = Join-Path $SessionDir ("oak_imgui_prev_" + $stamp + ".log")
    Move-Item $LogPath $prev -Force
}

try {
    Get-Process DayZ_x64, DayZ_BE, DayZLauncher -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    Start-DayZNoBattlEye -Log { param($m) Write-Session $m }
    $null = Wait-DayZProcess -TimeoutSeconds 180 -Log { param($m) Write-Session $m }
    $dayz = Wait-DayZWindow -TimeoutSeconds 180 -Log { param($m) Write-Session $m }
    $ready = $true
} catch {
    Write-Session "FAIL: $($_.Exception.Message)"
    exit 1
}

if (-not $ready) {
    Write-Session "FAIL: DayZ window never appeared"
    exit 1
}

Start-Sleep -Seconds 8

$dayz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
if (-not $dayz) {
    Write-Session "FAIL: DayZ_x64 exited before injection (possible crash on launch)"
    exit 1
}

$loader = Join-Path $Bin "OakImGuiOverlayLoader.exe"
$dll = Join-Path $Bin "dayz_internal.dll"
if (-not (Test-Path $loader) -or -not (Test-Path $dll)) {
    Write-Session "FAIL: missing loader or dayz_internal.dll in bin"
    exit 2
}

Write-Session ("loading dayz_internal.dll into pid={0}..." -f $dayz.Id)
$proc = Start-Process -FilePath $loader -ArgumentList "`"$dll`"", $dayz.Id -WorkingDirectory $Bin -PassThru -Wait -NoNewWindow
Write-Session ("loader exit={0}" -f $proc.ExitCode)
if ($proc.ExitCode -ne 0) {
    Write-Session "FAIL: loader failed"
    exit 3
}

Write-Session ("waiting {0}s for dll attach / present hook / imgui ok..." -f $WaitSeconds)
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$pass = $false
while ((Get-Date) -lt $deadline) {
    if (Test-Path $LogPath) {
        $text = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
        if ($null -eq $text) { $text = "" }
        $hasAttach = ($text -match "dll attach")
        $hasHook = ($text -match "present hooked") -or ($text -match "hook installed")
        $hasImgui = ($text -match "imgui ok")
        if ($hasAttach -and $hasHook -and $hasImgui) {
            $pass = $true
            break
        }
        if (($text -match "imgui init failed") -or ($text -match "CreateSwapChain failed") -or ($text -match "no game window")) {
            Write-Session "FAIL: init error in log"
            Get-Content $LogPath | ForEach-Object { Write-Session ("LOG " + $_) }
            exit 4
        }
    }
    Start-Sleep -Seconds 2
}

Write-Session "oak_imgui.log contents:"
if (Test-Path $LogPath) {
    Get-Content $LogPath | ForEach-Object { Write-Session ("LOG " + $_) }
} else {
    Write-Session "LOG missing"
}

$rpt = Get-ChildItem (Join-Path $env:LOCALAPPDATA "DayZ") -Filter "*.RPT" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($rpt) {
    $rptCopy = Join-Path $SessionDir ("dayz_internal_" + $stamp + ".RPT")
    Copy-Item $rpt.FullName $rptCopy -Force
    Write-Session ("copied RPT to {0} size={1}" -f $rptCopy, $rpt.Length)
}

# Confirm module is mapped into DayZ
$dayz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
if ($dayz) {
    $mods = @($dayz.Modules | Where-Object { $_.ModuleName -ieq "dayz_internal.dll" })
    if ($mods.Count -gt 0) {
        Write-Session ("module mapped: {0} @ 0x{1:X}" -f $mods[0].FileName, $mods[0].BaseAddress.ToInt64())
    } else {
        Write-Session "WARN: dayz_internal.dll not visible in DayZ module list (may need admin to enumerate)"
    }
}

if (-not $pass) {
    Write-Session "RESULT: FAIL - missing dll attach + present hooked + imgui ok within timeout"
    if (-not $KeepDayZRunning) {
        Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    exit 5
}

Write-Session "RESULT: init PASS - dayz_internal attached, Present hooked, ImGui ok"

if ($SoakSeconds -le 0) {
    if (-not $KeepDayZRunning) {
        Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    exit 0
}

Write-Session ("soak {0}s - watching DayZ_x64 stay alive..." -f $SoakSeconds)
$soakStart = Get-Date
$soakEnd = $soakStart.AddSeconds($SoakSeconds)
$soakTick = 0
while ((Get-Date) -lt $soakEnd) {
    $dayz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
    if (-not $dayz) {
        Write-Session "RESULT: FAIL - DayZ_x64 exited during soak (crash or close)"
        exit 6
    }
    if (Test-Path $LogPath) {
        $text = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
        if ($text -match "imgui init failed|CreateSwapChain failed|no game window|crashhunt\[FATAL\]|crashhunt\[VEH\]|liveqa\[crashgate\]") {
            Write-Session "RESULT: FAIL - error marker in oak_imgui.log during soak"
            Get-Content $LogPath | Select-Object -Last 30 | ForEach-Object { Write-Session ("LOG " + $_) }
            if (-not $KeepDayZRunning) {
                Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            }
            exit 7
        }
    }
    $soakTick++
    if (($soakTick % 5) -eq 0) {
        Write-Session ("soak alive pid={0} elapsed={1}s/{2}s" -f $dayz.Id, [int]((Get-Date) - $soakStart).TotalSeconds, $SoakSeconds)
    }
    Start-Sleep -Seconds 2
}

Write-Session ("RESULT: PASS - init ok + {0}s soak with no crash" -f $SoakSeconds)
if (-not $KeepDayZRunning) {
    Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Session "DayZ_x64 stopped (use -KeepDayZRunning to leave it open)"
}
exit 0
