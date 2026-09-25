# Hunt Health/Blood/Shock labels under DayZPlayer objects (external RPM).
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class OV4 {
  [DllImport("kernel32")] public static extern IntPtr OpenProcess(int a,bool b,int c);
  [DllImport("kernel32")] public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] b,int n,out int r);
  [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("psapi")] public static extern bool EnumProcessModulesEx(IntPtr h,IntPtr[] m,int cb,out int n,int f);
  public static bool Ok(ulong p){return p>0x100000000UL&&p<0x7FFFFFFFFFFFUL;}
  public static T Rd<T>(IntPtr h,ulong a) where T:struct {
    int s=Marshal.SizeOf(typeof(T)); byte[] b=new byte[s]; int r;
    if(!ReadProcessMemory(h,(IntPtr)(long)a,b,s,out r)||r!=s) return default(T);
    GCHandle g=GCHandle.Alloc(b,GCHandleType.Pinned);
    try{return (T)Marshal.PtrToStructure(g.AddrOfPinnedObject(),typeof(T));}finally{g.Free();}
  }
  public static ulong Ru(IntPtr h,ulong a){return Rd<ulong>(h,a);}
  public static int Ri(IntPtr h,ulong a){return Rd<int>(h,a);}
  public static float Rf(IntPtr h,ulong a){return Rd<float>(h,a);}
  public static byte Rb(IntPtr h,ulong a){return Rd<byte>(h,a);}
  public static string Eng(IntPtr h,ulong s){
    if(!Ok(s)) return "";
    ushort l=Rd<ushort>(h,s+8);
    if(l>=1&&l<=80){ byte[] b=new byte[l]; int r; ReadProcessMemory(h,(IntPtr)(long)(s+0x10),b,l,out r);
      string t=Encoding.ASCII.GetString(b,0,r).TrimEnd('\0');
      bool good=true; foreach(char ch in t) if(ch<32||ch>126) good=false; if(good&&t.Length>1) return t; }
    if(s>0x7FF000000000UL){
      byte[] raw=new byte[48]; int rr; ReadProcessMemory(h,(IntPtr)(long)s,raw,48,out rr);
      var sb=new StringBuilder();
      for(int i=0;i<48&&raw[i]!=0;i++){ if(raw[i]<32||raw[i]>126) break; sb.Append((char)raw[i]); }
      if(sb.Length>1) return sb.ToString();
    }
    return "";
  }
  public static ulong ModBase(IntPtr h){ IntPtr[] m=new IntPtr[4]; int n; EnumProcessModulesEx(h,m,IntPtr.Size*4,out n,3); return (ulong)m[0].ToInt64(); }
}
"@

$proc = Get-Process DayZ_x64 | Select-Object -First 1
$h = [OV4]::OpenProcess(0x0410, $false, $proc.Id)
$base = [OV4]::ModBase($h)
$world = [OV4]::Ru($h, $base + 0x4264058)
$localStub = [OV4]::Ru($h, $world + 0x2960)

$players = New-Object System.Collections.Generic.List[UInt64]
foreach ($loff in @([UInt64]0xF48, [UInt64]0x1090)) {
  $data = [OV4]::Ru($h, $world + $loff)
  $cnt = [OV4]::Ri($h, $world + $loff + 8)
  if (-not [OV4]::Ok($data) -or $cnt -lt 1 -or $cnt -gt 4096) { continue }
  for ($i = 0; $i -lt [Math]::Min($cnt, 200); $i++) {
    $ent = [OV4]::Ru($h, $data + [UInt64]($i * 8))
    if (-not [OV4]::Ok($ent)) { continue }
    $typ = [OV4]::Ru($h, $ent + 0x180)
    if (-not [OV4]::Ok($typ)) { continue }
    $cfg = [OV4]::Eng($h, [OV4]::Ru($h, $typ + 0xD0))
    if ($cfg -ne "dayzplayer") { continue }
    if (-not $players.Contains($ent)) { [void]$players.Add($ent) }
  }
}
Write-Host ("players={0} localStub=0x{1:X}" -f $players.Count, $localStub)

foreach ($ent in $players) {
  $isLocal = ($ent -eq $localStub)
  $nid = [OV4]::Ri($h, $ent + 0x6E4)
  $dead = [OV4]::Rb($h, $ent + 0xE2)
  Write-Host ("`n==== ent=0x{0:X} local={1} net={2} dead={3} ====" -f $ent, [int]$isLocal, $nid, $dead)
  foreach ($off in @(0x6F0, 0x6F8, 0x700, 0x708, 0x710, 0x718, 0x610, 0x640, 0x6A0, 0x658, 0x7E8)) {
    Write-Host ("  +0x{0:X} = 0x{1:X}" -f $off, [OV4]::Ru($h, $ent + [UInt64]$off))
  }

  $hits = 0
  for ($off = 0x80; $off -le 0xB00 -and $hits -lt 50; $off += 8) {
    $obj = [OV4]::Ru($h, $ent + [UInt64]$off)
    if (-not [OV4]::Ok($obj)) { continue }
    if ($obj -ge $base -and $obj -lt ($base + 0x8000000)) { continue }
    for ($io = 0; $io -le 0x120; $io += 8) {
      $np = [OV4]::Ru($h, $obj + [UInt64]$io)
      if (-not [OV4]::Ok($np)) { continue }
      $sn = [OV4]::Eng($h, $np)
      if (-not $sn) { continue }
      $want = @("Health", "Blood", "Shock", "GlobalHealth", "Energy", "Water", "Stamina")
      if ($want -notcontains $sn) { continue }
      $vals = @()
      foreach ($d in @(8, 4, -4, -8, 12, 16, -12)) {
        $fo = $io + $d
        if ($fo -lt 0 -or $fo -gt 0x200) { continue }
        $v = [OV4]::Rf($h, $obj + [UInt64]$fo)
        if ($v -eq $v -and $v -ge 0 -and $v -le 20000) {
          $vals += ("d{0}={1:F2}" -f $d, $v)
        }
      }
      Write-Host ("  HIT ent+0x{0:X} obj+0x{1:X} '{2}' {3}" -f $off, $io, $sn, ($vals -join " "))
      $hits++
    }
  }
  Write-Host ("  nameHits={0}" -f $hits)

  Write-Host "  entity float candidates:"
  for ($off = 0x500; $off -le 0x900; $off += 4) {
    $v = [OV4]::Rf($h, $ent + [UInt64]$off)
    if ($v -ne $v) { continue }
    if (($v -gt 0.2 -and $v -le 1.0) -or ($v -ge 90 -and $v -le 100) -or
        ($v -ge 4000 -and $v -le 5500) -or ($v -ge 1000 -and $v -le 1500)) {
      Write-Host ("    ent+0x{0:X} = {1:F3}" -f $off, $v)
    }
  }
}
[void][OV4]::CloseHandle($h)
