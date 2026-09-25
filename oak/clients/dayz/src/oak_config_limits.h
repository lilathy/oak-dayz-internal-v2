#pragma once

// Hard ceilings (Appendix B) and defaults (Appendix A) for Oak DayZ client config v2.

enum OakAimTargetFilter : int
{
    OakAimPlayers = 0,
    OakAimZombies = 1,
    OakAimBoth = 2,
};

enum OakCorpseSortMode : int
{
    OakCorpseSortDistance = 0,
    OakCorpseSortAge = 1,
    OakCorpseSortPlayerFirst = 2,
};

enum OakTrapSortMode : int
{
    OakTrapSortDistance = 0,
    OakTrapSortType = 1,
    OakTrapSortArmedFirst = 2,
};

enum { OAK_WAYPOINT_MAX = 64 };
enum { OAK_LOOT_FILTER_MAX = 64 };
enum { OAK_CONFIG_VERSION = 4 };

// Distance ceilings (meters)
static const int OAK_CAP_DIST_M = 4000;
static const int OAK_CAP_MAGNET_M = 50;
static const int OAK_CAP_FREECAM_SPEED = 50;

// Count ceilings (per frame / session)
static const int OAK_CAP_ESP_UPDATE_HZ = 60;
static const int OAK_CAP_ESP_UPDATE_HZ_MIN = 1;
static const int OAK_CAP_MAX_ENTITIES_SCANNED = 4096;
static const int OAK_CAP_MAX_ENTITIES_DRAWN = 1024;
static const int OAK_CAP_MAX_LABELS = 512;
static const int OAK_CAP_MAX_SKELETONS = 256;
static const int OAK_CAP_MAX_LOOT = 2048;
static const int OAK_CAP_MAX_CORPSES = 256;
static const int OAK_CAP_MAX_TRAPS = 128;
static const int OAK_CAP_MAX_PLAYERS_DRAWN = 128;
static const int OAK_CAP_MAX_ZOMBIES_DRAWN = 512;
static const int OAK_CAP_MAX_ANIMALS_DRAWN = 128;
static const int OAK_CAP_MAX_AIM_TARGETS = 128;
static const int OAK_CAP_FRAME_BUDGET_MS = 8;
static const int OAK_CAP_FRAME_BUDGET_MS_MIN = 1;
static const int OAK_CAP_FPS_LIMIT_MIN = 30;
static const int OAK_CAP_FPS_LIMIT = 360;
static const int OAK_CAP_FRAME_LATENCY_MAX = 4;

// Soft warning thresholds (UI only)
static const int OAK_WARN_MAX_ENTITIES_DRAWN = 300;
static const int OAK_WARN_MAX_LOOT = 500;
static const int OAK_WARN_MAX_SKELETONS = 50;
static const int OAK_WARN_ESP_UPDATE_HZ = 30;

struct OakPerfLimits
{
    int espUpdateHz = 20;
    int maxEntitiesScanned = 512;
    int maxEntitiesDrawn = 100;
    int maxLabelsPerFrame = 50;
    int maxSkeletonsPerFrame = 25;
    int maxLootPerFrame = 150;
    int maxCorpsesPerFrame = 50;
    int maxTrapsPerFrame = 30;
    int maxZombiesDrawn = 60;
    int maxAnimalsDrawn = 30;
    int maxPlayersDrawn = 40;
    int maxAimTargetsEvaluated = 32;
    int lodNearM = 100;
    int lodMidM = 300;
    int lodFarM = 500;
    bool cullOffscreen = true;
    bool asyncScan = true;
    int frameBudgetMs = 4;
    // Frame Boost module (Session) — Present unlock + FPS cap.
    bool frameBoost = false;      // master module toggle
    bool fpsCapEnabled = false;
    int fpsCap = 240;
    bool unlockPresent = false;   // force DXGI SyncInterval=0
    bool allowTearing = true;
    // Default off: exclusive FS toggles break NVIDIA Freestyle (nvppex) on DayZ.
    // Enable only if you need blit uncapping and are not using NVIDIA filters.
    bool breakRefreshLock = false;
    bool highPerfMode = false;    // priority + timer + no power throttle
    bool patchDayzCfg = false;
    int maxFrameLatency = 0;
};

struct OakPerfStats
{
    int scanned = 0;
    int drawn = 0;
    int labelsDrawn = 0;
    int skeletonsDrawn = 0;
    int lootDrawn = 0;
    int corpsesDrawn = 0;
    int trapsDrawn = 0;
    int playersDrawn = 0;
    int zombiesDrawn = 0;
    int animalsDrawn = 0;
    float frameMs = 0.f;
    int effectiveScanCap = 0;
    int effectiveDrawCap = 0;
};

struct OakCorpseEspSettings
{
    bool playerCorpses = true;
    bool infectedCorpses = true;
    bool box = true;
    bool name = true;
    bool distance = true;
    bool skeleton = false;
    bool inventorySummary = false;
    int inventoryMaxLines = 4;
    int inventoryTruncateLen = 48;
    bool fadeByAge = false;
    int sortMode = OakCorpseSortDistance;
};

struct OakTrapEspSettings
{
    bool box = true;
    bool name = true;
    bool distance = true;
    bool typeLabel = true;
    bool highlightArmed = true;
    int sortMode = OakTrapSortDistance;
};

inline int OakClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline void OakClampPerfLimits(OakPerfLimits* p)
{
    if (!p) return;
    p->espUpdateHz = OakClampInt(p->espUpdateHz, OAK_CAP_ESP_UPDATE_HZ_MIN, OAK_CAP_ESP_UPDATE_HZ);
    p->maxEntitiesScanned = OakClampInt(p->maxEntitiesScanned, 50, OAK_CAP_MAX_ENTITIES_SCANNED);
    p->maxEntitiesDrawn = OakClampInt(p->maxEntitiesDrawn, 20, OAK_CAP_MAX_ENTITIES_DRAWN);
    p->maxLabelsPerFrame = OakClampInt(p->maxLabelsPerFrame, 10, OAK_CAP_MAX_LABELS);
    p->maxSkeletonsPerFrame = OakClampInt(p->maxSkeletonsPerFrame, 5, OAK_CAP_MAX_SKELETONS);
    p->maxLootPerFrame = OakClampInt(p->maxLootPerFrame, 20, OAK_CAP_MAX_LOOT);
    p->maxCorpsesPerFrame = OakClampInt(p->maxCorpsesPerFrame, 10, OAK_CAP_MAX_CORPSES);
    p->maxTrapsPerFrame = OakClampInt(p->maxTrapsPerFrame, 10, OAK_CAP_MAX_TRAPS);
    p->maxZombiesDrawn = OakClampInt(p->maxZombiesDrawn, 5, OAK_CAP_MAX_ZOMBIES_DRAWN);
    p->maxAnimalsDrawn = OakClampInt(p->maxAnimalsDrawn, 5, OAK_CAP_MAX_ANIMALS_DRAWN);
    p->maxPlayersDrawn = OakClampInt(p->maxPlayersDrawn, 5, OAK_CAP_MAX_PLAYERS_DRAWN);
    p->maxAimTargetsEvaluated = OakClampInt(p->maxAimTargetsEvaluated, 8, OAK_CAP_MAX_AIM_TARGETS);
    p->lodNearM = OakClampInt(p->lodNearM, 25, OAK_CAP_DIST_M);
    p->lodMidM = OakClampInt(p->lodMidM, 50, OAK_CAP_DIST_M);
    p->lodFarM = OakClampInt(p->lodFarM, 100, OAK_CAP_DIST_M);
    p->frameBudgetMs = OakClampInt(p->frameBudgetMs, OAK_CAP_FRAME_BUDGET_MS_MIN, OAK_CAP_FRAME_BUDGET_MS);
    p->fpsCap = OakClampInt(p->fpsCap, OAK_CAP_FPS_LIMIT_MIN, OAK_CAP_FPS_LIMIT);
    p->maxFrameLatency = OakClampInt(p->maxFrameLatency, 0, OAK_CAP_FRAME_LATENCY_MAX);
}

inline void OakSyncAimbotBoolsFromFilter(int filter, bool* aimPlayers, bool* aimZombies)
{
    if (!aimPlayers || !aimZombies) return;
    switch (filter)
    {
    default:
    case OakAimPlayers: *aimPlayers = true;  *aimZombies = false; break;
    case OakAimZombies: *aimPlayers = false; *aimZombies = true;  break;
    case OakAimBoth:    *aimPlayers = true;  *aimZombies = true;  break;
    }
}

inline int OakDeriveAimTargetFilter(bool aimPlayers, bool aimZombies)
{
    if (aimPlayers && aimZombies) return OakAimBoth;
    if (aimZombies) return OakAimZombies;
    return OakAimPlayers;
}

enum { OAK_LOOT_CAT_COUNT = 7 };

enum OakLootSortMode : int
{
    OakLootSortDistance = 0,
    OakLootSortName = 1,
    OakLootSortCategory = 2,
};

enum OakEspBoxStyle : int
{
    OakBox2D = 0,      // full connected rectangle
    OakBoxCorner = 1,
    OakBox3D = 2,
};

enum OakHudCorner : int
{
    OakHudTopLeft = 0,
    OakHudTopRight = 1,
    OakHudBottomLeft = 2,
    OakHudBottomRight = 3,
};

enum OakCompassPos : int
{
    OakCompassTop = 0,
    OakCompassBottom = 1,
};

enum OakAimPriority : int
{
    OakAimPriCrosshair = 0,
    OakAimPriDistance = 1,
    OakAimPriLowHp = 2,
};

enum OakPanicScope : int
{
    OakPanicAll = 0,
    OakPanicEspOnly = 1,
    OakPanicCombatOnly = 2,
};

struct OakLootCatSettings
{
    bool enabled = true;
    int maxDistance = 80;
    int maxCount = 50;
    float color[4] = { 0.70f, 0.82f, 0.95f, 0.85f };
    bool useCustomColor = false;
};

struct OakEspStyleSettings
{
    int boxStyle = OakBoxCorner;
    float boxThickness = 1.5f;
    float fillOpacity = 0.f;
    bool outlineOnly = false;
    float skelThickness = 1.0f;
    float colorVisible[4] = { 0.40f, 0.85f, 1.00f, 0.95f };
    float colorOccluded[4] = { 0.55f, 0.55f, 0.55f, 0.75f };
    bool useVisibilityColors = false;
    int healthBarPos = 0; // 0=L 1=T 2=B 3=R
};

struct OakAimbotExtras
{
    int priority = OakAimPriCrosshair;
    int smoothVariancePct = 0;
    int reactionDelayMs = 0;
    bool requireLos = false;
};

struct OakUiExtras
{
    float menuAccent[4] = { 0.55f, 0.72f, 0.98f, 1.0f };
    int panicScope = OakPanicAll;
    unsigned bindHoldMask = 0; // bit per toggle bind (esp, aimbot, magic, fullbright, overlays, steam, contMag, freecam)
    int profileHotkey[3] = { 0, 0, 0 }; // VK for Legit/Rage/PvE quick load
};

struct OakThreatRingSettings
{
    bool enabled = false;
    bool showPlayers = true;
    bool showZombies = true;
    bool zombiesOnly = false;
    bool hideInMenu = true;
    bool showDistLabels = false;
    int maxDistanceM = 80;
    float ringRadiusPx = 120.f;
    float colorPlayer[4] = { 1.00f, 0.30f, 0.30f, 0.90f };
    float colorZombie[4] = { 1.00f, 0.55f, 0.15f, 0.85f };
    float colorFriend[4] = { 0.40f, 0.80f, 1.00f, 0.45f };
};

struct OakThreatCounterSettings
{
    bool enabled = false;
    bool hideWhenZero = true;
    int maxDistanceM = 80;
    float offsetX = 0.f;
    float offsetY = -42.f;
};

struct OakCompassSettings
{
    bool enabled = false;
    bool showDegrees = true;
    bool showWaypointBearing = true;
    int position = OakCompassTop;
};

struct OakLookDirectionSettings
{
    bool enabled = true;
    bool players = true;
    bool zombies = true;
    int maxDistanceM = 200;
    float lineLengthM = 4.f;
    float color[4] = { 0.40f, 0.85f, 1.00f, 0.85f };
    float colorZombie[4] = { 0.95f, 0.45f, 0.30f, 0.80f };
};

struct OakPlayerTrailSettings
{
    bool enabled = false;
    int maxPlayers = 16;
    int maxPoints = 24;
    float durationSec = 8.f;
    float color[4] = { 0.50f, 0.70f, 1.00f, 0.60f };
};

struct OakDeathMarkerSettings
{
    bool enabled = false;
    bool autoClearOnRespawn = true;
};

struct OakNightBoostSettings
{
    bool enabled = false;
    int brightness = 60;
};

struct OakCrosshairHighlightSettings
{
    bool enabled = false;
    float colorDefault[4] = { 0.90f, 0.95f, 1.00f, 0.85f };
    float colorHighlight[4] = { 1.00f, 0.25f, 0.25f, 0.95f };
};

struct OakFovHighlightSettings
{
    bool enabled = false;
    float colorIdle[4] = { 0.70f, 0.75f, 0.78f, 0.30f };
    float colorActive[4] = { 0.47f, 0.86f, 1.00f, 0.67f };
};

struct OakReloadBarSettings
{
    bool enabled = false;
    float width = 120.f;
    float height = 6.f;
    float offsetY = 22.f;
    float colorBg[4] = { 0.00f, 0.00f, 0.00f, 0.50f };
    float colorFill[4] = { 0.30f, 0.90f, 0.40f, 0.90f };
};

struct OakHeliCrashEspSettings
{
    bool enabled = false;
    int maxDistanceM = 2000;
    float color[4] = { 1.00f, 0.50f, 0.10f, 0.90f };
};

struct OakGridCoordsHudSettings
{
    bool enabled = false;
    int corner = OakHudBottomLeft;
};

struct OakWaypointHudSettings
{
    bool enabled = false;
    bool showBearing = true;
    int corner = OakHudBottomLeft;
    int activeIndex = 0;
};

struct OakStanceIconSettings
{
    bool enabled = false;
    int maxDistanceM = 250;
};

// Phase B — batch 4 ESP / loot (T4-9 .. T4-27)
enum OakVisColorHeuristic : int
{
    OakVisHeuristicNearFar = 0,
    OakVisHeuristicDepth = 1,
};

enum { OAK_BATCH4_MAX_CONTAINER_LINES = 8 };
enum { OAK_BATCH4_MAX_QUALITY_PROBES = 32 };
enum { OAK_BATCH4_MAX_QTY_PROBES = 32 };
enum { OAK_BATCH4_MAX_CONTAINER_PROBES = 12 };
enum { OAK_BATCH4_LOOKING_THROTTLE_MS = 150 };

struct OakLootQualityEspSettings
{
    bool enabled = true;
    bool showBadge = true;
    int maxProbesPerFrame = OAK_BATCH4_MAX_QUALITY_PROBES;
};

struct OakLootQuantityEspSettings
{
    bool enabled = true;
    int maxProbesPerFrame = OAK_BATCH4_MAX_QTY_PROBES;
};

struct OakEspVisibilityHeuristicSettings
{
    bool enabled = true; // kept for config; style useVisibilityColors is the user-facing toggle
    int mode = OakVisHeuristicNearFar; // fallback when LOS budget exhausted
    float depthOccludeM = 120.f;
};

struct OakContainerContentsEspSettings
{
    bool enabled = false;
    int maxLines = 4;
    int maxContainersPerFrame = OAK_BATCH4_MAX_CONTAINER_PROBES;
};

struct OakLookingAtMeSettings
{
    bool enabled = false;
    bool players = true;
    bool zombies = true;
    int maxDistanceM = 200;
    int angleDeg = 35; // cone half-angle toward local player
    float dotThreshold = 0.82f; // legacy; overwritten from angleDeg when reading config
    float color[4] = { 1.00f, 0.25f, 0.25f, 0.95f };
    float colorZombie[4] = { 1.00f, 0.55f, 0.18f, 0.95f };
};

struct OakContaminationEspSettings
{
    bool enabled = false;
    int maxDistanceM = 2500;
    float color[4] = { 0.55f, 0.90f, 0.20f, 0.40f };
    bool showLabels = true;
    float defaultRadiusM = 100.f; // used when entity radius float not found
    // Ring height relative to local player / camera (meters). Positive = above feet.
    // Primary blob always tracks viewer height so it never "floats away" above you.
    float yOffsetM = 0.f; // fine-tune around underground base (see draw)
    bool drawGroundRing = true; // faint ring at zone ground Y
};

struct OakPlayerInvViewerSettings
{
    bool enabled = false;
    int maxDistanceM = 150;
    float crosshairRadiusPx = 80.f;
    float color[4] = { 0.95f, 0.85f, 0.30f, 0.95f };
};

struct OakBatch4EspSettings
{
    bool lootExtrasEnabled = false;
    OakLootQualityEspSettings quality;
    OakLootQuantityEspSettings quantity;
    OakEspVisibilityHeuristicSettings visibility;
    OakContainerContentsEspSettings containerContents;
    OakLookingAtMeSettings lookingAtMe;
    OakContaminationEspSettings contamination;
    OakPlayerInvViewerSettings playerInvViewer;
};

inline int OakLootCategoryIndex(int lootCat) // LootCategory enum from main.cpp order
{
    switch (lootCat)
    {
    case 1: return 0; // Weapon
    case 2: return 1; // Ammo
    case 3: return 2; // Medical
    case 4: return 3; // Food
    case 5: return 4; // Clothing
    case 6: return 5; // Tool
    default: return 6; // Other
    }
}

// --- Batch 4 (docs/remaining.txt) feature settings ---

struct OakSilentAimSettings
{
    bool enabled = false;
    int maxDistanceM = 400;
    int fovPx = 160;
    int bone = 0; // 0=head 1=chest
    int key = 0;  // legacy; prefer bindSilentAim
    bool bypass25m = true;
    bool drawFov = true;
    bool drawLock = true;
    bool players = true;
    bool zombies = true;
};

struct OakGrenadeTeleportSettings
{
    bool enabled = false;
    int fovPx = 120;
    int maxDistanceM = 100;
    bool drawFov = true;
    bool players = true;
    bool zombies = true;
};

struct OakTriggerbotSettings
{
    bool enabled = false;
    int delayMs = 0;
    int maxDistanceM = 200;
    bool requireAds = false;
    bool requireLos = false; // prop-sphere heuristic (oak_batch4_esp.inl)
    int deadzonePx = 36;
    bool players = true;
    bool zombies = true;
};

struct OakWallBypassSettings
{
    bool enabled = false;
    float maxPenetrationM = 15.f;
};

// Remaining % — 0 = fully suppressed, 100 = vanilla kick/sway.
struct OakRecoilSettings
{
    bool noRecoil = false;
    bool noSway = false;
    int recoilPct = 0;
    int swayPct = 0;
};

struct OakAimbotBatch4Settings
{
    int aimAssistKey = 0;
    bool autoFireWhenLocked = false;
};

enum OakShadowEntityKind : int
{
    OakShadowKindPlayer = 0,
    OakShadowKindZombie,
    OakShadowKindItem,
    OakShadowKindVehicle,
    OakShadowKindContainer,
    OakShadowKindCorpse,
};

enum OakShadowChamPattern : int
{
    OakShadowPatternSolid = 0,
    OakShadowPatternPulse,
    OakShadowPatternScroll,
    OakShadowPatternRainbow,
    OakShadowPatternStripes,
};

struct OakShadowChamsSettings
{
    bool enabled = false;
    bool players = true;
    bool zombies = true;
    bool items = true;
    bool hands = true;
    bool vehicles = false;
    bool containers = false;
    bool corpses = false;
    int pattern = OakShadowPatternSolid;
    float fillAlpha = 0.42f;
    float outlineThick = 2.5f;
    float animSpeed = 1.0f;
    float color[4] = { 0.20f, 0.85f, 1.00f, 0.90f };
    float color2[4] = { 0.85f, 0.25f, 1.00f, 0.90f };
    float outlineColor[4] = { 0.05f, 0.05f, 0.08f, 0.95f };
};

struct OakWorldMiscSettings
{
    bool fovChanger = false;
    float horizontalFov = 95.f;
    bool fovKeepWhileAds = false; // when true, ADS/optics do not narrow FOV
    bool timeLock = false;
    float lockHour = 12.f;
    bool clearWeather = false;
    bool thirdPerson = false;
    float thirdPersonDistanceM = 2.8f; // soft shoulder-cam pullback
    float thirdPersonHeightM = 1.55f;
    bool speedHack = false;
    float speedMultiplier = 1.35f;
    int speedHackKey = 0;
    bool infiniteStamina = false;
    bool streamProof = false;
    bool wireframe = false;
};

struct OakExploitSettings
{
    bool lootThroughWalls = false;
    int lootThroughWallsKey = 0;
    int lootThroughWallsRangeM = 12;
    bool grenadeThroughWalls = false;
    bool warp = false;
    float warpDistanceM = 150.f; // unused (legacy config key)
    int warpKey = 0x54; // T — HOLD to cut outbound, release to flush
    int warpCooldownMs = 1500;
    // Max hold window: 0=Safe 4s, 1=Balanced 7s, 2=Aggressive 12s
    int warpMode = 1;
    bool warpFlat = true; // legacy
    bool warpHoldChain = false; // legacy
    bool noclip = false;
    float noclipSpeed = 8.f;
    int noclipKey = 0;
    bool doorUnlock = false;
    bool lootLivingPlayers = false;
    bool mugPlayer = false;
    bool itemDupe = false;
};

inline void OakDefaultLootCatColors(int idx, float* r, float* g, float* b, float* a)
{
    if (!r || !g || !b || !a) return;
    switch (idx)
    {
    case 0: *r = 1.00f; *g = 0.35f; *b = 0.25f; break;
    case 1: *r = 1.00f; *g = 0.75f; *b = 0.20f; break;
    case 2: *r = 0.35f; *g = 1.00f; *b = 0.45f; break;
    case 3: *r = 0.95f; *g = 0.65f; *b = 0.20f; break;
    case 4: *r = 0.65f; *g = 0.55f; *b = 1.00f; break;
    case 5: *r = 0.55f; *g = 0.85f; *b = 0.95f; break;
    default: *r = 0.35f; *g = 0.75f; *b = 1.00f; break;
    }
    *a = 1.0f;
}
