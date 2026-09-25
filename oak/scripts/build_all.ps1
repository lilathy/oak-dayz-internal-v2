


param(
    [string]$Configuration = "Release",
    [switch]$Clean = $false
)

$ErrorActionPreference = "Stop"
if ($PSScriptRoot) {

    $ProjectRoot = (Split-Path $PSScriptRoot -Parent)
} else {
    $ProjectRoot = Get-Location
}
$BinDir = Join-Path $ProjectRoot "bin"

Write-Host "oak build" -ForegroundColor Cyan
Write-Host ""
Write-Host "checking stuff..." -ForegroundColor Yellow


$msbuildCmd = Get-Command msbuild -ErrorAction SilentlyContinue
$msbuild = $null
if ($msbuildCmd) { $msbuild = $msbuildCmd.Source }

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    $candidates = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { $msbuild = $p; break }
    }
}

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    Write-Host "msbuild not found, need vs2022" -ForegroundColor Red
    exit 1
}
Write-Host "  msbuild ok ($msbuild)" -ForegroundColor Green


$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {

    $cmakePaths = @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe",
        "${env:LOCALAPPDATA}\Microsoft\WindowsApps\cmake.exe"
    )
    
    foreach ($path in $cmakePaths) {
        if (Test-Path $path) {
            $cmake = $path
            break
        }
    }
}
if ($cmake -and $cmake -isnot [string]) {
    $cmake = $cmake.Source
}
if (-not $cmake -or -not (Test-Path $cmake)) {
    Write-Host "cmake not found" -ForegroundColor Red
    Write-Host "  get it from cmake.org" -ForegroundColor Yellow
    exit 1
}
Write-Host "  cmake ok" -ForegroundColor Green


$dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnet) {
    Write-Host "dotnet not found, need .net 8" -ForegroundColor Red
    exit 1
}
Write-Host "  dotnet ok" -ForegroundColor Green

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "node not found — required for always-on DLL watermark mutate" -ForegroundColor Red
    exit 1
}
Write-Host "  node ok" -ForegroundColor Green

$wdkPath = "${env:WindowsKitsRoot}DDK"
if (-not (Test-Path $wdkPath)) {
    Write-Host "wdk not found, driver might not build" -ForegroundColor Yellow
} else {
    Write-Host "  wdk ok" -ForegroundColor Green
}

Write-Host ""


if ($Clean) {
    Write-Host "cleaning..." -ForegroundColor Yellow
    if (Test-Path $BinDir) {
        Remove-Item -Path $BinDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path (Join-Path $ProjectRoot "build")) {
        Remove-Item -Path (Join-Path $ProjectRoot "build") -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path (Join-Path $ProjectRoot "injector\build")) {
        Remove-Item -Path (Join-Path $ProjectRoot "injector\build") -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "  cleaned" -ForegroundColor Green
    Write-Host ""
}


if (-not (Test-Path $BinDir)) {
    New-Item -ItemType Directory -Path $BinDir | Out-Null
}


Write-Host "building driver..." -ForegroundColor Yellow
$dayzRoot = Join-Path $ProjectRoot "clients\dayz"
$driverProj = Join-Path $dayzRoot "driver\oak.vcxproj"
$driverSys = Join-Path $dayzRoot "driver\bin\oak.sys"
$driverOk = $false
if (Test-Path $driverProj) {
    try {
        & $msbuild $driverProj `
            /p:Configuration=$Configuration `
            /p:Platform=x64 `
            /m `
            /nologo `
            /v:minimal
        
        if ($LASTEXITCODE -eq 0 -and (Test-Path $driverSys)) {
            $driverOk = $true
            Write-Host "  driver ok (msbuild)" -ForegroundColor Green
        }
    } catch {
        Write-Host "  msbuild driver toolset unavailable: $_" -ForegroundColor Yellow
    }
}
if (-not $driverOk) {
    Write-Host "  falling back to WDK cl/link build..." -ForegroundColor Yellow
    & (Join-Path $dayzRoot "scripts\build_driver_wdk.ps1")
    if ($LASTEXITCODE -eq 0 -and (Test-Path $driverSys)) {
        $driverOk = $true
    }
}
if ($driverOk) {
    Copy-Item $driverSys $BinDir -Force
    Write-Host "  driver copied" -ForegroundColor Green
} else {
    Write-Host "  driver failed - install WDK" -ForegroundColor Red
    exit 1
}
Write-Host ""


Write-Host "building injector..." -ForegroundColor Yellow
$injectorBuildDir = Join-Path $dayzRoot "injector\build"
if (-not (Test-Path $injectorBuildDir)) {
    New-Item -ItemType Directory -Path $injectorBuildDir | Out-Null
}

Push-Location $injectorBuildDir
try {

    Write-Host "  cmake config..." -ForegroundColor Gray
    & $cmake .. -DCMAKE_BUILD_TYPE=$Configuration
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  cmake failed" -ForegroundColor Red
        Write-Host "  trying vs generator..." -ForegroundColor Yellow

        & $cmake .. -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  cmake failed" -ForegroundColor Red
            exit 1
        }
    }
    

    Write-Host "  building..." -ForegroundColor Gray
    & $cmake --build . --config $Configuration
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  injector ok" -ForegroundColor Green
        

        $possiblePaths = @(
            "$Configuration\oak_loader.exe",
            "Release\oak_loader.exe",
            "Debug\oak_loader.exe",
            "oak_loader.exe"
        )
        
        foreach ($relPath in $possiblePaths) {
            $oakLoaderExe = Join-Path $injectorBuildDir $relPath
            if (Test-Path $oakLoaderExe) {
                Copy-Item $oakLoaderExe $BinDir -Force
                Write-Host "  oak_loader copied" -ForegroundColor Green
                break
            }
        }
        
        foreach ($relPath in $possiblePaths) {
            $injectorExe = Join-Path $injectorBuildDir ($relPath -replace "oak_loader", "injector")
            if (Test-Path $injectorExe) {
                Copy-Item $injectorExe $BinDir -Force
                Write-Host "  injector copied" -ForegroundColor Green
                break
            }
        }
    } else {
        Write-Host "  injector failed" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}
Write-Host ""


Write-Host "building dll..." -ForegroundColor Yellow
$buildDir = Join-Path $dayzRoot "build"
if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

Push-Location $buildDir
try {

    Write-Host "  cmake config..." -ForegroundColor Gray
    & $cmake $dayzRoot -DCMAKE_BUILD_TYPE=$Configuration
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  trying vs generator..." -ForegroundColor Yellow

        & $cmake $dayzRoot -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  cmake failed" -ForegroundColor Red
            exit 1
        }
    }
    

    Write-Host "  building..." -ForegroundColor Gray
    & $cmake --build . --config $Configuration
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  dll ok" -ForegroundColor Green
        

        $possiblePaths = @(
            "$Configuration\dayz_internal.dll",
            "Release\dayz_internal.dll",
            "Debug\dayz_internal.dll",
            "dayz_internal.dll"
        )
        
        foreach ($relPath in $possiblePaths) {
            $dllPath = Join-Path $buildDir $relPath
            if (Test-Path $dllPath) {
                # Belt-and-suspenders: CMake POST_BUILD already mutates; re-gate before copy.
                & (Join-Path $PSScriptRoot "obfuscate_client.ps1") -Dll $dllPath
                if ($LASTEXITCODE -ne 0) {
                    Write-Host "  obfuscate/ship-gate failed" -ForegroundColor Red
                    exit 1
                }
                Copy-Item $dllPath $BinDir -Force
                Write-Host "  dll copied (mutated)" -ForegroundColor Green
                break
            }
        }
    } else {
        Write-Host "  dll failed" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}
Write-Host ""

# OakPanel (external ClickGUI) is retired — in-game ImGui only.

Write-Host "done" -ForegroundColor Green
Write-Host "output: $BinDir" -ForegroundColor Cyan
Get-ChildItem $BinDir -File | ForEach-Object {
    Write-Host "  $($_.Name)" -ForegroundColor White
}
Write-Host ""
