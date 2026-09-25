# Disassemble HealthCalc candidates + find code xrefs to GlobalHealth string
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class OV9 {
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
  public static ulong ModBase(IntPtr h){ IntPtr[] m=new IntPtr[4]; int n; EnumProcessModulesEx(h,m,IntPtr.Size*4,out n,3); return (ulong)m[0].ToInt64(); }
  public static byte[] Rb(IntPtr h,ulong a,int n){ byte[] b=new byte[n]; int r; ReadProcessMemory(h,(IntPtr)(long)a,b,n,out r); if(r!=n) Array.Resize(ref b,r); return b; }
}
"@

function HexDump([byte[]]$b, [ulong]$addr) {
  for ($i = 0; $i -lt $b.Length; $i += 16) {
    $slice = $b[$i..([Math]::Min($i+15, $b.Length-1))]
    $hex = ($slice | ForEach-Object { "{0:X2}" -f $_ }) -join " "
    Write-Host ("  {0:X}  {1}" -f ($addr+[uint64]$i), $hex)
  }
}

$proc = Get-Process DayZ_x64 | Select-Object -First 1
$h = [OV9]::OpenProcess(0x0410, $false, $proc.Id)
$base = [OV9]::ModBase($h)
Write-Host ("base=0x{0:X}" -f $base)

$rvas = @(
  @{N="HealthCalc_1"; R=0x962C0},
  @{N="HealthCalc_6"; R=0x4197D0},
  @{N="HealthCalc_8"; R=0x780750},
  @{N="HealthCalc_10"; R=0x7A08D0},
  @{N="Blood_Func"; R=0x4AA100},
  @{N="DayZPlayer_GetName"; R=0x4E5130}
)
foreach ($x in $rvas) {
  $addr = $base + [UInt64]$x.R
  $b = [OV9]::Rb($h, $addr, 64)
  Write-Host ("`n==== {0} @ 0x{1:X} (RVA 0x{2:X}) ====" -f $x.N, $addr, $x.R)
  HexDump $b $addr
}

# Find LEA/MOV references to GlobalHealth string in .text via scanning for imm32/imm64
$ghAddrs = @(0x7FF6D9E17A30, 0x7FF6D9E17EDE, 0x7FF6D9E18051)
# Recompute relative to current base — strings may move; find again
$modSize = 0x6000000
$needle = [Text.Encoding]::ASCII.GetBytes("GlobalHealth")
$ghFound = New-Object System.Collections.Generic.List[UInt64]
$chunk = New-Object byte[] 0x100000
for ($off = [UInt64]0; $off -lt [UInt64]$modSize; $off += 0x100000) {
  $r = 0
  if (-not [OV9]::ReadProcessMemory($h, [IntPtr]($base + $off), $chunk, $chunk.Length, [ref]$r) -or $r -lt 12) { continue }
  for ($i = 0; $i -le $r - 12; $i++) {
    $ok = $true
    for ($j = 0; $j -lt 12; $j++) { if ($chunk[$i+$j] -ne $needle[$j]) { $ok = $false; break } }
    if ($ok -and ($i+12 -ge $r -or $chunk[$i+12] -eq 0)) { [void]$ghFound.Add($base + $off + [UInt64]$i) }
  }
}
Write-Host ("`nGlobalHealth strings: {0}" -f $ghFound.Count)
$ghFound | ForEach-Object { Write-Host ("  0x{0:X}" -f $_) }

# Scan executable sections for RIP-relative LEA pointing at GH: 48 8D 0D xx xx xx xx / 48 8D 15 / 4C 8D 05 etc
Write-Host "`nScanning .text for RIP-rel refs to GlobalHealth..."
$textStart = $base + 0x1000
$textEnd = $base + 0xA00000  # rough
$refs = New-Object System.Collections.Generic.List[string]
$scanBuf = New-Object byte[] 0x200000
for ($off = [UInt64]0; $off + 7 -lt ($textEnd - $textStart) -and $refs.Count -lt 40; $off += 0x200000) {
  $n = [int][Math]::Min([UInt64]0x200000, ($textEnd - $textStart - $off))
  $r = 0
  if (-not [OV9]::ReadProcessMemory($h, [IntPtr]($textStart + $off), $scanBuf, $n, [ref]$r) -or $r -lt 7) { continue }
  for ($i = 0; $i -le $r - 7; $i++) {
    # LEA r64, [rip+disp32] : 48/4C 8D 05/0D/15/1D/25/2D/35/3D
    $b0 = $scanBuf[$i]; $b1 = $scanBuf[$i+1]; $b2 = $scanBuf[$i+2]
    $isRex = ($b0 -eq 0x48 -or $b0 -eq 0x4C)
    if (-not $isRex -or $b1 -ne 0x8D) { continue }
    $modrm = $b2
    if (($modrm -band 0xC7) -ne 0x05) { continue } # mod=00 reg=? rm=101 rip-rel
    $disp = [BitConverter]::ToInt32($scanBuf, $i + 3)
    $instr = $textStart + $off + [UInt64]$i
    $target = [UInt64]([Int64]$instr + 7 + $disp)
    foreach ($g in $ghFound) {
      if ($target -eq $g) {
        $rva = $instr - $base
        $refs.Add(("LEA @0x{0:X} RVA=0x{1:X} -> GH 0x{2:X}" -f $instr, $rva, $g))
        break
      }
    }
  }
}
Write-Host ("refs={0}" -f $refs.Count)
$refs | ForEach-Object { Write-Host $_ }

# For each ref, dump surrounding 32 bytes before (to see function start / call pattern)
foreach ($line in ($refs | Select-Object -First 12)) {
  if ($line -match "RVA=0x([0-9A-Fa-f]+)") {
    $rva = [Convert]::ToUInt64($Matches[1], 16)
    $addr = $base + $rva - 32
    $b = [OV9]::Rb($h, $addr, 80)
    Write-Host ("`ncontext around ref RVA 0x{0:X}:" -f $rva)
    HexDump $b $addr
  }
}

[void][OV9]::CloseHandle($h)
