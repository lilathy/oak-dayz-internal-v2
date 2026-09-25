param([string]$Configuration = "Release")

$ErrorActionPreference = "Stop"
$OakRoot = Split-Path $PSScriptRoot -Parent
$DriverDir = Join-Path $OakRoot "driver"
$SrcDir = Join-Path $DriverDir "src"
$OutDir = Join-Path $DriverDir "bin"
$ObjDir = Join-Path $DriverDir "obj\wdk"
New-Item -ItemType Directory -Force -Path $OutDir, $ObjDir | Out-Null

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
$kitVer = "10.0.26100.0"
$kmInc = Join-Path $kitsRoot "Include\$kitVer\km"
$sharedInc = Join-Path $kitsRoot "Include\$kitVer\shared"
$kmLib = Join-Path $kitsRoot "Lib\$kitVer\km\x64"

if (-not (Test-Path (Join-Path $kmInc "ntddk.h"))) {
    throw "WDK km headers missing at $kmInc"
}
if (-not (Test-Path (Join-Path $kmLib "ntoskrnl.lib"))) {
    throw "WDK km libs missing at $kmLib"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "VS C++ tools not found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found" }

$tmpBat = Join-Path $env:TEMP "oak_build_driver.bat"
$lines = @(
    "@echo off",
    "setlocal",
    "call `"$vcvars`" || exit /b 1",
    "set `"INCLUDE=$kmInc;$sharedInc;%INCLUDE%`"",
    "set `"LIB=$kmLib;%LIB%`"",
    "cd /d `"$ObjDir`"",
    "cl /nologo /c /W3 /O2 /Gy /Gw /GS- /kernel /Zp8 /D_AMD64_ /D_WIN64 /DPOOL_NX_OPTIN=1 /DWIN64=1 /DNTSTRSAFE_LIB /I`"$kmInc`" /I`"$sharedInc`" /I`"$SrcDir`" `"$SrcDir\main.cpp`" `"$SrcDir\memory.cpp`" `"$SrcDir\process.cpp`" `"$SrcDir\mapper.cpp`" `"$SrcDir\ssdt.cpp`"",
    "if errorlevel 1 exit /b 1",
    "link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /NODEFAULTLIB /OUT:`"$OutDir\oak.sys`" /LIBPATH:`"$kmLib`" ntoskrnl.lib BufferOverflowK.lib ntstrsafe.lib main.obj memory.obj process.obj mapper.obj ssdt.obj",
    "if errorlevel 1 exit /b 1",
    "echo Built $OutDir\oak.sys"
)
Set-Content -Path $tmpBat -Value ($lines -join "`r`n") -Encoding ASCII

Write-Host "Building oak.sys via WDK..."
& cmd.exe /c "`"$tmpBat`""
if ($LASTEXITCODE -ne 0) { throw "driver build failed ($LASTEXITCODE)" }

$sys = Join-Path $OutDir "oak.sys"
Copy-Item $sys (Join-Path $OakRoot "bin\oak.sys") -Force
$item = Get-Item $sys
Write-Host ("OK: {0} ({1} bytes)" -f $item.FullName, $item.Length)
