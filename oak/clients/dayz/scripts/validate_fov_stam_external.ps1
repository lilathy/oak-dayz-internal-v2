# External validation: measure camera tan(halfFOV) and stamina 0..1 while cheat runs.
# PASS only if memory matches expected physical effect for 3+ samples.
param([int]$Seconds = 12)

$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class V {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint a, bool b, int c);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion U; }
  [StructLayout(LayoutKind.Explicit)] public struct InputUnion { [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk,wScan; public uint dwFlags,time; public IntPtr dwExtraInfo; }
  public static float Rf(IntPtr h, long a){ byte[] b=new byte[4]; int r; if(!ReadProcessMemory(h,(IntPtr)a,b,4,out r)||r!=4) return float.NaN; return BitConverter.ToSingle(b,0); }
  public static long Rq(IntPtr h, long a){ byte[] b=new byte[8]; int r; if(!ReadProcessMemory(h,(IntPtr)a,b,8,out r)||r!=8) return 0; return BitConverter.ToInt64(b,0); }
  public static void Key(ushort vk, bool down){ var i=new INPUT[1]; i[0].type=1; i[0].U.ki.wVk=vk; i[0].U.ki.dwFlags=down?0u:2u; SendInput(1,i,Marshal.SizeOf(typeof(INPUT))); }
}
"@

$p = Get-Process DayZ_x64 -ErrorAction Stop
$h = [V]::OpenProcess(0x1F0FFF, $false, $p.Id)
$mods = New-Object IntPtr[] 512; $n=0
# reuse Enum via reflection-less: find module from MainModule if possible
$base = $p.MainModule.BaseAddress.ToInt64()
$world = [V]::Rq($h, $base + 0x4264058)
$cam = [V]::Rq($h, $world + 0x1B8)
$lp = [V]::Rq($h, $world + 0x2960)
Write-Host ("world=0x{0:X} cam=0x{1:X} lp=0x{2:X}" -f $world,$cam,$lp)

$cfgFov = 120.0
$wantTan = [math]::Tan(($cfgFov * 0.5) * [math]::PI / 180.0)
Write-Host ("wantTan(120deg)={0:N4}" -f $wantTan)

try { [V]::SetForegroundWindow($p.MainWindowHandle) | Out-Null } catch {}
[V]::Key(0x57,$true); [V]::Key(0x10,$true) # W+Shift

$fovPass=0; $fovFail=0; $stamPass=0; $stamFail=0
$tEnd = [DateTime]::UtcNow.AddSeconds($Seconds)
while ([DateTime]::UtcNow -lt $tEnd) {
  $px = [V]::Rf($h, $cam + 0xD0)
  $st = [V]::Rf($h, $lp + 0x6A4)
  $err = if ($wantTan -gt 0.01) { [math]::Abs($px - $wantTan) / $wantTan } else { 9 }
  if ($err -le 0.12) { $fovPass++ } else { $fovFail++ }
  if ($st -ge 0.85 -and $st -le 1.05) { $stamPass++ } else { $stamFail++ }
  Write-Host ("t sample projX={0:N4} err={1:P0} stam={2:N3}" -f $px,$err,$st)
  Start-Sleep -Milliseconds 400
}
[V]::Key(0x57,$false); [V]::Key(0x10,$false)

Write-Host ("FOV samples pass={0} fail={1}" -f $fovPass,$fovFail)
Write-Host ("STAM samples pass={0} fail={1}" -f $stamPass,$stamFail)
$okFov = ($fovPass -ge 5 -and $fovPass -gt $fovFail*2)
$okStam = ($stamPass -ge 5 -and $stamPass -gt $stamFail*2)
Write-Host ("RESULT fov={0} stam={1}" -f ($(if($okFov){'PASS'}else{'FAIL'})), ($(if($okStam){'PASS'}else{'FAIL'})))
[void][V]::CloseHandle($h)
if (-not ($okFov -and $okStam)) { exit 10 } else { exit 0 }
