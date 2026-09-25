# Launch DayZ (-noBattlEye), inject dayz_internal.dll, verify init + soak without crash.
# One-command local smoke:  powershell -File smoke_dayz_no_crash.ps1
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$Build,
    [int]$WaitSeconds = 90,
    [int]$SoakSeconds = 60,
    [switch]$KeepDayZRunning,
    [switch]$SkipGate
)

$ErrorActionPreference = "Stop"
$DayzRoot = Split-Path $PSScriptRoot -Parent
$OakRoot = Split-Path (Split-Path $DayzRoot -Parent) -Parent
$Bin = Join-Path $DayzRoot "bin"
$BuildDir = Join-Path $DayzRoot "build"
$DllName = "dayz_internal.dll"
$LoaderName = "OakImGuiOverlayLoader.exe"

function Stage-DayzBin {
    param([string]$Config)
    New-Item -ItemType Directory -Force -Path $Bin | Out-Null
    $dll = Join-Path $BuildDir "$Config\$DllName"
    $loader = Join-Path $BuildDir "$Config\$LoaderName"
    if (-not (Test-Path $dll)) { throw "Missing $dll - run with -Build" }
    if (-not (Test-Path $loader)) { throw "Missing $loader - run with -Build" }
    if (-not $SkipGate -and $Config -eq "Release") {
        $gate = Join-Path $OakRoot "packages\protect\tools\ship_security_gate.mjs"
        Write-Host "ship gate: $dll"
        $gateJson = node $gate --dll $dll 2>&1 | Out-String
        if ($gateJson -notmatch '"ok"\s*:\s*true') { throw "Ship gate failed: $gateJson" }
    }
    Copy-Item $dll (Join-Path $Bin $DllName) -Force
    Copy-Item $loader (Join-Path $Bin $LoaderName) -Force
    Write-Host "staged -> $Bin ($Config)"
}

function Build-DayzClient {
    param([string]$Config)
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
        Push-Location $BuildDir
        try {
            cmake $DayzRoot -G "Visual Studio 17 2022" -A x64
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
        } finally { Pop-Location }
    }
    Push-Location $BuildDir
    try {
        cmake --build . --config $Config --target DayZInternal
        $dll = Join-Path $BuildDir "$Config\$DllName"
        if (-not (Test-Path $dll)) { throw "cmake build failed ($Config) - no $DllName" }
        if ($LASTEXITCODE -ne 0 -and $Config -eq "Debug") {
            Write-Warning "DayZInternal POST_BUILD gate failed (expected for Debug under OneDrive) - using $dll"
        } elseif ($LASTEXITCODE -ne 0) {
            throw "cmake build failed ($Config)"
        }
        cmake --build . --config $Config --target OakImGuiOverlayLoader
        if ($LASTEXITCODE -ne 0) { throw "loader build failed ($Config)" }
    } finally { Pop-Location }
}

$needBuild = $Build
if (-not $needBuild) {
    $binDll = Join-Path $Bin $DllName
    $binLoader = Join-Path $Bin $LoaderName
    if (-not (Test-Path $binDll) -or -not (Test-Path $binLoader)) { $needBuild = $true }
}

if ($needBuild) {
    Write-Host "== Build $Configuration =="
    Build-DayzClient -Config $Configuration
    Stage-DayzBin -Config $Configuration
} else {
    Write-Host "using existing bin/ (pass -Build to rebuild)"
}

if ($Configuration -eq "Release") {
    Write-Host @"
NOTE: Release DLL requires protection/bootstrap (OAK_REQUIRE_PROTECTION).
For no-auth local smoke use -Configuration Debug (default), or set OAK_SMOKE_LOGIN/OAK_SMOKE_PASSWORD
and run apps/launcher/tools/LaunchSmoke instead.
"@
}

$testScript = Join-Path $PSScriptRoot "test_internal_dayz.ps1"
& $testScript -WaitSeconds $WaitSeconds -SoakSeconds $SoakSeconds -KeepDayZRunning:$KeepDayZRunning
$code = $LASTEXITCODE
$scan = Join-Path $PSScriptRoot "scan_crash_log.ps1"
if (Test-Path $scan) {
    & $scan
    if ($LASTEXITCODE -ne 0 -and $code -eq 0) { $code = 2 }
}
exit $code
