[CmdletBinding()]
param(
    [string]$AccountName = "",
    [switch]$ListOnly,
    [int]$SteamLoginTimeoutSeconds = 240
)

$ErrorActionPreference = "Stop"

function Get-SteamExe {
    $candidates = @()
    try {
        $key = Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop
        if ($key.SteamExe) { $candidates += [string]$key.SteamExe }
        if ($key.SteamPath) { $candidates += (Join-Path ([string]$key.SteamPath) "steam.exe") }
    } catch {}
    $candidates += @(
        "${env:ProgramFiles(x86)}\Steam\steam.exe",
        "$env:ProgramFiles\Steam\steam.exe"
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Steam was not found. Install Steam or repair HKCU\Software\Valve\Steam."
}

function Get-SteamAccounts([string]$SteamExe) {
    $loginUsers = Join-Path (Split-Path $SteamExe -Parent) "config\loginusers.vdf"
    if (-not (Test-Path -LiteralPath $loginUsers)) { return @() }

    $raw = [IO.File]::ReadAllText($loginUsers)
    $matches = [regex]::Matches(
        $raw,
        '(?ms)^\s*"(?<steamId>\d{17})"\s*\{\s*(?<body>.*?)(?=^\s*"\d{17}"\s*\{|^\s*\}\s*$)'
    )
    $accounts = foreach ($match in $matches) {
        $body = $match.Groups["body"].Value
        $accountMatch = [regex]::Match($body, '"AccountName"\s*"(?<value>[^"]*)"')
        if (-not $accountMatch.Success -or [string]::IsNullOrWhiteSpace($accountMatch.Groups["value"].Value)) {
            continue
        }
        $personaMatch = [regex]::Match($body, '"PersonaName"\s*"(?<value>[^"]*)"')
        $recentMatch = [regex]::Match($body, '"MostRecent"\s*"(?<value>[01])"')
        [pscustomobject]@{
            SteamId = [uint64]$match.Groups["steamId"].Value
            AccountName = $accountMatch.Groups["value"].Value
            PersonaName = if ($personaMatch.Success) { $personaMatch.Groups["value"].Value } else { "" }
            MostRecent = $recentMatch.Success -and $recentMatch.Groups["value"].Value -eq "1"
        }
    }
    return @($accounts | Sort-Object @{ Expression = "MostRecent"; Descending = $true }, PersonaName)
}

function Get-ActiveSteamAccountId {
    try {
        $value = (Get-ItemProperty "HKCU:\Software\Valve\Steam\ActiveProcess" -ErrorAction Stop).ActiveUser
        if ($null -ne $value) { return [uint32]$value }
    } catch {}
    return [uint32]0
}

function Get-SteamAccountId([uint64]$SteamId) {
    return [uint32]($SteamId -band [uint64]4294967295)
}

function Stop-Steam([string]$SteamExe) {
    if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { return }
    Write-Host "Closing Steam so it can switch accounts..."
    Start-Process -FilePath $SteamExe -ArgumentList "-shutdown" | Out-Null
    $deadline = (Get-Date).AddSeconds(45)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "Steam did not close. Exit Steam from its tray icon, then run this file again."
}

function Wait-SteamAccount([uint32]$ExpectedAccountId, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $steam = Get-Process steam -ErrorAction SilentlyContinue
        $helper = Get-Process steamwebhelper -ErrorAction SilentlyContinue
        if ($steam -and $helper -and (Get-ActiveSteamAccountId) -eq $ExpectedAccountId) {
            Start-Sleep -Seconds 2
            return
        }
        Start-Sleep -Seconds 1
    }
    throw "Steam did not finish signing into the selected account. Complete the Steam password/Guard prompt and retry."
}

function Resolve-OakLauncher {
    $oakRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
    $candidates = @(
        (Join-Path $oakRoot "apps\launcher\bin\Release\net8.0-windows\win-x64\publish\OakLauncher.exe"),
        (Join-Path $oakRoot "apps\launcher\bin\Release\net8.0-windows\OakLauncher.exe"),
        (Join-Path $PSScriptRoot "OakLauncher.exe"),
        (Join-Path $PSScriptRoot "..\OakLauncher.exe"),
        "C:\oak\dayz\OakLauncher.exe",
        "C:\oak\OakLauncher.exe",
        (Join-Path $oakRoot "apps\launcher\bin\Debug\net8.0-windows\OakLauncher.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw "OakLauncher.exe was not found. Build apps\launcher or place OakLauncher.exe in C:\oak\dayz."
}

if (Get-Process DayZ_x64 -ErrorAction SilentlyContinue) {
    throw "DayZ is already running. Close it before switching Steam accounts."
}

$steamExe = Get-SteamExe
$accounts = @(Get-SteamAccounts $steamExe)
if ($accounts.Count -eq 0) {
    throw "Steam has no saved accounts in config\loginusers.vdf. Sign into Steam once, remember the account, then retry."
}

if ($ListOnly) {
    $accounts | Select-Object AccountName, PersonaName, MostRecent, SteamId | Format-Table -AutoSize
    exit 0
}

$selected = $null
if ($AccountName) {
    $selected = $accounts | Where-Object { $_.AccountName -ieq $AccountName } | Select-Object -First 1
    if (-not $selected) { throw "Steam account '$AccountName' is not saved on this PC." }
} else {
    Write-Host ""
    Write-Host "Pick the Steam account for DayZ:"
    for ($i = 0; $i -lt $accounts.Count; $i++) {
        $account = $accounts[$i]
        $recent = if ($account.MostRecent) { " (last used)" } else { "" }
        $label = if ($account.PersonaName) {
            "$($account.PersonaName) [$($account.AccountName)]"
        } else {
            $account.AccountName
        }
        Write-Host ("  [{0}] {1}{2}" -f ($i + 1), $label, $recent)
    }
    do {
        $choice = Read-Host "Account"
        $number = 0
        $valid = [int]::TryParse($choice, [ref]$number) -and $number -ge 1 -and $number -le $accounts.Count
        if (-not $valid) { Write-Host "Enter a number from 1 to $($accounts.Count)." -ForegroundColor Yellow }
    } while (-not $valid)
    $selected = $accounts[$number - 1]
}

$expectedId = Get-SteamAccountId $selected.SteamId
$alreadyReady = (Get-Process steam -ErrorAction SilentlyContinue) -and
    (Get-Process steamwebhelper -ErrorAction SilentlyContinue) -and
    (Get-ActiveSteamAccountId) -eq $expectedId

if ($alreadyReady) {
    Write-Host "Steam is already signed into $($selected.AccountName)."
} else {
    Stop-Steam $steamExe
    Write-Host "Starting Steam as $($selected.AccountName). Complete any password or Steam Guard prompt."
    Start-Process -FilePath $steamExe -ArgumentList @("-login", $selected.AccountName) | Out-Null
    Wait-SteamAccount -ExpectedAccountId $expectedId -TimeoutSeconds $SteamLoginTimeoutSeconds
    Write-Host "Steam account verified."
}

$launcher = Resolve-OakLauncher
Write-Host "Starting Oak. It will launch DayZ automatically after Oak sign-in/session restore..."
Start-Process -FilePath $launcher -ArgumentList "--auto-launch-dayz" | Out-Null
