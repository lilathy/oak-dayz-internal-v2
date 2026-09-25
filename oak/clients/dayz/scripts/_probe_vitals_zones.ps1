# Find all DayZPlayers across entity lists; dump remote +0x7F8 and NetworkClientPtr
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class OV6 {
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
  public static string Eng(IntPtr h,ulong s){
    if(!Ok(s)) return "";
    ushort l=Rd<ushort>(h,s+8);
    if(l>=1&&l<=80){ byte[] b=new byte[l]; int r; ReadProcessMemory(h,(IntPtr)(long)(s+0x10),b,l,out r);
      string t=Encoding.ASCII.GetString(b,0,r).TrimEnd('\0');
      bool good=true; foreach(char ch in t) if(ch<32||ch>126) good=false; if(good&&t.Length>1) return t; }
    if(s>0x7FF000000000UL){
      byte[] raw=new byte[64]; int rr; ReadProcessMemory(h,(IntPtr)(long)s,raw,64,out rr);
      var sb=new StringBuilder();
      for(int i=0;i<64&&raw[i]!=0;i++){ if(raw[i]<32||raw[i]>126) break; sb.Append((char)raw[i]); }
      if(sb.Length>1) return sb.ToString();
    }
    return "";
  }
  public static string Rtti(IntPtr h,ulong obj,ulong baseAddr){
    if(!Ok(obj)) return "";
    ulong vt=Ru(h,obj);
    if(vt<baseAddr||vt>baseAddr+0x8000000) return "";
    ulong col=Ru(h,vt-8);
    if(col>=baseAddr&&col<baseAddr+0x8000000){
      int rva=Ri(h,col+0x0C);
      ulong td=baseAddr+(ulong)(uint)rva;
      if(td>baseAddr&&td<baseAddr+0x8000000){
        byte[] raw=new byte[96]; int rr; ReadProcessMemory(h,(IntPtr)(long)(td+0x10),raw,96,out rr);
        var sb=new StringBuilder();
        for(int i=0;i<96&&raw[i]!=0;i++){ if(raw[i]<32||raw[i]>126) break; sb.Append((char)raw[i]); }
        return sb.ToString();
      }
    }
    return "";
  }
  public static ulong ModBase(IntPtr h){ IntPtr[] m=new IntPtr[4]; int n; EnumProcessModulesEx(h,m,IntPtr.Size*4,out n,3); return (ulong)m[0].ToInt64(); }
}
"@

$proc = Get-Process DayZ_x64 | Select-Object -First 1
$h = [OV6]::OpenProcess(0x0410, $false, $proc.Id)
$base = [OV6]::ModBase($h)
$world = [OV6]::Ru($h, $base + 0x4264058)
Write-Host ("PID={0} base=0x{1:X} world=0x{2:X}" -f $proc.Id, $base, $world)

$players = New-Object System.Collections.Generic.List[UInt64]
foreach ($loff in @(0xF48, 0x1090, 0x2010, 0x1F90)) {
  $data = [OV6]::Ru($h, $world + [UInt64]$loff)
  $cnt = [OV6]::Ri($h, $world + [UInt64]$loff + 8)
  Write-Host ("list+0x{0:X} data=0x{1:X} cnt={2}" -f $loff, $data, $cnt)
  if (-not [OV6]::Ok($data) -or $cnt -lt 1 -or $cnt -gt 5000) { continue }
  $shown = 0
  for ($i = 0; $i -lt [Math]::Min($cnt, 500); $i++) {
    $ent = [OV6]::Ru($h, $data + [UInt64]($i * 8))
    if (-not [OV6]::Ok($ent)) { continue }
    $rt = [OV6]::Rtti($h, $ent, $base)
    if ($rt -notmatch "DayZPlayer") { continue }
    if (-not $players.Contains($ent)) { [void]$players.Add($ent) }
    $typ = [OV6]::Ru($h, $ent + 0x180)
    $cfg = ""
    if ([OV6]::Ok($typ)) { $cfg = [OV6]::Eng($h, [OV6]::Ru($h, $typ + 0xD0)) }
    $net = [OV6]::Ri($h, $ent + 0x6E4)
    $ncp = [OV6]::Ru($h, $ent + 0x50)
    $dm = [OV6]::Ru($h, $ent + 0x700)
    $eh = [OV6]::Ru($h, $ent + 0x108)
    $st = [OV6]::Ru($h, $ent + 0x6F0)
    Write-Host ("  ent=0x{0:X} cfg={1} net={2} ncp=0x{3:X} dm=0x{4:X} eh=0x{5:X} st=0x{6:X}" -f $ent, $cfg, $net, $ncp, $dm, $eh, $st)
    $shown++
  }
  Write-Host ("  dayzplayers={0}" -f $shown)
}
Write-Host ("unique players={0}" -f $players.Count)

foreach ($ent in $players) {
  Write-Host ("`n==== PLAYER 0x{0:X} ====" -f $ent)
  # Scan every 8-byte slot for zone-holder with Health/Blood/Shock names OR normalized HP float
  $hits = 0
  for ($off = 0x40; $off -le 0x900 -and $hits -lt 20; $off += 8) {
    $obj = [OV6]::Ru($h, $ent + [UInt64]$off)
    if (-not [OV6]::Ok($obj)) { continue }
    if ($obj -ge $base -and $obj -lt ($base + 0x8000000)) { continue }
    $arr = [OV6]::Ru($h, $obj + 0x28)
    $c = [OV6]::Ri($h, $obj + 0x34)
    if (-not [OV6]::Ok($arr) -or $c -lt 1 -or $c -gt 64) {
      $arr = [OV6]::Ru($h, $obj + 0x20); $c = [OV6]::Ri($h, $obj + 0x28)
    }
    if (-not [OV6]::Ok($arr) -or $c -lt 1 -or $c -gt 64) { continue }
    $names = New-Object System.Collections.Generic.List[string]
    $hasVals = $false
    for ($i = 0; $i -lt [Math]::Min($c, 12); $i++) {
      $slot = $arr + [UInt64]($i * 0x10)
      $np = [OV6]::Ru($h, $slot)
      $sn = [OV6]::Eng($h, $np)
      if (-not $sn -and [OV6]::Ok($np)) {
        # try cstring
        $raw = New-Object byte[] 32
        $rr = 0
        [void][OV6]::ReadProcessMemory($h, [IntPtr]$np, $raw, 32, [ref]$rr)
        $sb = New-Object System.Text.StringBuilder
        for ($k = 0; $k -lt 32 -and $raw[$k] -ne 0; $k++) {
          if ($raw[$k] -lt 32 -or $raw[$k] -gt 126) { $sb.Clear(); break }
          [void]$sb.Append([char]$raw[$k])
        }
        if ($sb.Length -gt 1) { $sn = $sb.ToString() }
      }
      $fv = [OV6]::Rf($h, $slot + 8)
      if ($sn) { [void]$names.Add(("{0}={1:F2}" -f $sn, $fv)) }
      if ($fv -gt 0.01 -and $fv -le 5000) { $hasVals = $true }
      if ([OV6]::Ok($np)) {
        $z0 = [OV6]::Rf($h, $np); $z4 = [OV6]::Rf($h, $np + 4)
        if ($z0 -gt 0 -and $z0 -le 1.05 -and $z4 -gt 0.05 -and $z4 -le 1.05) {
          [void]$names.Add(("z{0}:{1:F2}/{2:F2}" -f $i, $z0, $z4))
          $hasVals = $true
        }
      }
    }
    $joined = $names -join "|"
    $f10 = [OV6]::Rf($h, $obj + 0x10)
    $interesting = ($joined -match "Health|Blood|Shock|Global|Energy|Water") -or
                   ($f10 -gt 0.01 -and $f10 -le 1.05) -or
                   ($hasVals -and $c -le 8)
    if ($interesting -or ($c -ge 2 -and $c -le 8 -and $hasVals)) {
      Write-Host ("  ZONE ent+0x{0:X} obj=0x{1:X} c={2} f10={3:F3} rtti={4}" -f $off, $obj, $c, $f10, ([OV6]::Rtti($h, $obj, $base)))
      Write-Host ("       {0}" -f $joined)
      $hits++
    }
  }
  Write-Host ("  zoneHits={0}" -f $hits)

  # Dump PlayerStats records under +0x6F0
  $st = [OV6]::Ru($h, $ent + 0x6F0)
  if ([OV6]::Ok($st)) {
    Write-Host ("  Stats root=0x{0:X} rtti={1}" -f $st, ([OV6]::Rtti($h, $st, $base)))
    foreach ($boff in @(0,8,0x10,0x18,0x20,0x28,0x30,0x38,0x40,0x48)) {
      $arr = [OV6]::Ru($h, $st + [UInt64]$boff)
      if (-not [OV6]::Ok($arr)) { continue }
      for ($i = 0; $i -lt 40; $i++) {
        $rec = [OV6]::Ru($h, $arr + [UInt64]($i * 8))
        if (-not [OV6]::Ok($rec)) { continue }
        $v = [OV6]::Rf($h, $rec + 0x2C)
        if (-not ($v -eq $v) -or $v -lt 0 -or $v -gt 6000) { continue }
        $sn = ""
        foreach ($no in @(0x8,0x10,0x18,0x0)) {
          $sn = [OV6]::Eng($h, [OV6]::Ru($h, $rec + [UInt64]$no))
          if ($sn) { break }
          # cstring in module
          $np = [OV6]::Ru($h, $rec + [UInt64]$no)
          if ($np -gt $base -and $np -lt ($base + 0x8000000)) {
            $raw = New-Object byte[] 32; $rr=0
            [void][OV6]::ReadProcessMemory($h, [IntPtr]$np, $raw, 32, [ref]$rr)
            $sb = New-Object System.Text.StringBuilder
            for ($k=0; $k -lt 32 -and $raw[$k] -ne 0; $k++) {
              if ($raw[$k] -lt 32 -or $raw[$k] -gt 126) { $sb.Clear(); break }
              [void]$sb.Append([char]$raw[$k])
            }
            if ($sb.Length -gt 1) { $sn = $sb.ToString(); break }
          }
        }
        if ($sn -match "Health|Blood|Shock|Energy|Water|Stamina|food|water" -or ($v -ge 90 -and $v -le 100) -or ($v -ge 4000 -and $v -le 5500)) {
          Write-Host ("    st+0x{0:X}[{1}] v={2:F2} name='{3}' rec=0x{4:X}" -f $boff, $i, $v, $sn, $rec)
        }
      }
    }
  }

  # Follow skeleton? +0x7E8 was non-null earlier
  $sk = [OV6]::Ru($h, $ent + 0x7E8)
  Write-Host ("  skeleton+0x7E8=0x{0:X} rtti={1}" -f $sk, ([OV6]::Rtti($h, $sk, $base)))
}

[void][OV6]::CloseHandle($h)
