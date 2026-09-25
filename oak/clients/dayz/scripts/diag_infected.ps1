$ErrorActionPreference = 'Stop'
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class InfScan {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a, bool b, int p);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] buf, int n, out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  public static ulong Ru64(IntPtr h, ulong a) { byte[] b=new byte[8]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,8,out r); return BitConverter.ToUInt64(b,0); }
  public static int Ri32(IntPtr h, ulong a) { byte[] b=new byte[4]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,4,out r); return BitConverter.ToInt32(b,0); }
  public static float Rf(IntPtr h, ulong a) { byte[] b=new byte[4]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,4,out r); return BitConverter.ToSingle(b,0); }
  public static bool Ok(ulong p) { return p >= 0x100000000UL && p < 0x7FFFFFFFFFFFUL; }
  public static string Arma(IntPtr h, ulong s) {
    if (!Ok(s)) return null;
    int l = Ri32(h, s+8) & 0xFFFF;
    if (l <= 0 || l > 120) return null;
    byte[] buf = new byte[l]; int r;
    ReadProcessMemory(h, (IntPtr)(long)(s+0x10), buf, l, out r);
    return Encoding.ASCII.GetString(buf);
  }
}
"@
$proc = Get-Process DayZ_x64
$h = [InfScan]::OpenProcess(0x0410, $false, $proc.Id)
$base = [UInt64]([Int64]$proc.MainModule.BaseAddress.ToInt64())
$world = [InfScan]::Ru64($h, $base + [UInt64]0x4264058)
$infected = 0; $players = 0; $animals = 0; $other = 0
function Count-Ptr($data, $count) {
  for ($i=0; $i -lt $count; $i++) {
    $e = [InfScan]::Ru64($h, $data + [UInt64]($i*8))
    if (-not [InfScan]::Ok($e)) { continue }
    $t = [InfScan]::Ru64($h, $e + 0x180)
    if (-not [InfScan]::Ok($t)) { continue }
    $cfg = [InfScan]::Arma($h, [InfScan]::Ru64($h, $t + [UInt64]0xD0))
    $tn = [InfScan]::Arma($h, [InfScan]::Ru64($h, $t + [UInt64]0x98))
    if ($cfg -eq 'dayzinfected') { $script:infected++; if ($script:infected -le 5) { Write-Host ("INFECTED {0}" -f $tn) } }
    elseif ($cfg -eq 'dayzplayer') { $script:players++ }
    elseif ($cfg -eq 'dayzanimal') { $script:animals++ }
    elseif ($cfg) { $script:other++ }
  }
}
function Count-Slow($data, $count) {
  for ($i=0; $i -lt $count; $i++) {
    $ea = $data + [UInt64]($i * 0x18)
    $flag = [InfScan]::Ri32($h, $ea) -band 0xFFFF
    if ($flag -ne 1) { continue }
    $e = [InfScan]::Ru64($h, $ea + 8)
    if (-not [InfScan]::Ok($e)) { continue }
    $t = [InfScan]::Ru64($h, $e + 0x180)
    if (-not [InfScan]::Ok($t)) { continue }
    $cfg = [InfScan]::Arma($h, [InfScan]::Ru64($h, $t + [UInt64]0xD0))
    $tn = [InfScan]::Arma($h, [InfScan]::Ru64($h, $t + [UInt64]0x98))
    if ($cfg -eq 'dayzinfected') { $script:infected++; if ($script:infected -le 8) { Write-Host ("SLOW INFECTED {0}" -f $tn) } }
    elseif ($cfg -eq 'dayzplayer') { $script:players++ }
    elseif ($cfg -eq 'dayzanimal') { $script:animals++ }
    elseif ($cfg) { $script:other++; if ($script:other -le 8) { Write-Host ("SLOW OTHER {0}/{1}" -f $cfg, $tn) } }
  }
}
$near = [InfScan]::Ru64($h, $world + 0xF48); $nc = [InfScan]::Ri32($h, $world + 0xF50)
$far = [InfScan]::Ru64($h, $world + 0x1090); $fc = [InfScan]::Ri32($h, $world + 0x1098)
$slow = [InfScan]::Ru64($h, $world + 0x2010); $sc = [InfScan]::Ri32($h, $world + 0x2018)
Count-Ptr $near $nc
Count-Ptr $far $fc
Count-Slow $slow $sc
Write-Host ("totals: players={0} infected={1} animals={2} other={3} (near={4} far={5} slow={6})" -f $players,$infected,$animals,$other,$nc,$fc,$sc)
[void][InfScan]::CloseHandle($h)
