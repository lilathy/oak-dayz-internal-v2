# Shared DayZ launch helpers.
# Direct DayZ_x64.exe needs Steam running + steam_appid.txt (221100) in the game folder.
# steam -applaunch opens DayZ Launcher (Play screen) — avoid for automated smoke tests.

function Get-SteamExe {
    $default = "C:\Program Files (x86)\Steam\steam.exe"
    if (Test-Path -LiteralPath $default) { return $default }
    try {
        $root = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath
        if ($root) {
            $exe = Join-Path $root "steam.exe"
            if (Test-Path -LiteralPath $exe) { return $exe }
        }
    } catch { }
    throw "Steam not found (install Steam or fix registry SteamPath)"
}

function Get-DayZDir {
    $default = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
    if (Test-Path -LiteralPath (Join-Path $default "DayZ_x64.exe")) { return $default }
    throw "DayZ not found at $default"
}

function Wait-SteamReady {
    param(
        [int]$TimeoutSeconds = 90,
        [scriptblock]$Log = { param($m) Write-Host $m }
    )
    $steam = Get-SteamExe
    if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
        & $Log "starting Steam..."
        Start-Process -FilePath $steam
    } else {
        & $Log "Steam process already running"
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $steamProc = Get-Process steam -ErrorAction SilentlyContinue
        $helper = Get-Process steamwebhelper -ErrorAction SilentlyContinue
        if ($steamProc -and $helper) {
            Start-Sleep -Seconds 4
            & $Log "Steam ready (steam + steamwebhelper)"
            return $steam
        }
        Start-Sleep -Seconds 1
    }
    throw "Steam did not become ready within ${TimeoutSeconds}s (log in to Steam and retry)"
}

function Ensure-DayZSteamAppId {
    param(
        [string]$DayZDir = (Get-DayZDir),
        [scriptblock]$Log = { param($m) Write-Host $m }
    )
    $appIdFile = Join-Path $DayZDir "steam_appid.txt"
    $want = "221100"
    $have = if (Test-Path -LiteralPath $appIdFile) { (Get-Content -LiteralPath $appIdFile -Raw).Trim() } else { "" }
    if ($have -ne $want) {
        Set-Content -LiteralPath $appIdFile -Value $want -NoNewline
        & $Log "wrote steam_appid.txt ($want)"
    }
}

function Start-DayZNoBattlEye {
    param(
        [string]$DayZDir = (Get-DayZDir),
        [string[]]$ExtraArgs = @(),
        [string]$ConnectHost = "127.0.0.1",
        [int]$ConnectPort = 2302,
        [scriptblock]$Log = { param($m) Write-Host $m }
    )
    $null = Wait-SteamReady -Log $Log
    Ensure-DayZSteamAppId -DayZDir $DayZDir -Log $Log

    Get-Process DayZLauncher -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

    $exe = Join-Path $DayZDir "DayZ_x64.exe"
    if (-not (Test-Path -LiteralPath $exe)) { throw "DayZ_x64.exe not found: $exe" }

    $args = @("-noBattlEye")
    if ($ConnectHost) { $args += "-connect=$ConnectHost" }
    if ($ConnectPort -gt 0) { $args += "-port=$ConnectPort" }
    if ($ExtraArgs) { $args += $ExtraArgs }

    & $Log ("launching DayZ direct: {0} {1}" -f $exe, ($args -join " "))
    Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $DayZDir
}

function Wait-DayZProcess {
    param(
        [int]$TimeoutSeconds = 180,
        [scriptblock]$Log = { param($m) Write-Host $m }
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $p = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
        if ($p) {
            & $Log ("DayZ_x64 running pid={0}" -f $p.Id)
            return $p
        }
        $launcher = Get-Process DayZLauncher -ErrorAction SilentlyContinue
        if ($launcher) {
            & $Log "WARN: DayZLauncher is open (waiting for DayZ_x64...)"
        }
        Start-Sleep -Seconds 1
    }
    throw "DayZ_x64 did not start within ${TimeoutSeconds}s"
}

function Wait-DayZWindow {
    param(
        [int]$TimeoutSeconds = 180,
        [scriptblock]$Log = { param($m) Write-Host $m }
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $p = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
        if (-not $p) {
            throw "DayZ_x64 exited while waiting for game window"
        }
        if ($p.MainWindowHandle -ne [IntPtr]::Zero -and $p.MainWindowTitle) {
            & $Log ("DayZ window ready pid={0} title={1}" -f $p.Id, $p.MainWindowTitle)
            return $p
        }
        Start-Sleep -Seconds 1
    }
    throw "DayZ window did not appear within ${TimeoutSeconds}s"
}
