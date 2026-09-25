DayZ + Oak account launch
=========================

Normal one-file entry point:
  StartOakDayZ.bat

It lists the Steam accounts already saved on the PC, switches to the selected
account, verifies Steam finished signing in, then starts OakLauncher. Oak
restores its saved session (or shows sign-in) and automatically runs the full
protected DayZ + BattlEye + ImGui launch.

Direct developer launch
=======================

Admin required. Double-click:
  C:\oak\dayz\LaunchDayZSafe.bat
Or:
  powershell -File C:\oak\dayz\LaunchDayZSafe.ps1 -Steam -Inject

Order (important):
1. Stop BEDaisy if loaded (blocks iqvw64e with 0xC0000022)
2. Map oak.sys via oak_loader (Admin / SeLoadDriverPrivilege)
3. Start DayZ_BE.exe (BattlEye)
4. Hold NVIDIA Overlay off ~45s through D3D init
5. oak.sys auto-injects into DayZ_x64

Do NOT use -NoBE for BattlEye servers.
Do NOT RunOnce without elevation (gets 0xC0000061).

Stage DLL at C:\oak\dayz\dayz_internal.dll (Debug for local without API handoff;
Release needs OakLauncher bootstrap/lease).

Log: %LOCALAPPDATA%\DayZ\oak_nvidia_launch.log
Inject: C:\oak\inject.log
Client: %LOCALAPPDATA%\DayZ\oak_imgui.log
