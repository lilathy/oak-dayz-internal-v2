# Scan DayZ process for GlobalHealth/Blood/Shock string refs near the player object graph
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class OV7 {
  [DllImport("kernel32")] public static extern IntPtr OpenProcess(int a,bool b,int c);
  [DllImport("kernel32")] public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] b,int n,out int r);
  [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32")] public static extern bool VirtualQueryEx(IntPtr h,IntPtr a,out MEMORY_BASIC_INFORMATION m,uint l);
  [DllImport("psapi")] public static extern bool EnumProcessModulesEx(IntPtr h,IntPtr[] m,int cb,out int n,int f);
  [DllImport("psapi")] public static extern uint GetModuleFileNameEx(IntPtr h,IntPtr m,StringBuilder b,int s);
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
  public static ulong ModBase(IntPtr h){ IntPtr[] m=new IntPtr[4]; int n; EnumProcessModulesEx(h,m,IntPtr.Size*4,out n,3); return (ulong)m[0].ToInt64(); }
  public static List<ulong> FindAscii(IntPtr h,ulong start,ulong end,string s){
    var hits=new List<ulong>();
    byte[] needle=Encoding.ASCII.GetBytes(s);
    ulong cur=start;
    MEMORY_BASIC_INFORMATION mbi;
    while(cur<end){
      if(!VirtualQueryEx(h,(IntPtr)(long)cur,out mbi,(uint)Marshal.SizeOf(typeof(MEMORY_BASIC_INFORMATION)))) break;
      ulong baseA=(ulong)mbi.BaseAddress.ToInt64();
      ulong size=mbi.RegionSize.ToUInt64();
      ulong next=baseA+size; if(next<=cur) break;
      bool readable=(mbi.State==0x1000) && ((mbi.Protect&0xEE)!=0) && ((mbi.Protect&0x100)==0);
      if(readable && size>0 && size<0x4000000){
        // chunked read
        int chunk=(int)Math.Min(size, 0x100000);
        for(ulong off=0; off<size; off+=(ulong)chunk){
          int n=(int)Math.Min((ulong)chunk, size-off);
          byte[] buf=new byte[n]; int r;
          if(!ReadProcessMemory(h,(IntPtr)(long)(baseA+off),buf,n,out r)||r<=0) continue;
          for(int i=0;i<=r-needle.Length;i++){
            bool ok=true;
            for(int j=0;j<needle.Length;j++) if(buf[i+j]!=needle[j]){ok=false;break;}
            if(ok) hits.Add(baseA+off+(ulong)i);
          }
        }
      }
      cur=next;
      if(hits.Count>200) break;
    }
    return hits;
  }
}
"@

$proc = Get-Process DayZ_x64 | Select-Object -First 1
$h = [OV7]::OpenProcess(0x0410, $false, $proc.Id)
$base = [OV7]::ModBase($h)
Write-Host ("PID={0} base=0x{1:X} scanning module for GlobalHealth..." -f $proc.Id, $base)

# Scan only DayZ_x64 image for the strings (fast)
$modHits = [OV7]::FindAscii($h, $base, $base + 0x6000000, "GlobalHealth")
Write-Host ("GlobalHealth in module: {0}" -f $modHits.Count)
foreach ($a in ($modHits | Select-Object -First 8)) { Write-Host ("  0x{0:X}" -f $a) }

$bloodHits = [OV7]::FindAscii($h, $base, $base + 0x6000000, "Blood")
# Filter likely exact tokens near Health context — just count / sample short ones by checking neighbors later
Write-Host ("Blood string count (module, unfiltered): {0}" -f $bloodHits.Count)

# For each GlobalHealth string, find pointers TO that address in nearby heap by scanning player-sized window... 
# Better: scan heap regions for qword == GlobalHealth addr, then check surrounding float layout
$gh = if ($modHits.Count -gt 0) { $modHits[0] } else { 0 }
if ($gh -eq 0) { Write-Host "no GlobalHealth"; [void][OV7]::CloseHandle($h); exit 1 }
Write-Host ("Using GlobalHealth @ 0x{0:X}" -f $gh)

# Also try Shock and Health exact
$shockHits = [OV7]::FindAscii($h, $base, $base + 0x6000000, "Shock")
$healthHits = [OV7]::FindAscii($h, $base, $base + 0x6000000, "Health")
Write-Host ("Shock={0} Health={1}" -f $shockHits.Count, $healthHits.Count)

# Build candidate name addresses (prefer exact null-terminated matches by checking byte after)
function Select-Exact([ulong[]]$addrs, [string]$s, $h) {
  $out = New-Object System.Collections.Generic.List[ulong]
  $b = [byte[]]::new(1)
  foreach ($a in $addrs) {
    $r=0
    [void][OV7]::ReadProcessMemory($h, [IntPtr]($a + $s.Length), $b, 1, [ref]$r)
    if ($b[0] -eq 0) { [void]$out.Add($a) }
  }
  return $out
}
$ghExact = Select-Exact $modHits "GlobalHealth" $h
$bloodExact = Select-Exact ($bloodHits | Select-Object -First 80) "Blood" $h
$shockExact = Select-Exact ($shockHits | Select-Object -First 80) "Shock" $h
$healthExact = Select-Exact ($healthHits | Select-Object -First 120) "Health" $h
Write-Host ("exact GlobalHealth={0} Blood={1} Shock={2} Health={3}" -f $ghExact.Count,$bloodExact.Count,$shockExact.Count,$healthExact.Count)

# Scan committed heap for pointers equal to any GlobalHealth exact string
$want = New-Object 'System.Collections.Generic.HashSet[ulong]'
foreach ($a in $ghExact) { [void]$want.Add([ulong]$a) }
foreach ($a in ($healthExact | Select-Object -First 30)) { [void]$want.Add([ulong]$a) }

$ptrHits = New-Object System.Collections.Generic.List[string]
$cur = [UInt64]0x100000000
$mbi = New-Object OV7+MEMORY_BASIC_INFORMATION
$scanned = 0
while ($cur -lt 0x7FFFFFFFFFFF -and $ptrHits.Count -lt 40 -and $scanned -lt 800) {
  if (-not [OV7]::VirtualQueryEx($h, [IntPtr]$cur, [ref]$mbi, [uint][Runtime.InteropServices.Marshal]::SizeOf([type][OV7+MEMORY_BASIC_INFORMATION]))) { break }
  $baseA = [UInt64]$mbi.BaseAddress.ToInt64()
  $size = [UInt64]$mbi.RegionSize.ToUInt64()
  $next = $baseA + $size
  if ($next -le $cur) { break }
  $readable = ($mbi.State -eq 0x1000) -and (($mbi.Protect -band 0xEE) -ne 0) -and (($mbi.Protect -band 0x100) -eq 0)
  # skip image
  if ($readable -and $size -gt 0 -and $size -lt 0x2000000 -and -not ($baseA -ge $base -and $baseA -lt ($base+0x8000000))) {
    $scanned++
    $chunk = [int][Math]::Min($size, 0x80000)
    for ($off = [UInt64]0; $off -lt $size -and $ptrHits.Count -lt 40; $off += [UInt64]$chunk) {
      $n = [int][Math]::Min([UInt64]$chunk, $size - $off)
      $buf = New-Object byte[] $n
      $r = 0
      if (-not [OV7]::ReadProcessMemory($h, [IntPtr]($baseA + $off), $buf, $n, [ref]$r) -or $r -lt 8) { continue }
      for ($i = 0; $i -le $r - 8; $i += 8) {
        $val = [BitConverter]::ToUInt64($buf, $i)
        if (-not $want.Contains($val)) { continue }
        $addr = $baseA + $off + [UInt64]$i
        $f = 0.0
        if ($i + 12 -le $r) { $f = [BitConverter]::ToSingle($buf, $i + 8) }
        $ptrHits.Add(("ptrHit @0x{0:X} ->str 0x{1:X} f@+8={2:F3}" -f $addr, $val, $f))
      }
    }
  }
  $cur = $next
}
Write-Host ("heap ptrHits={0} regionsScanned={1}" -f $ptrHits.Count, $scanned)
$ptrHits | Select-Object -First 25 | ForEach-Object { Write-Host $_ }

# Also: from known player, walk one level of pointers and search for string ptrs
$world = [OV7]::Ru($h, $base + 0x4264058)
$near = [OV7]::Ru($h, $world + 0xF48)
$ent = [OV7]::Ru($h, $near)
Write-Host ("`nplayer=0x{0:X} BFS for health strings" -f $ent)
$targets = New-Object 'System.Collections.Generic.HashSet[ulong]'
foreach ($a in $ghExact) { [void]$targets.Add([ulong]$a) }
foreach ($a in ($bloodExact | Select-Object -First 20)) { [void]$targets.Add([ulong]$a) }
foreach ($a in ($shockExact | Select-Object -First 20)) { [void]$targets.Add([ulong]$a) }
foreach ($a in ($healthExact | Select-Object -First 40)) { [void]$targets.Add([ulong]$a) }

$q = New-Object System.Collections.Generic.Queue[object]
$seen = New-Object 'System.Collections.Generic.HashSet[ulong]'
[void]$q.Enqueue(@{A=$ent; D=0; Path="ent"})
[void]$seen.Add($ent)
$found = 0
while ($q.Count -gt 0 -and $found -lt 30) {
  $node = $q.Dequeue()
  $obj = [UInt64]$node.A
  $depth = [int]$node.D
  if ($depth -gt 3) { continue }
  for ($off = 0; $off -le 0xA00; $off += 8) {
    $p = [OV7]::Ru($h, $obj + [UInt64]$off)
    if (-not [OV7]::Ok($p)) { continue }
    if ($targets.Contains($p)) {
      $f = [OV7]::Rf($h, $obj + [UInt64]$off + 8)
      Write-Host ("  FOUND {0}+0x{1:X} -> nameStr f@+8={2:F3} path={3}" -f $node.Path, $off, $f, $node.Path)
      # dump neighbors
      for ($d = -16; $d -le 32; $d += 4) {
        $fv = [OV7]::Rf($h, $obj + [UInt64]($off + $d))
        if ($fv -eq $fv -and $fv -ge 0 -and $fv -le 10000) {
          Write-Host ("    neigh d{0}={1:F3}" -f $d, $fv)
        }
      }
      $found++
    }
    if ($depth -lt 3 -and $p -lt $base -and $seen.Add($p)) {
      if ($seen.Count -lt 800) {
        [void]$q.Enqueue(@{A=$p; D=$depth+1; Path=("{0}+0x{1:X}" -f $node.Path, $off)})
      }
    }
  }
}
Write-Host ("bfsFound={0} seen={1}" -f $found, $seen.Count)
[void][OV7]::CloseHandle($h)
