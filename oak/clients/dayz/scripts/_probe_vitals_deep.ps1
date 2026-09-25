# Deep vitals probe: localStub + remote EntityHealth / EntityType DamageSystemData
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class OV5 {
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
    // MSVC RTTI: vt-8 -> col, +0x0C -> type_desc, +0x14 -> name
    ulong col=Ru(h,vt-8);
    if(!Ok(col)&&col!=0) {
      // relative RVA style sometimes
    }
    if(col>=baseAddr&&col<baseAddr+0x8000000){
      int rva=Ri(h,col+0x0C);
      ulong td=baseAddr+(ulong)(uint)rva;
      if(td>baseAddr&&td<baseAddr+0x8000000){
        byte[] raw=new byte[80]; int rr; ReadProcessMemory(h,(IntPtr)(long)(td+0x10),raw,80,out rr);
        var sb=new StringBuilder();
        for(int i=0;i<80&&raw[i]!=0;i++){ if(raw[i]<32||raw[i]>126) break; sb.Append((char)raw[i]); }
        return sb.ToString();
      }
    }
    return "";
  }
  public static ulong ModBase(IntPtr h){ IntPtr[] m=new IntPtr[4]; int n; EnumProcessModulesEx(h,m,IntPtr.Size*4,out n,3); return (ulong)m[0].ToInt64(); }
  public static void DumpZone(IntPtr h,ulong eh,string tag){
    if(!Ok(eh)){ Console.WriteLine("  ["+tag+"] eh=null"); return; }
    float f10=Rf(h,eh+0x10); float f14=Rf(h,eh+0x14); float f18=Rf(h,eh+0x18);
    Console.WriteLine(string.Format("  [{0}] eh=0x{1:X} +0x10={2:F3} +0x14={3:F3} +0x18={4:F3}", tag, eh, f10, f14, f18));
    foreach(ulong toff in new ulong[]{0x28UL,0x20UL,0x18UL,0x30UL}){
      ulong arr=Ru(h,eh+toff);
      int c34=Ri(h,eh+0x34); int c30=Ri(h,eh+0x30); int c28=Ri(h,eh+0x28); int c1C=Ri(h,eh+0x1C);
      Console.WriteLine(string.Format("    table@+0x{0:X}=0x{1:X} counts 34={2} 30={3} 28={4} 1C={5}", toff, arr, c34, c30, c28, c1C));
      if(!Ok(arr)) continue;
      int cnt=c34; if(cnt<1||cnt>64) cnt=c30; if(cnt<1||cnt>64) cnt=c28; if(cnt<1||cnt>64) continue;
      for(int i=0;i<Math.Min(cnt,16);i++){
        ulong slot=arr+(ulong)(i*0x10);
        ulong np=Ru(h,slot);
        string sn=Eng(h,np);
        if(string.IsNullOrEmpty(sn)) sn=Eng(h,Ru(h,np)); // maybe ptr to engstring obj
        float fv=Rf(h,slot+8);
        ulong zp=Ru(h,slot);
        float z0=Ok(zp)?Rf(h,zp):0, z4=Ok(zp)?Rf(h,zp+4):0;
        Console.WriteLine(string.Format("      [{0}] name='{1}' f@+8={2:F3} zoneCur={3:F3} zoneMax={4:F3} np=0x{5:X}", i, sn, fv, z0, z4, np));
      }
    }
  }
}
"@

$procs = @(Get-Process DayZ_x64 -ErrorAction SilentlyContinue)
if ($procs.Count -eq 0) { Write-Host "no DayZ"; exit 1 }
Write-Host ("DayZ processes: {0}" -f ($procs.Id -join ","))

foreach ($proc in $procs) {
  Write-Host ("`n######## PID {0} ########" -f $proc.Id)
  $h = [OV5]::OpenProcess(0x0410, $false, $proc.Id)
  $base = [OV5]::ModBase($h)
  $world = [OV5]::Ru($h, $base + 0x4264058)
  $localStub = [OV5]::Ru($h, $world + 0x2960)
  Write-Host ("base=0x{0:X} world=0x{1:X} localStub=0x{2:X}" -f $base, $world, $localStub)

  $players = New-Object System.Collections.Generic.List[UInt64]
  if ([OV5]::Ok($localStub)) { [void]$players.Add($localStub) }
  foreach ($loff in @([UInt64]0xF48, [UInt64]0x1090, [UInt64]0x1F50, [UInt64]0x1F58)) {
    $data = [OV5]::Ru($h, $world + $loff)
    $cnt = [OV5]::Ri($h, $world + $loff + 8)
    Write-Host ("  list+0x{0:X} data=0x{1:X} cnt={2}" -f $loff, $data, $cnt)
    if (-not [OV5]::Ok($data) -or $cnt -lt 1 -or $cnt -gt 4096) { continue }
    for ($i = 0; $i -lt [Math]::Min($cnt, 300); $i++) {
      $ent = [OV5]::Ru($h, $data + [UInt64]($i * 8))
      if (-not [OV5]::Ok($ent)) { continue }
      $typ = [OV5]::Ru($h, $ent + 0x180)
      if (-not [OV5]::Ok($typ)) { continue }
      $cfg = [OV5]::Eng($h, [OV5]::Ru($h, $typ + 0xD0))
      if ($cfg -ne "dayzplayer" -and $cfg -ne "SurvivorM_Mirek" -and $cfg -notmatch "Survivor") { continue }
      if (-not $players.Contains($ent)) { [void]$players.Add($ent) }
      Write-Host ("    player ent=0x{0:X} cfg={1}" -f $ent, $cfg)
    }
  }
  Write-Host ("players total={0}" -f $players.Count)

  foreach ($ent in $players) {
    $isLocal = ($ent -eq $localStub)
    $typ = [OV5]::Ru($h, $ent + 0x180)
    $cfg = ""
    if ([OV5]::Ok($typ)) { $cfg = [OV5]::Eng($h, [OV5]::Ru($h, $typ + 0xD0)) }
    Write-Host ("`n==== ent=0x{0:X} local={1} cfg={2} typ=0x{3:X} ====" -f $ent, [int]$isLocal, $cfg, $typ)
    Write-Host ("  rtti={0}" -f [OV5]::Rtti($h, $ent, $base))

    foreach ($off in @(0x100,0x108,0x110,0x118,0x120,0x128,0x130,0x138,0x140,0x148,0x150,
                       0x6E0,0x6E8,0x6F0,0x6F8,0x700,0x708,0x710,0x718,0x720,0x728,0x730)) {
      $p = [OV5]::Ru($h, $ent + [UInt64]$off)
      $rt = if ([OV5]::Ok($p)) { [OV5]::Rtti($h, $p, $base) } else { "" }
      if ([OV5]::Ok($p) -or $off -in @(0x108,0x700,0x6F0,0x6F8)) {
        Write-Host ("  +0x{0:X} = 0x{1:X} rtti='{2}'" -f $off, $p, $rt)
      }
    }

    # Classic EntityHealth @ +0x108
    [OV5]::DumpZone($h, [OV5]::Ru($h, $ent + 0x108), "EH@108")
    # Current DamageManager @ +0x700
    [OV5]::DumpZone($h, [OV5]::Ru($h, $ent + 0x700), "DM@700")
    # Stats @ +0x6F0
    $st = [OV5]::Ru($h, $ent + 0x6F0)
    if ([OV5]::Ok($st)) {
      Write-Host ("  Stats@6F0=0x{0:X} rtti={1}" -f $st, [OV5]::Rtti($h, $st, $base))
      for ($io = 0; $io -le 0x80; $io += 8) {
        $np = [OV5]::Ru($h, $st + [UInt64]$io)
        $sn = [OV5]::Eng($h, $np)
        if ($sn) { Write-Host ("    st+0x{0:X} eng='{1}'" -f $io, $sn) }
        if ([OV5]::Ok($np) -and $np -lt $base) {
          $sn2 = [OV5]::Eng($h, [OV5]::Ru($h, $np))
          if ($sn2 -match "Health|Blood|Shock|Energy|Water|Stamina") {
            $fv = [OV5]::Rf($h, $np + 0x2C)
            Write-Host ("    nested st+0x{0:X}->obj eng='{1}' val@2C={2:F3}" -f $io, $sn2, $fv)
          }
        }
      }
    }

    # EntityType DamageSystemData fallback (UC edit2): typ+0x118 NO deref as base
    if ([OV5]::Ok($typ)) {
      foreach ($toff in @(0x100,0x108,0x110,0x118,0x120,0x128,0x130,0x140,0x148,0x150,0x160,0x170,0x180)) {
        $ds = $typ + [UInt64]$toff
        $f4 = [OV5]::Rf($h, $ds + 4)
        $arr = [OV5]::Ru($h, $ds + 0x18)
        $cnt = [OV5]::Ri($h, $ds + 0x24)
        if (($f4 -gt 0 -and $f4 -le 100) -or ([OV5]::Ok($arr) -and $cnt -ge 1 -and $cnt -le 64)) {
          Write-Host ("  typ+0x{0:X} as DSD: hp@+4={1:F3} arr@+18=0x{2:X} cnt@+24={3}" -f $toff, $f4, $arr, $cnt)
        }
        # also try as pointer
        $dsp = [OV5]::Ru($h, $typ + [UInt64]$toff)
        if ([OV5]::Ok($dsp)) {
          $pf = [OV5]::Rf($h, $dsp + 0x10)
          $parr = [OV5]::Ru($h, $dsp + 0x28)
          $pc = [OV5]::Ri($h, $dsp + 0x34)
          if (($pf -gt 0 -and $pf -le 1.05) -or ([OV5]::Ok($parr) -and $pc -ge 1 -and $pc -le 64)) {
            Write-Host ("  *(typ+0x{0:X})=0x{1:X} f10={2:F3} arr28=0x{3:X} c34={4} rtti={5}" -f $toff, $dsp, $pf, $parr, $pc, [OV5]::Rtti($h, $dsp, $base))
            [OV5]::DumpZone($h, $dsp, ("typPtr@{0:X}" -f $toff))
          }
        }
      }
    }

    # Wide pointer scan for zone-holder shape under entity
    Write-Host "  zone-holder scan:"
    $zh = 0
    for ($off = 0x80; $off -le 0x800 -and $zh -lt 12; $off += 8) {
      $obj = [OV5]::Ru($h, $ent + [UInt64]$off)
      if (-not [OV5]::Ok($obj)) { continue }
      if ($obj -ge $base -and $obj -lt ($base + 0x8000000)) { continue }
      $arr = [OV5]::Ru($h, $obj + 0x28)
      $c = [OV5]::Ri($h, $obj + 0x34)
      if (-not [OV5]::Ok($arr) -or $c -lt 1 -or $c -gt 64) {
        $arr = [OV5]::Ru($h, $obj + 0x20); $c = [OV5]::Ri($h, $obj + 0x28)
      }
      if (-not [OV5]::Ok($arr) -or $c -lt 1 -or $c -gt 64) { continue }
      $f10 = [OV5]::Rf($h, $obj + 0x10)
      $names = @()
      for ($i = 0; $i -lt [Math]::Min($c, 8); $i++) {
        $sn = [OV5]::Eng($h, [OV5]::Ru($h, $arr + [UInt64]($i * 0x10)))
        if ($sn) { $names += $sn }
      }
      $joined = $names -join ","
      if ($joined -match "Health|Blood|Shock|Global" -or ($f10 -gt 0 -and $f10 -le 1.05)) {
        Write-Host ("    ent+0x{0:X} obj=0x{1:X} f10={2:F3} c={3} names={4} rtti={5}" -f $off, $obj, $f10, $c, $joined, [OV5]::Rtti($h, $obj, $base))
        $zh++
      }
    }
  }
  [void][OV5]::CloseHandle($h)
}
