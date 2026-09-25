@echo off
setlocal EnableExtensions
:: Steam Launch Options (no quotes, forward slashes - VDF "C:\..." becomes "C:"):
:: C:/oak/dayz/steam_wrap.cmd %command%
cd /d C:\oak\dayz
:: LaunchDayZSafe.bat owns elevation and deliberately launches DayZ_BE itself.
:: Do not forward Steam's DayZLauncher command: that would start a second,
:: non-injected game while the elevated kernel launch is still preparing.
call "C:\oak\dayz\LaunchDayZSafe.bat"
exit /b %ERRORLEVEL%
