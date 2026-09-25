# Stable DayZ launch with NVIDIA App open.
# DayZ + Freestyle PPE (ppeGetVersion) crashes at D3D init.
# This holds NVIDIA Overlay/nvcontainer off through graphics init, then restores.

[CmdletBinding(PositionalBinding = $false)]
param(
    [int]$InitSeconds = 45,
    [switch]$Steam,
    [switch]$SteamCmd,
    [switch]$NoBE,
    [switch]$Inject,
    [string]$Connect = "",
    [int]$Port = 2302,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GameArgs
)

$ErrorActionPreference = "Continue"
$Log = Join-Path $env:LOCALAPPDATA "DayZ\oak_nvidia_launch.log"
function L([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m
    try { Add-Content -Path $Log -Value $line -EA SilentlyContinue } catch {}
    Write-Host $line
}

$dayzDir = "C:\Program Files (x86)\Steam\steamapps\common\DayZ"
$be = Join-Path $dayzDir "DayZ_BE.exe"
$x64 = Join-Path $dayzDir "DayZ_x64.exe"
$overlayExe = "C:\Program Files\NVIDIA Corporation\NVIDIA App\CEF\NVIDIA Overlay.exe"
$share = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\ShareSettings.json"
$idb = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\CefCache\Default\IndexedDB\https_nvfile_0.indexeddb.leveldb"
$ls = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA Overlay\CefCache\Default\Local Storage\leveldb"
$wlPath = Join-Path $env:LOCALAPPDATA "NVIDIA Corporation\NVIDIA App\NvBackend\FeatureWhitelist.json"
$cfg = @(
    (Join-Path $env:USERPROFILE "OneDrive\Documents\DayZ\DayZ.cfg"),
    (Join-Path $env:USERPROFILE "Documents\DayZ\DayZ.cfg")
) | Where-Object { Test-Path $_ } | Select-Object -First 1

function Kill-NvHooks {
    foreach ($n in @("NVIDIA Overlay", "nvcontainer")) {
        Get-Process -Name $n -EA SilentlyContinue | ForEach-Object {
            try { Stop-Process -Id $_.Id -Force -EA SilentlyContinue } catch {}
        }
    }
}

function Disable-DayZFreestyleWhitelist {
    if (-not (Test-Path $wlPath)) { return }
    try {
        $j = Get-Content $wlPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $list = [System.Collections.Generic.List[object]]::new()
        $found = $false
        foreach ($item in @($j.data)) {
            if ($item.cms_appId -eq 101054511) {
                $item | Add-Member -NotePropertyName isFreeStyleSupported -NotePropertyValue $false -Force
                $found = $true
            }
            $list.Add($item) | Out-Null
        }
        if (-not $found) {
            $list.Add([pscustomobject]@{ cms_appId = 101054511; isFreeStyleSupported = $false }) | Out-Null
        }
        $j.data = $list.ToArray()
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [IO.File]::WriteAllText($wlPath, ($j | ConvertTo-Json -Depth 8 -Compress), $utf8)
        L "FeatureWhitelist: DayZ isFreeStyleSupported=false"
    } catch { L ("whitelist skip: " + $_.Exception.Message) }
}

function Clear-FilterCache {
    Kill-NvHooks
    Start-Sleep -Milliseconds 400
    foreach ($d in @($idb, $ls)) {
        if (Test-Path $d) {
            Get-ChildItem $d -Force -EA SilentlyContinue | Remove-Item -Recurse -Force -EA SilentlyContinue
        }
    }
    if (Test-Path $share) {
        try {
            $j = Get-Content $share -Raw | ConvertFrom-Json
            if ($null -ne $j.settings.video) {
                $j.settings.video.irEnabled = $false
                $utf8 = New-Object System.Text.UTF8Encoding $false
                [IO.File]::WriteAllText($share, ($j | ConvertTo-Json -Depth 12), $utf8)
            }
        } catch {}
    }
    if ($cfg) {
        try {
            $t = Get-Content $cfg -Raw
            foreach ($k in @("MSAA", "FSAA", "AToC")) {
                if ($t -match ("(?m)^" + $k + "=")) {
                    $t = [regex]::Replace($t, ("(?m)^" + $k + "=.*$"), ($k + "=0;"))
                }
            }
            Set-Content $cfg $t.TrimEnd() -Encoding ASCII -EA SilentlyContinue
        } catch {}
    }
    L "filter cache cleared; IR/AA off"
}

function Start-Overlay {
    try { Start-Service NvContainerLocalSystem -EA SilentlyContinue } catch {}
    Start-Sleep 2
    if (-not (Get-Process "NVIDIA Overlay" -EA SilentlyContinue)) {
        if (Test-Path $overlayExe) { Start-Process $overlayExe | Out-Null }
    }
}

function Hold-NvThroughInit([int]$secs) {
    L ("Holding NVIDIA off for " + $secs + "s through D3D init...")
    $deadline = (Get-Date).AddSeconds($secs)
    $saw = $false
    while ((Get-Date) -lt $deadline) {
        Kill-NvHooks
        if (Get-Process DayZ_x64 -EA SilentlyContinue) { $saw = $true }
        if ($saw -and -not (Get-Process DayZ_x64 -EA SilentlyContinue)) {
            L "FAIL: DayZ_x64 died during init"
            return $false
        }
        Start-Sleep -Milliseconds 200
    }
    return [bool](Get-Process DayZ_x64 -EA SilentlyContinue)
}

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Enable-OakLoadDriverPriv {
    $code = @'
using System; using System.Runtime.InteropServices;
public class OakTokPriv {
  [DllImport("advapi32.dll", ExactSpelling=true, SetLastError=true)] public static extern bool AdjustTokenPrivileges(IntPtr t, bool d, ref TOKEN_PRIVILEGES n, int buf, IntPtr p, IntPtr r);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool LookupPrivilegeValue(string s, string n, out LUID l);
  [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
  public struct LUID { public uint LowPart; public int HighPart; }
  public struct TOKEN_PRIVILEGES { public uint Count; public LUID Luid; public uint Attr; }
  public static void Enable(string name) {
    IntPtr tok; if (!OpenProcessToken(GetCurrentProcess(), 0x28u, out tok)) return;
    LUID l; if (!LookupPrivilegeValue(null, name, out l)) return;
    TOKEN_PRIVILEGES tp; tp.Count=1; tp.Luid=l; tp.Attr=2;
    AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
  }
}
'@
    if (-not ([System.Management.Automation.PSTypeName]"OakTokPriv").Type) {
        Add-Type -TypeDefinition $code -ErrorAction SilentlyContinue
    }
    [OakTokPriv]::Enable("SeLoadDriverPrivilege")
    [OakTokPriv]::Enable("SeDebugPrivilege")
}

# Map oak.sys BEFORE DayZ_BE. BEDaisy blocks iqvw64e (0xC0000022) if already loaded.
function Invoke-OakKernelMap {
    param(
        [string]$Stage = "C:\oak\dayz",
        [string]$OakRoot = "C:\oak"
    )
    if (-not (Test-Admin)) {
        L "FAIL: BE kernel inject needs Administrator (SeLoadDriverPrivilege)"
        return $false
    }
    Enable-OakLoadDriverPriv

    $dllSrc = @(
        (Join-Path $Stage "dayz_internal.dll"),
        "..\..\..\clients\dayz\build\Debug\dayz_internal.dll",
        "..\..\..\clients\dayz\build\Release\dayz_internal.dll",
        "..\..\..\clients\dayz\bin\dayz_internal.dll"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    $oakLoader = @(
        (Join-Path $Stage "oak_loader.exe"),
        (Join-Path $OakRoot "oak_loader.exe")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    $oakSys = @(
        (Join-Path $Stage "oak.sys"),
        (Join-Path $OakRoot "oak.sys")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1

    if (-not $dllSrc -or -not $oakLoader -or -not $oakSys) {
        L "FAIL: missing dayz_internal.dll / oak_loader.exe / oak.sys under C:\oak\dayz"
        return $false
    }
    L ("DLL: " + $dllSrc + " (" + ((Get-Item $dllSrc).Length) + " bytes)")

    New-Item -ItemType Directory -Force -Path $Stage,$OakRoot | Out-Null
    function Copy-IfDifferent([string]$from, [string]$to) {
        $f = [IO.Path]::GetFullPath($from); $t = [IO.Path]::GetFullPath($to)
        if ($f -ne $t) { Copy-Item -Force $f $t }
    }
    Copy-IfDifferent $dllSrc (Join-Path $Stage "dayz_internal.dll")
    Copy-IfDifferent $dllSrc (Join-Path $OakRoot "dayz_internal.dll")
    Copy-IfDifferent $oakSys (Join-Path $Stage "oak.sys")
    Copy-IfDifferent $oakLoader (Join-Path $Stage "oak_loader.exe")

    Get-Process oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    # Stop BEDaisy only - do NOT sc config/delete the service (BE recreates it).
    try { sc.exe stop BEDaisy | Out-Null } catch {}
    Start-Sleep 1

    Remove-Item -Force -EA SilentlyContinue @(
        (Join-Path $Stage "dll_attach.flag"),
        (Join-Path $Stage "inject.log"),
        (Join-Path $OakRoot "dll_attach.flag"),
        (Join-Path $OakRoot "inject.log"),
        (Join-Path $OakRoot "loader.log")
    )

    L "BE kernel: mapping oak.sys (BEDaisy stopped)..."
    $loaderStarted = Get-Date
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $Stage "oak_loader.exe"
    $psi.Arguments = ("`"{0}`"" -f (Join-Path $Stage "oak.sys"))
    $psi.WorkingDirectory = $Stage
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $waitUntil = (Get-Date).AddSeconds(25)
    while (-not $p.HasExited -and (Get-Date) -lt $waitUntil) { Start-Sleep -Milliseconds 400 }
    try { $p.StandardInput.WriteLine() } catch {}
    Start-Sleep 1
    if (-not $p.HasExited) { Start-Sleep 3 }
    if (-not $p.HasExited) { try { $p.Kill() } catch {} }

    $loaderLog = Join-Path $OakRoot "loader.log"
    if (-not (Test-Path $loaderLog) -or ((Get-Item $loaderLog).LastWriteTime -lt $loaderStarted.AddSeconds(-2))) {
        L "FAIL: oak_loader did not write a fresh loader.log"
        return $false
    }
    $lr = Get-Content $loaderLog -Raw -EA SilentlyContinue
    if ($lr -notmatch "result=ok") {
        L ("FAIL: oak_loader map failed: " + ($lr -replace "`r?`n", " | "))
        if ($lr -match "0xC0000022") {
            L "HINT: 0xC0000022 = BEDaisy/AV still blocking iqvw. Stop BEDaisy and retry, or reboot if driversipolicy reloaded."
        }
        if ($lr -match "0xC0000061") {
            L "HINT: 0xC0000061 = not elevated. Run LaunchDayZSafe.bat as Administrator."
        }
        return $false
    }
    L "oak_loader result=ok - will auto-inject when DayZ_x64 starts"
    return $true
}

function Wait-OakInject {
    param(
        [System.Diagnostics.Process]$DayZ,
        [string]$Stage = "C:\oak\dayz",
        [string]$OakRoot = "C:\oak",
        [int]$TimeoutSec = 240
    )
    $imguiLog = Join-Path $env:LOCALAPPDATA "DayZ\oak_imgui.log"
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $ok = $false
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id $DayZ.Id -EA SilentlyContinue)) {
            L "FAIL: DayZ died during kernel inject"
            return $false
        }
        $inj = ""
        foreach ($p in @((Join-Path $Stage "inject.log"), (Join-Path $OakRoot "inject.log"))) {
            if (Test-Path $p) { $inj += (Get-Content $p -Raw -EA SilentlyContinue) }
        }
        $attach = (Test-Path (Join-Path $Stage "dll_attach.flag")) -or (Test-Path (Join-Path $OakRoot "dll_attach.flag"))
        $mapOk = $inj -match "INJECTION COMPLETE|DllMain OK|SUCCESS via spoofed"
        if ($mapOk -or $attach) {
            if (Test-Path $imguiLog) {
                $tail = @(Get-Content $imguiLog -Tail 120 -EA SilentlyContinue)
                $sawAttach = $false
                foreach ($line in $tail) {
                    if ($line -match "dll attach|BE detected|main thread started") { $sawAttach = $true }
                    if ($sawAttach -and $line -match "imgui ok|present hooked") { $ok = $true; break }
                }
            }
            if ($ok) { break }
            # Attach alone is not enough — BE sessions often attach then die in crashhunt.
            if ($mapOk -and $attach -and (Test-Path $imguiLog)) {
                $raw = Get-Content $imguiLog -Raw -EA SilentlyContinue
                if ($raw -match "imgui ok|present hooked|protection authorized") { $ok = $true; break }
            }
        }
        if ($inj -match "DayZ died|BE kill") {
            L ("FAIL: inject log reports death: " + (($inj -split "`n") | Select-Object -Last 3 | Out-String))
            return $false
        }
        Start-Sleep 2
    }
    if ($ok) { L "BE kernel inject OK" } else { L "WARN: inject markers incomplete; check C:\oak\inject.log / oak_imgui.log" }
    return $ok
}

# --- run ---
try { Add-Content -Path $Log -Value "" -EA SilentlyContinue } catch {}
L "=== Oak NVIDIA-safe DayZ launch ==="
Get-Process DayZ_x64, DayZ_BE, DayZLauncher, oak_loader -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
Start-Sleep 1

Disable-DayZFreestyleWhitelist
Clear-FilterCache
Kill-NvHooks

if (-not (Get-Process steam -EA SilentlyContinue)) {
    $steamExe = "C:\Program Files (x86)\Steam\steam.exe"
    if (Test-Path $steamExe) { Start-Process $steamExe; Start-Sleep 6 }
}

# BE path: map oak.sys BEFORE DayZ_BE so BEDaisy cannot block iqvw64e.
$kernelMapped = $false
if ($Inject -and -not $NoBE) {
    "1" | Set-Content -Path "C:\oak\dayz\be_launch.flag" -NoNewline -Encoding ASCII
    "1" | Set-Content -Path "C:\oak\be_launch.flag" -NoNewline -Encoding ASCII
    if (-not (Invoke-OakKernelMap)) { exit 6 }
    $kernelMapped = $true
}
elseif ($NoBE) {
    Remove-Item -Force -EA SilentlyContinue "C:\oak\dayz\be_launch.flag","C:\oak\be_launch.flag"
}

$started = $false

if ($NoBE) {
    if (-not (Test-Path $x64)) { throw "DayZ_x64.exe missing" }
    $appId = Join-Path $dayzDir "steam_appid.txt"
    if (-not (Test-Path $appId)) { Set-Content $appId "221100" -NoNewline }
    $nb = @("-noBattlEye")
    if ($Connect) { $nb += "-connect=$Connect" }
    if ($Port -gt 0 -and $Connect) { $nb += "-port=$Port" }
    L ("NoBE: " + $x64 + " " + ($nb -join " "))
    Start-Process -FilePath $x64 -ArgumentList $nb -WorkingDirectory $dayzDir | Out-Null
    $started = $true
}
elseif ($SteamCmd -and $GameArgs -and $GameArgs.Count -gt 0) {
    # Steam Launch Options: ... -SteamCmd -- %command%
    $exe = $GameArgs[0]
    $rest = @()
    if ($GameArgs.Count -gt 1) { $rest = $GameArgs[1..($GameArgs.Count - 1)] }
    # If Steam pointed at DayZLauncher, replace with DayZ_BE so we actually start the game.
    if ($exe -match 'DayZLauncher\.exe$') {
        L "SteamCmd gave DayZLauncher - forcing DayZ_BE.exe"
        $exe = $be
        $rest = @()
    }
    L ("SteamCmd: " + $exe + " " + ($rest -join " "))
    if ($rest.Count -gt 0) {
        Start-Process -FilePath $exe -ArgumentList $rest -WorkingDirectory (Split-Path $exe -Parent) | Out-Null
    } else {
        Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe -Parent) | Out-Null
    }
    $started = $true
}
elseif ($Steam) {
    # steam:// opens launcher UI and waits - skip that; start BE under Steam ownership.
    L "Steam mode: launching DayZ_BE.exe (skips stuck DayZLauncher UI)"
    if (-not (Test-Path $be)) { throw "DayZ_BE.exe missing" }
    $appId = Join-Path $dayzDir "steam_appid.txt"
    if (-not (Test-Path $appId)) { Set-Content $appId "221100" -NoNewline }
    Start-Process -FilePath $be -WorkingDirectory $dayzDir | Out-Null
    $started = $true
}
else {
    $exe = if (Test-Path $be) { $be } else { $x64 }
    if (-not (Test-Path $exe)) { throw "DayZ not found" }
    $appId = Join-Path $dayzDir "steam_appid.txt"
    if (-not (Test-Path $appId)) { Set-Content $appId "221100" -NoNewline }
    L ("Starting " + $exe)
    Start-Process -FilePath $exe -WorkingDirectory $dayzDir | Out-Null
    $started = $true
}

if (-not $started) { L "FAIL: nothing started"; Start-Overlay; exit 1 }

if (-not (Hold-NvThroughInit $InitSeconds)) {
    Start-Overlay
    if (-not (Get-Process DayZ_x64 -EA SilentlyContinue)) { exit 3 }
    exit 2
}

$dz = Get-Process DayZ_x64 -EA SilentlyContinue | Select-Object -First 1
L ("PASS: DayZ pid=" + $dz.Id)

$injectOk = -not $Inject
if ($Inject) {
    if ($NoBE) {
        $umLoader = @(
            "C:\oak\dayz\OakImGuiOverlayLoader.exe",
            "..\..\..\clients\dayz\bin\OakImGuiOverlayLoader.exe"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        $dllSrc = @(
            "..\..\..\clients\dayz\build\Release\dayz_internal.dll",
            "C:\oak\dayz\dayz_internal.dll"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($umLoader -and $dllSrc) {
            L ("NoBE usermode inject: " + $dllSrc)
            $lp = Start-Process -FilePath $umLoader -ArgumentList "`"$dllSrc`"", $dz.Id -WorkingDirectory (Split-Path $umLoader) -PassThru -Wait -NoNewWindow
            L ("usermode loader exit=" + $lp.ExitCode)
            $injectOk = $lp.ExitCode -eq 0
        } else {
            L "FAIL: NoBE inject skipped (missing usermode loader/dll)"
        }
    }
    elseif ($kernelMapped) {
        L "Waiting for auto-inject into DayZ_x64..."
        $injectOk = Wait-OakInject -DayZ $dz
    }
}

L ("Restoring NVIDIA overlay, pid=" + $dz.Id)
Start-Overlay
Start-Sleep 5
# NvBackend often rewrites FeatureWhitelist on overlay start - stamp DayZ again.
Disable-DayZFreestyleWhitelist

if (-not (Get-Process -Id $dz.Id -EA SilentlyContinue)) {
    L "FAIL: DayZ died after overlay restore"
    exit 4
}

$crash = Get-ChildItem "$env:LOCALAPPDATA\DayZ\crash_*.log" -EA SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($crash -and $crash.LastWriteTime -gt (Get-Date).AddMinutes(-2)) {
    L ("WARN: recent crash log " + $crash.Name + " @ " + $crash.LastWriteTime)
}

if ($Inject -and -not $injectOk) {
    L "FAIL: DayZ is running but injection did not complete"
    exit 7
}

L ("OK DayZ running pid=" + $dz.Id)
L "Note: Freestyle Game Filters crash DayZ (NVIDIA ppeGetVersion). Overlay restored; use NVCP Digital Vibrance instead of Freestyle."
exit 0
