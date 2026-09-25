#pragma once

#include <d3d11.h>
#include <Windows.h>

#include "oak_config_limits.h"

struct ImDrawList;

// In-game ImGui menu (DX11 Present path) + ESP settings bridge for dayz_internal.
void ImGuiMenu_Log(const char* msg);
const char* ImGuiMenu_LogPath();
bool ImGuiMenu_Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context);
// Create ImGui context early (worker thread) — under BE, CreateContext on the
// Present thread AVs. Safe to call before D3D backends exist.
bool ImGuiMenu_PrecreateContext();
// Call OUTSIDE Present — runs D3DCompile/CreateDeviceObjects safely.
bool ImGuiMenu_Warmup();
bool ImGuiMenu_IsWarmed();
void ImGuiMenu_Shutdown();
void ImGuiMenu_OnPresent(ID3D11RenderTargetView* rtv); // legacy full frame

// Split frame so ESP can draw into ImGui lists between NewFrame and Render.
bool ImGuiMenu_BeginFrame();
void ImGuiMenu_EndFrame(ID3D11RenderTargetView* rtv);
ImDrawList* ImGuiMenu_EspDrawList();

bool ImGuiMenu_IsOpen();
void ImGuiMenu_SetOpen(bool open);
// Feed relative mouse when the menu owns look (DayZ recenters the OS cursor).
void ImGuiMenu_OnRawMouse(int dx, int dy);
unsigned ImGuiMenu_FrameCount();
bool ImGuiMenu_WasDrawn();

struct ImGuiEspSettings {
    bool ready;
    bool espEnabled;
    bool espPlayers;
    bool espZombies;
    bool espAnimals;
    bool espItems;
    bool lootShowWeapons;
    bool lootShowAmmo;
    bool lootShowMedical;
    bool lootShowFood;
    bool lootShowClothing;
    bool lootShowTools;
    bool lootShowOther;
    bool espVehicles;
    bool espSkeleton;
    bool espChams;
    bool playerBox;
    bool playerName;
    bool playerDistance;
    bool zombieBox;
    bool zombieName;
    bool zombieDistance;
    bool drawLocalPlayer;
    int playerMaxDistance;
    int zombieMaxDistance;
    int animalMaxDistance;
    int itemMaxDistance;
    int vehicleMaxDistance;
    bool itemBox;
    bool itemName;
    bool itemDistance;
    float colorItemBox[4];
    float colorItemName[4];
    float colorPlayerBox[4];
    float colorPlayerSkeleton[4];
    float colorPlayerName[4];
    float colorPlayerChams[4];
    float colorZombieBox[4];
    float colorZombieSkeleton[4];
    float colorZombieName[4];
    float colorZombieChams[4];

    // Combat visuals
    bool bulletTracers;
    bool impactMarkers;
    bool shotIndicators;
    bool hitMarkers;
    bool crosshair;
    bool weaponEsp;
    bool healthBars;       // master for remote ESP bars
    bool barHealth;
    bool barBlood;
    bool barShock;
    bool barStamina;
    bool barHunger;
    bool barThirst;
    bool localVitalsHud;
    bool localWeaponAmmo;
    bool grenadeTrajectory;

    // Friends — aimbot / magic bullet skip these SteamIDs
    int friendCount;
    unsigned long long friendSteamId[16];
    char friendName[16][64];
    int tracerLifetimeMs;
    int impactLifetimeMs;
    float colorTracer[4];
    float colorImpact[4];
    float colorShotInd[4];
    float colorHitMarker[4];
    float colorCrosshair[4];
    float colorGrenade[4];

    // Aimbot / magic bullet / ammo
    bool aimbotEnabled;
    bool magicBullet;
    bool magicBulletAutoFire;   // auto LMB when targets are in MB range
    bool magicBulletChain;      // one bullet snaps through every target in range
    bool magicBulletDrawFov;
    bool aimbotPlayers;
    bool aimbotZombies;
    int aimTargetFilter; // OakAimTargetFilter
    bool aimbotDrawFov;
    bool fastBullets;
    bool noDispersion;
    bool perfectBallistics;
    int aimbotFov;
    int aimbotSmooth;
    int aimbotBone;      // 0=head, 1=chest
    int aimbotKey;       // VK_*
    int aimbotMaxDistance;
    int magicBulletMaxDistance;
    int magicBulletFov;      // px — independent of aimbot FOV
    int crosshairStyle;      // 0 cross, 1 gap-cross, 2 dot, 3 circle, 4 T
    int crosshairSize;
    int crosshairGap;
    int crosshairThickness;

    // World visuals
    bool espContainers;
    bool espCorpses;
    bool espTraps;
    bool fullbright;
    int fullbrightBrightness; // 1..100 — scales EyeAccom when fullbright/daytime on
    int containerMaxDistance;
    int corpseMaxDistance;
    int trapMaxDistance;
    float colorContainer[4];
    float colorCorpse[4];
    float colorTrap[4];

    // Misc
    bool miscMiddleClickDespawn;
    bool miscLootMagnet;
    bool miscContainerMagnet;
    bool miscDaytimeLock;
    bool miscDisableOverlays;
    bool miscSteamNames;
    bool miscSteamAvatars;
    bool miscDrawWaypoints;
    bool miscFreecam;
    bool miscFreecamMoveBody;
    int miscFreecamSpeed;
    bool miscNoGrass;
    int miscLootMagnetRange;
    int miscContainerMagnetRange;
    int miscDespawnRange;

    // Keybinds — 0 = unset (user configures). Middle-click despawn defaults to Mouse3.
    int bindEsp;
    int bindAimbot;
    int bindMagicBullet;
    int bindFullbright;
    int bindMiddleClickDespawn;
    int bindLootMagnet;
    int bindContainerMagnet;
    int bindDaytimeLock;
    int bindDisableOverlays;
    int bindPanic;
    int bindAddWaypoint;
    int bindCopyCoords;
    int bindSteamNames;
    int bindPullBasePart;
    int bindFreecam;

    // Performance / limits (config v2)
    OakPerfLimits perf;

    // Corpse & trap customization
    OakCorpseEspSettings corpseEsp;
    OakTrapEspSettings trapEsp;

    // Loot granularity (Phase 2)
    OakLootCatSettings lootCats[OAK_LOOT_CAT_COUNT];
    int lootSortMode;
    int lootFilterBlacklistCount;
    char lootFilterBlacklist[OAK_LOOT_FILTER_MAX][48];
    int lootFilterWhitelistCount;
    char lootFilterWhitelist[OAK_LOOT_FILTER_MAX][48];

    // ESP style (Phase 3)
    OakEspStyleSettings playerStyle;
    OakEspStyleSettings zombieStyle;

    // Combat extras (Phase 4)
    OakAimbotExtras aimExtras;

    // UI / keybinds (Phase 6)
    OakUiExtras uiExtras;

    // Waypoints — mirrored for ESP draw
    int waypointCount;
    float waypointX[OAK_WAYPOINT_MAX];
    float waypointY[OAK_WAYPOINT_MAX];
    float waypointZ[OAK_WAYPOINT_MAX];
    char waypointName[OAK_WAYPOINT_MAX][32];
    float waypointColor[OAK_WAYPOINT_MAX][4];

    // Awareness / HUD features (config v3)
    OakThreatRingSettings threatRing;
    OakThreatCounterSettings threatCounter;
    OakCompassSettings compass;
    OakLookDirectionSettings lookDirection;
    OakPlayerTrailSettings playerTrail;
    OakDeathMarkerSettings deathMarker;
    OakNightBoostSettings nightBoost;
    OakCrosshairHighlightSettings crosshairHighlight;
    OakFovHighlightSettings fovHighlight;
    OakReloadBarSettings reloadBar;
    OakHeliCrashEspSettings heliCrashEsp;
    OakGridCoordsHudSettings gridCoordsHud;
    OakWaypointHudSettings waypointHud;
    OakStanceIconSettings stanceIcon;
    OakShadowChamsSettings shadowChams;
    OakBatch4EspSettings batch4Esp;
    OakSilentAimSettings silentAim;
    OakGrenadeTeleportSettings grenadeTeleport;
    OakTriggerbotSettings triggerbot;
    OakWallBypassSettings wallBypass;
    OakRecoilSettings recoil;
    OakAimbotBatch4Settings aimbotB4;
    OakWorldMiscSettings worldMisc;
    OakExploitSettings exploits;
    bool streamProof;
    int bindSilentAim;
    int bindAimAssist;
};

// Misc UI helpers called from main (clipboard / waypoint add from hotkey)
ID3D11Device* ImGuiMenu_GetD3DDevice();
ID3D11DeviceContext* ImGuiMenu_GetD3DContext();
void ImGuiMenu_AddWaypointHere(float x, float y, float z, const char* name);
void ImGuiMenu_ClearWaypoints();
bool ImGuiMenu_ConsumePanicLatch(); // true once when panic fired from menu/hotkey path
void ImGuiMenu_RequestCopyCoords();
bool ImGuiMenu_ConsumeCopyCoords();
void ImGuiMenu_RequestPullBasePart();
bool ImGuiMenu_ConsumePullBasePart();

// Push runtime misc toggles back into the menu so keybinds aren't overwritten next frame
void ImGuiMenu_PushMiscFlags(
    bool despawn, bool magnet, bool containerMagnet, bool daytime, bool overlays,
    bool steamNames, bool steamAvatars, bool drawWp, bool freecam,
    bool esp, bool aimbot, bool magic, bool fullbright);
void ImGuiMenu_PushAmmoFlags(bool fastBullets, bool noDispersion, bool perfectBallistics);
// Dual-client live QA: force features that need soak evidence (3PP/recoil/bars/names/freecam).
void ImGuiMenu_PushDualQa(bool thirdPerson, bool noRecoil, bool healthBars, bool freecam);
// Crash-matrix: apply one named step (or "baseline"/"idle") into g_Ui. Returns false if unknown.
bool ImGuiMenu_ApplyCrashMatrixStep(const char* stepId);
// True while crash-matrix flag is armed (Present path skips dual-QA fights).
bool ImGuiMenu_CrashMatrixActive();

void ImGuiMenu_GetEspSettings(ImGuiEspSettings* out);
void ImGuiMenu_SetPerfStats(const OakPerfStats* stats);
void ImGuiMenu_GetPerfStats(OakPerfStats* out);
void ImGuiMenu_SetPerfBoostStatus(const char* note);
const char* ImGuiMenu_GetPerfBoostStatus();
void ImGuiMenu_SetPlayerPos(float x, float y, float z);
void ImGuiMenu_SaveConfig();
void ImGuiMenu_LoadConfig();
void ImGuiMenu_ApplyProfile(int profileId); // 0=Legit 1=Rage 2=PvE
void ImGuiMenu_SaveProfile(int profileId);
void ImGuiMenu_LoadProfile(int profileId);
void ImGuiMenu_SaveNamedProfile(const char* name);
void ImGuiMenu_LoadNamedProfile(const char* name);
void ImGuiMenu_ExportConfig();
void ImGuiMenu_ImportConfig();
void ImGuiMenu_ResetSection(const char* section);
void ImGuiMenu_ResetAll();
void ImGuiMenu_SetBatch4Status(const char* door);
void ImGuiMenu_SetWarpStatus(const char* note);
void ImGuiMenu_SetExploitWarp(bool on);
void ImGuiMenu_SetRecoilStatus(bool ok, const char* note);
const char* ImGuiMenu_RecoilNote();
bool ImGuiMenu_RecoilOk();
const char* ImGuiMenu_Version();
