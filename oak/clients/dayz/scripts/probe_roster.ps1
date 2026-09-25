$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class RP3 {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a,bool b,int p);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] b,int n,out int r);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("psapi.dll", SetLastError=true)] public static extern bool EnumProcessModules(IntPtr h, IntPtr[] m, int size, out int needed);
  public static ulong U64(IntPtr h, ulong a){byte[] b=new byte[8]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,8,out r); return BitConverter.ToUInt64(b,0);}
  public static int I32(IntPtr h, ulong a){byte[] b=new byte[4]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,4,out r); return BitConverter.ToInt32(b,0);}
  public static bool Ok(ulong p){return p>=0x100000000UL && p<0x00007FFFFFFFFFFFUL;}
  public static bool SteamOk(ulong s){return s>76561197960265728UL && s<80000000000000000UL;}
  public static string Eng(IntPtr h, ulong strObj){
    if(!Ok(strObj)) return "";
    ulong data = U64(h, strObj+0x10);
    int len = I32(h, strObj+0x8);
    if(Ok(data) && len>0 && len<64){
      byte[] b=new byte[len]; int r; ReadProcessMemory(h,(IntPtr)(long)data,b,len,out r);
      return Encoding.ASCII.GetString(b).TrimEnd('\0');
    }
    if(Ok(data)){
      byte[] b=new byte[128]; int r; ReadProcessMemory(h,(IntPtr)(long)data,b,128,out r);
      string ws=Encoding.Unicode.GetString(b); int z=ws.IndexOf('\0'); return z>=0?ws.Substring(0,z):ws;
    }
    return "";
  }
}
"@

$p = Get-Process DayZ_x64 -ErrorAction Stop
$h = [RP3]::OpenProcess(0x0410, $false, $p.Id)
$mods = New-Object IntPtr[] 1024
$needed = 0
[void][RP3]::EnumProcessModules($h, $mods, 8192, [ref]$needed)
$base = [uint64]$mods[0].ToInt64()
$net = [RP3]::U64($h, $base + 0x100FBD0)
$nm  = [RP3]::U64($h, $base + 0x100FC40)
$client = [uint64]0
if ([RP3]::Ok($nm)) { $client = [RP3]::U64($h, $nm + 0x50) }
if (-not [RP3]::Ok($client) -and [RP3]::Ok($net)) { $client = [RP3]::U64($h, $net + 0x50) }
Write-Host ("base={0:X} net={1:X} nm={2:X} client={3:X}" -f $base, $net, $nm, $client)
if (-not [RP3]::Ok($client)) { [RP3]::CloseHandle($h); throw "no client" }

for ($off = 0; $off -le 0x40; $off += 4) {
  $v = [RP3]::I32($h, $client + [uint64]$off)
  if ($v -gt 0 -and $v -le 128) { Write-Host ("client+0x{0:X2} int={1}" -f $off, $v) }
}
for ($off = 0; $off -le 0x48; $off += 8) {
  $ptr = [RP3]::U64($h, $client + [uint64]$off)
  if ([RP3]::Ok($ptr)) { Write-Host ("client+0x{0:X2} ptr={1:X}" -f $off, $ptr) }
}

$board = [RP3]::U64($h, $client + 0x18)
$c1c = [RP3]::I32($h, $client + 0x1C)
$c24 = [RP3]::I32($h, $client + 0x24)
$c10 = [RP3]::I32($h, $client + 0x10)
$c28 = [RP3]::I32($h, $client + 0x28)
Write-Host ("board18={0:X} counts 1C={1} 24={2} 10={3} 28={4}" -f $board, $c1c, $c24, $c10, $c28)
$candidates = @($c1c, $c24, $c10, $c28) | Where-Object { $_ -gt 0 -and $_ -le 128 }
$n = 0
if ($candidates) { $n = ($candidates | Measure-Object -Maximum).Maximum }
Write-Host "using n=$n"

for ($i = 0; $i -lt [Math]::Min($n, 16); $i++) {
  $ident = [RP3]::U64($h, $board + [uint64]($i * 8))
  if (-not [RP3]::Ok($ident)) { Write-Host "[$i] bad"; continue }
  $n30 = [RP3]::I32($h, $ident + 0x30)
  $n24 = [RP3]::I32($h, $ident + 0x24)
  $sid = [RP3]::U64($h, $ident + 0xA0)
  $np = [RP3]::U64($h, $ident + 0xF8)
  $name = [RP3]::Eng($h, $np)
  if ([string]::IsNullOrEmpty($name)) { $np = [RP3]::U64($h, $ident + 0xF0); $name = [RP3]::Eng($h, $np) }
  if ([string]::IsNullOrEmpty($name)) { $np = [RP3]::U64($h, $ident + 0xB0); $name = [RP3]::Eng($h, $np) }
  Write-Host ("[{0}] ident={1:X} net30={2} net24={3} steam={4} steamOk={5} name='{6}'" -f $i, $ident, $n30, $n24, $sid, ([RP3]::SteamOk($sid)), $name)
}

Write-Host "--- scan client for multi steam identity tables ---"
for ($off = 0; $off -le 0x200; $off += 8) {
  $tbl = [RP3]::U64($h, $client + [uint64]$off)
  $sz = [RP3]::I32($h, $client + [uint64]$off + 8)
  if (-not [RP3]::Ok($tbl)) { continue }
  if ($sz -lt 2 -or $sz -gt 128) { continue }
  $hits = 0
  $names = New-Object System.Collections.Generic.List[string]
  for ($i = 0; $i -lt [Math]::Min($sz, 8); $i++) {
    $ident = [RP3]::U64($h, $tbl + [uint64]($i * 8))
    if (-not [RP3]::Ok($ident)) { continue }
    $sid = [RP3]::U64($h, $ident + 0xA0)
    if ([RP3]::SteamOk($sid)) {
      $hits++
      $np = [RP3]::U64($h, $ident + 0xF8)
      $nm2 = [RP3]::Eng($h, $np)
      if (-not $nm2) { $np = [RP3]::U64($h, $ident + 0xF0); $nm2 = [RP3]::Eng($h, $np) }
      $names.Add("$nm2")
    }
  }
  if ($hits -ge 2) {
    Write-Host ("HIT client+0x{0:X} tbl={1:X} sz={2} steamHits={3} names={4}" -f $off, $tbl, $sz, $hits, ([string]::Join(',', $names)))
  }
}

# Also scan Network object
Write-Host "--- scan Network object ---"
if ([RP3]::Ok($net)) {
  for ($off = 0; $off -le 0x200; $off += 8) {
    $tbl = [RP3]::U64($h, $net + [uint64]$off)
    $sz = [RP3]::I32($h, $net + [uint64]$off + 8)
    if (-not [RP3]::Ok($tbl)) { continue }
    if ($sz -lt 2 -or $sz -gt 128) { continue }
    $hits = 0
    $names = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt [Math]::Min($sz, 8); $i++) {
      $ident = [RP3]::U64($h, $tbl + [uint64]($i * 8))
      if (-not [RP3]::Ok($ident)) { continue }
      $sid = [RP3]::U64($h, $ident + 0xA0)
      if ([RP3]::SteamOk($sid)) {
        $hits++
        $np = [RP3]::U64($h, $ident + 0xF8)
        $nm2 = [RP3]::Eng($h, $np)
        if (-not $nm2) { $np = [RP3]::U64($h, $ident + 0xF0); $nm2 = [RP3]::Eng($h, $np) }
        $names.Add("$nm2")
      }
    }
    if ($hits -ge 2) {
      Write-Host ("HIT net+0x{0:X} tbl={1:X} sz={2} steamHits={3} names={4}" -f $off, $tbl, $sz, $hits, ([string]::Join(',', $names)))
    }
  }
}

[RP3]::CloseHandle($h)
