# Launch DayZ NoBE to local server (no UAC elevation required for direct exe).
$ErrorActionPreference = "Stop"
. "..\..\..\clients\dayz\scripts\_dayz_steam_launch.ps1"
Wait-SteamReady -Log { param($m) Write-Host $m } | Out-Null
Ensure-DayZSteamAppId -Log { param($m) Write-Host $m }
Write-Host "Launching DayZ..."
Start-DayZNoBattlEye -ConnectHost "127.0.0.1" -ConnectPort 2302 -Log { param($m) Write-Host $m }
$p = Wait-DayZProcess -TimeoutSeconds 180 -Log { param($m) Write-Host $m }
Write-Host ("OK pid={0}" -f $p.Id)
