# Always mutate + security-gate a client DLL.
# Usage:
#   .\scripts\obfuscate_client.ps1
#   .\scripts\obfuscate_client.ps1 -Slug dayz
#   .\scripts\obfuscate_client.ps1 -Dll "C:\path\to\dayz_internal.dll"

param(
    [string]$Dll = "",
    [string]$Slug = "dayz"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
if (-not $Dll) {
    $Dll = Join-Path $Root "clients\$Slug\build\Release\${Slug}_internal.dll"
    if ($Slug -eq "eft") {
        $Dll = Join-Path $Root "clients\eft\build\Release\eft_internal.dll"
    }
    if ($Slug -eq "dayz") {
        $Dll = Join-Path $Root "clients\dayz\build\Release\dayz_internal.dll"
    }
}
if (-not (Test-Path $Dll)) {
    throw "DLL not found: $Dll"
}

$gate = Join-Path $Root "packages\protect\tools\ship_security_gate.mjs"
Write-Host "Oak obfuscate: $Dll" -ForegroundColor Cyan
node $gate --dll $Dll --mutate
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
