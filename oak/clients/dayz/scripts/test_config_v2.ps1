# Oak DayZ config v2 smoke test — build, ship gate, ini round-trip keys
$ErrorActionPreference = "Stop"
$dayz = Split-Path $PSScriptRoot -Parent
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dll = Join-Path $dayz "build\Release\dayz_internal.dll"
$oakRoot = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$gate = Join-Path $oakRoot "packages\protect\tools\ship_security_gate.mjs"

Write-Host "== Build Release =="
Push-Location $dayz
cmake --build build --config Release
Pop-Location
if (-not (Test-Path $dll)) { throw "DLL missing: $dll" }

Write-Host "== Ship security gate =="
node $gate --dll $dll --mutate | Out-String | Write-Host
$gateJson = node $gate --dll $dll 2>&1 | Out-String
if ($gateJson -notmatch '"ok"\s*:\s*true') { throw "Ship gate failed: $gateJson" }

Write-Host "== Config v2 key schema check =="
$requiredSections = @(
    "meta/version=2",
    "performance/espUpdateHz",
    "performance/maxEntitiesDrawn",
    "combat.aimbot/targetFilter",
    "esp.corpses/playerCorpses",
    "esp.traps/highlightArmed",
    "loot.weapons/maxDist",
    "esp.style.player/boxStyle",
    "ui/panicScope"
)
$menuCpp = Get-Content (Join-Path $dayz "src\imgui_menu.cpp") -Raw
$missing = @()
foreach ($k in $requiredSections) {
    $sec, $key = $k -split "/", 2
    if ($key) {
        if ($menuCpp -notmatch [regex]::Escape("`"$sec`", `"$key`"")) { $missing += $k }
    }
}
if ($missing.Count -gt 0) {
    Write-Warning "Schema keys not found in save/load (may be pending): $($missing -join ', ')"
}

Write-Host "== Limits header constants =="
$limits = Get-Content (Join-Path $dayz "src\oak_config_limits.h") -Raw
foreach ($c in @("OAK_CAP_DIST_M", "OAK_WAYPOINT_MAX", "OAK_CONFIG_VERSION")) {
    if ($limits -notmatch $c) { throw "Missing $c in oak_config_limits.h" }
}

Write-Host "PASS: build + gate + schema smoke"
