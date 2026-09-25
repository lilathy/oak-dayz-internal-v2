$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class DayZDiag2 {
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
  public static float Rf(IntPtr h, ulong a) {
    byte[] b = new byte[4]; int r; ReadProcessMemory(h, (IntPtr)(long)a, b, 4, out r);
    return BitConverter.ToSingle(b, 0);
  }
  public static bool Ok(ulong p) { return p >= 0x100000000UL && p < 0x7FFFFFFFFFFFUL; }
}
"@

$proc = Get-Process DayZ_x64
$h = [DayZDiag2]::OpenProcess(0x0410, $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed" }

$modBase = [UInt64]([Int64]$proc.MainModule.BaseAddress.ToInt64())
$worldOff = [UInt64]0x4264058
$world = [DayZDiag2]::Ru64($h, ($modBase + $worldOff))
Write-Host ("base=0x{0:X} world=0x{1:X}" -f $modBase, $world)

$cam = [DayZDiag2]::Ru64($h, $world + 0x1B8)
$local = [DayZDiag2]::Ru64($h, $world + 0x2960)
$pon = [DayZDiag2]::Ru64($h, $world + 0x2968)
Write-Host ("camera=0x{0:X} local=0x{1:X} playerOn=0x{2:X}" -f $cam, $local, $pon)

if ([DayZDiag2]::Ok($cam)) {
  Write-Host ("camPos=({0:N1},{1:N1},{2:N1})" -f ([DayZDiag2]::Rf($h, $cam + 0x2C)), ([DayZDiag2]::Rf($h, $cam + 0x30)), ([DayZDiag2]::Rf($h, $cam + 0x34)))
}

function Show-List([string]$name, [UInt64]$off, [UInt64]$coff) {
  $q = [DayZDiag2]::Ru64($h, $world + $off)
  $c = [DayZDiag2]::Ri32($h, $world + $coff)
  $e = [DayZDiag2]::Ru64($h, $world + $off + 8)
  Write-Host ("{0}: q=0x{1:X} count={2} end=0x{3:X}" -f $name, $q, $c, $e)
  if ([DayZDiag2]::Ok($q)) {
    Write-Host ("  obj[0]=0x{0:X} [8]={1} [10]=0x{2:X}" -f ([DayZDiag2]::Ru64($h, $q)), ([DayZDiag2]::Ri32($h, $q + 8)), ([DayZDiag2]::Ru64($h, $q + 0x10)))
  }

  $data = $q
  $count = $c
  if (-not [DayZDiag2]::Ok($data) -or $count -le 0 -or $count -gt 5000) {
    if ([DayZDiag2]::Ok($q)) {
      $data = [DayZDiag2]::Ru64($h, $q)
      $count = [DayZDiag2]::Ri32($h, $q + 8)
    }
  }
  if ((-not [DayZDiag2]::Ok($data) -or $count -le 0) -and [DayZDiag2]::Ok($q) -and [DayZDiag2]::Ok($e) -and ($e -gt $q) -and ((($e - $q) % 8) -eq 0)) {
    $data = $q
    $count = [int](($e - $q) / 8)
  }
  Write-Host ("  resolved data=0x{0:X} count={1}" -f $data, $count)

  if ([DayZDiag2]::Ok($data) -and $count -gt 0 -and $count -lt 5000) {
    $n = [Math]::Min($count, 8)
    for ($i = 0; $i -lt $n; $i++) {
      $ent = [DayZDiag2]::Ru64($h, $data + [UInt64]($i * 8))
      $vs = [UInt64]0
      $cfg = "?"
      if ([DayZDiag2]::Ok($ent)) {
        $vs = [DayZDiag2]::Ru64($h, $ent + 0x1C8)
        $type = [DayZDiag2]::Ru64($h, $ent + 0x180)
        if ([DayZDiag2]::Ok($type)) {
          $cfgPtr = [DayZDiag2]::Ru64($h, $type + 0xA8)
          if ([DayZDiag2]::Ok($cfgPtr)) {
            $len = [DayZDiag2]::Ri32($h, $cfgPtr + 8) -band 0xFFFF
            if ($len -gt 0 -and $len -lt 64) {
              $buf = New-Object byte[] $len
              $rr = 0
              [void][DayZDiag2]::ReadProcessMemory($h, [IntPtr]($cfgPtr + 0x10), $buf, $len, [ref]$rr)
              $cfg = [Text.Encoding]::ASCII.GetString($buf)
            }
          }
        }
      }
      $pos = ""
      if ([DayZDiag2]::Ok($vs)) {
        $pos = (" pos=({0:N0},{1:N0},{2:N0})" -f ([DayZDiag2]::Rf($h, $vs + 0x2C)), ([DayZDiag2]::Rf($h, $vs + 0x30)), ([DayZDiag2]::Rf($h, $vs + 0x34)))
      }
      Write-Host ("  [{0}] ent=0x{1:X} vsOk={2} cfg={3}{4}" -f $i, $ent, ([DayZDiag2]::Ok($vs)), $cfg, $pos)
    }
  }
}

Show-List "Near" 0xF48 0xF50
Show-List "Far" 0x1090 0x1098
Show-List "Slow" 0x2010 0x2018
Write-Host ("SlowValid@1F90={0}" -f ([DayZDiag2]::Ri32($h, $world + 0x1F90)))
Show-List "Item" 0x2060 0x2068

if ([DayZDiag2]::Ok($local)) {
  Write-Host ("local VS=0x{0:X} FVS=0x{1:X}" -f ([DayZDiag2]::Ru64($h, $local + 0x1C8)), ([DayZDiag2]::Ru64($h, $local + 0x120)))
  Write-Host ("local skel 7E0=0x{0:X} 7E8=0x{1:X}" -f ([DayZDiag2]::Ru64($h, $local + 0x7E0)), ([DayZDiag2]::Ru64($h, $local + 0x7E8)))
  # LocalOffset -0xA8 as manager->entity
  $viaOff = [DayZDiag2]::Ru64($h, [UInt64]($local - 0xA8))
  Write-Host ("read(local-0xA8)=0x{0:X} ok={1}" -f $viaOff, ([DayZDiag2]::Ok($viaOff)))
}
if ([DayZDiag2]::Ok($pon)) {
  Write-Host ("playerOn VS=0x{0:X}" -f ([DayZDiag2]::Ru64($h, $pon + 0x1C8)))
}

$bl = [DayZDiag2]::Ru64($h, $world + 0xE00)
$bc = [DayZDiag2]::Ri32($h, $world + 0xE08)
Write-Host ("BulletList=0x{0:X} BulletCount={1}" -f $bl, $bc)
if ([DayZDiag2]::Ok($bl)) {
  Write-Host ("  asObj data=0x{0:X} count={1}" -f ([DayZDiag2]::Ru64($h, $bl)), ([DayZDiag2]::Ri32($h, $bl + 8)))
}

[void][DayZDiag2]::CloseHandle($h)
