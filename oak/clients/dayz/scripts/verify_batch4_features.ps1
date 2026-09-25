# Automated Batch4 feature verification - sends real input to DayZ and parses verify[*] log lines.
param(
    [int]$InitWaitSeconds = 90,
    [switch]$SkipLaunch,
    [switch]$KeepDayZRunning
)

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($SkipLaunch) { $argList += "-SkipLaunch" }
    if ($KeepDayZRunning) { $argList += "-KeepDayZRunning" }
    $argList += "-InitWaitSeconds", $InitWaitSeconds
    Start-Process powershell.exe -Verb RunAs -ArgumentList ($argList -join " ") -Wait
    exit $LASTEXITCODE
}

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_dayz_steam_launch.ps1")

$OakRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $OakRoot "bin"
$LogPath = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class OakInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public InputUnion U;
    }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx, dy, mouseData, dwFlags, time;
        public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk, wScan;
        public uint dwFlags, time;
        public IntPtr dwExtraInfo;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public static void Key(ushort vk, bool down) {
        var inp = new INPUT[1];
        inp[0].type = INPUT_KEYBOARD;
        inp[0].U.ki.wVk = vk;
        inp[0].U.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
        SendInput(1, inp, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void HoldKeys(ushort[] vks, int ms) {
        foreach (var vk in vks) { Key(vk, true); }
        System.Threading.Thread.Sleep(ms);
        foreach (var vk in vks) { Key(vk, false); }
    }
}
"@

function Write-Step([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $msg
    Write-Host $line
}

function Wait-LogMarker([string]$pattern, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $LogPath) {
            $text = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
            if ($text -match $pattern) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Get-VerifyLines([string]$tag, [datetime]$since) {
    if (-not (Test-Path $LogPath)) { return @() }
    Get-Content $LogPath -ErrorAction SilentlyContinue |
        Where-Object { $_ -match $tag -and $_ -match "verify\[" } |
        Where-Object {
            if ($_ -match '^\[(\d{2}:\d{2}:\d{2})\]') {
                try {
                    $t = [datetime]::ParseExact($Matches[1], "HH:mm:ss", $null)
                    $t = Get-Date -Hour $t.Hour -Minute $t.Minute -Second $t.Second
                    return $t -ge $since
                } catch { return $true }
            }
            return $true
        }
}

function Parse-Pass([string]$line) {
    if ($line -match 'PASS=(\d)') { return [int]$Matches[1] -eq 1 }
    return $null
}

if (-not $SkipLaunch) {
    Get-Process DayZ_x64, DayZ_BE, DayZLauncher -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
    if (Test-Path $LogPath) { Remove-Item $LogPath -Force -ErrorAction SilentlyContinue }

    Start-DayZNoBattlEye -Log { param($m) Write-Step $m }
    $null = Wait-DayZProcess -TimeoutSeconds 180 -Log { param($m) Write-Step $m }
    $dayzWin = Wait-DayZWindow -TimeoutSeconds 180 -Log { param($m) Write-Step $m }
    Start-Sleep -Seconds 25

    $loader = Join-Path $Bin "OakImGuiOverlayLoader.exe"
    $dll = Join-Path $Bin "dayz_internal.dll"
    if (-not (Test-Path $loader) -or -not (Test-Path $dll)) {
        Write-Step "FAIL: build dayz_internal.dll + loader into bin first"
        exit 2
    }
    $dayz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
    if (-not $dayz) { Write-Step "FAIL: DayZ not running"; exit 3 }
    Write-Step "Injecting pid=$($dayz.Id)..."
    $proc = Start-Process -FilePath $loader -ArgumentList "`"$dll`"", $dayz.Id `
        -WorkingDirectory $Bin -PassThru -Wait -NoNewWindow
    if ($proc.ExitCode -ne 0) { Write-Step "FAIL: loader exit=$($proc.ExitCode)"; exit 4 }
} else {
    $dayz = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
    if (-not $dayz) { Write-Step "FAIL: DayZ not running (-SkipLaunch)"; exit 3 }
}

Write-Step "Waiting for overlay init (up to ${InitWaitSeconds}s)..."
if (-not (Wait-LogMarker "present hooked" $InitWaitSeconds)) {
    Write-Step "FAIL: present hooked not seen"
    if (Test-Path $LogPath) { Get-Content $LogPath -Tail 20 }
    exit 5
}
if (-not (Wait-LogMarker "imgui ok" 60)) {
    Write-Step "FAIL: imgui ok not seen"
    exit 5
}
if (-not (Wait-LogMarker "LocalPlayer Pos" 90)) {
    Write-Step "FAIL: not in-game (no LocalPlayer Pos in log)"
    exit 5
}
Write-Step "In-game ready - waiting 8s for world settle..."
Start-Sleep -Seconds 8

$hwnd = (Get-Process DayZ_x64).MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Step "WARN: no MainWindowHandle - input may not reach game"
} else {
    [OakInput]::ShowWindow($hwnd, 9) | Out-Null
    [OakInput]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 400
}

Write-Step "Holding W+Shift 4.5s (stamina sprint test)..."
$since = Get-Date
[OakInput]::HoldKeys(@(0x57, 0x10), 4500)  # W + Shift
Start-Sleep -Seconds 2

Write-Step "Holding W 3s (speed test)..."
[OakInput]::HoldKeys(@(0x57), 3000)  # W
Start-Sleep -Seconds 3

$results = @{}
foreach ($tag in @("batch4-fov", "batch4-stam", "batch4-speed")) {
    $lines = @(Get-VerifyLines $tag $since)
    if ($lines.Count -eq 0) {
        $results[$tag] = @{ pass = $false; line = "(no verify line)" }
        continue
    }
    $last = $lines[-1]
    $pass = Parse-Pass $last
    $results[$tag] = @{ pass = $pass; line = $last }
}

Write-Step "=== verify results ==="
$allPass = $true
foreach ($k in $results.Keys) {
    $r = $results[$k]
    $status = if ($r.pass -eq $true) { "PASS" } elseif ($r.pass -eq $false) { "FAIL" } else { "UNKNOWN" }
    if ($r.pass -ne $true) { $allPass = $false }
    Write-Step ("{0}: {1} - {2}" -f $k, $status, $r.line)
}

if (Test-Path $LogPath) {
    Write-Step "--- recent verify lines ---"
    Get-Content $LogPath | Select-String "verify\[batch4" | Select-Object -Last 10 |
        ForEach-Object { Write-Step $_.Line }
}

if (-not $KeepDayZRunning) {
    Get-Process DayZ_x64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($allPass) {
    Write-Step "OVERALL: PASS"
    exit 0
}
Write-Step "OVERALL: FAIL - measured values did not pass thresholds (see verify lines)"
exit 10
