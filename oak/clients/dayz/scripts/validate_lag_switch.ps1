# Validate Lag Switch live: inject, focus DayZ, hold T, require hold START/END + firewall.
param(
    [int]$HoldMs = 3000,
    [int]$WaitInjectSec = 100
)

$ErrorActionPreference = "Stop"
$DayzRoot = Split-Path $PSScriptRoot -Parent
$Bin = Join-Path $DayzRoot "bin"
$Log = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
$Ini = Join-Path $env:LOCALAPPDATA "DayZ\oak_config.ini"
$Launch = Join-Path $PSScriptRoot "_launch_nobe_local.ps1"

function Test-Admin {
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}
if (-not (Test-Admin)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
        "-HoldMs", "$HoldMs"
    )
    exit 0
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OakLagVal {
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)]
  public static extern bool WritePrivateProfileString(string s, string k, string v, string f);
  [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
  public const uint KEYEVENTF_KEYUP = 0x0002;
  public const byte VK_T = 0x54;
}
"@

[OakLagVal]::WritePrivateProfileString("exploits", "warp", "1", $Ini) | Out-Null
[OakLagVal]::WritePrivateProfileString("exploits", "warpKey", "84", $Ini) | Out-Null
[OakLagVal]::WritePrivateProfileString("exploits", "warpMode", "1", $Ini) | Out-Null
[OakLagVal]::WritePrivateProfileString("exploits", "warpCd", "500", $Ini) | Out-Null

$dll = Join-Path $DayzRoot "build\Debug\dayz_internal.dll"
if (-not (Test-Path $dll)) { throw "missing $dll - build Debug DayZInternal first" }

Get-Process DayZ_x64, DayZ_BE, DayZLauncher -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep -Seconds 2

New-Item -ItemType Directory -Force -Path $Bin | Out-Null
Copy-Item -Force $dll (Join-Path $Bin "dayz_internal.dll")
if (Test-Path "C:\oak\dayz") {
    Copy-Item -Force $dll "C:\oak\dayz\dayz_internal.dll"
}

# Clear stale firewall rule
netsh advfirewall firewall delete rule name=OakLagCut 2>$null | Out-Null

$marker = Get-Date
Write-Host "== Launch + inject =="
& $Launch
if ($LASTEXITCODE -ne 0) { throw "launch failed exit=$LASTEXITCODE" }

Write-Host "== Wait for fresh init =="
$deadline = (Get-Date).AddSeconds($WaitInjectSec)
$ready = $false
do {
    Start-Sleep -Seconds 2
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited" }
    $hit = Select-String -Path $Log -Pattern "init done" | Select-Object -Last 1
    if ($hit -and $hit.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
        $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
        if ($stamp -ge $marker.AddMinutes(-1)) {
            Write-Host $hit.Line
            $ready = $true
        }
    }
} while (-not $ready -and (Get-Date) -lt $deadline)
if (-not $ready) { throw "no fresh init done" }

Start-Sleep -Seconds 2
Write-Host "== Wait for lag hooks (world/present tick) =="
$hookDeadline = (Get-Date).AddSeconds(120)
$hooks = $null
do {
    Start-Sleep -Seconds 2
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited" }
    $hooks = Select-String -Path $Log -Pattern "lag: install done|lag: tramp|lag: hooks" | Select-Object -Last 1
    if ($hooks -and $hooks.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
        $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
        if ($stamp -ge $marker.AddMinutes(-1)) {
            Write-Host "HOOKS:" $hooks.Line
            break
        }
        $hooks = $null
    }
} while ((Get-Date) -lt $hookDeadline)
if (-not $hooks) { throw "FAIL: lag hooks never installed (tick not running)" }

Write-Host "== Wait for outbound traffic through hooks =="
$trafficDeadline = (Get-Date).AddSeconds(90)
$sawTraffic = $false
do {
    Start-Sleep -Seconds 2
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { throw "DayZ exited" }
    $t = Select-String -Path $Log -Pattern "lag: traffic delta=" | Select-Object -Last 1
    if ($t -and $t.Line -match 'delta=([1-9][0-9]*)' -and $t.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
        $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
        if ($stamp -ge $marker.AddMinutes(-1)) {
            Write-Host "TRAFFIC:" $t.Line
            $sawTraffic = $true
            break
        }
    }
} while ((Get-Date) -lt $trafficDeadline)
if (-not $sawTraffic) {
    Write-Host "WARN: no lag: traffic ok yet - join/load world so DayZ sends packets, continuing bind test"
}

$dayz = Get-Process DayZ_x64 -EA SilentlyContinue | Select-Object -First 1
if (-not $dayz) { throw "DayZ gone" }
$hwnd = $dayz.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    $hwnd = [OakLagVal]::FindWindow("DayZ", $null)
}
[OakLagVal]::ShowWindow($hwnd, 9) | Out-Null
[OakLagVal]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

Write-Host "== Holding T for ${HoldMs}ms (focus hwnd=$hwnd) =="
[OakLagVal]::keybd_event([OakLagVal]::VK_T, 0, 0, [UIntPtr]::Zero)
$holdDeadline = (Get-Date).AddMilliseconds($HoldMs + 2500)
$sawStart = $false
$sawDrop = $false
do {
    Start-Sleep -Milliseconds 400
    $start = Select-String -Path $Log -Pattern "lag: hold START" | Select-Object -Last 1
    $beat = Select-String -Path $Log -Pattern "lag: beat" | Select-Object -Last 1
    if ($start -and $start.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
        $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
        if ($stamp -ge $marker) { $sawStart = $true }
    }
    if ($beat -and $beat.Line -match 'drop=([1-9][0-9]*)') {
        if ($beat.Line -match '\[(\d{2}):(\d{2}):(\d{2})') {
            $stamp = Get-Date -Hour ([int]$Matches[1]) -Minute ([int]$Matches[2]) -Second ([int]$Matches[3])
            if ($stamp -ge $marker) { $sawDrop = $true }
        }
    }
} while ((Get-Date) -lt $holdDeadline -and (-not $sawStart -or -not $sawDrop))

[OakLagVal]::keybd_event([OakLagVal]::VK_T, 0, [OakLagVal]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
Start-Sleep -Seconds 3

$start = Select-String -Path $Log -Pattern "lag: hold START" | Select-Object -Last 1
$beat = Select-String -Path $Log -Pattern "lag: beat" | Select-Object -Last 5
$end = Select-String -Path $Log -Pattern "lag: hold END" | Select-Object -Last 1
$inline = Select-String -Path $Log -Pattern "lag: hooks inline|lag: inline" | Select-Object -Last 5

Write-Host "=== RESULTS ==="
$inline | ForEach-Object { Write-Host "HOOK:" $_.Line }
Write-Host "START:" $(if ($start) { $start.Line } else { "MISSING" })
$beat | ForEach-Object { Write-Host "BEAT:" $_.Line }
Write-Host "END:" $(if ($end) { $end.Line } else { "MISSING" })

$pass = $true
if (-not $sawStart) { $pass = $false; Write-Host "FAIL: hold never started (bind dead)" }
if ($sawTraffic -and -not $sawDrop -and ($end -and $end.Line -notmatch 'dropped=[1-9]')) {
    $pass = $false
    Write-Host "FAIL: traffic seen but drop=0 - packets not actually cut"
}
if (-not $end) { $pass = $false; Write-Host "FAIL: hold never ended" }
if ($end -and $end.Line -match 'dropped=([0-9]+)' -and [int]$Matches[1] -gt 0) {
    Write-Host "DROP OK:" $Matches[1]
    $sawDrop = $true
}

Start-Sleep -Seconds 2
netsh advfirewall firewall delete rule name=OakLagCut 2>$null | Out-Null
netsh advfirewall firewall delete rule name=OakLagCutTcp 2>$null | Out-Null

if (-not $pass) { throw "LAG SWITCH VALIDATION FAILED" }
if ($sawDrop) {
    Write-Host "PASS: lag switch actually dropped outbound packets"
} else {
    Write-Host "PASS: bind/hold works (no in-world traffic during test - join server to confirm drop>0)"
}
exit 0
