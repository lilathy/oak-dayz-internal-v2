@echo off
setlocal EnableExtensions
set "LOG=%LOCALAPPDATA%\DayZ\oak_nvidia_wrap.log"
echo [%DATE% %TIME%] wrap start args=%*>>"%LOG%"
:: Keep this legacy entry point injection-safe. The old filter-only path launched
:: DayZ without -Inject, which made startup appear successful but injected nothing.
call "%~dp0steam_wrap.cmd" %*
set "EC=%ERRORLEVEL%"
echo [%DATE% %TIME%] wrap exit=%EC%>>"%LOG%"
exit /b %EC%
