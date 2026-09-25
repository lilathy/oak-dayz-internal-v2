param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$OakRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutDir) { $OutDir = Join-Path $OakRoot "bin" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ tools not found" }

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$src = Join-Path $OakRoot "src\offset_dumper.cpp"
$out = Join-Path $OutDir "OakOffsetDumper.exe"
$tmpBat = Join-Path $env:TEMP "oak_build_dumper.bat"

@"
@echo off
call "$vcvars" || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /Fe:"$out" "$src" /link /nologo psapi.lib
exit /b %ERRORLEVEL%
"@ | Set-Content -Path $tmpBat -Encoding ASCII

Write-Host "Building OakOffsetDumper -> $out"
& cmd /c $tmpBat
if ($LASTEXITCODE -ne 0) { throw "cl failed: $LASTEXITCODE" }

# Clean obj next to src if cl dropped it there
Remove-Item (Join-Path $OakRoot "src\offset_dumper.obj") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $OutDir "offset_dumper.obj") -ErrorAction SilentlyContinue
Get-ChildItem $OutDir -Filter "offset_dumper.obj" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem (Get-Location) -Filter "offset_dumper.obj" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "OK: $out"
Get-Item $out | Select-Object FullName, Length, LastWriteTime
