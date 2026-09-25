# Find heap slots that point at GlobalHealth / Health module strings; BFS from local player.
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class OV8 {
  [DllImport("kernel32")] public static extern IntPtr OpenProcess(int a,bool b,int c);
  [DllImport("kernel32")] public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] b,int n,out int r);
  [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32")] public static extern bool VirtualQueryEx(IntPtr h,IntPtr a,out MEMORY_BASIC_INFORMATION m,uint l);
  [DllImport("psapi")] public static extern bool EnumProcessModulesEx(IntPtr h,IntPtr[] m,int cb,out int n,int f);
  [StructLayout(LayoutKind.Sequential)] public struct MEMORY_BASIC_INFORMATION {
    public IntPtr BaseAddress; public IntPtr AllocationBase; public uint AllocationProtect;
    public UIntPtr RegionSize; public uint State; public uint Protect; public uint Type;
  }
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
  public static List<ulong> FindAsciiInRange(IntPtr h,ulong start,ulong end,string s){
    var hits=new List<ulong>();
    byte[] needle=Encoding.ASCII.GetBytes(s);
    byte[] buf=new byte[(int)Math.Min(end-start, 0x800000)];
    // read in 1MB chunks
    for(ulong off=0; off+ (ulong)needle.Length < (end-start); ){
      int n=(int)Math.Min(0x100000UL, end-start-off);
      int r; if(!ReadProcessMemory(h,(IntPtr)(long)(start+off),buf,n,out r)||r<=0){ off+=(ulong)Math.Max(n,1); continue; }
      for(int i=0;i<=r-needle.Length;i++){
        bool ok=true; for(int j=0;j<needle.Length;j++) if(buf[i+j]!=needle[j]){ok=false;break;}
        if(!ok) continue;
        // null terminated-ish
        if(i+needle.Length < r && buf[i+needle.Length]!=0 && buf[i+needle.Length]>=33) continue;
        hits.Add(start+off+(ulong)i);
      }
      off += (ulong)Math.Max(r - needle.Length, 1);
      if(hits.Count>50) break;
    }
    return hits;
  }
}
"@

$proc = Get-Process DayZ_x64 | Select-Object -First 1
$h = [OV8]::OpenProcess(0x0410, $false, $proc.Id)
$base = [OV8]::ModBase($h)
Write-Host ("PID={0} base=0x{1:X}" -f $proc.Id, $base)

$gh = [OV8]::FindAsciiInRange($h, $base, $base + 0x6000000, "GlobalHealth")
$blood = [OV8]::FindAsciiInRange($h, $base, $base + 0x6000000, "Blood")
$shock = [OV8]::FindAsciiInRange($h, $base, $base + 0x6000000, "Shock")
$health = [OV8]::FindAsciiInRange($h, $base, $base + 0x6000000, "Health")
Write-Host ("strings GH={0} Blood={1} Shock={2} Health={3}" -f $gh.Count,$blood.Count,$shock.Count,$health.Count)
$gh | ForEach-Object { Write-Host ("  GH 0x{0:X}" -f $_) }

$targets = New-Object 'System.Collections.Generic.HashSet[UInt64]'
foreach ($a in $gh) { [void]$targets.Add([UInt64]$a) }
foreach ($a in $blood) { [void]$targets.Add([UInt64]$a) }
foreach ($a in $shock) { [void]$targets.Add([UInt64]$a) }
# Health is noisy — only keep ones near GlobalHealth pages
foreach ($a in $health) {
  foreach ($g in $gh) {
    if ([Math]::Abs([Int64]($a - $g)) -lt 0x2000) { [void]$targets.Add([UInt64]$a); break }
  }
}
Write-Host ("target name addrs={0}" -f $targets.Count)

$world = [OV8]::Ru($h, $base + 0x4264058)
$nearData = [OV8]::Ru($h, $world + 0xF48)
$ent = [OV8]::Ru($h, $nearData)
Write-Host ("localDayZPlayer?=0x{0:X} rtti={1}" -f $ent, ([OV8]::Rtti($h, $ent, $base)))

# BFS depth 4 from player through heap pointers
$q = New-Object System.Collections.Queue
$seen = New-Object 'System.Collections.Generic.HashSet[UInt64]'
[void]$q.Enqueue((New-Object PSObject -Property @{A=[UInt64]$ent; D=0; Path="P"}))
[void]$seen.Add([UInt64]$ent)
$found = 0
$maxSeen = 2500
while ($q.Count -gt 0 -and $found -lt 40 -and $seen.Count -lt $maxSeen) {
  $node = $q.Dequeue()
  $obj = [UInt64]$node.A
  $depth = [int]$node.D
  if ($depth -gt 4) { continue }
  $limit = if ($depth -eq 0) { 0xA00 } else { 0x120 }
  for ($off = 0; $off -le $limit; $off += 8) {
    $p = [OV8]::Ru($h, $obj + [UInt64]$off)
    if ($targets.Contains($p)) {
      $f = [OV8]::Rf($h, $obj + [UInt64]$off + 8)
      $rt = [OV8]::Rtti($h, $obj, $base)
      Write-Host ("HIT path={0}+0x{1:X} str=0x{2:X} f+8={3:F3} containerRtti={4}" -f $node.Path, $off, $p, $f, $rt)
      for ($d = -24; $d -le 40; $d += 4) {
        $ao = $off + $d
        if ($ao -lt 0) { continue }
        $fv = [OV8]::Rf($h, $obj + [UInt64]$ao)
        $pv = [OV8]::Ru($h, $obj + [UInt64]$ao)
        $mark = ""
        if ($targets.Contains($pv)) { $mark = " <STR>" }
        if ($fv -eq $fv -and $fv -ge 0 -and $fv -le 10000 -and ($fv -ne 0 -or $mark)) {
          Write-Host ("  +0x{0:X}(d{1}) f={2:F3} p=0x{3:X}{4}" -f $ao, $d, $fv, $pv, $mark)
        }
      }
      # Also print how to reach from player: find parent offset chain
      Write-Host ("  fullPath={0}+0x{1:X}" -f $node.Path, $off)
      $found++
    }
    if (-not [OV8]::Ok($p)) { continue }
    if ($p -ge $base -and $p -lt ($base + 0x8000000)) { continue }
    if ($depth -lt 4 -and $seen.Add($p) -and $seen.Count -lt $maxSeen) {
      [void]$q.Enqueue((New-Object PSObject -Property @{A=$p; D=($depth+1); Path=("{0}+0x{1:X}" -f $node.Path, $off)}))
    }
  }
}
Write-Host ("bfsFound={0} seen={1}" -f $found, $seen.Count)

# Also scan ALL qword slots on player for objects whose +0x10 is 0..1 and +0x28 looks like array
Write-Host "`nplayer slot scan for EH-shaped objects:"
for ($off = 0x40; $off -le 0x900; $off += 8) {
  $obj = [OV8]::Ru($h, $ent + [UInt64]$off)
  if (-not [OV8]::Ok($obj)) { continue }
  if ($obj -ge $base -and $obj -lt ($base + 0x8000000)) { continue }
  $f10 = [OV8]::Rf($h, $obj + 0x10)
  $arr = [OV8]::Ru($h, $obj + 0x28)
  $c = [OV8]::Ri($h, $obj + 0x34)
  $looks = ($f10 -gt 0.01 -and $f10 -le 1.05) -or ([OV8]::Ok($arr) -and $c -ge 1 -and $c -le 32)
  if (-not $looks) { continue }
  # check if any slot points to our target strings
  $names = @()
  if ([OV8]::Ok($arr) -and $c -ge 1 -and $c -le 32) {
    for ($i = 0; $i -lt $c; $i++) {
      $np = [OV8]::Ru($h, $arr + [UInt64]($i * 0x10))
      if ($targets.Contains($np)) {
        $fv = [OV8]::Rf($h, $arr + [UInt64]($i * 0x10) + 8)
        $names += ("strHit f={0:F2}" -f $fv)
      }
    }
  }
  if ($names.Count -gt 0 -or ($f10 -gt 0.01 -and $f10 -le 1.05 -and $c -ge 1 -and $c -le 8)) {
    Write-Host ("  +0x{0:X} obj=0x{1:X} f10={2:F3} c={3} rtti={4} {5}" -f $off, $obj, $f10, $c, ([OV8]::Rtti($h, $obj, $base)), ($names -join ","))
  }
}

[void][OV8]::CloseHandle($h)
