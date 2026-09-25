param([switch]$WriteTest)
$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OakProbe {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool b, int c);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out int r);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out int w);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("psapi.dll")] public static extern bool EnumProcessModulesEx(IntPtr h, IntPtr[] m, int cb, out int n, uint f);
  [DllImport("psapi.dll", CharSet=CharSet.Unicode)] public static extern uint GetModuleBaseName(IntPtr h, IntPtr m, StringBuilder n, int s);
  public static float Rf(IntPtr h, long a) {
    byte[] b = new byte[4]; int r;
    if (!ReadProcessMemory(h, (IntPtr)a, b, 4, out r) || r != 4) return float.NaN;
    return BitConverter.ToSingle(b, 0);
  }
  public static long Rq(IntPtr h, long a) {
    byte[] b = new byte[8]; int r;
    if (!ReadProcessMemory(h, (IntPtr)a, b, 8, out r) || r != 8) return 0;
    return BitConverter.ToInt64(b, 0);
  }
  public static bool Wf(IntPtr h, long a, float v) {
    byte[] b = BitConverter.GetBytes(v); int w;
    return WriteProcessMemory(h, (IntPtr)a, b, 4, out w) && w == 4;
  }
}
"@

$p = Get-Process DayZ_x64 -ErrorAction SilentlyContinue
if (-not $p) { throw "DayZ_x64 not running" }
$h = [OakProbe]::OpenProcess(0x1F0FFF, $false, $p.Id)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed - need admin" }

$mods = New-Object IntPtr[] 1024
$n = 0
[void][OakProbe]::EnumProcessModulesEx($h, $mods, (8*1024), [ref]$n, 3)
$mod = [IntPtr]::Zero
for ($i=0; $i -lt ($n/8); $i++) {
  $sb = New-Object System.Text.StringBuilder 260
  [void][OakProbe]::GetModuleBaseName($h, $mods[$i], $sb, 260)
  if ($sb.ToString() -ieq "DayZ_x64.exe") { $mod = $mods[$i]; break }
}
if ($mod -eq [IntPtr]::Zero) { throw "DayZ_x64.exe module not found" }
$base = $mod.ToInt64()
Write-Host ("module=0x{0:X}" -f $base)

$WorldOff = 0x4264058
$CamOff = 0x1B8
$FovCtx = 0x1008CE0
$FovBase = 0x9C4
$Tick = 0xFF4998
$Stamina = 0x6A4
$LocalPlayer = 0x2960
$ProjD1 = 0xD0
$ProjD2 = 0xDC
$VP = 0x58

$world = [OakProbe]::Rq($h, $base + $WorldOff)
Write-Host ("World=0x{0:X}" -f $world)
$cam = [OakProbe]::Rq($h, $world + $CamOff)
$lp = [OakProbe]::Rq($h, $world + $LocalPlayer)
Write-Host ("Camera=0x{0:X}" -f $cam)
Write-Host ("LocalPlayer=0x{0:X}" -f $lp)

$projX = [OakProbe]::Rf($h, $cam + $ProjD1)
$projY = [OakProbe]::Rf($h, $cam + $ProjD2 + 4)
$vpX = [OakProbe]::Rf($h, $cam + $VP)
$vpY = [OakProbe]::Rf($h, $cam + $VP + 4)
Write-Host ("projX={0:N3} projY={1:N3} vp={2:N0}x{3:N0}" -f $projX, $projY, $vpX, $vpY)
if ($projX -gt 1 -and $vpX -gt 1) {
  $half = [math]::Atan($vpX / (2.0 * $projX))
  $deg = $half * 2.0 * 180.0 / [math]::PI
  Write-Host ("computedHorizFovDeg={0:N2}" -f $deg)
}

$embed = [OakProbe]::Rf($h, $base + $FovCtx + $FovBase)
Write-Host ("embed_FovBase={0}" -f $embed)
$fovPtr = [OakProbe]::Rq($h, $base + $FovCtx)
Write-Host ("FOV_Context_as_ptr=0x{0:X}" -f $fovPtr)
if ($fovPtr -gt 0x100000000) {
  Write-Host ("FOV_Context_deref_FovBase={0}" -f ([OakProbe]::Rf($h, $fovPtr + $FovBase)))
}

Write-Host "FOV_Context scale-like and degree-like floats:"
for ($o=0; $o -le 0xA00; $o += 4) {
  $f = [OakProbe]::Rf($h, $base + $FovCtx + $o)
  if ([float]::IsNaN($f) -or [float]::IsInfinity($f)) { continue }
  if ($f -gt 0.2 -and $f -lt 3.5) {
    $d = 75.0 / $f
    Write-Host ("  +0x{0:X} scale={1:N5} deg75={2:N1}" -f $o, $f, $d)
  }
  elseif ($f -gt 40 -and $f -lt 140) {
    Write-Host ("  +0x{0:X} degish={1:N3}" -f $o, $f)
  }
}

Write-Host "Camera candidate floats:"
for ($o=0x40; $o -le 0x120; $o += 4) {
  $f = [OakProbe]::Rf($h, $cam + $o)
  if ([float]::IsNaN($f)) { continue }
  if (($f -gt 40 -and $f -lt 140) -or ($f -gt 0.3 -and $f -lt 2.5)) {
    Write-Host ("  cam+0x{0:X}={1:N5}" -f $o, $f)
  }
}

$tickF = [OakProbe]::Rf($h, $base + $Tick)
$tickP = [OakProbe]::Rq($h, $base + $Tick)
Write-Host ("Tick_float={0} Tick_ptr=0x{1:X}" -f $tickF, $tickP)
if ($tickP -gt 0x100000000) { Write-Host ("Tick_indirect={0}" -f ([OakProbe]::Rf($h, $tickP))) }

if ($lp -gt 0x100000000) {
  $st = [OakProbe]::Rf($h, $lp + $Stamina)
  Write-Host ("entity_Stamina_0x6A4={0}" -f $st)
}

if ($WriteTest) {
  Write-Host "WRITE_TEST start: set embed to 0.625 for 120deg"
  $before = $projX
  [void][OakProbe]::Wf($h, $base + $FovCtx + $FovBase, [float]0.625)
  Start-Sleep -Milliseconds 1200
  $after = [OakProbe]::Rf($h, $cam + $ProjD1)
  $embed2 = [OakProbe]::Rf($h, $base + $FovCtx + $FovBase)
  Write-Host ("embed_after={0} proj_before={1:N3} proj_after={2:N3}" -f $embed2, $before, $after)

  Write-Host "WRITE_TEST2: force projX smaller for wider FOV"
  $halfT = (120.0 * 0.5) * [math]::PI / 180.0
  $newProj = $vpX / (2.0 * [math]::Tan($halfT))
  [void][OakProbe]::Wf($h, $cam + $ProjD1, [float]$newProj)
  Start-Sleep -Milliseconds 50
  $after2 = [OakProbe]::Rf($h, $cam + $ProjD1)
  Write-Host ("forced_proj={0:N3} readback={1:N3}" -f $newProj, $after2)
  Start-Sleep -Milliseconds 500
  $after3 = [OakProbe]::Rf($h, $cam + $ProjD1)
  Write-Host ("proj_500ms_later={0:N3} game_reset={1}" -f $after3, ($after3 -ne $after2))
}

[void][OakProbe]::CloseHandle($h)
