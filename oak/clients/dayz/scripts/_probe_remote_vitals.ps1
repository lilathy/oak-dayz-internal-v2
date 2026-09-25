# External RPM probe: find DamageManager / health floats on local + remote DayZPlayers.
param([int]$ProcessId = 0)
$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OakVitP {
  [DllImport("kernel32")] public static extern IntPtr OpenProcess(int a, bool b, int c);
  [DllImport("kernel32")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int n, out int r);
  [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("psapi")] public static extern bool EnumProcessModulesEx(IntPtr h, IntPtr[] m, int cb, out int n, int f);
  public static bool Ok(ulong p) { return p > 0x100000000UL && p < 0x7FFFFFFFFFFFUL; }
  public static T Rd<T>(IntPtr h, ulong a) where T : struct {
    int s = Marshal.SizeOf(typeof(T));
    byte[] b = new byte[s]; int r;
    if (!ReadProcessMemory(h, (IntPtr)(long)a, b, s, out r) || r != s) return default(T);
    GCHandle g = GCHandle.Alloc(b, GCHandleType.Pinned);
    try { return (T)Marshal.PtrToStructure(g.AddrOfPinnedObject(), typeof(T)); }
    finally { g.Free(); }
  }
  public static ulong Ru(IntPtr h, ulong a) { return Rd<ulong>(h, a); }
  public static int Ri(IntPtr h, ulong a) { return Rd<int>(h, a); }
  public static float Rf(IntPtr h, ulong a) { return Rd<float>(h, a); }
  public static string Eng(IntPtr h, ulong s) {
    if (!Ok(s)) return "";
    ushort l = Rd<ushort>(h, s + 8);
    if (l >= 1 && l <= 80) {
      byte[] b = new byte[l]; int r;
      ReadProcessMemory(h, (IntPtr)(long)(s + 0x10), b, l, out r);
      string t = Encoding.ASCII.GetString(b, 0, r).TrimEnd('\0');
      bool good = true;
      foreach (char ch in t) if (ch < 32 || ch > 126) good = false;
      if (good && t.Length > 0) return t;
    }
    byte[] raw = new byte[48]; int rr;
    ReadProcessMemory(h, (IntPtr)(long)s, raw, 48, out rr);
    var sb = new StringBuilder();
    for (int i = 0; i < 48 && raw[i] != 0; i++) {
      if (raw[i] < 32 || raw[i] > 126) break;
      sb.Append((char)raw[i]);
    }
    return sb.ToString();
  }
  public static ulong ModuleBase(IntPtr h) {
    IntPtr[] m = new IntPtr[8]; int n;
    if (!EnumProcessModulesEx(h, m, IntPtr.Size * m.Length, out n, 0x03)) return 0;
    return (ulong)m[0].ToInt64();
  }
}
"@

function Dump-Dmg($h, $base, $dmg, $tag) {
  if (-not [OakVitP]::Ok($dmg) -or ($dmg -ge $base -and $dmg -lt ($base + 0x8000000))) {
    Write-Host "$tag INVALID dmg=$dmg"; return
  }
  $a28 = [OakVitP]::Ru($h, $dmg + 0x28)
  $c30 = [OakVitP]::Ri($h, $dmg + 0x30)
  $c34 = [OakVitP]::Ri($h, $dmg + 0x34)
  $f08 = [OakVitP]::Rf($h, $dmg + 0x08)
  $f0C = [OakVitP]::Rf($h, $dmg + 0x0C)
  $f10 = [OakVitP]::Rf($h, $dmg + 0x10)
  $f14 = [OakVitP]::Rf($h, $dmg + 0x14)
  $f18 = [OakVitP]::Rf($h, $dmg + 0x18)
  Write-Host ("{0} dmg=0x{1:X} a28=0x{2:X} c30={3} c34={4} f08={5:F3} f0C={6:F3} f10={7:F3} f14={8:F3} f18={9:F3}" -f `
    $tag, $dmg, $a28, $c30, $c34, $f08, $f0C, $f10, $f14, $f18)
  $tc = if ($c34 -gt 0 -and $c34 -le 64) { $c34 } elseif ($c30 -gt 0 -and $c30 -le 64) { $c30 } else { 0 }
  if ([OakVitP]::Ok($a28) -and $tc -gt 0) {
    for ($i = 0; $i -lt [Math]::Min($tc, 16); $i++) {
      $slot = $a28 + [UInt64]($i * 0x10)
      $zone = [OakVitP]::Ru($h, $slot)
      $namep = [OakVitP]::Ru($h, $slot + 8)
      $sn = [OakVitP]::Eng($h, $namep)
      $z0 = 0.0; $z4 = 0.0; $z8 = 0.0; $zC = 0.0
      if ([OakVitP]::Ok($zone)) {
        $z0 = [OakVitP]::Rf($h, $zone)
        $z4 = [OakVitP]::Rf($h, $zone + 4)
        $z8 = [OakVitP]::Rf($h, $zone + 8)
        $zC = [OakVitP]::Rf($h, $zone + 0xC)
        if (-not $sn) { $sn = [OakVitP]::Eng($h, [OakVitP]::Ru($h, $zone + 8)) }
      }
      $e8 = [OakVitP]::Rf($h, $slot + 8)
      # reinterpret slot+8 as float when name empty
      Write-Host ("  [{0}] sn='{1}' zone=0x{2:X} z0={3:F3} z4={4:F3} z8={5:F3} zC={6:F3} slotF8={7:F3}" -f `
        $i, $sn, $zone, $z0, $z4, $z8, $zC, $e8)
    }
  }
  # Try pointer-table layout at +0x18/+0x20
  foreach ($pair in @(@(0x18,0x20), @(0x20,0x28), @(0x30,0x38), @(0x38,0x40), @(0x40,0x48))) {
    $arr = [OakVitP]::Ru($h, $dmg + [UInt64]$pair[0])
    $cnt = [OakVitP]::Ri($h, $dmg + [UInt64]$pair[1])
    if ([OakVitP]::Ok($arr) -and $cnt -gt 0 -and $cnt -le 64) {
      Write-Host ("  table@+0x{0:X}/+0x{1:X} arr=0x{2:X} cnt={3}" -f $pair[0], $pair[1], $arr, $cnt)
    }
  }
}

$procs = @(Get-Process DayZ_x64 -EA SilentlyContinue)
if ($ProcessId) { $procs = @($procs | Where-Object Id -eq $ProcessId) }
if (-not $procs) { throw "No DayZ_x64" }

foreach ($proc in $procs) {
  Write-Host "`n======== PID $($proc.Id) ========"
  $h = [OakVitP]::OpenProcess(0x0410, $false, $proc.Id)
  if ($h -eq [IntPtr]::Zero) { Write-Host "OpenProcess failed"; continue }
  $base = [OakVitP]::ModuleBase($h)
  Write-Host ("base=0x{0:X}" -f $base)
  $world = [OakVitP]::Ru($h, $base + 0x4264058)
  Write-Host ("world=0x{0:X}" -f $world)
  if (-not [OakVitP]::Ok($world)) { [OakVitP]::CloseHandle($h); continue }

  $localStub = [OakVitP]::Ru($h, $world + 0x2960)
  Write-Host ("localStub=0x{0:X}" -f $localStub)

  function Resolve-List([UInt64]$listOff) {
    $slot = $world + $listOff
    $q = [OakVitP]::Ru($h, $slot)
    $end = [OakVitP]::Ru($h, $slot + 8)
    # embed start/end
    if ([OakVitP]::Ok($q) -and [OakVitP]::Ok($end) -and $end -gt $q -and (($end - $q) % 8) -eq 0) {
      $c = [int](($end - $q) / 8)
      if ($c -gt 0 -and $c -le 4096) {
        return [pscustomobject]@{ Data = $q; Count = $c; How = "embed-start-end" }
      }
    }
    $c0 = [OakVitP]::Ri($h, $slot + 8)
    if ([OakVitP]::Ok($q) -and $c0 -gt 0 -and $c0 -le 4096) {
      return [pscustomobject]@{ Data = $q; Count = $c0; How = "embed-data-count" }
    }
    if ([OakVitP]::Ok($q)) {
      $d = [OakVitP]::Ru($h, $q)
      $c = [OakVitP]::Ri($h, $q + 8)
      if ([OakVitP]::Ok($d) -and $c -gt 0 -and $c -le 4096) {
        return [pscustomobject]@{ Data = $d; Count = $c; How = "obj-data-count" }
      }
      $d2 = [OakVitP]::Ru($h, $q + 0x10)
      $c2 = [OakVitP]::Ri($h, $q + 8)
      if ([OakVitP]::Ok($d2) -and $c2 -gt 0 -and $c2 -le 4096) {
        return [pscustomobject]@{ Data = $d2; Count = $c2; How = "obj-count-data@10" }
      }
    }
    return $null
  }

  $players = @()
  foreach ($loff in @([UInt64]0xF48, [UInt64]0x1090)) {
    $lst = Resolve-List $loff
    if (-not $lst) { Write-Host ("list 0x{0:X}: fail" -f $loff); continue }
    Write-Host ("list 0x{0:X}: data=0x{1:X} count={2} how={3}" -f $loff, $lst.Data, $lst.Count, $lst.How)
    for ($i = 0; $i -lt [Math]::Min($lst.Count, 128); $i++) {
      $ent = [OakVitP]::Ru($h, $lst.Data + [UInt64]($i * 8))
      if (-not [OakVitP]::Ok($ent)) { continue }
      $typ = [OakVitP]::Ru($h, $ent + 0x180)
      if (-not [OakVitP]::Ok($typ)) { continue }
      $cfgP = [OakVitP]::Ru($h, $typ + 0xD0)
      $cfg = [OakVitP]::Eng($h, $cfgP)
      if ($cfg -ne "dayzplayer") { continue }
      $dup = $false
      foreach ($ex in $players) { if ($ex.Ent -eq $ent) { $dup = $true; break } }
      if ($dup) { continue }
      $isLocal = ($ent -eq $localStub)
      $nid = [OakVitP]::Ri($h, $ent + 0x6E4)
      if ($nid -eq 0) { $nid = [OakVitP]::Ri($h, $ent + 0x6EC) }
      $players += [pscustomobject]@{ Ent = $ent; Local = $isLocal; Net = $nid; Idx = $i }
    }
  }
  # Always include localStub if it looks like a player
  if ([OakVitP]::Ok($localStub)) {
    $have = $false
    foreach ($ex in $players) { if ($ex.Ent -eq $localStub) { $have = $true; break } }
    if (-not $have) {
      $players += [pscustomobject]@{ Ent = $localStub; Local = $true; Net = 0; Idx = -1 }
    }
  }
  Write-Host ("dayzplayers near={0}" -f $players.Count)
  foreach ($p in $players) {
    Write-Host ("`n-- player local={0} net={1} ent=0x{2:X} near[{3}] --" -f [int]$p.Local, $p.Net, $p.Ent, $p.Idx)
    foreach ($off in @(0x6F8, 0x700, 0x708, 0x710, 0x6A8, 0x6E8, 0x6F0, 0x718, 0x720, 0x728, 0x690, 0x698, 0x6A0, 0x7E0, 0x7E8)) {
      $cand = [OakVitP]::Ru($h, $p.Ent + [UInt64]$off)
      if (-not [OakVitP]::Ok($cand)) { continue }
      if ($cand -ge $base -and $cand -lt ($base + 0x8000000)) { continue }
      $a28 = [OakVitP]::Ru($h, $cand + 0x28)
      $c34 = [OakVitP]::Ri($h, $cand + 0x34)
      $c30 = [OakVitP]::Ri($h, $cand + 0x30)
      $f10 = [OakVitP]::Rf($h, $cand + 0x10)
      $zoneish = [OakVitP]::Ok($a28) -and (($c34 -gt 0 -and $c34 -le 64) -or ($c30 -gt 0 -and $c30 -le 64))
      $fh = ($f10 -eq $f10) -and (($f10 -gt 0.05 -and $f10 -le 1.05) -or ($f10 -ge 5 -and $f10 -le 100))
      if (-not $zoneish -and -not $fh) { continue }
      Write-Host ("HIT off=0x{0:X} zoneish={1} fh={2} f10={3:F3}" -f $off, [int]$zoneish, [int]$fh, $f10)
      Dump-Dmg $h $base $cand ("off0x{0:X}" -f $off)
    }
    # Wide scan 0x5C0..0x900 for zone holders
    Write-Host "wide scan:"
    $hits = 0
    for ($off = 0x5C0; $off -le 0x900 -and $hits -lt 12; $off += 8) {
      $cand = [OakVitP]::Ru($h, $p.Ent + [UInt64]$off)
      if (-not [OakVitP]::Ok($cand)) { continue }
      if ($cand -ge $base -and $cand -lt ($base + 0x8000000)) { continue }
      $a28 = [OakVitP]::Ru($h, $cand + 0x28)
      $c34 = [OakVitP]::Ri($h, $cand + 0x34)
      $c30 = [OakVitP]::Ri($h, $cand + 0x30)
      $zoneish = [OakVitP]::Ok($a28) -and (($c34 -gt 0 -and $c34 -le 64) -or ($c30 -gt 0 -and $c30 -le 64))
      if (-not $zoneish) { continue }
      $hits++
      Write-Host ("  SCAN HIT off=0x{0:X} c34={1} c30={2}" -f $off, $c34, $c30)
      Dump-Dmg $h $base $cand ("scan0x{0:X}" -f $off)
    }
  }
  [void][OakVitP]::CloseHandle($h)
}
