# Validate hardcoded native call RVAs against DayZ_x64.exe's own .pdata unwind table.
# A call target MUST equal a RUNTIME_FUNCTION.BeginAddress; anything else means we are
# calling into the middle of a function (skips the prologue -> skewed stack -> engine AV).
param(
    [string]$Exe = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe",
    [string[]]$Rvas = @("0x4E5130")
)

$ErrorActionPreference = "Stop"
$bytes = [IO.File]::ReadAllBytes($Exe)

function Get-U16([int]$o) { [BitConverter]::ToUInt16($bytes, $o) }
function Get-U32([int]$o) { [BitConverter]::ToUInt32($bytes, $o) }

$peOff = Get-U32 0x3C
if ((Get-U32 $peOff) -ne 0x00004550) { throw "not a PE: $Exe" }
$numSections = Get-U16 ($peOff + 6)
$optSize = Get-U16 ($peOff + 20)
$optOff = $peOff + 24
$magic = Get-U16 $optOff
if ($magic -ne 0x20B) { throw "not PE32+ (magic=0x{0:X})" -f $magic }

# IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3
$dirOff = $optOff + 112 + (3 * 8)
$pdataRva = Get-U32 $dirOff
$pdataSize = Get-U32 ($dirOff + 4)
if ($pdataRva -eq 0 -or $pdataSize -eq 0) { throw "no .pdata in image" }

# Section table -> RVA to file offset
$secOff = $optOff + $optSize
$sections = @()
for ($i = 0; $i -lt $numSections; $i++) {
    $s = $secOff + ($i * 40)
    $sections += [pscustomobject]@{
        Name    = ([Text.Encoding]::ASCII.GetString($bytes, $s, 8)).Trim([char]0)
        VSize   = Get-U32 ($s + 8)
        VAddr   = Get-U32 ($s + 12)
        RawSize = Get-U32 ($s + 16)
        RawPtr  = Get-U32 ($s + 20)
    }
}
function RvaToOff([uint32]$rva) {
    foreach ($s in $sections) {
        $span = [Math]::Max($s.VSize, $s.RawSize)
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $span)) {
            return [int]($s.RawPtr + ($rva - $s.VAddr))
        }
    }
    throw ("RVA 0x{0:X} not in any section" -f $rva)
}

$pdataOff = RvaToOff $pdataRva
$count = [int]($pdataSize / 12)
Write-Host ("image: {0}" -f $Exe)
Write-Host ("pdata: rva=0x{0:X} size=0x{1:X} functions={2}" -f $pdataRva, $pdataSize, $count)

# .pdata is sorted by BeginAddress -> binary search for the containing function.
function Find-Function([uint32]$rva) {
    $lo = 0; $hi = $count - 1; $found = $null
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        $e = $pdataOff + ($mid * 12)
        $begin = Get-U32 $e
        $end = Get-U32 ($e + 4)
        if ($rva -lt $begin) { $hi = $mid - 1 }
        elseif ($rva -ge $end) { $lo = $mid + 1 }
        else { $found = [pscustomobject]@{ Begin = $begin; End = $end }; break }
    }
    return $found
}

# powershell -File collapses array args into one string, so re-split defensively.
$Rvas = @($Rvas | ForEach-Object { $_ -split '[\s,;]+' } | Where-Object { $_ })

foreach ($r in $Rvas) {
    $txt = ("$r").Trim()
    $hex = $txt -replace '[^0-9A-Fa-fx]', ''
    if ($hex -like '0x*') { $hex = $hex.Substring(2) }
    $hex = $hex -replace '[^0-9A-Fa-f]', ''
    $rva = [uint32][Convert]::ToUInt32($hex, 16)
    $fn = Find-Function $rva
    if ($null -eq $fn) {
        Write-Host ("RVA 0x{0:X}: NO function covers this address (leaf or not code) -> UNSAFE to call" -f $rva)
        continue
    }
    $isEntry = ($fn.Begin -eq $rva)
    Write-Host ("RVA 0x{0:X}: function 0x{1:X}..0x{2:X}  entry={3}  delta=+0x{4:X}{5}" -f `
            $rva, $fn.Begin, $fn.End, $isEntry, ($rva - $fn.Begin), `
        $(if ($isEntry) { "" } else { "   <-- MID-FUNCTION CALL, real entry is 0x{0:X}" -f $fn.Begin }))
}
