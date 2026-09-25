$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DayZTypeProbe2 {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a, bool b, int p);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] buf, int n, out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  public static ulong Ru64(IntPtr h, ulong a) {
    byte[] b = new byte[8]; int r; ReadProcessMemory(h, (IntPtr)(long)a, b, 8, out r);
    return BitConverter.ToUInt64(b, 0);
  }
  public static int Ri32(IntPtr h, ulong a) {
    byte[] b = new byte[4]; int r; ReadProcessMemory(h, (IntPtr)(long)a, b, 4, out r);
    return BitConverter.ToInt32(b, 0);
  }
  public static bool Ok(ulong p) { return p >= 0x100000000UL && p < 0x7FFFFFFFFFFFUL; }
  public static string ReadBytes(IntPtr h, ulong a, int n) {
    if (!Ok(a) || n <= 0 || n > 200) return null;
    byte[] buf = new byte[n]; int r;
    if (!ReadProcessMemory(h, (IntPtr)(long)a, buf, n, out r) || r <= 0) return null;
    for (int i = 0; i < r; i++) if (buf[i] < 32 || buf[i] > 126) { if (buf[i] == 0) { Array.Resize(ref buf, i); break; } else return null; }
    return Encoding.ASCII.GetString(buf);
  }
  public static string TryArma(IntPtr h, ulong strObj) {
    if (!Ok(strObj)) return null;
    int len = Ri32(h, strObj + 8) & 0xFFFF;
    if (len <= 0 || len > 120) return null;
    return ReadBytes(h, strObj + 0x10, len);
  }
}
"@

$proc = Get-Process DayZ_x64
$h = [DayZTypeProbe2]::OpenProcess(0x0410, $false, $proc.Id)
$base = [UInt64]([Int64]$proc.MainModule.BaseAddress.ToInt64())
$world = [DayZTypeProbe2]::Ru64($h, $base + [UInt64]0x4264058)

function Probe-Entity([UInt64]$ent, [string]$label) {
  $type = [DayZTypeProbe2]::Ru64($h, $ent + 0x180)
  Write-Host ("==== {0} ent=0x{1:X} type=0x{2:X} ====" -f $label, $ent, $type)
  if (-not [DayZTypeProbe2]::Ok($type)) { return }

  for ($o = 0x40; $o -le 0x140; $o += 8) {
    $p = [DayZTypeProbe2]::Ru64($h, $type + [UInt64]$o)
    $s = [DayZTypeProbe2]::TryArma($h, $p)
    if ($s) {
      Write-Host ("  HIT type+0x{0:X} = '{1}'" -f $o, $s)
    } else {
      # print raw ptr for known classic slots
      if ($o -eq 0x70 -or $o -eq 0xA8 -or $o -eq 0x68 -or $o -eq 0xB0) {
        Write-Host ("  type+0x{0:X} ptr=0x{1:X} ok={2}" -f $o, $p, ([DayZTypeProbe2]::Ok($p)))
        if ([DayZTypeProbe2]::Ok($p)) {
          $raw = [DayZTypeProbe2]::ReadBytes($h, $p, 32)
          if ($raw) { Write-Host ("    raw32='{0}'" -f $raw) }
          $l8 = [DayZTypeProbe2]::Ri32($h, $p + 8)
          Write-Host ("    int@+8={0}" -f $l8)
        }
      }
    }
  }
}

$near = [DayZTypeProbe2]::Ru64($h, $world + 0xF48)
$n0 = [DayZTypeProbe2]::Ru64($h, $near)
$n1 = [DayZTypeProbe2]::Ru64($h, $near + 8)
Probe-Entity $n0 "Near0"
Probe-Entity $n1 "Near1"

$local = [DayZTypeProbe2]::Ru64($h, $world + 0x2960)
Probe-Entity $local "Local"

# Slow 0x18
Write-Host "==== Slow 0x18 ===="
$slow = [DayZTypeProbe2]::Ru64($h, $world + 0x2010)
$sc = [DayZTypeProbe2]::Ri32($h, $world + 0x2018)
for ($i = 0; $i -lt [Math]::Min($sc, 15); $i++) {
  $ea = $slow + [UInt64]($i * 0x18)
  $flag = [DayZTypeProbe2]::Ri32($h, $ea) -band 0xFFFF
  $e = [DayZTypeProbe2]::Ru64($h, $ea + 8)
  if ($flag -eq 1 -and [DayZTypeProbe2]::Ok($e)) {
    $vs = [DayZTypeProbe2]::Ru64($h, $e + 0x1C8)
    Write-Host ("[{0}] ent=0x{1:X} vsOk={2}" -f $i, $e, ([DayZTypeProbe2]::Ok($vs)))
    if ([DayZTypeProbe2]::Ok($vs)) { Probe-Entity $e ("Slow$i"); break }
  } else {
    Write-Host ("[{0}] flag={1} ent=0x{2:X}" -f $i, $flag, $e)
  }
}

[void][DayZTypeProbe2]::CloseHandle($h)
