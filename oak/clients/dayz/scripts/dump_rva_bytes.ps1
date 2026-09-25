param(
    [string]$Exe = "C:\Program Files (x86)\Steam\steamapps\common\DayZ\DayZ_x64.exe"
)
$ErrorActionPreference = "Stop"
$Rvas = @(
    '0x7A0E80','0x7A0EC0','0x4E5120','0x923F10','0x923F20','0x923FB0',
    '0x912DF3','0x913370','0x98390','0x98450','0x2D7B10','0x2D7E40'
)
$bytes = [IO.File]::ReadAllBytes($Exe)
function Get-U16([int]$o) { [BitConverter]::ToUInt16($bytes, $o) }
function Get-U32([int]$o) { [BitConverter]::ToUInt32($bytes, $o) }
$pe = Get-U32 0x3C
$num = Get-U16 ($pe + 6)
$opt = Get-U16 ($pe + 20)
$optOff = $pe + 24
$sec = $optOff + $opt
$sections = @()
for ($i = 0; $i -lt $num; $i++) {
    $s = $sec + ($i * 40)
    $sections += [pscustomobject]@{
        VAddr = Get-U32 ($s + 12); RawPtr = Get-U32 ($s + 20)
        VSize = Get-U32 ($s + 8); RawSize = Get-U32 ($s + 16)
    }
}
function RvaToOff([uint32]$rva) {
    foreach ($s in $sections) {
        $span = [Math]::Max($s.VSize, $s.RawSize)
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + $span)) {
            return [int]($s.RawPtr + ($rva - $s.VAddr))
        }
    }
    throw ("bad rva 0x{0:X}" -f $rva)
}
foreach ($r in $Rvas) {
    $hex = ("$r").Trim() -replace '^0x', ''
    $hex = $hex -replace '[^0-9A-Fa-f]', ''
    $rva = [uint32][Convert]::ToUInt32($hex, 16)
    $o = RvaToOff $rva
    $parts = @()
    for ($i = 0; $i -lt 48; $i++) { $parts += ("{0:X2}" -f $bytes[$o + $i]) }
    Write-Host (("0x{0:X}: {1}" -f $rva, ($parts -join ' ')))
}
