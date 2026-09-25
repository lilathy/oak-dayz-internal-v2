#include "imgui_menu.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// Undocumented but exported from imgui_impl_win32.cpp — VK → ImGuiKey map
ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM wParam, LPARAM lParam);

static bool g_ImGuiReady = false;
static bool g_ImGuiWarmed = false;
static bool g_MenuOpen = false;
static HWND g_Hwnd = nullptr;

// Soft cursor / menu input state (no CRITICAL_SECTION — unsafe under manual-map).
static HANDLE g_SoftThread = nullptr;
static volatile LONG g_SoftRun = 0;
static volatile LONG g_SoftActive = 0;

static void OakSetMenuInputActive(bool active);
static void OakStopSoftCursorThread();
static void OakPollImGuiInput();
static WNDPROC g_OrigWndProc = nullptr;
static ID3D11DeviceContext* g_Ctx = nullptr;
static ID3D11Device* g_Device = nullptr;
static char g_LogPath[MAX_PATH] = {};
static unsigned g_PresentFrames = 0;
static bool g_WasDrawn = false;
static CRITICAL_SECTION g_LogCs;
static bool g_LogCsInit = false;

static void EnsureLogCs()
{
    // CRITICAL_SECTION is unsafe under BattlEye + manual-map (AV in hooked ICS).
    // Logging is best-effort single-writer; skip the CS entirely.
    g_LogCsInit = true;
}

static void EnsureLogPath()
{
    if (g_LogPath[0])
        return;

    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32)
    {
        if (!GetTempPathA(MAX_PATH - 16, g_LogPath))
            return;
        size_t len = (size_t)lstrlenA(g_LogPath);
        if (len + 14 >= MAX_PATH)
            return;
        lstrcatA(g_LogPath, "oak_imgui.log");
        return;
    }

    // "%LOCALAPPDATA%\DayZ\oak_imgui.log" — length-checked
    if (n + 22 >= MAX_PATH)
        return;
    wsprintfA(g_LogPath, "%s\\DayZ", base);
    CreateDirectoryA(g_LogPath, nullptr);
    wsprintfA(g_LogPath, "%s\\DayZ\\oak_imgui.log", base);
}

void ImGuiMenu_Log(const char* msg)
{
    if (!msg) return;
    EnsureLogPath();

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1024];
    wsprintfA(line, "[%02d:%02d:%02d.%03d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    // Keep one handle; rotate when large (AV re-scanning a growing log hitchs Present).
    static HANDLE s_Hf = INVALID_HANDLE_VALUE;
    static DWORD s_Bytes = 0;
    if (s_Hf == INVALID_HANDLE_VALUE)
    {
        s_Hf = CreateFileA(g_LogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_Hf == INVALID_HANDLE_VALUE) return;
        s_Bytes = GetFileSize(s_Hf, NULL);
        if (s_Bytes == INVALID_FILE_SIZE) s_Bytes = 0;
    }
    if (s_Bytes > (2u * 1024u * 1024u))
    {
        SetFilePointer(s_Hf, 0, NULL, FILE_BEGIN);
        SetEndOfFile(s_Hf);
        s_Bytes = 0;
    }
    DWORD w = 0;
    if (!WriteFile(s_Hf, line, (DWORD)lstrlenA(line), &w, NULL))
    {
        CloseHandle(s_Hf);
        s_Hf = INVALID_HANDLE_VALUE;
        return;
    }
    s_Bytes += w;
}

const char* ImGuiMenu_LogPath()
{
    EnsureLogPath();
    return g_LogPath;
}

unsigned ImGuiMenu_FrameCount()
{
    return g_PresentFrames;
}

bool ImGuiMenu_WasDrawn()
{
    return g_WasDrawn;
}

// ---------------------------------------------------------------------------
// Local UI state only — mirrors OakPanel layout. Does NOT drive game features.
// ---------------------------------------------------------------------------
struct UiState
{
    // Visuals — master + categories (perf-friendly defaults)
    bool espEnabled = true;
    bool espPlayers = true;
    bool espZombies = true;
    bool espAnimals = false;
    bool espItems = true;
    bool lootShowWeapons = true;
    bool lootShowAmmo = true;
    bool lootShowMedical = true;
    bool lootShowFood = true;
    bool lootShowClothing = true;
    bool lootShowTools = true;
    bool lootShowOther = true;
    bool espVehicles = true;
    bool espSkeleton = false;
    bool espChams = false;

    // Combat
    bool bulletTracers = true;
    bool impactMarkers = true;
    bool shotIndicators = true;
    bool hitMarkers = true;
    bool crosshair = true;
    bool weaponEsp = true;
    bool healthBars = true;
    bool barHealth = true;
    bool barBlood = true;
    bool barShock = true;
    bool barStamina = true;
    bool barHunger = true;
    bool barThirst = true;
    bool localVitalsHud = true;
    bool localWeaponAmmo = true;
    int friendCount = 0;
    unsigned long long friendSteamId[16] = {};
    char friendName[16][64] = {};
    bool grenadeTrajectory = true;
    int tracerLifetimeMs = 1500;
    int impactLifetimeMs = 800;
    ImVec4 colorTracer = ImVec4(1.00f, 0.82f, 0.25f, 0.85f);
    ImVec4 colorImpact = ImVec4(1.00f, 0.45f, 0.15f, 0.90f);
    ImVec4 colorShotInd = ImVec4(1.00f, 0.25f, 0.30f, 0.95f);
    ImVec4 colorHitMarker = ImVec4(1.00f, 0.30f, 0.30f, 1.0f);
    ImVec4 colorCrosshair = ImVec4(0.90f, 0.95f, 1.00f, 0.85f);
    ImVec4 colorGrenade = ImVec4(1.00f, 0.55f, 0.15f, 0.90f);

    // Aimbot / magic bullet (off by default)
    bool aimbotEnabled = false;
    bool magicBullet = false;
    bool magicBulletAutoFire = false;
    bool magicBulletChain = false;
    bool magicBulletDrawFov = true;
    bool aimbotPlayers = true;
    bool aimbotZombies = false;
    int aimTargetFilter = OakAimPlayers;
    bool aimbotDrawFov = true;
    bool fastBullets = false;
    bool noDispersion = false;
    bool perfectBallistics = false;
    int aimbotFov = 120;
    int aimbotSmooth = 3;
    int aimbotBone = 0;          // 0 head, 1 chest
    int aimbotKey = VK_XBUTTON1; // Mouse4 — avoids RMB ADS/unzoom fight
    int aimbotMaxDistance = 400;
    int magicBulletMaxDistance = 800;
    int magicBulletFov = 320;
    int crosshairStyle = 1;      // gap-cross
    int crosshairSize = 8;
    int crosshairGap = 4;
    int crosshairThickness = 2;

    // World extras
    bool espContainers = true;
    bool espCorpses = false;
    bool espTraps = true;
    bool fullbright = false;
    int fullbrightBrightness = 55; // mid range — 100 ≈ previous hard ~80 EyeAccom
    int containerMaxDistance = 120;
    int corpseMaxDistance = 150;
    int trapMaxDistance = 100;
    ImVec4 colorContainer = ImVec4(0.45f, 0.75f, 0.95f, 0.95f);
    ImVec4 colorCorpse = ImVec4(0.85f, 0.55f, 0.70f, 0.90f);
    ImVec4 colorTrap = ImVec4(1.00f, 0.25f, 0.25f, 0.95f);

    // Player — box + label, no bones
    bool playerBox = true;
    bool playerName = true;
    bool playerDistance = true;
    int playerMaxDistance = 400;
    ImVec4 colorPlayerBox = ImVec4(0.40f, 0.85f, 1.00f, 0.95f);
    ImVec4 colorPlayerSkeleton = ImVec4(0.50f, 0.90f, 1.00f, 0.80f);
    ImVec4 colorPlayerName = ImVec4(0.80f, 0.95f, 1.00f, 1.0f);
    ImVec4 colorPlayerChams = ImVec4(0.25f, 0.70f, 0.90f, 0.35f);

    // Zombie — markers only (names get noisy in hordes)
    bool zombieBox = true;
    bool zombieName = false;
    bool zombieDistance = true;
    int zombieMaxDistance = 150;
    ImVec4 colorZombieBox = ImVec4(0.95f, 0.40f, 0.35f, 0.90f);
    ImVec4 colorZombieSkeleton = ImVec4(0.95f, 0.55f, 0.30f, 0.75f);
    ImVec4 colorZombieName = ImVec4(0.95f, 0.60f, 0.55f, 0.95f);
    ImVec4 colorZombieChams = ImVec4(0.90f, 0.25f, 0.20f, 0.30f);

    // Animal
    bool animalBox = true;
    bool animalName = true;
    bool animalDistance = true;
    int animalMaxDistance = 150;
    ImVec4 colorAnimalBox = ImVec4(0.90f, 0.78f, 0.35f, 0.90f);
    ImVec4 colorAnimalName = ImVec4(0.95f, 0.88f, 0.55f, 0.95f);

    // Item — labels within short range
    bool itemBox = false;
    bool itemName = true;
    bool itemDistance = true;
    int itemMaxDistance = 80;
    ImVec4 colorItemBox = ImVec4(0.70f, 0.82f, 0.95f, 0.85f);
    ImVec4 colorItemName = ImVec4(0.88f, 0.92f, 0.98f, 0.95f);

    // Vehicle
    bool vehicleBox = true;
    bool vehicleName = true;
    bool vehicleDistance = true;
    int vehicleMaxDistance = 350;
    ImVec4 colorVehicleBox = ImVec4(0.65f, 0.55f, 0.95f, 0.90f);
    ImVec4 colorVehicleName = ImVec4(0.80f, 0.75f, 1.00f, 0.95f);

    // Misc
    bool miscMiddleClickDespawn = false;
    bool miscLootMagnet = false;
    bool miscContainerMagnet = false;
    bool miscDaytimeLock = false;
    bool miscDisableOverlays = false;
    bool miscSteamNames = true;
    bool miscSteamAvatars = true;
    bool miscDrawWaypoints = true;
    bool miscFreecam = false;
    bool miscFreecamMoveBody = false;
    int miscFreecamSpeed = 12;
    bool miscNoGrass = false;
    int miscLootMagnetRange = 8;
    int miscContainerMagnetRange = 60;
    int miscDespawnRange = 25;

    // Keybinds (0 = unset / empty)
    int bindEsp = 0;
    int bindAimbot = 0;
    int bindMagicBullet = 0;
    int bindFullbright = 0;
    int bindMiddleClickDespawn = VK_MBUTTON;
    int bindLootMagnet = 0;
    int bindContainerMagnet = 0;
    int bindDaytimeLock = 0;
    int bindDisableOverlays = 0;
    int bindPanic = 0;
    int bindAddWaypoint = 0;
    int bindCopyCoords = 0;
    int bindSteamNames = 0;
    int bindPullBasePart = 0;
    int bindFreecam = 'U'; // F6=wp, F7=coords, F8=QA, F9=panic — freecam on U

    OakPerfLimits perf = {};
    OakCorpseEspSettings corpseEsp = {};
    OakTrapEspSettings trapEsp = {};
    OakLootCatSettings lootCats[OAK_LOOT_CAT_COUNT] = {};
    int lootSortMode = OakLootSortDistance;
    int lootFilterBlacklistCount = 0;
    char lootFilterBlacklist[OAK_LOOT_FILTER_MAX][48] = {};
    int lootFilterWhitelistCount = 0;
    char lootFilterWhitelist[OAK_LOOT_FILTER_MAX][48] = {};
    OakEspStyleSettings playerStyle = {};
    OakEspStyleSettings zombieStyle = {};
    OakAimbotExtras aimExtras = {};
    OakUiExtras uiExtras = {};

    int waypointCount = 0;
    float waypointX[OAK_WAYPOINT_MAX] = {};
    float waypointY[OAK_WAYPOINT_MAX] = {};
    float waypointZ[OAK_WAYPOINT_MAX] = {};
    char waypointName[OAK_WAYPOINT_MAX][32] = {};
    float waypointColor[OAK_WAYPOINT_MAX][4] = {};

    OakThreatRingSettings threatRing = {};
    OakThreatCounterSettings threatCounter = {};
    OakCompassSettings compass = {};
    OakLookDirectionSettings lookDirection = {};
    OakPlayerTrailSettings playerTrail = {};
    OakDeathMarkerSettings deathMarker = {};
    OakNightBoostSettings nightBoost = {};
    OakCrosshairHighlightSettings crosshairHighlight = {};
    OakFovHighlightSettings fovHighlight = {};
    OakReloadBarSettings reloadBar = {};
    OakHeliCrashEspSettings heliCrashEsp = {};
    OakGridCoordsHudSettings gridCoordsHud = {};
    OakWaypointHudSettings waypointHud = {};
    OakStanceIconSettings stanceIcon = {};
    OakShadowChamsSettings shadowChams = {};

    OakBatch4EspSettings batch4Esp = {};
    OakSilentAimSettings silentAim = {};
    OakGrenadeTeleportSettings grenadeTeleport = {};
    OakTriggerbotSettings triggerbot = {};
    OakWallBypassSettings wallBypass = {};
    OakRecoilSettings recoil = {};
    OakAimbotBatch4Settings aimbotB4 = {};
    OakWorldMiscSettings worldMisc = {};
    OakExploitSettings exploits = {};
    int bindSilentAim = 0;
    int bindAimAssist = 0;

    // Settings
    int menuKey = 'K';
    bool streamProof = false;
    bool drawLocalPlayer = false;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
};

static UiState g_Ui;
static bool g_UiReady = false;
static OakPerfStats g_PerfStats = {};
static int g_ActiveTab = 0; // 0 = Visuals, 1 = Misc, 2 = Settings
static int g_VisualsSection = 0;
static int g_MiscSection = 0;
static int g_BindCaptureTarget = -1; // index into bind capture table, -1 = idle
static bool g_PanicLatch = false;
static bool g_CopyCoordsLatch = false;
static bool g_PullBasePartLatch = false;
static char g_Batch4StatusDoor[72] = "idle";
static char g_Batch4StatusWarp[72] = "idle";
static char g_RecoilStatusNote[96] = "off";
static bool g_RecoilStatusOk = false;

static ImFont* g_FontRegular = nullptr;
static ImFont* g_FontMedium = nullptr;
static ImFont* g_FontTitle = nullptr;
static ImFont* g_FontSmall = nullptr;

static bool PathExistsA(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool GetOwningModuleDir(char* out, DWORD outLen)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ImGuiMenu_Init),
            &mod))
        return false;

    char full[MAX_PATH] = {};
    if (!GetModuleFileNameA(mod, full, MAX_PATH))
        return false;

    char* slash = strrchr(full, '\\');
    if (!slash) return false;
    *slash = 0;
    if ((DWORD)lstrlenA(full) + 1 > outLen) return false;
    lstrcpyA(out, full);
    return true;
}

static bool ResolveFontFile(const char* fileName, char* out, DWORD outLen)
{
    // Manual kernel maps often have no real module path — also probe fixed stage dirs.
    char bases[8][MAX_PATH] = {};
    int nBase = 0;
    if (GetOwningModuleDir(bases[0], MAX_PATH))
        nBase = 1;
    lstrcpynA(bases[nBase++], "C:\\oak", MAX_PATH);
    lstrcpynA(bases[nBase++], "C:\\oak\\fonts", MAX_PATH);
    lstrcpynA(bases[nBase++], "C:\\oak\\dayz", MAX_PATH);
    lstrcpynA(bases[nBase++], "C:\\oak\\dayz\\fonts", MAX_PATH);
    {
        char local[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 16)
        {
            wsprintfA(bases[nBase], "%s\\DayZ", local);
            nBase++;
            wsprintfA(bases[nBase], "%s\\DayZ\\fonts", local);
            nBase++;
        }
    }

    const char* rels[] = {
        "\\fonts\\",
        "\\",
        "\\assets\\fonts\\",
        "\\..\\assets\\fonts\\",
        "\\..\\..\\assets\\fonts\\",
    };

    for (int b = 0; b < nBase; b++)
    {
        if (!bases[b][0]) continue;
        for (int i = 0; i < 5; i++)
        {
            char trial[MAX_PATH];
            wsprintfA(trial, "%s%s%s", bases[b], rels[i], fileName);
            if (PathExistsA(trial))
            {
                if (!GetFullPathNameA(trial, outLen, out, nullptr))
                    return false;
                return true;
            }
        }
    }
    return false;
}

static bool LoadUiFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    g_FontRegular = nullptr;
    g_FontMedium = nullptr;
    g_FontTitle = nullptr;
    g_FontSmall = nullptr;

    char regularPath[MAX_PATH] = {};
    char mediumPath[MAX_PATH] = {};
    char boldPath[MAX_PATH] = {};

    if (!ResolveFontFile("DMSans-Regular.ttf", regularPath, MAX_PATH) ||
        !ResolveFontFile("DMSans-Medium.ttf", mediumPath, MAX_PATH) ||
        !ResolveFontFile("DMSans-SemiBold.ttf", boldPath, MAX_PATH))
    {
        ImGuiMenu_Log("fonts: DM Sans not found — using ImGui default font");
        io.Fonts->AddFontDefault();
        g_FontRegular = io.FontDefault;
        g_FontMedium = g_FontRegular;
        g_FontSmall = g_FontRegular;
        g_FontTitle = g_FontRegular;
        return true;
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    g_FontRegular = io.Fonts->AddFontFromFileTTF(regularPath, 15.0f, &cfg);
    g_FontMedium  = io.Fonts->AddFontFromFileTTF(mediumPath, 15.0f, &cfg);
    g_FontSmall   = io.Fonts->AddFontFromFileTTF(mediumPath, 12.5f, &cfg);
    g_FontTitle   = io.Fonts->AddFontFromFileTTF(boldPath, 22.0f, &cfg);

    if (!g_FontRegular || !g_FontMedium || !g_FontTitle || !g_FontSmall)
    {
        ImGuiMenu_Log("fonts: AddFontFromFileTTF failed");
        return false;
    }

    io.FontDefault = g_FontRegular;

    char buf[320];
    wsprintfA(buf, "fonts: loaded DM Sans from %s", regularPath);
    ImGuiMenu_Log(buf);
    return true;
}

static void OakInitLootCatDefaults()
{
    static const char* kLootSec[OAK_LOOT_CAT_COUNT] = {
        "weapons", "ammo", "medical", "food", "clothing", "tools", "other"
    };
    (void)kLootSec;
    for (int i = 0; i < OAK_LOOT_CAT_COUNT; i++)
    {
        g_Ui.lootCats[i].enabled = true;
        g_Ui.lootCats[i].maxDistance = 80;
        g_Ui.lootCats[i].maxCount = 50;
        g_Ui.lootCats[i].useCustomColor = false;
        OakDefaultLootCatColors(i, &g_Ui.lootCats[i].color[0], &g_Ui.lootCats[i].color[1],
            &g_Ui.lootCats[i].color[2], &g_Ui.lootCats[i].color[3]);
    }
    g_Ui.lootCats[0].enabled = g_Ui.lootShowWeapons;
    g_Ui.lootCats[1].enabled = g_Ui.lootShowAmmo;
    g_Ui.lootCats[2].enabled = g_Ui.lootShowMedical;
    g_Ui.lootCats[3].enabled = g_Ui.lootShowFood;
    g_Ui.lootCats[4].enabled = g_Ui.lootShowClothing;
    g_Ui.lootCats[5].enabled = g_Ui.lootShowTools;
    g_Ui.lootCats[6].enabled = g_Ui.lootShowOther;
}

static void OakSyncLootShowFromCats()
{
    g_Ui.lootShowWeapons = g_Ui.lootCats[0].enabled;
    g_Ui.lootShowAmmo = g_Ui.lootCats[1].enabled;
    g_Ui.lootShowMedical = g_Ui.lootCats[2].enabled;
    g_Ui.lootShowFood = g_Ui.lootCats[3].enabled;
    g_Ui.lootShowClothing = g_Ui.lootCats[4].enabled;
    g_Ui.lootShowTools = g_Ui.lootCats[5].enabled;
    g_Ui.lootShowOther = g_Ui.lootCats[6].enabled;
}

static void OakSyncLootCatsFromShow()
{
    g_Ui.lootCats[0].enabled = g_Ui.lootShowWeapons;
    g_Ui.lootCats[1].enabled = g_Ui.lootShowAmmo;
    g_Ui.lootCats[2].enabled = g_Ui.lootShowMedical;
    g_Ui.lootCats[3].enabled = g_Ui.lootShowFood;
    g_Ui.lootCats[4].enabled = g_Ui.lootShowClothing;
    g_Ui.lootCats[5].enabled = g_Ui.lootShowTools;
    g_Ui.lootCats[6].enabled = g_Ui.lootShowOther;
}

static void ResetUiDefaults()
{
    g_Ui = UiState{};
    OakClampPerfLimits(&g_Ui.perf);
    OakSyncAimbotBoolsFromFilter(g_Ui.aimTargetFilter, &g_Ui.aimbotPlayers, &g_Ui.aimbotZombies);
    g_Ui.playerStyle.boxStyle = OakBoxCorner;
    g_Ui.zombieStyle.boxStyle = OakBoxCorner;
    g_Ui.playerStyle.colorVisible[0] = 0.40f; g_Ui.playerStyle.colorVisible[1] = 0.85f;
    g_Ui.playerStyle.colorVisible[2] = 1.00f; g_Ui.playerStyle.colorVisible[3] = 0.95f;
    g_Ui.zombieStyle.colorVisible[0] = 0.95f; g_Ui.zombieStyle.colorVisible[1] = 0.40f;
    g_Ui.zombieStyle.colorVisible[2] = 0.35f; g_Ui.zombieStyle.colorVisible[3] = 0.90f;
    OakInitLootCatDefaults();
    g_VisualsSection = 0;
    g_MiscSection = 0;
    g_BindCaptureTarget = -1;
}

static LRESULT CALLBACK ImGuiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (g_MenuOpen)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse &&
            (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONDOWN ||
             msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
             msg == WM_MOUSEWHEEL || msg == WM_MOUSEMOVE || msg == WM_MOUSEHOVER))
            return true;
        if (io.WantCaptureKeyboard &&
            (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP))
            return true;
    }

    if (g_OrigWndProc)
        return CallWindowProcW(g_OrigWndProc, hWnd, msg, wParam, lParam);
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static const char* MenuKeyName(int vk)
{
    switch (vk)
    {
    case 0:         return "None";
    case VK_INSERT: return "Insert";
    case VK_HOME:   return "Home";
    case VK_DELETE: return "Delete";
    case VK_F1:     return "F1";
    case VK_F2:     return "F2";
    case VK_F3:     return "F3";
    case VK_F4:     return "F4";
    case VK_F5:     return "F5";
    case VK_F6:     return "F6";
    case VK_F7:     return "F7";
    case VK_F8:     return "F8";
    case VK_END:    return "End";
    case VK_MBUTTON: return "Mouse 3";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_LBUTTON: return "Mouse 1";
    case VK_RBUTTON: return "Mouse 2";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_CAPITAL: return "Caps";
    case VK_TAB: return "Tab";
    case 'V': return "V";
    case 'B': return "B";
    case 'N': return "N";
    case 'M': return "M";
    case 'C': return "C";
    case 'X': return "X";
    case 'Z': return "Z";
    case 'G': return "G";
    case 'H': return "H";
    case 'J': return "J";
    case 'K': return "K";
    case 'L': return "L";
    case 'P': return "P";
    case 'O': return "O";
    case VK_OEM_3: return "`";
    default:        return "Custom";
    }
}

// Plain rebind row. The OakPanel UI draws its own chip, but this stays as the
// minimal path that arms g_BindCaptureTarget for PollBindCapture below.
static void BindPreference(const char* label, const char* desc, int* bind, int captureId)
{
    ImGui::PushID(captureId);
    ImGui::TextUnformatted(label);
    if (desc && desc[0])
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", desc);
    }
    const bool capturing = (g_BindCaptureTarget == captureId);
    char btn[48];
    if (capturing)
        lstrcpyA(btn, "Press key...");
    else
        wsprintfA(btn, "%s", MenuKeyName(*bind));
    if (ImGui::Button(btn, ImVec2(-1, 0)))
        g_BindCaptureTarget = captureId;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    {
        *bind = 0;
        if (g_BindCaptureTarget == captureId)
            g_BindCaptureTarget = -1;
    }
    ImGui::PopID();
}

static void PollBindCapture()
{
    if (g_BindCaptureTarget < 0)
        return;

    // Escape cancels
    if (GetAsyncKeyState(VK_ESCAPE) & 1)
    {
        g_BindCaptureTarget = -1;
        return;
    }

    int* targets[] = {
        &g_Ui.bindEsp, &g_Ui.bindAimbot, &g_Ui.bindMagicBullet, &g_Ui.bindFullbright,
        &g_Ui.bindMiddleClickDespawn, &g_Ui.bindLootMagnet, &g_Ui.bindContainerMagnet,
        &g_Ui.bindDaytimeLock, &g_Ui.bindDisableOverlays, &g_Ui.bindPanic,
        &g_Ui.bindAddWaypoint, &g_Ui.bindCopyCoords,         &g_Ui.bindSteamNames,
        &g_Ui.bindPullBasePart, &g_Ui.bindFreecam, &g_Ui.bindSilentAim,
        &g_Ui.exploits.warpKey
    };
    const int n = (int)(sizeof(targets) / sizeof(targets[0]));
    if (g_BindCaptureTarget >= n)
    {
        g_BindCaptureTarget = -1;
        return;
    }

    // Prefer mouse buttons first
    static const int mouseKeys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; i++)
    {
        if (GetAsyncKeyState(mouseKeys[i]) & 1)
        {
            *targets[g_BindCaptureTarget] = mouseKeys[i];
            g_BindCaptureTarget = -1;
            return;
        }
    }
    for (int vk = 8; vk <= 0xFE; vk++)
    {
        if (vk == VK_ESCAPE || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
            continue;
        if (GetAsyncKeyState(vk) & 1)
        {
            *targets[g_BindCaptureTarget] = vk;
            g_BindCaptureTarget = -1;
            return;
        }
    }
}

// OakPanel UI — ApplyStyle() and DrawMenu() live here. Kept in a separate
// fragment so the panel layout stays readable next to the state plumbing.
#include "imgui_oak_panel.inl"

// VirtualAlloc-based allocator for ImGui under BE + manual-map.
// BE hooks HeapAlloc/HeapFree on unbacked modules; VirtualAlloc is safer.
// Keep a 16-byte-aligned user pointer — ImGui/SSE paths AV on 8-byte-only align.
struct OakImGuiAllocHdr
{
    void* base;
    SIZE_T total;
};

static void* OakImGuiVirtualAlloc(size_t sz, void*)
{
    if (!sz) return nullptr;
    const SIZE_T align = 16;
    const SIZE_T hdr = (sizeof(OakImGuiAllocHdr) + align - 1) & ~(align - 1);
    SIZE_T total = hdr + sz + align;
    void* base = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!base) return nullptr;
    uintptr_t userAddr = ((uintptr_t)base + hdr + align - 1) & ~(align - 1);
    auto* h = (OakImGuiAllocHdr*)(userAddr - sizeof(OakImGuiAllocHdr));
    h->base = base;
    h->total = total;
    void* user = (void*)userAddr;
    ZeroMemory(user, sz);
    return user;
}

static void OakImGuiVirtualFree(void* p, void*)
{
    if (!p) return;
    auto* h = (OakImGuiAllocHdr*)((uintptr_t)p - sizeof(OakImGuiAllocHdr));
    VirtualFree(h->base, 0, MEM_RELEASE);
}

bool ImGuiMenu_PrecreateContext()
{
    if (ImGui::GetCurrentContext())
        return true;

    HANDLE hfPre = CreateFileA("C:\\oak\\imgui_precreate.flag", GENERIC_WRITE, FILE_SHARE_READ,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfPre != INVALID_HANDLE_VALUE) { CloseHandle(hfPre); }

    ImGui::SetAllocatorFunctions(&OakImGuiVirtualAlloc, &OakImGuiVirtualFree, nullptr);

    __try
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        char msg[64];
        wsprintfA(msg, "precreate_AV 0x%08X\r\n", GetExceptionCode());
        HANDLE hf = CreateFileA("C:\\oak\\imgui_precreate.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(hf, msg, (DWORD)lstrlenA(msg), &w, NULL); CloseHandle(hf); }
        return false;
    }

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_precreate.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) { const char m[] = "precreate_ok\r\n"; DWORD w; WriteFile(hf, m, sizeof(m)-1, &w, NULL); CloseHandle(hf); }
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    return ImGui::GetCurrentContext() != nullptr;
}

bool ImGuiMenu_Init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context)
{
    // Breadcrumb via Win32 only — proves call entered under manual map
    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", GENERIC_WRITE, FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "enter\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }
    // Avoid ImGuiMenu_Log early — its CRITICAL_SECTION path crashed under manual map
    // before any log line appeared. Use raw WriteFile breadcrumbs instead.
    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "after_flag\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }
    if (g_ImGuiReady)
        return true;
    if (!hwnd || !device || !context)
        return false;

    g_Hwnd = hwnd;
    g_Ctx = context;
    g_Device = device;

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "before_ctx\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    if (!ImGui::GetCurrentContext())
    {
        // Use VirtualAlloc-based allocator (bypasses BE heap hooks)
        ImGui::SetAllocatorFunctions(&OakImGuiVirtualAlloc, &OakImGuiVirtualFree, nullptr);
        {
            HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
                const char msg[] = "alloc_set\r\n";
                DWORD w = 0;
                WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
                CloseHandle(hf);
            }
        }
        __try
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
                char msg[64];
                wsprintfA(msg, "create_AV 0x%08X\r\n", GetExceptionCode());
                DWORD w = 0;
                WriteFile(hf, msg, (DWORD)lstrlenA(msg), &w, NULL);
                CloseHandle(hf);
            }
            return false;
        }
    }
    else
    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "ctx_premade\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "ctx_ok\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ApplyStyle();
    LoadUiFonts(); // before DX11 backend so atlas builds with these fonts

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "fonts_ok\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    if (!ImGui_ImplWin32_Init(hwnd))
        return false;
    if (!ImGui_ImplDX11_Init(device, context))
    {
        ImGui_ImplWin32_Shutdown();
        return false;
    }

    // Precompiled shaders — no d3dcompiler.dll needed

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "backends_ok\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    // NOTE: Do NOT call ImGui_ImplDX11_NewFrame/CreateDeviceObjects here if we are
    // still inside IDXGISwapChain::Present — D3DCompile during Present kills DayZ.
    // Warmup happens on a deferred timer from hkPresent after oPresent returns.

    // Do NOT subclass the game WndProc under manual-map — CFG blocks calls into
    // unbacked RX and kills DayZ on the next message. Menu toggle uses GetAsyncKeyState.
    g_OrigWndProc = nullptr;
    g_ImGuiReady = true;
    g_ImGuiWarmed = false;
    g_MenuOpen = false;
    g_PresentFrames = 0;
    g_WasDrawn = false;
    ResetUiDefaults();
    g_UiReady = true;
    ImGuiMenu_LoadConfig();
    {
        char vbuf[96];
        wsprintfA(vbuf, "Oak %s config loaded", ImGuiMenu_Version());
        ImGuiMenu_Log(vbuf);
    }

    {
        HANDLE hf = CreateFileA("C:\\oak\\imgui_init.flag", FILE_APPEND_DATA, FILE_SHARE_READ,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "ok\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }
    return true;
}

bool ImGuiMenu_IsWarmed()
{
    return g_ImGuiWarmed;
}

ID3D11Device* ImGuiMenu_GetD3DDevice()
{
    return g_Device;
}

ID3D11DeviceContext* ImGuiMenu_GetD3DContext()
{
    return g_Ctx;
}

bool ImGuiMenu_Warmup()
{
    if (!g_ImGuiReady)
        return false;
    if (g_ImGuiWarmed)
        return true;

    // CreateDeviceObjects via NewFrame — uses precompiled shaders (no D3DCompile).
    // Call from the Present thread only (D3D11 immediate context is not free-threaded).
    ImGuiMenu_Log("ImGuiMenu_Warmup: CreateDeviceObjects...");
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::EndFrame();
    g_ImGuiWarmed = true;
    g_MenuOpen = false; // start closed — press K; auto-open + raw-input detach was unstable under BE
    OakSetMenuInputActive(false);
    ImGuiMenu_Log("ImGuiMenu_Warmup: OK (menu closed, toggle=K)");
    return true;
}

static void OakForceDeletedModulesOff();

void ImGuiMenu_GetEspSettings(ImGuiEspSettings* out)
{
    if (!out) return;
    out->ready = g_UiReady && g_ImGuiReady;
    OakSyncLootCatsFromShow();
    out->espEnabled = g_Ui.espEnabled;
    out->espPlayers = g_Ui.espPlayers;
    out->espZombies = g_Ui.espZombies;
    out->espAnimals = g_Ui.espAnimals;
    out->espItems = g_Ui.espItems;
    out->lootShowWeapons = g_Ui.lootShowWeapons;
    out->lootShowAmmo = g_Ui.lootShowAmmo;
    out->lootShowMedical = g_Ui.lootShowMedical;
    out->lootShowFood = g_Ui.lootShowFood;
    out->lootShowClothing = g_Ui.lootShowClothing;
    out->lootShowTools = g_Ui.lootShowTools;
    out->lootShowOther = g_Ui.lootShowOther;
    out->espVehicles = g_Ui.espVehicles;
    out->espSkeleton = g_Ui.espSkeleton;
    out->espChams = g_Ui.espChams;
    out->playerBox = g_Ui.playerBox;
    out->playerName = g_Ui.playerName;
    out->playerDistance = g_Ui.playerDistance;
    out->zombieBox = g_Ui.zombieBox;
    out->zombieName = g_Ui.zombieName;
    out->zombieDistance = g_Ui.zombieDistance;
    out->drawLocalPlayer = g_Ui.drawLocalPlayer;
    out->playerMaxDistance = g_Ui.playerMaxDistance;
    out->zombieMaxDistance = g_Ui.zombieMaxDistance;
    out->animalMaxDistance = g_Ui.animalMaxDistance;
    out->itemMaxDistance = g_Ui.itemMaxDistance;
    out->vehicleMaxDistance = g_Ui.vehicleMaxDistance;
    out->itemBox = g_Ui.itemBox;
    out->itemName = g_Ui.itemName;
    out->itemDistance = g_Ui.itemDistance;

    auto copy4 = [](float* dst, const ImVec4& c) {
        dst[0] = c.x; dst[1] = c.y; dst[2] = c.z; dst[3] = c.w;
    };
    copy4(out->colorItemBox, g_Ui.colorItemBox);
    copy4(out->colorItemName, g_Ui.colorItemName);
    copy4(out->colorPlayerBox, g_Ui.colorPlayerBox);
    copy4(out->colorPlayerSkeleton, g_Ui.colorPlayerSkeleton);
    copy4(out->colorPlayerName, g_Ui.colorPlayerName);
    copy4(out->colorPlayerChams, g_Ui.colorPlayerChams);
    copy4(out->colorZombieBox, g_Ui.colorZombieBox);
    copy4(out->colorZombieSkeleton, g_Ui.colorZombieSkeleton);
    copy4(out->colorZombieName, g_Ui.colorZombieName);
    copy4(out->colorZombieChams, g_Ui.colorZombieChams);

    out->bulletTracers = g_Ui.bulletTracers;
    out->impactMarkers = g_Ui.impactMarkers;
    out->shotIndicators = g_Ui.shotIndicators;
    out->hitMarkers = g_Ui.hitMarkers;
    out->crosshair = g_Ui.crosshair;
    out->weaponEsp = g_Ui.weaponEsp;
    out->healthBars = g_Ui.healthBars;
    out->barHealth = g_Ui.barHealth;
    out->barBlood = g_Ui.barBlood;
    out->barShock = g_Ui.barShock;
    out->barStamina = g_Ui.barStamina;
    out->barHunger = g_Ui.barHunger;
    out->barThirst = g_Ui.barThirst;
    out->localVitalsHud = g_Ui.localVitalsHud;
    out->localWeaponAmmo = g_Ui.localWeaponAmmo;
    out->friendCount = g_Ui.friendCount;
    for (int fi = 0; fi < 16; fi++)
    {
        out->friendSteamId[fi] = g_Ui.friendSteamId[fi];
        for (int c = 0; c < 64; c++)
            out->friendName[fi][c] = g_Ui.friendName[fi][c];
    }
    out->grenadeTrajectory = g_Ui.grenadeTrajectory;
    out->tracerLifetimeMs = g_Ui.tracerLifetimeMs;
    out->impactLifetimeMs = g_Ui.impactLifetimeMs;
    copy4(out->colorTracer, g_Ui.colorTracer);
    copy4(out->colorImpact, g_Ui.colorImpact);
    copy4(out->colorShotInd, g_Ui.colorShotInd);
    copy4(out->colorHitMarker, g_Ui.colorHitMarker);
    copy4(out->colorCrosshair, g_Ui.colorCrosshair);
    copy4(out->colorGrenade, g_Ui.colorGrenade);

    out->aimbotEnabled = g_Ui.aimbotEnabled;
    out->magicBullet = g_Ui.magicBullet;
    out->magicBulletAutoFire = g_Ui.magicBulletAutoFire;
    out->magicBulletChain = g_Ui.magicBulletChain;
    out->magicBulletDrawFov = g_Ui.magicBulletDrawFov;
    out->aimbotPlayers = g_Ui.aimbotPlayers;
    out->aimbotZombies = g_Ui.aimbotZombies;
    out->aimTargetFilter = g_Ui.aimTargetFilter;
    out->aimbotDrawFov = g_Ui.aimbotDrawFov;
    out->fastBullets = g_Ui.fastBullets;
    out->noDispersion = g_Ui.noDispersion;
    out->perfectBallistics = g_Ui.perfectBallistics;
    out->aimbotFov = g_Ui.aimbotFov;
    out->aimbotSmooth = g_Ui.aimbotSmooth;
    out->aimbotBone = g_Ui.aimbotBone;
    out->aimbotKey = g_Ui.aimbotKey;
    out->aimbotMaxDistance = g_Ui.aimbotMaxDistance;
    out->magicBulletMaxDistance = g_Ui.magicBulletMaxDistance;
    out->magicBulletFov = g_Ui.magicBulletFov;
    out->crosshairStyle = g_Ui.crosshairStyle;
    out->crosshairSize = g_Ui.crosshairSize;
    out->crosshairGap = g_Ui.crosshairGap;
    out->crosshairThickness = g_Ui.crosshairThickness;

    out->espContainers = g_Ui.espContainers;
    out->espCorpses = g_Ui.espCorpses;
    out->espTraps = g_Ui.espTraps;
    out->fullbright = g_Ui.fullbright;
    out->fullbrightBrightness = g_Ui.fullbrightBrightness;
    out->containerMaxDistance = g_Ui.containerMaxDistance;
    out->corpseMaxDistance = g_Ui.corpseMaxDistance;
    out->trapMaxDistance = g_Ui.trapMaxDistance;
    copy4(out->colorContainer, g_Ui.colorContainer);
    copy4(out->colorCorpse, g_Ui.colorCorpse);
    copy4(out->colorTrap, g_Ui.colorTrap);

    out->miscMiddleClickDespawn = g_Ui.miscMiddleClickDespawn;
    out->miscLootMagnet = g_Ui.miscLootMagnet;
    out->miscContainerMagnet = g_Ui.miscContainerMagnet;
    out->miscDaytimeLock = g_Ui.miscDaytimeLock;
    out->miscDisableOverlays = g_Ui.miscDisableOverlays;
    out->miscSteamNames = g_Ui.miscSteamNames;
    out->miscSteamAvatars = g_Ui.miscSteamAvatars;
    out->miscDrawWaypoints = g_Ui.miscDrawWaypoints;
    out->miscFreecam = g_Ui.miscFreecam;
    out->miscFreecamMoveBody = g_Ui.miscFreecamMoveBody;
    out->miscFreecamSpeed = g_Ui.miscFreecamSpeed;
    out->miscNoGrass = g_Ui.miscNoGrass;
    out->miscLootMagnetRange = g_Ui.miscLootMagnetRange;
    out->miscContainerMagnetRange = g_Ui.miscContainerMagnetRange;
    out->miscDespawnRange = g_Ui.miscDespawnRange;

    out->bindEsp = g_Ui.bindEsp;
    out->bindAimbot = g_Ui.bindAimbot;
    out->bindMagicBullet = g_Ui.bindMagicBullet;
    out->bindFullbright = g_Ui.bindFullbright;
    out->bindMiddleClickDespawn = g_Ui.bindMiddleClickDespawn;
    out->bindLootMagnet = g_Ui.bindLootMagnet;
    out->bindContainerMagnet = g_Ui.bindContainerMagnet;
    out->bindDaytimeLock = g_Ui.bindDaytimeLock;
    out->bindDisableOverlays = g_Ui.bindDisableOverlays;
    out->bindPanic = g_Ui.bindPanic;
    out->bindAddWaypoint = g_Ui.bindAddWaypoint;
    out->bindCopyCoords = g_Ui.bindCopyCoords;
    out->bindSteamNames = g_Ui.bindSteamNames;
    out->bindPullBasePart = g_Ui.bindPullBasePart;
    out->bindFreecam = g_Ui.bindFreecam;

    out->perf = g_Ui.perf;
    out->corpseEsp = g_Ui.corpseEsp;
    out->trapEsp = g_Ui.trapEsp;
    for (int i = 0; i < OAK_LOOT_CAT_COUNT; i++)
        out->lootCats[i] = g_Ui.lootCats[i];
    out->lootSortMode = g_Ui.lootSortMode;
    out->lootFilterBlacklistCount = g_Ui.lootFilterBlacklistCount;
    out->lootFilterWhitelistCount = g_Ui.lootFilterWhitelistCount;
    if (out->lootFilterBlacklistCount < 0) out->lootFilterBlacklistCount = 0;
    if (out->lootFilterBlacklistCount > OAK_LOOT_FILTER_MAX) out->lootFilterBlacklistCount = OAK_LOOT_FILTER_MAX;
    if (out->lootFilterWhitelistCount < 0) out->lootFilterWhitelistCount = 0;
    if (out->lootFilterWhitelistCount > OAK_LOOT_FILTER_MAX) out->lootFilterWhitelistCount = OAK_LOOT_FILTER_MAX;
    for (int i = 0; i < OAK_LOOT_FILTER_MAX; i++)
    {
        for (int c = 0; c < 48; c++)
        {
            out->lootFilterBlacklist[i][c] = g_Ui.lootFilterBlacklist[i][c];
            out->lootFilterWhitelist[i][c] = g_Ui.lootFilterWhitelist[i][c];
        }
    }
    out->playerStyle = g_Ui.playerStyle;
    out->zombieStyle = g_Ui.zombieStyle;
    out->aimExtras = g_Ui.aimExtras;
    out->uiExtras = g_Ui.uiExtras;

    out->waypointCount = g_Ui.waypointCount;
    if (out->waypointCount < 0) out->waypointCount = 0;
    if (out->waypointCount > OAK_WAYPOINT_MAX) out->waypointCount = OAK_WAYPOINT_MAX;
    for (int i = 0; i < OAK_WAYPOINT_MAX; i++)
    {
        out->waypointX[i] = g_Ui.waypointX[i];
        out->waypointY[i] = g_Ui.waypointY[i];
        out->waypointZ[i] = g_Ui.waypointZ[i];
        for (int c = 0; c < 32; c++)
            out->waypointName[i][c] = g_Ui.waypointName[i][c];
        for (int c = 0; c < 4; c++)
            out->waypointColor[i][c] = g_Ui.waypointColor[i][c];
    }

    out->threatRing = g_Ui.threatRing;
    out->threatCounter = g_Ui.threatCounter;
    out->compass = g_Ui.compass;
    out->lookDirection = g_Ui.lookDirection;
    out->playerTrail = g_Ui.playerTrail;
    out->deathMarker = g_Ui.deathMarker;
    out->nightBoost = g_Ui.nightBoost;
    out->crosshairHighlight = g_Ui.crosshairHighlight;
    out->fovHighlight = g_Ui.fovHighlight;
    out->reloadBar = g_Ui.reloadBar;
    out->heliCrashEsp = g_Ui.heliCrashEsp;
    out->gridCoordsHud = g_Ui.gridCoordsHud;
    out->waypointHud = g_Ui.waypointHud;
    out->stanceIcon = g_Ui.stanceIcon;
    out->shadowChams = g_Ui.shadowChams;
    out->batch4Esp = g_Ui.batch4Esp;
    out->silentAim = g_Ui.silentAim;
    out->grenadeTeleport = g_Ui.grenadeTeleport;
    out->triggerbot = g_Ui.triggerbot;
    OakForceDeletedModulesOff();
    out->wallBypass = g_Ui.wallBypass;
    out->recoil = g_Ui.recoil;
    out->aimbotB4 = g_Ui.aimbotB4;
    out->worldMisc = g_Ui.worldMisc;
    out->exploits = g_Ui.exploits;
    out->streamProof = g_Ui.streamProof;
    out->bindSilentAim = g_Ui.bindSilentAim;
    out->bindAimAssist = g_Ui.bindAimAssist;
}

void ImGuiMenu_AddWaypointHere(float x, float y, float z, const char* name)
{
    if (g_Ui.waypointCount >= OAK_WAYPOINT_MAX) return;
    int i = g_Ui.waypointCount++;
    g_Ui.waypointX[i] = x;
    g_Ui.waypointY[i] = y;
    g_Ui.waypointZ[i] = z;
    if (name && name[0])
    {
        for (int c = 0; c < 31; c++)
        {
            g_Ui.waypointName[i][c] = name[c];
            if (!name[c]) break;
        }
        g_Ui.waypointName[i][31] = 0;
    }
    else
        wsprintfA(g_Ui.waypointName[i], "WP%d", i + 1);
    static const float kWpPalette[][4] = {
        { 1.00f, 0.77f, 0.28f, 0.95f },
        { 0.40f, 0.85f, 1.00f, 0.95f },
        { 0.95f, 0.40f, 0.55f, 0.95f },
        { 0.55f, 0.95f, 0.45f, 0.95f },
        { 0.75f, 0.55f, 1.00f, 0.95f },
        { 1.00f, 0.50f, 0.20f, 0.95f },
    };
    int pi = i % 6;
    for (int c = 0; c < 4; c++)
        g_Ui.waypointColor[i][c] = kWpPalette[pi][c];
}

void ImGuiMenu_ClearWaypoints()
{
    g_Ui.waypointCount = 0;
}

bool ImGuiMenu_ConsumePanicLatch()
{
    bool v = g_PanicLatch;
    g_PanicLatch = false;
    return v;
}

void ImGuiMenu_RequestCopyCoords()
{
    g_CopyCoordsLatch = true;
}

bool ImGuiMenu_ConsumeCopyCoords()
{
    bool v = g_CopyCoordsLatch;
    g_CopyCoordsLatch = false;
    return v;
}

void ImGuiMenu_RequestPullBasePart()
{
    g_PullBasePartLatch = true;
}

bool ImGuiMenu_ConsumePullBasePart()
{
    bool v = g_PullBasePartLatch;
    g_PullBasePartLatch = false;
    return v;
}

void ImGuiMenu_PushMiscFlags(
    bool despawn, bool magnet, bool containerMagnet, bool daytime, bool overlays,
    bool steamNames, bool steamAvatars, bool drawWp, bool freecam,
    bool esp, bool aimbot, bool magic, bool fullbright)
{
    g_Ui.miscMiddleClickDespawn = despawn;
    g_Ui.miscLootMagnet = magnet;
    g_Ui.miscContainerMagnet = containerMagnet;
    g_Ui.miscDaytimeLock = daytime;
    g_Ui.miscDisableOverlays = overlays;
    g_Ui.miscSteamNames = steamNames;
    g_Ui.miscSteamAvatars = steamAvatars;
    g_Ui.miscDrawWaypoints = drawWp;
    g_Ui.miscFreecam = freecam;
    g_Ui.espEnabled = esp;
    g_Ui.aimbotEnabled = aimbot;
    g_Ui.magicBullet = magic;
    g_Ui.fullbright = fullbright;
}

void ImGuiMenu_PushAmmoFlags(bool fastBullets, bool noDispersion, bool perfectBallistics)
{
    g_Ui.fastBullets = fastBullets;
    g_Ui.noDispersion = noDispersion;
    g_Ui.perfectBallistics = perfectBallistics;
}

void ImGuiMenu_PushDualQa(bool thirdPerson, bool noRecoil, bool healthBars, bool freecam)
{
    g_Ui.espEnabled = true;
    g_Ui.espPlayers = true;
    g_Ui.healthBars = healthBars;
    g_Ui.barHealth = true;
    g_Ui.localVitalsHud = true;
    g_Ui.miscSteamNames = true;
    g_Ui.worldMisc.thirdPerson = thirdPerson;
    g_Ui.recoil.noRecoil = noRecoil;
    g_Ui.recoil.noSway = noRecoil;
    if (noRecoil)
    {
        g_Ui.recoil.recoilPct = 0;
        g_Ui.recoil.swayPct = 0;
    }
    g_Ui.miscFreecam = freecam;
}

static void OakCrashMatrixBaselineUi()
{
    // Safe floor: light ESP only, no combat/world writes.
    g_Ui.espEnabled = true;
    g_Ui.espPlayers = true;
    g_Ui.espZombies = true;
    g_Ui.espAnimals = false;
    g_Ui.espItems = false;
    g_Ui.espVehicles = false;
    g_Ui.espSkeleton = false;
    g_Ui.espChams = false;
    g_Ui.playerBox = true;
    g_Ui.zombieBox = true;
    g_Ui.playerName = false;
    g_Ui.zombieName = false;
    g_Ui.healthBars = false;
    g_Ui.localVitalsHud = false;
    g_Ui.playerStyle.useVisibilityColors = false;
    g_Ui.zombieStyle.useVisibilityColors = false;
    g_Ui.aimbotEnabled = false;
    g_Ui.magicBullet = false;
    g_Ui.silentAim.enabled = false;
    g_Ui.fastBullets = false;
    g_Ui.noDispersion = false;
    g_Ui.perfectBallistics = false;
    g_Ui.recoil.noRecoil = false;
    g_Ui.recoil.noSway = false;
    g_Ui.recoil.recoilPct = 100;
    g_Ui.recoil.swayPct = 100;
    g_Ui.worldMisc = OakWorldMiscSettings{};
    g_Ui.exploits = OakExploitSettings{};
    g_Ui.miscFreecam = false;
    g_Ui.fullbright = false;
    g_Ui.miscDaytimeLock = false;
    g_Ui.miscLootMagnet = false;
    g_Ui.miscContainerMagnet = false;
    g_Ui.miscMiddleClickDespawn = false;
    g_Ui.grenadeTeleport.enabled = false;
    g_Ui.bulletTracers = false;
    g_Ui.impactMarkers = false;
    g_Ui.shotIndicators = false;
    g_Ui.hitMarkers = false;
    g_Ui.crosshair = false;
    g_Ui.grenadeTrajectory = false;
    g_Ui.perf.frameBoost = false;
    g_Ui.perf.unlockPresent = false;
    g_Ui.batch4Esp.lookingAtMe.enabled = false;
    g_Ui.batch4Esp.playerInvViewer.enabled = false;
    g_Ui.miscSteamNames = false;
    g_Ui.miscSteamAvatars = false;
}

bool ImGuiMenu_CrashMatrixActive()
{
    return GetFileAttributesA("C:\\oak\\dayz\\oak_crash_matrix.flag") != INVALID_FILE_ATTRIBUTES;
}

bool ImGuiMenu_ApplyCrashMatrixStep(const char* stepId)
{
    if (!stepId || !stepId[0])
        return false;

    OakCrashMatrixBaselineUi();

    auto eq = [&](const char* a) -> bool {
        return _stricmp(stepId, a) == 0;
    };

    if (eq("baseline") || eq("idle") || eq("done"))
        return true;
    if (eq("esp_off"))
    {
        g_Ui.espEnabled = false;
        return true;
    }

    if (eq("esp_players_full"))
    {
        g_Ui.espPlayers = true;
        g_Ui.espSkeleton = true;
        g_Ui.espChams = true;
        g_Ui.playerName = true;
        g_Ui.playerDistance = true;
        g_Ui.healthBars = true;
        g_Ui.barHealth = true;
        g_Ui.barBlood = true;
        g_Ui.barShock = true;
        g_Ui.localVitalsHud = true;
        g_Ui.miscSteamNames = true;
        g_Ui.playerStyle.useVisibilityColors = true;
        return true;
    }
    if (eq("esp_zombies_full"))
    {
        g_Ui.espZombies = true;
        g_Ui.espSkeleton = true;
        g_Ui.espChams = true;
        g_Ui.zombieName = true;
        g_Ui.zombieDistance = true;
        g_Ui.healthBars = true;
        g_Ui.zombieStyle.useVisibilityColors = true;
        return true;
    }
    if (eq("esp_items"))
    {
        g_Ui.espItems = true;
        g_Ui.lootShowWeapons = true;
        g_Ui.lootShowAmmo = true;
        g_Ui.lootShowMedical = true;
        g_Ui.lootShowFood = true;
        g_Ui.lootShowClothing = true;
        g_Ui.lootShowTools = true;
        g_Ui.lootShowOther = true;
        return true;
    }
    if (eq("esp_world"))
    {
        g_Ui.espVehicles = true;
        g_Ui.espAnimals = true;
        g_Ui.espContainers = true;
        g_Ui.espCorpses = true;
        g_Ui.espTraps = true;
        return true;
    }
    if (eq("ammo_fast")) { g_Ui.fastBullets = true; return true; }
    if (eq("ammo_nodisp")) { g_Ui.noDispersion = true; return true; }
    if (eq("ammo_perfect")) { g_Ui.perfectBallistics = true; return true; }
    if (eq("ammo_all"))
    {
        g_Ui.fastBullets = true;
        g_Ui.noDispersion = true;
        g_Ui.perfectBallistics = true;
        return true;
    }
    if (eq("recoil"))
    {
        g_Ui.recoil.noRecoil = true;
        g_Ui.recoil.noSway = true;
        g_Ui.recoil.recoilPct = 0;
        g_Ui.recoil.swayPct = 0;
        return true;
    }
    if (eq("fov"))
    {
        g_Ui.worldMisc.fovChanger = true;
        g_Ui.worldMisc.horizontalFov = 110.f;
        g_Ui.worldMisc.fovKeepWhileAds = true;
        return true;
    }
    if (eq("thirdperson"))
    {
        g_Ui.worldMisc.thirdPerson = true;
        return true;
    }
    if (eq("streamproof"))
    {
        g_Ui.worldMisc.streamProof = true;
        return true;
    }
    if (eq("freecam"))
    {
        g_Ui.miscFreecam = true;
        return true;
    }
    if (eq("fullbright"))
    {
        g_Ui.fullbright = true;
        g_Ui.fullbrightBrightness = 70;
        return true;
    }
    if (eq("weather_time"))
    {
        g_Ui.worldMisc.clearWeather = true;
        g_Ui.worldMisc.timeLock = true;
        g_Ui.worldMisc.lockHour = 12.f;
        return true;
    }
    if (eq("aimbot"))
    {
        g_Ui.aimbotEnabled = true;
        g_Ui.aimbotDrawFov = true;
        g_Ui.aimbotPlayers = true;
        g_Ui.aimbotZombies = true;
        return true;
    }
    if (eq("magic_bullet"))
    {
        g_Ui.magicBullet = true;
        return true;
    }
    if (eq("silent_aim"))
    {
        g_Ui.silentAim.enabled = true;
        return true;
    }
    if (eq("combat_visuals"))
    {
        g_Ui.bulletTracers = true;
        g_Ui.impactMarkers = true;
        g_Ui.shotIndicators = true;
        g_Ui.hitMarkers = true;
        g_Ui.crosshair = true;
        g_Ui.grenadeTrajectory = true;
        return true;
    }
    if (eq("lag_switch"))
    {
        // Enable module; hold is key-driven — still exercises install/hooks.
        g_Ui.exploits.warp = true;
        return true;
    }
    if (eq("door_unlock"))
    {
        g_Ui.exploits.doorUnlock = true;
        return true;
    }
    if (eq("loot_magnet"))
    {
        g_Ui.miscLootMagnet = true;
        g_Ui.miscContainerMagnet = true;
        return true;
    }
    if (eq("grenade_tp"))
    {
        g_Ui.grenadeTeleport.enabled = true;
        return true;
    }
    if (eq("frame_boost"))
    {
        g_Ui.perf.frameBoost = true;
        g_Ui.perf.unlockPresent = true;
        g_Ui.perf.highPerfMode = true;
        return true;
    }
    if (eq("looking_inv"))
    {
        g_Ui.batch4Esp.lookingAtMe.enabled = true;
        g_Ui.batch4Esp.lookingAtMe.players = true;
        g_Ui.batch4Esp.playerInvViewer.enabled = true;
        return true;
    }
    if (eq("combo_esp"))
    {
        ImGuiMenu_ApplyCrashMatrixStep("esp_players_full");
        g_Ui.espZombies = true;
        g_Ui.espSkeleton = true;
        g_Ui.espItems = true;
        g_Ui.espVehicles = true;
        g_Ui.espAnimals = true;
        g_Ui.espContainers = true;
        g_Ui.espCorpses = true;
        g_Ui.espTraps = true;
        return true;
    }
    if (eq("combo_combat"))
    {
        ImGuiMenu_ApplyCrashMatrixStep("ammo_all");
        g_Ui.recoil.noRecoil = true;
        g_Ui.recoil.noSway = true;
        g_Ui.recoil.recoilPct = 0;
        g_Ui.recoil.swayPct = 0;
        g_Ui.aimbotEnabled = true;
        g_Ui.aimbotDrawFov = true;
        g_Ui.magicBullet = true;
        g_Ui.bulletTracers = true;
        g_Ui.crosshair = true;
        return true;
    }
    if (eq("combo_world"))
    {
        g_Ui.worldMisc.fovChanger = true;
        g_Ui.worldMisc.horizontalFov = 110.f;
        g_Ui.worldMisc.thirdPerson = true;
        g_Ui.worldMisc.streamProof = true;
        g_Ui.worldMisc.clearWeather = true;
        g_Ui.worldMisc.timeLock = true;
        g_Ui.fullbright = true;
        return true;
    }
    if (eq("combo_kitchen_sink"))
    {
        ImGuiMenu_ApplyCrashMatrixStep("combo_esp");
        g_Ui.fastBullets = true;
        g_Ui.noDispersion = true;
        g_Ui.perfectBallistics = true;
        g_Ui.recoil.noRecoil = true;
        g_Ui.recoil.noSway = true;
        g_Ui.recoil.recoilPct = 0;
        g_Ui.worldMisc.fovChanger = true;
        g_Ui.worldMisc.horizontalFov = 110.f;
        g_Ui.worldMisc.thirdPerson = true;
        g_Ui.worldMisc.streamProof = true;
        g_Ui.aimbotEnabled = true;
        g_Ui.magicBullet = true;
        g_Ui.exploits.doorUnlock = true;
        g_Ui.bulletTracers = true;
        g_Ui.crosshair = true;
        g_Ui.perf.frameBoost = true;
        return true;
    }
    if (eq("thrash_esp"))
    {
        // Outer runner flips this mid-step; first arm is ON.
        g_Ui.espEnabled = true;
        g_Ui.espPlayers = true;
        g_Ui.espZombies = true;
        g_Ui.espSkeleton = true;
        g_Ui.espItems = true;
        return true;
    }

    return false;
}

const char* ImGuiMenu_Version()
{
    return "4.0.0-batch4";
}

void ImGuiMenu_SetPerfStats(const OakPerfStats* stats)
{
    if (stats)
        g_PerfStats = *stats;
}

void ImGuiMenu_GetPerfStats(OakPerfStats* out)
{
    if (!out) return;
    *out = g_PerfStats;
}

static void OakForceDeletedModulesOff()
{
    // Deleted: impossible / server-stub modules (never re-enable).
    g_Ui.wallBypass = {};
    g_Ui.worldMisc.speedHack = false;
    g_Ui.worldMisc.speedHackKey = 0;
    g_Ui.worldMisc.infiniteStamina = false;
    g_Ui.exploits.noclip = false;
    g_Ui.exploits.lootThroughWalls = false;
    g_Ui.exploits.lootLivingPlayers = false;
    g_Ui.exploits.mugPlayer = false;
    g_Ui.exploits.itemDupe = false;
}

static const char* OakProfileName(int id)
{
    switch (id)
    {
    case 0: return "legit";
    case 1: return "rage";
    case 2: return "pve";
    default: return "legit";
    }
}

static void OakProfilePath(char* out, DWORD outLen, int id)
{
    if (!out || outLen < 40) return;
    out[0] = 0;
    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 48) return;
    char dir[MAX_PATH];
    wsprintfA(dir, "%s\\DayZ", base);
    CreateDirectoryA(dir, nullptr);
    wsprintfA(out, "%s\\DayZ\\oak_profile_%s.ini", base, OakProfileName(id));
}

void ImGuiMenu_ApplyProfile(int profileId)
{
    // Built-in defaults — then overlay saved profile file if present
    g_Ui.espEnabled = true;
    g_Ui.espPlayers = true;
    g_Ui.espZombies = true;
    g_Ui.espAnimals = true;
    g_Ui.espItems = true;
    g_Ui.espVehicles = true;
    g_Ui.espSkeleton = true;
    g_Ui.espChams = false;
    g_Ui.healthBars = true;
    g_Ui.barHealth = true;
    g_Ui.barBlood = true;
    g_Ui.barShock = true;
    g_Ui.barStamina = true;
    g_Ui.barHunger = true;
    g_Ui.barThirst = true;
    g_Ui.localVitalsHud = true;
    g_Ui.localWeaponAmmo = true;
    g_Ui.weaponEsp = true;
    g_Ui.espContainers = true;
    g_Ui.espCorpses = false;
    g_Ui.espTraps = true;
    g_Ui.crosshair = true;
    g_Ui.fullbright = false;
    g_Ui.miscFreecam = false;

    if (profileId == 0) // Legit
    {
        g_Ui.aimbotEnabled = true;
        g_Ui.magicBullet = false;
        g_Ui.aimbotPlayers = true;
        g_Ui.aimbotZombies = false;
        g_Ui.aimbotDrawFov = true;
        g_Ui.aimbotFov = 80;
        g_Ui.aimbotSmooth = 12;
        g_Ui.aimbotBone = 0;
        g_Ui.aimbotMaxDistance = 250;
        g_Ui.magicBulletMaxDistance = 400;
        g_Ui.fastBullets = false;
        g_Ui.noDispersion = false;
        g_Ui.perfectBallistics = false;
        g_Ui.bulletTracers = false;
        g_Ui.impactMarkers = false;
        g_Ui.shotIndicators = false;
        g_Ui.hitMarkers = true;
        g_Ui.grenadeTrajectory = false;
        g_Ui.playerMaxDistance = 350;
        g_Ui.zombieMaxDistance = 120;
        g_Ui.itemMaxDistance = 60;
        g_Ui.espItems = false;
        g_Ui.crosshairStyle = 1;
        g_Ui.aimTargetFilter = OakAimPlayers;
    }
    else if (profileId == 1) // Rage
    {
        g_Ui.aimbotEnabled = true;
        g_Ui.magicBullet = true;
        g_Ui.aimbotPlayers = true;
        g_Ui.aimbotZombies = true;
        g_Ui.aimbotDrawFov = true;
        g_Ui.aimbotFov = 400;
        g_Ui.aimbotSmooth = 2;
        g_Ui.aimbotBone = 0;
        g_Ui.aimbotMaxDistance = 800;
        g_Ui.magicBulletMaxDistance = 1200;
        g_Ui.fastBullets = true;
        g_Ui.noDispersion = true;
        g_Ui.perfectBallistics = true;
        g_Ui.bulletTracers = true;
        g_Ui.impactMarkers = true;
        g_Ui.shotIndicators = true;
        g_Ui.hitMarkers = true;
        g_Ui.grenadeTrajectory = true;
        g_Ui.playerMaxDistance = 800;
        g_Ui.zombieMaxDistance = 300;
        g_Ui.itemMaxDistance = 120;
        g_Ui.espItems = true;
        g_Ui.espChams = true;
        g_Ui.fullbright = true;
        g_Ui.crosshairStyle = 0;
        g_Ui.aimTargetFilter = OakAimBoth;
    }
    else // PvE
    {
        g_Ui.aimbotEnabled = true;
        g_Ui.magicBullet = false;
        g_Ui.aimbotPlayers = false;
        g_Ui.aimbotZombies = true;
        g_Ui.aimbotDrawFov = true;
        g_Ui.aimbotFov = 160;
        g_Ui.aimbotSmooth = 6;
        g_Ui.aimbotBone = 1;
        g_Ui.aimbotMaxDistance = 200;
        g_Ui.magicBulletMaxDistance = 400;
        g_Ui.fastBullets = false;
        g_Ui.noDispersion = true;
        g_Ui.perfectBallistics = false;
        g_Ui.bulletTracers = false;
        g_Ui.impactMarkers = false;
        g_Ui.shotIndicators = false;
        g_Ui.hitMarkers = true;
        g_Ui.grenadeTrajectory = true;
        g_Ui.espPlayers = true;
        g_Ui.espZombies = true;
        g_Ui.espAnimals = true;
        g_Ui.espItems = true;
        g_Ui.espVehicles = true;
        g_Ui.playerMaxDistance = 400;
        g_Ui.zombieMaxDistance = 200;
        g_Ui.animalMaxDistance = 200;
        g_Ui.itemMaxDistance = 100;
        g_Ui.crosshairStyle = 2;
        g_Ui.aimTargetFilter = OakAimZombies;
    }

    OakSyncAimbotBoolsFromFilter(g_Ui.aimTargetFilter, &g_Ui.aimbotPlayers, &g_Ui.aimbotZombies);

    char path[MAX_PATH] = {};
    OakProfilePath(path, MAX_PATH, profileId);
    if (path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
    {
        // Soft-load overrides from saved profile without replacing oak_config.ini
        auto B = [&](const char* sec, const char* key, bool* v) {
            *v = GetPrivateProfileIntA(sec, key, *v ? 1 : 0, path) != 0;
        };
        auto I = [&](const char* sec, const char* key, int* v, int lo, int hi) {
            int x = GetPrivateProfileIntA(sec, key, *v, path);
            if (x < lo) x = lo;
            if (x > hi) x = hi;
            *v = x;
        };
        B("esp", "enabled", &g_Ui.espEnabled);
        B("esp", "players", &g_Ui.espPlayers);
        B("esp", "zombies", &g_Ui.espZombies);
        B("esp", "animals", &g_Ui.espAnimals);
        B("esp", "items", &g_Ui.espItems);
        B("combat", "aimbot", &g_Ui.aimbotEnabled);
        B("combat", "magic", &g_Ui.magicBullet);
        B("combat", "mbAutoFire", &g_Ui.magicBulletAutoFire);
        B("combat", "mbChain", &g_Ui.magicBulletChain);
        B("combat", "aimPlayers", &g_Ui.aimbotPlayers);
        B("combat", "aimZombies", &g_Ui.aimbotZombies);
        I("combat", "fov", &g_Ui.aimbotFov, 20, 800);
        I("combat", "smooth", &g_Ui.aimbotSmooth, 1, 30);
        I("combat", "aimDist", &g_Ui.aimbotMaxDistance, 50, 1500);
        I("combat", "mbDist", &g_Ui.magicBulletMaxDistance, 100, 2000);
        B("combat", "tracers", &g_Ui.bulletTracers);
        B("combat", "hitMarkers", &g_Ui.hitMarkers);
        B("combat", "grenade", &g_Ui.grenadeTrajectory);
        B("combat", "fastBullets", &g_Ui.fastBullets);
        B("combat", "noDisp", &g_Ui.noDispersion);
    }

    char msg[96];
    wsprintfA(msg, "profile: applied %s", OakProfileName(profileId));
    ImGuiMenu_Log(msg);
}

static void OakWriteInt(const char* path, const char* sec, const char* key, int v); // defined below

void ImGuiMenu_SaveProfile(int profileId)
{
    char path[MAX_PATH] = {};
    OakProfilePath(path, MAX_PATH, profileId);
    if (!path[0]) return;

    auto B = [&](const char* sec, const char* key, bool v) { OakWriteInt(path, sec, key, v ? 1 : 0); };
    auto I = [&](const char* sec, const char* key, int v) { OakWriteInt(path, sec, key, v); };

    B("esp", "enabled", g_Ui.espEnabled);
    B("esp", "players", g_Ui.espPlayers);
    B("esp", "zombies", g_Ui.espZombies);
    B("esp", "animals", g_Ui.espAnimals);
    B("esp", "items", g_Ui.espItems);
    B("esp", "vehicles", g_Ui.espVehicles);
    B("esp", "skeleton", g_Ui.espSkeleton);
    B("esp", "chams", g_Ui.espChams);
    B("esp", "healthBars", g_Ui.healthBars);
    I("esp", "playerMax", g_Ui.playerMaxDistance);
    I("esp", "zombieMax", g_Ui.zombieMaxDistance);
    I("esp", "itemMax", g_Ui.itemMaxDistance);

    B("combat", "aimbot", g_Ui.aimbotEnabled);
    B("combat", "magic", g_Ui.magicBullet);
    B("combat", "mbAutoFire", g_Ui.magicBulletAutoFire);
    B("combat", "mbChain", g_Ui.magicBulletChain);
    B("combat", "mbDrawFov", g_Ui.magicBulletDrawFov);
    B("combat", "aimPlayers", g_Ui.aimbotPlayers);
    B("combat", "aimZombies", g_Ui.aimbotZombies);
    B("combat", "drawFov", g_Ui.aimbotDrawFov);
    B("combat", "fastBullets", g_Ui.fastBullets);
    B("combat", "noDisp", g_Ui.noDispersion);
    B("combat", "perfectBal", g_Ui.perfectBallistics);
    B("combat", "tracers", g_Ui.bulletTracers);
    B("combat", "impacts", g_Ui.impactMarkers);
    B("combat", "shotInd", g_Ui.shotIndicators);
    B("combat", "hitMarkers", g_Ui.hitMarkers);
    B("combat", "crosshair", g_Ui.crosshair);
    B("combat", "grenade", g_Ui.grenadeTrajectory);
    I("combat", "fov", g_Ui.aimbotFov);
    I("combat", "smooth", g_Ui.aimbotSmooth);
    I("combat", "bone", g_Ui.aimbotBone);
    I("combat", "aimDist", g_Ui.aimbotMaxDistance);
    I("combat", "mbDist", g_Ui.magicBulletMaxDistance);
    I("combat", "mbFov", g_Ui.magicBulletFov);
    I("combat", "xhStyle", g_Ui.crosshairStyle);

    B("misc", "fullbright", g_Ui.fullbright);
    I("misc", "fbBright", g_Ui.fullbrightBrightness);
    WritePrivateProfileStringA("meta", "profile", OakProfileName(profileId), path);
    WritePrivateProfileStringA("meta", "version", "3", path);

    char msg[96];
    wsprintfA(msg, "profile: saved %s", OakProfileName(profileId));
    ImGuiMenu_Log(msg);
}

void ImGuiMenu_LoadProfile(int profileId)
{
    char path[MAX_PATH] = {};
    OakProfilePath(path, MAX_PATH, profileId);
    if (!path[0] || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        ImGuiMenu_ApplyProfile(profileId);
        return;
    }
    ImGuiMenu_ApplyProfile(profileId);
    char msg[96];
    wsprintfA(msg, "profile: loaded %s", OakProfileName(profileId));
    ImGuiMenu_Log(msg);
}

static void OakConfigPath(char* out, DWORD outLen)
{
    if (!out || outLen < 32) return;
    out[0] = 0;
    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 40) return;
    char dir[MAX_PATH];
    wsprintfA(dir, "%s\\DayZ", base);
    CreateDirectoryA(dir, nullptr);
    wsprintfA(out, "%s\\DayZ\\oak_config.ini", base);
}

static void OakConfigPathBackup(char* out, DWORD outLen)
{
    if (!out || outLen < 32) return;
    CreateDirectoryA("C:\\oak", nullptr);
    wsprintfA(out, "C:\\oak\\oak_config.ini");
}

static void OakNamedProfilePath(char* out, DWORD outLen, const char* name)
{
    if (!out || outLen < 48 || !name || !name[0]) { if (out && outLen) out[0] = 0; return; }
    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 64) { out[0] = 0; return; }
    char dir[MAX_PATH];
    wsprintfA(dir, "%s\\DayZ", base);
    CreateDirectoryA(dir, nullptr);
    wsprintfA(out, "%s\\DayZ\\oak_profile_%s.ini", base, name);
}

static void OakExportConfigPath(char* out, DWORD outLen)
{
    if (!out || outLen < 48) return;
    out[0] = 0;
    char base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 48) return;
    char dir[MAX_PATH];
    wsprintfA(dir, "%s\\DayZ", base);
    CreateDirectoryA(dir, nullptr);
    wsprintfA(out, "%s\\DayZ\\oak_config_export.ini", base);
}

static void OakWritePerfSection(const char* path, const OakPerfLimits& p)
{
    auto B = [&](const char* key, bool v) { OakWriteInt(path, "performance", key, v ? 1 : 0); };
    auto I = [&](const char* key, int v) { OakWriteInt(path, "performance", key, v); };
    I("espUpdateHz", p.espUpdateHz);
    I("maxEntitiesScanned", p.maxEntitiesScanned);
    I("maxEntitiesDrawn", p.maxEntitiesDrawn);
    I("maxLabelsPerFrame", p.maxLabelsPerFrame);
    I("maxSkeletonsPerFrame", p.maxSkeletonsPerFrame);
    I("maxLootPerFrame", p.maxLootPerFrame);
    I("maxCorpsesPerFrame", p.maxCorpsesPerFrame);
    I("maxTrapsPerFrame", p.maxTrapsPerFrame);
    I("maxZombiesDrawn", p.maxZombiesDrawn);
    I("maxAnimalsDrawn", p.maxAnimalsDrawn);
    I("maxPlayersDrawn", p.maxPlayersDrawn);
    I("maxAimTargetsEvaluated", p.maxAimTargetsEvaluated);
    I("lodNearM", p.lodNearM);
    I("lodMidM", p.lodMidM);
    I("lodFarM", p.lodFarM);
    B("cullOffscreen", p.cullOffscreen);
    B("asyncScan", p.asyncScan);
    I("frameBudgetMs", p.frameBudgetMs);
    B("frameBoost", p.frameBoost);
    B("fpsCapEnabled", p.fpsCapEnabled);
    I("fpsCap", p.fpsCap);
    B("unlockPresent", p.unlockPresent);
    B("allowTearing", p.allowTearing);
    B("breakRefreshLock", p.breakRefreshLock);
    B("highPerfMode", p.highPerfMode);
    B("patchDayzCfg", p.patchDayzCfg);
    I("maxFrameLatency", p.maxFrameLatency);
}

static void OakReadPerfSection(const char* path, OakPerfLimits* p)
{
    if (!p) return;
    auto B = [&](const char* key, bool* v) {
        *v = GetPrivateProfileIntA("performance", key, *v ? 1 : 0, path) != 0;
    };
    auto I = [&](const char* key, int* v, int lo, int hi) {
        int x = GetPrivateProfileIntA("performance", key, *v, path);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        *v = x;
    };
    I("espUpdateHz", &p->espUpdateHz, OAK_CAP_ESP_UPDATE_HZ_MIN, OAK_CAP_ESP_UPDATE_HZ);
    I("maxEntitiesScanned", &p->maxEntitiesScanned, 50, OAK_CAP_MAX_ENTITIES_SCANNED);
    I("maxEntitiesDrawn", &p->maxEntitiesDrawn, 20, OAK_CAP_MAX_ENTITIES_DRAWN);
    I("maxLabelsPerFrame", &p->maxLabelsPerFrame, 10, OAK_CAP_MAX_LABELS);
    I("maxSkeletonsPerFrame", &p->maxSkeletonsPerFrame, 5, OAK_CAP_MAX_SKELETONS);
    I("maxLootPerFrame", &p->maxLootPerFrame, 20, OAK_CAP_MAX_LOOT);
    I("maxCorpsesPerFrame", &p->maxCorpsesPerFrame, 10, OAK_CAP_MAX_CORPSES);
    I("maxTrapsPerFrame", &p->maxTrapsPerFrame, 10, OAK_CAP_MAX_TRAPS);
    I("maxZombiesDrawn", &p->maxZombiesDrawn, 5, OAK_CAP_MAX_ZOMBIES_DRAWN);
    I("maxAnimalsDrawn", &p->maxAnimalsDrawn, 5, OAK_CAP_MAX_ANIMALS_DRAWN);
    I("maxPlayersDrawn", &p->maxPlayersDrawn, 5, OAK_CAP_MAX_PLAYERS_DRAWN);
    I("maxAimTargetsEvaluated", &p->maxAimTargetsEvaluated, 8, OAK_CAP_MAX_AIM_TARGETS);
    I("lodNearM", &p->lodNearM, 25, OAK_CAP_DIST_M);
    I("lodMidM", &p->lodMidM, 50, OAK_CAP_DIST_M);
    I("lodFarM", &p->lodFarM, 100, OAK_CAP_DIST_M);
    B("cullOffscreen", &p->cullOffscreen);
    B("asyncScan", &p->asyncScan);
    I("frameBudgetMs", &p->frameBudgetMs, OAK_CAP_FRAME_BUDGET_MS_MIN, OAK_CAP_FRAME_BUDGET_MS);
    B("frameBoost", &p->frameBoost);
    B("fpsCapEnabled", &p->fpsCapEnabled);
    I("fpsCap", &p->fpsCap, OAK_CAP_FPS_LIMIT_MIN, OAK_CAP_FPS_LIMIT);
    B("unlockPresent", &p->unlockPresent);
    B("allowTearing", &p->allowTearing);
    B("breakRefreshLock", &p->breakRefreshLock);
    B("highPerfMode", &p->highPerfMode);
    B("patchDayzCfg", &p->patchDayzCfg);
    I("maxFrameLatency", &p->maxFrameLatency, 0, OAK_CAP_FRAME_LATENCY_MAX);
    OakClampPerfLimits(p);
}

static void OakWriteFloat4Arr(const char* path, const char* sec, const char* key, const float* c)
{
    char buf[96];
    wsprintfA(buf, "%d %d %d %d",
        (int)(c[0] * 1000.f), (int)(c[1] * 1000.f), (int)(c[2] * 1000.f), (int)(c[3] * 1000.f));
    WritePrivateProfileStringA(sec, key, buf, path);
}

static void OakReadFloat4Arr(const char* path, const char* sec, const char* key, float* c)
{
    char buf[96] = {};
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
    int a = 0, b = 0, d = 0, e = 0;
    if (buf[0] && sscanf_s(buf, "%d %d %d %d", &a, &b, &d, &e) == 4)
    {
        c[0] = a / 1000.f; c[1] = b / 1000.f; c[2] = d / 1000.f; c[3] = e / 1000.f;
    }
}

static void OakWriteFeaturesSections(const char* path)
{
    auto B = [&](const char* sec, const char* key, bool v) { OakWriteInt(path, sec, key, v ? 1 : 0); };
    auto I = [&](const char* sec, const char* key, int v) { OakWriteInt(path, sec, key, v); };
    auto F = [&](const char* sec, const char* key, const float* c) { OakWriteFloat4Arr(path, sec, key, c); };

    const OakThreatRingSettings& tr = g_Ui.threatRing;
    B("visuals.threatRing", "enabled", tr.enabled);
    B("visuals.threatRing", "showPlayers", tr.showPlayers);
    B("visuals.threatRing", "showZombies", tr.showZombies);
    B("visuals.threatRing", "zombiesOnly", tr.zombiesOnly);
    B("visuals.threatRing", "hideInMenu", tr.hideInMenu);
    B("visuals.threatRing", "showDistLabels", tr.showDistLabels);
    I("visuals.threatRing", "maxDist", tr.maxDistanceM);
    I("visuals.threatRing", "ringRadius", (int)tr.ringRadiusPx);
    F("visuals.threatRing", "colorPlayer", tr.colorPlayer);
    F("visuals.threatRing", "colorZombie", tr.colorZombie);
    F("visuals.threatRing", "colorFriend", tr.colorFriend);

    const OakThreatCounterSettings& tc = g_Ui.threatCounter;
    B("visuals.threatCounter", "enabled", tc.enabled);
    B("visuals.threatCounter", "hideWhenZero", tc.hideWhenZero);
    I("visuals.threatCounter", "maxDist", tc.maxDistanceM);
    I("visuals.threatCounter", "offsetX", (int)tc.offsetX);
    I("visuals.threatCounter", "offsetY", (int)tc.offsetY);

    const OakCompassSettings& cp = g_Ui.compass;
    B("visuals.compass", "enabled", cp.enabled);
    B("visuals.compass", "showDegrees", cp.showDegrees);
    B("visuals.compass", "showWpBearing", cp.showWaypointBearing);
    I("visuals.compass", "position", cp.position);

    const OakLookDirectionSettings& ld = g_Ui.lookDirection;
    B("esp.lookDirection", "enabled", ld.enabled);
    B("esp.lookDirection", "players", ld.players);
    B("esp.lookDirection", "zombies", ld.zombies);
    I("esp.lookDirection", "maxDist", ld.maxDistanceM);
    I("esp.lookDirection", "lineLen", (int)(ld.lineLengthM * 10.f));
    F("esp.lookDirection", "color", ld.color);
    F("esp.lookDirection", "colorZombie", ld.colorZombie);

    const OakPlayerTrailSettings& pt = g_Ui.playerTrail;
    B("esp.playerTrail", "enabled", pt.enabled);
    I("esp.playerTrail", "maxPlayers", pt.maxPlayers);
    I("esp.playerTrail", "maxPoints", pt.maxPoints);
    I("esp.playerTrail", "duration", (int)(pt.durationSec * 10.f));
    F("esp.playerTrail", "color", pt.color);

    const OakDeathMarkerSettings& dm = g_Ui.deathMarker;
    B("misc.deathMarker", "enabled", dm.enabled);
    B("misc.deathMarker", "autoClear", dm.autoClearOnRespawn);

    const OakNightBoostSettings& nb = g_Ui.nightBoost;
    B("visuals.nightBoost", "enabled", nb.enabled);
    I("visuals.nightBoost", "brightness", nb.brightness);

    const OakShadowChamsSettings& sc = g_Ui.shadowChams;
    B("visuals.shadowChams", "enabled", sc.enabled);
    B("visuals.shadowChams", "players", sc.players);
    B("visuals.shadowChams", "zombies", sc.zombies);
    B("visuals.shadowChams", "items", sc.items);
    B("visuals.shadowChams", "hands", sc.hands);
    B("visuals.shadowChams", "vehicles", sc.vehicles);
    B("visuals.shadowChams", "containers", sc.containers);
    B("visuals.shadowChams", "corpses", sc.corpses);
    I("visuals.shadowChams", "pattern", sc.pattern);
    I("visuals.shadowChams", "fillAlpha", (int)(sc.fillAlpha * 100.f));
    I("visuals.shadowChams", "outline", (int)(sc.outlineThick * 10.f));
    I("visuals.shadowChams", "animSpeed", (int)(sc.animSpeed * 10.f));
    F("visuals.shadowChams", "color", sc.color);
    F("visuals.shadowChams", "color2", sc.color2);
    F("visuals.shadowChams", "outlineColor", sc.outlineColor);

    const OakCrosshairHighlightSettings& ch = g_Ui.crosshairHighlight;
    B("hud.crosshairHighlight", "enabled", ch.enabled);
    F("hud.crosshairHighlight", "colorDefault", ch.colorDefault);
    F("hud.crosshairHighlight", "colorHighlight", ch.colorHighlight);

    const OakFovHighlightSettings& fh = g_Ui.fovHighlight;
    B("combat.fovHighlight", "enabled", fh.enabled);
    F("combat.fovHighlight", "colorIdle", fh.colorIdle);
    F("combat.fovHighlight", "colorActive", fh.colorActive);

    const OakReloadBarSettings& rb = g_Ui.reloadBar;
    B("hud.reloadBar", "enabled", rb.enabled);
    I("hud.reloadBar", "width", (int)rb.width);
    I("hud.reloadBar", "height", (int)rb.height);
    I("hud.reloadBar", "offsetY", (int)rb.offsetY);
    F("hud.reloadBar", "colorBg", rb.colorBg);
    F("hud.reloadBar", "colorFill", rb.colorFill);

    const OakHeliCrashEspSettings& hc = g_Ui.heliCrashEsp;
    B("esp.heliCrash", "enabled", hc.enabled);
    I("esp.heliCrash", "maxDist", hc.maxDistanceM);
    F("esp.heliCrash", "color", hc.color);

    const OakGridCoordsHudSettings& gc = g_Ui.gridCoordsHud;
    B("hud.gridCoords", "enabled", gc.enabled);
    I("hud.gridCoords", "corner", gc.corner);

    const OakWaypointHudSettings& wh = g_Ui.waypointHud;
    B("hud.waypointDistance", "enabled", wh.enabled);
    B("hud.waypointDistance", "showBearing", wh.showBearing);
    I("hud.waypointDistance", "corner", wh.corner);
    I("hud.waypointDistance", "activeIndex", wh.activeIndex);

    const OakStanceIconSettings& st = g_Ui.stanceIcon;
    B("esp.stanceIcon", "enabled", st.enabled);
    I("esp.stanceIcon", "maxDist", st.maxDistanceM);

    const OakBatch4EspSettings& b4 = g_Ui.batch4Esp;
    B("esp.batch4", "lootExtras", b4.lootExtrasEnabled);
    B("esp.batch4.quality", "enabled", b4.quality.enabled);
    B("esp.batch4.quality", "showBadge", b4.quality.showBadge);
    I("esp.batch4.quality", "maxProbes", b4.quality.maxProbesPerFrame);
    B("esp.batch4.quantity", "enabled", b4.quantity.enabled);
    I("esp.batch4.quantity", "maxProbes", b4.quantity.maxProbesPerFrame);
    B("esp.batch4.visibility", "enabled", b4.visibility.enabled);
    I("esp.batch4.visibility", "mode", b4.visibility.mode);
  { int d = (int)b4.visibility.depthOccludeM; I("esp.batch4.visibility", "depthM", d); }
    B("esp.batch4.container", "enabled", b4.containerContents.enabled);
    I("esp.batch4.container", "maxLines", b4.containerContents.maxLines);
    I("esp.batch4.container", "maxPerFrame", b4.containerContents.maxContainersPerFrame);
    B("esp.batch4.looking", "enabled", b4.lookingAtMe.enabled);
    B("esp.batch4.looking", "players", b4.lookingAtMe.players);
    B("esp.batch4.looking", "zombies", b4.lookingAtMe.zombies);
    I("esp.batch4.looking", "maxDist", b4.lookingAtMe.maxDistanceM);
    I("esp.batch4.looking", "angleDeg", b4.lookingAtMe.angleDeg);
  { int dt = (int)(b4.lookingAtMe.dotThreshold * 100.f); I("esp.batch4.looking", "dotPct", dt); }
    F("esp.batch4.looking", "color", b4.lookingAtMe.color);
    F("esp.batch4.looking", "colorZombie", b4.lookingAtMe.colorZombie);
    B("esp.batch4.contamination", "enabled", b4.contamination.enabled);
    I("esp.batch4.contamination", "maxDist", b4.contamination.maxDistanceM);
    B("esp.batch4.contamination", "showLabels", b4.contamination.showLabels);
    F("esp.batch4.contamination", "color", b4.contamination.color);
    I("esp.batch4.contamination", "yOffsetCm", (int)(b4.contamination.yOffsetM * 100.f));
    B("esp.batch4.contamination", "groundRing", b4.contamination.drawGroundRing);
    B("esp.batch4.invViewer", "enabled", b4.playerInvViewer.enabled);
    I("esp.batch4.invViewer", "maxDist", b4.playerInvViewer.maxDistanceM);
  { int r = (int)b4.playerInvViewer.crosshairRadiusPx; I("esp.batch4.invViewer", "radiusPx", r); }
    F("esp.batch4.invViewer", "color", b4.playerInvViewer.color);

    B("combat.silentAim", "enabled", g_Ui.silentAim.enabled);
    I("combat.silentAim", "maxDist", g_Ui.silentAim.maxDistanceM);
    I("combat.silentAim", "fovPx", g_Ui.silentAim.fovPx);
    I("combat.silentAim", "bone", g_Ui.silentAim.bone);
    I("combat.silentAim", "key", g_Ui.silentAim.key);
    B("combat.silentAim", "bypass25m", g_Ui.silentAim.bypass25m);
    B("combat.silentAim", "drawFov", g_Ui.silentAim.drawFov);
    B("combat.silentAim", "drawLock", g_Ui.silentAim.drawLock);
    B("combat.silentAim", "players", g_Ui.silentAim.players);
    B("combat.silentAim", "zombies", g_Ui.silentAim.zombies);
    B("combat.grenadeTp", "enabled", g_Ui.grenadeTeleport.enabled);
    I("combat.grenadeTp", "fovPx", g_Ui.grenadeTeleport.fovPx);
    I("combat.grenadeTp", "maxDist", g_Ui.grenadeTeleport.maxDistanceM);
    B("combat.grenadeTp", "drawFov", g_Ui.grenadeTeleport.drawFov);
    B("combat.grenadeTp", "players", g_Ui.grenadeTeleport.players);
    B("combat.grenadeTp", "zombies", g_Ui.grenadeTeleport.zombies);
    B("combat.triggerbot", "enabled", g_Ui.triggerbot.enabled);
    I("combat.triggerbot", "delayMs", g_Ui.triggerbot.delayMs);
    I("combat.triggerbot", "maxDist", g_Ui.triggerbot.maxDistanceM);
    I("combat.triggerbot", "deadzonePx", g_Ui.triggerbot.deadzonePx);
    B("combat.triggerbot", "requireAds", g_Ui.triggerbot.requireAds);
    B("combat.triggerbot", "requireLos", g_Ui.triggerbot.requireLos);
    B("combat.triggerbot", "players", g_Ui.triggerbot.players);
    B("combat.triggerbot", "zombies", g_Ui.triggerbot.zombies);
    B("combat.recoil", "noRecoil", g_Ui.recoil.noRecoil);
    B("combat.recoil", "noSway", g_Ui.recoil.noSway);
    I("combat.recoil", "recoilPct", g_Ui.recoil.recoilPct);
    I("combat.recoil", "swayPct", g_Ui.recoil.swayPct);
    B("combat.aimbot", "autoFire", g_Ui.aimbotB4.autoFireWhenLocked);
    I("combat.aimbot", "aimAssistKey", g_Ui.aimbotB4.aimAssistKey);
    I("bind", "silentAim", g_Ui.bindSilentAim);
    I("bind", "aimAssist", g_Ui.bindAimAssist);
}

static void OakReadFeaturesSections(const char* path)
{
    auto B = [&](const char* sec, const char* key, bool* v) {
        *v = GetPrivateProfileIntA(sec, key, *v ? 1 : 0, path) != 0;
    };
    auto I = [&](const char* sec, const char* key, int* v, int lo, int hi) {
        int x = GetPrivateProfileIntA(sec, key, *v, path);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        *v = x;
    };
    auto F = [&](const char* sec, const char* key, float* c) { OakReadFloat4Arr(path, sec, key, c); };

    OakThreatRingSettings& tr = g_Ui.threatRing;
    B("visuals.threatRing", "enabled", &tr.enabled);
    B("visuals.threatRing", "showPlayers", &tr.showPlayers);
    B("visuals.threatRing", "showZombies", &tr.showZombies);
    B("visuals.threatRing", "zombiesOnly", &tr.zombiesOnly);
    B("visuals.threatRing", "hideInMenu", &tr.hideInMenu);
    B("visuals.threatRing", "showDistLabels", &tr.showDistLabels);
    I("visuals.threatRing", "maxDist", &tr.maxDistanceM, 10, OAK_CAP_DIST_M);
  { int r = (int)tr.ringRadiusPx; I("visuals.threatRing", "ringRadius", &r, 40, 400); tr.ringRadiusPx = (float)r; }
    F("visuals.threatRing", "colorPlayer", tr.colorPlayer);
    F("visuals.threatRing", "colorZombie", tr.colorZombie);
    F("visuals.threatRing", "colorFriend", tr.colorFriend);

    OakThreatCounterSettings& tc = g_Ui.threatCounter;
    B("visuals.threatCounter", "enabled", &tc.enabled);
    B("visuals.threatCounter", "hideWhenZero", &tc.hideWhenZero);
    I("visuals.threatCounter", "maxDist", &tc.maxDistanceM, 10, OAK_CAP_DIST_M);
  { int ox = (int)tc.offsetX; I("visuals.threatCounter", "offsetX", &ox, -200, 200); tc.offsetX = (float)ox; }
  { int oy = (int)tc.offsetY; I("visuals.threatCounter", "offsetY", &oy, -200, 200); tc.offsetY = (float)oy; }

    OakCompassSettings& cp = g_Ui.compass;
    B("visuals.compass", "enabled", &cp.enabled);
    B("visuals.compass", "showDegrees", &cp.showDegrees);
    B("visuals.compass", "showWpBearing", &cp.showWaypointBearing);
    I("visuals.compass", "position", &cp.position, 0, 1);

    OakLookDirectionSettings& ld = g_Ui.lookDirection;
    B("esp.lookDirection", "enabled", &ld.enabled);
    B("esp.lookDirection", "players", &ld.players);
    B("esp.lookDirection", "zombies", &ld.zombies);
    I("esp.lookDirection", "maxDist", &ld.maxDistanceM, 20, OAK_CAP_DIST_M);
  { int ll = (int)(ld.lineLengthM * 10.f); I("esp.lookDirection", "lineLen", &ll, 10, 80); ld.lineLengthM = ll / 10.f; }
    F("esp.lookDirection", "color", ld.color);
    F("esp.lookDirection", "colorZombie", ld.colorZombie);

    OakPlayerTrailSettings& pt = g_Ui.playerTrail;
    B("esp.playerTrail", "enabled", &pt.enabled);
    I("esp.playerTrail", "maxPlayers", &pt.maxPlayers, 1, 32);
    I("esp.playerTrail", "maxPoints", &pt.maxPoints, 4, 64);
  { int ds = (int)(pt.durationSec * 10.f); I("esp.playerTrail", "duration", &ds, 10, 120); pt.durationSec = ds / 10.f; }
    F("esp.playerTrail", "color", pt.color);

    OakDeathMarkerSettings& dm = g_Ui.deathMarker;
    B("misc.deathMarker", "enabled", &dm.enabled);
    B("misc.deathMarker", "autoClear", &dm.autoClearOnRespawn);

    OakNightBoostSettings& nb = g_Ui.nightBoost;
    B("visuals.nightBoost", "enabled", &nb.enabled);
    I("visuals.nightBoost", "brightness", &nb.brightness, 5, 100);

    OakShadowChamsSettings& sc = g_Ui.shadowChams;
    B("visuals.shadowChams", "enabled", &sc.enabled);
    B("visuals.shadowChams", "players", &sc.players);
    B("visuals.shadowChams", "zombies", &sc.zombies);
    B("visuals.shadowChams", "items", &sc.items);
    B("visuals.shadowChams", "hands", &sc.hands);
    B("visuals.shadowChams", "vehicles", &sc.vehicles);
    B("visuals.shadowChams", "containers", &sc.containers);
    B("visuals.shadowChams", "corpses", &sc.corpses);
    I("visuals.shadowChams", "pattern", &sc.pattern, 0, 4);
  { int fa = (int)(sc.fillAlpha * 100.f); I("visuals.shadowChams", "fillAlpha", &fa, 5, 95); sc.fillAlpha = fa / 100.f; }
  { int ot = (int)(sc.outlineThick * 10.f); I("visuals.shadowChams", "outline", &ot, 10, 60); sc.outlineThick = ot / 10.f; }
  { int asp = (int)(sc.animSpeed * 10.f); I("visuals.shadowChams", "animSpeed", &asp, 1, 80); sc.animSpeed = asp / 10.f; }
    F("visuals.shadowChams", "color", sc.color);
    F("visuals.shadowChams", "color2", sc.color2);
    F("visuals.shadowChams", "outlineColor", sc.outlineColor);

    OakCrosshairHighlightSettings& ch = g_Ui.crosshairHighlight;
    B("hud.crosshairHighlight", "enabled", &ch.enabled);
    F("hud.crosshairHighlight", "colorDefault", ch.colorDefault);
    F("hud.crosshairHighlight", "colorHighlight", ch.colorHighlight);

    OakFovHighlightSettings& fh = g_Ui.fovHighlight;
    B("combat.fovHighlight", "enabled", &fh.enabled);
    F("combat.fovHighlight", "colorIdle", fh.colorIdle);
    F("combat.fovHighlight", "colorActive", fh.colorActive);

    OakReloadBarSettings& rb = g_Ui.reloadBar;
    B("hud.reloadBar", "enabled", &rb.enabled);
  { int w = (int)rb.width; I("hud.reloadBar", "width", &w, 40, 300); rb.width = (float)w; }
  { int h = (int)rb.height; I("hud.reloadBar", "height", &h, 2, 20); rb.height = (float)h; }
  { int oy = (int)rb.offsetY; I("hud.reloadBar", "offsetY", &oy, 0, 80); rb.offsetY = (float)oy; }
    F("hud.reloadBar", "colorBg", rb.colorBg);
    F("hud.reloadBar", "colorFill", rb.colorFill);

    OakHeliCrashEspSettings& hc = g_Ui.heliCrashEsp;
    B("esp.heliCrash", "enabled", &hc.enabled);
    I("esp.heliCrash", "maxDist", &hc.maxDistanceM, 100, OAK_CAP_DIST_M);
    F("esp.heliCrash", "color", hc.color);

    OakGridCoordsHudSettings& gc = g_Ui.gridCoordsHud;
    B("hud.gridCoords", "enabled", &gc.enabled);
    I("hud.gridCoords", "corner", &gc.corner, 0, 3);

    OakWaypointHudSettings& wh = g_Ui.waypointHud;
    B("hud.waypointDistance", "enabled", &wh.enabled);
    B("hud.waypointDistance", "showBearing", &wh.showBearing);
    I("hud.waypointDistance", "corner", &wh.corner, 0, 3);
    I("hud.waypointDistance", "activeIndex", &wh.activeIndex, 0, OAK_WAYPOINT_MAX - 1);

    OakStanceIconSettings& st = g_Ui.stanceIcon;
    B("esp.stanceIcon", "enabled", &st.enabled);
    I("esp.stanceIcon", "maxDist", &st.maxDistanceM, 20, OAK_CAP_DIST_M);

    OakBatch4EspSettings& b4 = g_Ui.batch4Esp;
    B("esp.batch4", "lootExtras", &b4.lootExtrasEnabled);
    B("esp.batch4.quality", "enabled", &b4.quality.enabled);
    B("esp.batch4.quality", "showBadge", &b4.quality.showBadge);
    I("esp.batch4.quality", "maxProbes", &b4.quality.maxProbesPerFrame, 4, OAK_BATCH4_MAX_QUALITY_PROBES);
    B("esp.batch4.quantity", "enabled", &b4.quantity.enabled);
    I("esp.batch4.quantity", "maxProbes", &b4.quantity.maxProbesPerFrame, 4, OAK_BATCH4_MAX_QTY_PROBES);
    B("esp.batch4.visibility", "enabled", &b4.visibility.enabled);
    I("esp.batch4.visibility", "mode", &b4.visibility.mode, 0, 1);
  { int d = (int)b4.visibility.depthOccludeM;
    I("esp.batch4.visibility", "depthM", &d, 25, OAK_CAP_DIST_M);
    b4.visibility.depthOccludeM = (float)d; }
    B("esp.batch4.container", "enabled", &b4.containerContents.enabled);
    I("esp.batch4.container", "maxLines", &b4.containerContents.maxLines, 1, OAK_BATCH4_MAX_CONTAINER_LINES);
    I("esp.batch4.container", "maxPerFrame", &b4.containerContents.maxContainersPerFrame, 1, OAK_BATCH4_MAX_CONTAINER_PROBES);
    B("esp.batch4.looking", "enabled", &b4.lookingAtMe.enabled);
    B("esp.batch4.looking", "players", &b4.lookingAtMe.players);
    B("esp.batch4.looking", "zombies", &b4.lookingAtMe.zombies);
    I("esp.batch4.looking", "maxDist", &b4.lookingAtMe.maxDistanceM, 20, OAK_CAP_DIST_M);
    I("esp.batch4.looking", "angleDeg", &b4.lookingAtMe.angleDeg, 10, 70);
  { int dt = (int)(b4.lookingAtMe.dotThreshold * 100.f);
    I("esp.batch4.looking", "dotPct", &dt, 50, 99);
    b4.lookingAtMe.dotThreshold = dt / 100.f; }
    F("esp.batch4.looking", "color", b4.lookingAtMe.color);
    F("esp.batch4.looking", "colorZombie", b4.lookingAtMe.colorZombie);
    B("esp.batch4.contamination", "enabled", &b4.contamination.enabled);
    I("esp.batch4.contamination", "maxDist", &b4.contamination.maxDistanceM, 100, OAK_CAP_DIST_M);
    B("esp.batch4.contamination", "showLabels", &b4.contamination.showLabels);
    F("esp.batch4.contamination", "color", b4.contamination.color);
    {
        int yCm = GetPrivateProfileIntA("esp.batch4.contamination", "yOffsetCm",
            (int)(b4.contamination.yOffsetM * 100.f), path);
        if (yCm < -1000) yCm = -1000;
        if (yCm > 4000) yCm = 4000;
        b4.contamination.yOffsetM = yCm / 100.f;
    }
    B("esp.batch4.contamination", "groundRing", &b4.contamination.drawGroundRing);
    B("esp.batch4.invViewer", "enabled", &b4.playerInvViewer.enabled);
    I("esp.batch4.invViewer", "maxDist", &b4.playerInvViewer.maxDistanceM, 20, OAK_CAP_DIST_M);
  { int r = (int)b4.playerInvViewer.crosshairRadiusPx;
    I("esp.batch4.invViewer", "radiusPx", &r, 20, 300);
    b4.playerInvViewer.crosshairRadiusPx = (float)r; }
    F("esp.batch4.invViewer", "color", b4.playerInvViewer.color);

    B("combat.silentAim", "enabled", &g_Ui.silentAim.enabled);
    I("combat.silentAim", "maxDist", &g_Ui.silentAim.maxDistanceM, 20, OAK_CAP_DIST_M);
    I("combat.silentAim", "fovPx", &g_Ui.silentAim.fovPx, 20, 800);
    I("combat.silentAim", "bone", &g_Ui.silentAim.bone, 0, 1);
    I("combat.silentAim", "key", &g_Ui.silentAim.key, 0, 255);
    B("combat.silentAim", "bypass25m", &g_Ui.silentAim.bypass25m);
    B("combat.silentAim", "drawFov", &g_Ui.silentAim.drawFov);
    B("combat.silentAim", "drawLock", &g_Ui.silentAim.drawLock);
    B("combat.silentAim", "players", &g_Ui.silentAim.players);
    B("combat.silentAim", "zombies", &g_Ui.silentAim.zombies);
    B("combat.grenadeTp", "enabled", &g_Ui.grenadeTeleport.enabled);
    I("combat.grenadeTp", "fovPx", &g_Ui.grenadeTeleport.fovPx, 10, 800);
    I("combat.grenadeTp", "maxDist", &g_Ui.grenadeTeleport.maxDistanceM, 10, OAK_CAP_DIST_M);
    B("combat.grenadeTp", "drawFov", &g_Ui.grenadeTeleport.drawFov);
    B("combat.grenadeTp", "players", &g_Ui.grenadeTeleport.players);
    B("combat.grenadeTp", "zombies", &g_Ui.grenadeTeleport.zombies);
    B("combat.triggerbot", "enabled", &g_Ui.triggerbot.enabled);
    I("combat.triggerbot", "delayMs", &g_Ui.triggerbot.delayMs, 0, 500);
    I("combat.triggerbot", "maxDist", &g_Ui.triggerbot.maxDistanceM, 20, OAK_CAP_DIST_M);
    I("combat.triggerbot", "deadzonePx", &g_Ui.triggerbot.deadzonePx, 4, 120);
    B("combat.triggerbot", "requireAds", &g_Ui.triggerbot.requireAds);
    B("combat.triggerbot", "requireLos", &g_Ui.triggerbot.requireLos);
    B("combat.triggerbot", "players", &g_Ui.triggerbot.players);
    B("combat.triggerbot", "zombies", &g_Ui.triggerbot.zombies);
    B("combat.recoil", "noRecoil", &g_Ui.recoil.noRecoil);
    B("combat.recoil", "noSway", &g_Ui.recoil.noSway);
    I("combat.recoil", "recoilPct", &g_Ui.recoil.recoilPct, 0, 100);
    I("combat.recoil", "swayPct", &g_Ui.recoil.swayPct, 0, 100);
    B("combat.aimbot", "autoFire", &g_Ui.aimbotB4.autoFireWhenLocked);
    I("combat.aimbot", "aimAssistKey", &g_Ui.aimbotB4.aimAssistKey, 0, 255);
    I("bind", "silentAim", &g_Ui.bindSilentAim, 0, 255);
    I("bind", "aimAssist", &g_Ui.bindAimAssist, 0, 255);
}

static void OakWriteCorpseTrapSections(const char* path)
{
    auto B = [&](const char* sec, const char* key, bool v) { OakWriteInt(path, sec, key, v ? 1 : 0); };
    auto I = [&](const char* sec, const char* key, int v) { OakWriteInt(path, sec, key, v); };
    const OakCorpseEspSettings& c = g_Ui.corpseEsp;
    B("esp.corpses", "playerCorpses", c.playerCorpses);
    B("esp.corpses", "infectedCorpses", c.infectedCorpses);
    B("esp.corpses", "box", c.box);
    B("esp.corpses", "name", c.name);
    B("esp.corpses", "distance", c.distance);
    B("esp.corpses", "skeleton", c.skeleton);
    B("esp.corpses", "inventorySummary", c.inventorySummary);
    I("esp.corpses", "inventoryMaxLines", c.inventoryMaxLines);
    I("esp.corpses", "inventoryTruncateLen", c.inventoryTruncateLen);
    B("esp.corpses", "fadeByAge", c.fadeByAge);
    I("esp.corpses", "sortMode", c.sortMode);
    const OakTrapEspSettings& t = g_Ui.trapEsp;
    B("esp.traps", "box", t.box);
    B("esp.traps", "name", t.name);
    B("esp.traps", "distance", t.distance);
    B("esp.traps", "typeLabel", t.typeLabel);
    B("esp.traps", "highlightArmed", t.highlightArmed);
    I("esp.traps", "sortMode", t.sortMode);
}

static void OakReadCorpseTrapSections(const char* path)
{
    auto B = [&](const char* sec, const char* key, bool* v) {
        *v = GetPrivateProfileIntA(sec, key, *v ? 1 : 0, path) != 0;
    };
    auto I = [&](const char* sec, const char* key, int* v, int lo, int hi) {
        int x = GetPrivateProfileIntA(sec, key, *v, path);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        *v = x;
    };
    OakCorpseEspSettings& c = g_Ui.corpseEsp;
    B("esp.corpses", "playerCorpses", &c.playerCorpses);
    B("esp.corpses", "infectedCorpses", &c.infectedCorpses);
    B("esp.corpses", "box", &c.box);
    B("esp.corpses", "name", &c.name);
    B("esp.corpses", "distance", &c.distance);
    B("esp.corpses", "skeleton", &c.skeleton);
    B("esp.corpses", "inventorySummary", &c.inventorySummary);
    I("esp.corpses", "inventoryMaxLines", &c.inventoryMaxLines, 1, 12);
    I("esp.corpses", "inventoryTruncateLen", &c.inventoryTruncateLen, 16, 128);
    B("esp.corpses", "fadeByAge", &c.fadeByAge);
    I("esp.corpses", "sortMode", &c.sortMode, 0, 2);
    OakTrapEspSettings& t = g_Ui.trapEsp;
    B("esp.traps", "box", &t.box);
    B("esp.traps", "name", &t.name);
    B("esp.traps", "distance", &t.distance);
    B("esp.traps", "typeLabel", &t.typeLabel);
    B("esp.traps", "highlightArmed", &t.highlightArmed);
    I("esp.traps", "sortMode", &t.sortMode, 0, 2);
}

static void OakWriteInt(const char* path, const char* sec, const char* key, int v);
static void OakWriteFloat4(const char* path, const char* sec, const char* key, const ImVec4& c);
static void OakReadFloat4(const char* path, const char* sec, const char* key, ImVec4* c);

static void OakWriteLootStyleSections(const char* path)
{
    static const char* kSec[OAK_LOOT_CAT_COUNT] = {
        "loot.weapons", "loot.ammo", "loot.medical", "loot.food",
        "loot.clothing", "loot.tools", "loot.other"
    };
    auto B = [&](const char* sec, const char* key, bool v) { OakWriteInt(path, sec, key, v ? 1 : 0); };
    auto I = [&](const char* sec, const char* key, int v) { OakWriteInt(path, sec, key, v); };
    for (int i = 0; i < OAK_LOOT_CAT_COUNT; i++)
    {
        const OakLootCatSettings& c = g_Ui.lootCats[i];
        B(kSec[i], "enabled", c.enabled);
        I(kSec[i], "maxDist", c.maxDistance);
        I(kSec[i], "maxCount", c.maxCount);
        B(kSec[i], "customColor", c.useCustomColor);
        OakWriteFloat4(path, kSec[i], "color", ImVec4(c.color[0], c.color[1], c.color[2], c.color[3]));
    }
    I("loot", "sortMode", g_Ui.lootSortMode);
    I("loot", "blCount", g_Ui.lootFilterBlacklistCount);
    for (int i = 0; i < g_Ui.lootFilterBlacklistCount && i < OAK_LOOT_FILTER_MAX; i++)
    {
        char k[16];
        wsprintfA(k, "bl%d", i);
        WritePrivateProfileStringA("loot", k, g_Ui.lootFilterBlacklist[i], path);
    }
    I("loot", "wlCount", g_Ui.lootFilterWhitelistCount);
    for (int i = 0; i < g_Ui.lootFilterWhitelistCount && i < OAK_LOOT_FILTER_MAX; i++)
    {
        char k[16];
        wsprintfA(k, "wl%d", i);
        WritePrivateProfileStringA("loot", k, g_Ui.lootFilterWhitelist[i], path);
    }

    auto WStyle = [&](const char* sec, const OakEspStyleSettings& s) {
        I(sec, "boxStyle", s.boxStyle);
        I(sec, "boxThick", (int)(s.boxThickness * 10.f));
        I(sec, "fillOp", (int)(s.fillOpacity * 100.f));
        B(sec, "outlineOnly", s.outlineOnly);
        I(sec, "skelThick", (int)(s.skelThickness * 10.f));
        B(sec, "visColors", s.useVisibilityColors);
        I(sec, "barPos", s.healthBarPos);
        OakWriteFloat4(path, sec, "colVis", ImVec4(s.colorVisible[0], s.colorVisible[1], s.colorVisible[2], s.colorVisible[3]));
        OakWriteFloat4(path, sec, "colOcc", ImVec4(s.colorOccluded[0], s.colorOccluded[1], s.colorOccluded[2], s.colorOccluded[3]));
    };
    WStyle("esp.style.player", g_Ui.playerStyle);
    WStyle("esp.style.zombie", g_Ui.zombieStyle);

    I("combat.aimbot", "priority", g_Ui.aimExtras.priority);
    I("combat.aimbot", "smoothVar", g_Ui.aimExtras.smoothVariancePct);
    I("combat.aimbot", "reactMs", g_Ui.aimExtras.reactionDelayMs);
    B("combat.aimbot", "requireLos", g_Ui.aimExtras.requireLos);

    I("ui", "panicScope", g_Ui.uiExtras.panicScope);
    I("ui", "bindHoldMask", (int)g_Ui.uiExtras.bindHoldMask);
    I("ui", "profKey0", g_Ui.uiExtras.profileHotkey[0]);
    I("ui", "profKey1", g_Ui.uiExtras.profileHotkey[1]);
    I("ui", "profKey2", g_Ui.uiExtras.profileHotkey[2]);
    OakWriteFloat4(path, "ui", "menuAccent",
        ImVec4(g_Ui.uiExtras.menuAccent[0], g_Ui.uiExtras.menuAccent[1],
               g_Ui.uiExtras.menuAccent[2], g_Ui.uiExtras.menuAccent[3]));
}

static void OakReadLootStyleSections(const char* path)
{
    static const char* kSec[OAK_LOOT_CAT_COUNT] = {
        "loot.weapons", "loot.ammo", "loot.medical", "loot.food",
        "loot.clothing", "loot.tools", "loot.other"
    };
    auto B = [&](const char* sec, const char* key, bool* v) {
        *v = GetPrivateProfileIntA(sec, key, *v ? 1 : 0, path) != 0;
    };
    auto I = [&](const char* sec, const char* key, int* v, int lo, int hi) {
        int x = GetPrivateProfileIntA(sec, key, *v, path);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        *v = x;
    };
    for (int i = 0; i < OAK_LOOT_CAT_COUNT; i++)
    {
        OakLootCatSettings& c = g_Ui.lootCats[i];
        B(kSec[i], "enabled", &c.enabled);
        I(kSec[i], "maxDist", &c.maxDistance, 0, OAK_CAP_DIST_M);
        I(kSec[i], "maxCount", &c.maxCount, 1, OAK_CAP_MAX_LOOT);
        B(kSec[i], "customColor", &c.useCustomColor);
        ImVec4 col = ImVec4(c.color[0], c.color[1], c.color[2], c.color[3]);
        OakReadFloat4(path, kSec[i], "color", &col);
        c.color[0] = col.x; c.color[1] = col.y; c.color[2] = col.z; c.color[3] = col.w;
    }
    I("loot", "sortMode", &g_Ui.lootSortMode, 0, 2);
    I("loot", "blCount", &g_Ui.lootFilterBlacklistCount, 0, OAK_LOOT_FILTER_MAX);
    for (int i = 0; i < g_Ui.lootFilterBlacklistCount && i < OAK_LOOT_FILTER_MAX; i++)
    {
        char k[16];
        wsprintfA(k, "bl%d", i);
        GetPrivateProfileStringA("loot", k, "", g_Ui.lootFilterBlacklist[i], 48, path);
    }
    I("loot", "wlCount", &g_Ui.lootFilterWhitelistCount, 0, OAK_LOOT_FILTER_MAX);
    for (int i = 0; i < g_Ui.lootFilterWhitelistCount && i < OAK_LOOT_FILTER_MAX; i++)
    {
        char k[16];
        wsprintfA(k, "wl%d", i);
        GetPrivateProfileStringA("loot", k, "", g_Ui.lootFilterWhitelist[i], 48, path);
    }
    OakSyncLootShowFromCats();

    auto RStyle = [&](const char* sec, OakEspStyleSettings* s) {
        if (!s) return;
        I(sec, "boxStyle", &s->boxStyle, 0, 2);
        int bt = (int)(s->boxThickness * 10.f);
        I(sec, "boxThick", &bt, 5, 50);
        s->boxThickness = bt / 10.f;
        int fo = (int)(s->fillOpacity * 100.f);
        I(sec, "fillOp", &fo, 0, 100);
        s->fillOpacity = fo / 100.f;
        B(sec, "outlineOnly", &s->outlineOnly);
        int st = (int)(s->skelThickness * 10.f);
        I(sec, "skelThick", &st, 5, 40);
        s->skelThickness = st / 10.f;
        B(sec, "visColors", &s->useVisibilityColors);
        I(sec, "barPos", &s->healthBarPos, 0, 3);
        ImVec4 cv = ImVec4(s->colorVisible[0], s->colorVisible[1], s->colorVisible[2], s->colorVisible[3]);
        ImVec4 co = ImVec4(s->colorOccluded[0], s->colorOccluded[1], s->colorOccluded[2], s->colorOccluded[3]);
        OakReadFloat4(path, sec, "colVis", &cv);
        OakReadFloat4(path, sec, "colOcc", &co);
        s->colorVisible[0] = cv.x; s->colorVisible[1] = cv.y; s->colorVisible[2] = cv.z; s->colorVisible[3] = cv.w;
        s->colorOccluded[0] = co.x; s->colorOccluded[1] = co.y; s->colorOccluded[2] = co.z; s->colorOccluded[3] = co.w;
    };
    RStyle("esp.style.player", &g_Ui.playerStyle);
    RStyle("esp.style.zombie", &g_Ui.zombieStyle);

    I("combat.aimbot", "priority", &g_Ui.aimExtras.priority, 0, 2);
    I("combat.aimbot", "smoothVar", &g_Ui.aimExtras.smoothVariancePct, 0, 50);
    I("combat.aimbot", "reactMs", &g_Ui.aimExtras.reactionDelayMs, 0, 500);
    B("combat.aimbot", "requireLos", &g_Ui.aimExtras.requireLos);

    I("ui", "panicScope", &g_Ui.uiExtras.panicScope, 0, 2);
    {
        int mask = (int)g_Ui.uiExtras.bindHoldMask;
        I("ui", "bindHoldMask", &mask, 0, 0xFFFF);
        g_Ui.uiExtras.bindHoldMask = (unsigned)mask;
    }
    I("ui", "profKey0", &g_Ui.uiExtras.profileHotkey[0], 0, 255);
    I("ui", "profKey1", &g_Ui.uiExtras.profileHotkey[1], 0, 255);
    I("ui", "profKey2", &g_Ui.uiExtras.profileHotkey[2], 0, 255);
    {
        ImVec4 acc = ImVec4(g_Ui.uiExtras.menuAccent[0], g_Ui.uiExtras.menuAccent[1],
            g_Ui.uiExtras.menuAccent[2], g_Ui.uiExtras.menuAccent[3]);
        OakReadFloat4(path, "ui", "menuAccent", &acc);
        g_Ui.uiExtras.menuAccent[0] = acc.x;
        g_Ui.uiExtras.menuAccent[1] = acc.y;
        g_Ui.uiExtras.menuAccent[2] = acc.z;
        g_Ui.uiExtras.menuAccent[3] = acc.w;
    }
}

static void OakWriteInt(const char* path, const char* sec, const char* key, int v)
{
    char buf[32];
    wsprintfA(buf, "%d", v);
    WritePrivateProfileStringA(sec, key, buf, path);
}

static void OakWriteFloat4(const char* path, const char* sec, const char* key, const ImVec4& c)
{
    char buf[96];
    // hundredths — enough for colors
    wsprintfA(buf, "%d %d %d %d",
        (int)(c.x * 1000.f), (int)(c.y * 1000.f), (int)(c.z * 1000.f), (int)(c.w * 1000.f));
    WritePrivateProfileStringA(sec, key, buf, path);
}

static void OakReadFloat4(const char* path, const char* sec, const char* key, ImVec4* c)
{
    if (!c) return;
    char buf[96] = {};
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
    int a = 0, b = 0, d = 0, e = 0;
    if (buf[0] && sscanf_s(buf, "%d %d %d %d", &a, &b, &d, &e) == 4)
    {
        c->x = a / 1000.f; c->y = b / 1000.f; c->z = d / 1000.f; c->w = e / 1000.f;
    }
}

void ImGuiMenu_SaveConfig()
{
    char path[MAX_PATH] = {};
    OakConfigPath(path, MAX_PATH);
    if (!path[0]) return;

    OakSyncLootCatsFromShow();
    auto B = [&](const char* sec, const char* key, bool v) { OakWriteInt(path, sec, key, v ? 1 : 0); };
    auto I = [&](const char* sec, const char* key, int v) { OakWriteInt(path, sec, key, v); };

    B("esp", "enabled", g_Ui.espEnabled);
    B("esp", "players", g_Ui.espPlayers);
    B("esp", "zombies", g_Ui.espZombies);
    B("esp", "animals", g_Ui.espAnimals);
    B("esp", "items", g_Ui.espItems);
    B("esp", "vehicles", g_Ui.espVehicles);
    B("esp", "skeleton", g_Ui.espSkeleton);
    B("esp", "chams", g_Ui.espChams);
    B("esp", "healthBars", g_Ui.healthBars);
    B("esp", "weaponEsp", g_Ui.weaponEsp);
    B("esp", "barHealth", g_Ui.barHealth);
    B("esp", "barBlood", g_Ui.barBlood);
    B("esp", "barShock", g_Ui.barShock);
    B("esp", "barStamina", g_Ui.barStamina);
    B("esp", "barHunger", g_Ui.barHunger);
    B("esp", "barThirst", g_Ui.barThirst);
    B("esp", "localVitals", g_Ui.localVitalsHud);
    B("esp", "localAmmo", g_Ui.localWeaponAmmo);
    B("esp", "containers", g_Ui.espContainers);
    B("esp", "corpses", g_Ui.espCorpses);
    B("esp", "traps", g_Ui.espTraps);
    B("esp", "playerBox", g_Ui.playerBox);
    B("esp", "playerName", g_Ui.playerName);
    B("esp", "playerDist", g_Ui.playerDistance);
    B("esp", "zombieBox", g_Ui.zombieBox);
    B("esp", "zombieName", g_Ui.zombieName);
    B("esp", "zombieDist", g_Ui.zombieDistance);
    B("esp", "drawLocal", g_Ui.drawLocalPlayer);
    B("esp", "itemBox", g_Ui.itemBox);
    B("esp", "itemName", g_Ui.itemName);
    B("esp", "itemDist", g_Ui.itemDistance);
    I("esp", "playerMax", g_Ui.playerMaxDistance);
    I("esp", "zombieMax", g_Ui.zombieMaxDistance);
    I("esp", "animalMax", g_Ui.animalMaxDistance);
    I("esp", "itemMax", g_Ui.itemMaxDistance);
    I("esp", "vehicleMax", g_Ui.vehicleMaxDistance);
    I("esp", "containerMax", g_Ui.containerMaxDistance);
    I("esp", "corpseMax", g_Ui.corpseMaxDistance);
    I("esp", "trapMax", g_Ui.trapMaxDistance);
    I("esp", "tracerMs", g_Ui.tracerLifetimeMs);
    I("esp", "impactMs", g_Ui.impactLifetimeMs);

    B("combat", "aimbot", g_Ui.aimbotEnabled);
    B("combat", "magic", g_Ui.magicBullet);
    B("combat", "mbAutoFire", g_Ui.magicBulletAutoFire);
    B("combat", "mbChain", g_Ui.magicBulletChain);
    B("combat", "mbDrawFov", g_Ui.magicBulletDrawFov);
    B("combat", "aimPlayers", g_Ui.aimbotPlayers);
    B("combat", "aimZombies", g_Ui.aimbotZombies);
    {
        const char* tf = "players";
        if (g_Ui.aimTargetFilter == OakAimZombies) tf = "zombies";
        else if (g_Ui.aimTargetFilter == OakAimBoth) tf = "both";
        WritePrivateProfileStringA("combat.aimbot", "targetFilter", tf, path);
    }
    B("combat", "drawFov", g_Ui.aimbotDrawFov);
    B("combat", "fastBullets", g_Ui.fastBullets);
    B("combat", "noDisp", g_Ui.noDispersion);
    B("combat", "perfectBal", g_Ui.perfectBallistics);
    B("combat", "tracers", g_Ui.bulletTracers);
    B("combat", "impacts", g_Ui.impactMarkers);
    B("combat", "shotInd", g_Ui.shotIndicators);
    B("combat", "hitMarkers", g_Ui.hitMarkers);
    B("combat", "crosshair", g_Ui.crosshair);
    B("combat", "grenade", g_Ui.grenadeTrajectory);
    I("combat", "fov", g_Ui.aimbotFov);
    I("combat", "smooth", g_Ui.aimbotSmooth);
    I("combat", "bone", g_Ui.aimbotBone);
    I("combat", "key", g_Ui.aimbotKey);
    I("combat", "aimDist", g_Ui.aimbotMaxDistance);
    I("combat", "mbDist", g_Ui.magicBulletMaxDistance);
    I("combat", "mbFov", g_Ui.magicBulletFov);
    I("combat", "xhStyle", g_Ui.crosshairStyle);
    I("combat", "xhSize", g_Ui.crosshairSize);
    I("combat", "xhGap", g_Ui.crosshairGap);
    I("combat", "xhThick", g_Ui.crosshairThickness);

    B("misc", "fullbright", g_Ui.fullbright);
    I("misc", "fbBright", g_Ui.fullbrightBrightness);
    B("misc", "steamNames", g_Ui.miscSteamNames);
    B("misc", "steamAvatars", g_Ui.miscSteamAvatars);
    B("misc", "daytime", g_Ui.miscDaytimeLock);
    B("misc", "overlays", g_Ui.miscDisableOverlays);
    B("misc", "despawn", g_Ui.miscMiddleClickDespawn);
    B("misc", "lootMagnet", g_Ui.miscLootMagnet);
    B("misc", "containerMagnet", g_Ui.miscContainerMagnet);
    B("misc", "drawWp", g_Ui.miscDrawWaypoints);
    I("misc", "lootMagRange", g_Ui.miscLootMagnetRange);
    I("misc", "contMagRange", g_Ui.miscContainerMagnetRange);
    I("misc", "despawnRange", g_Ui.miscDespawnRange);

    B("misc", "freecam", g_Ui.miscFreecam);
    B("misc", "freecamBody", g_Ui.miscFreecamMoveBody);
    I("misc", "freecamSpeed", g_Ui.miscFreecamSpeed);
    B("misc", "noGrass", g_Ui.miscNoGrass);
    I("misc", "menuKey", g_Ui.menuKey);
    B("misc", "streamProof", g_Ui.streamProof);

    B("worldMisc", "fov", g_Ui.worldMisc.fovChanger);
    I("worldMisc", "fovDeg", (int)g_Ui.worldMisc.horizontalFov);
    B("worldMisc", "fovKeepAds", g_Ui.worldMisc.fovKeepWhileAds);
    B("worldMisc", "timeLock", g_Ui.worldMisc.timeLock);
    I("worldMisc", "lockHour", (int)g_Ui.worldMisc.lockHour);
    B("worldMisc", "clearWx", g_Ui.worldMisc.clearWeather);
    B("worldMisc", "thirdPerson", g_Ui.worldMisc.thirdPerson);
    I("worldMisc", "thirdPersonDistCm", (int)(g_Ui.worldMisc.thirdPersonDistanceM * 100.f));
    I("worldMisc", "thirdPersonHeightCm", (int)(g_Ui.worldMisc.thirdPersonHeightM * 100.f));
    B("worldMisc", "streamProof", g_Ui.worldMisc.streamProof);
    B("worldMisc", "wireframe", g_Ui.worldMisc.wireframe);
    B("combat.batch4", "silentAim", g_Ui.silentAim.enabled);
    I("combat.batch4", "silentDist", g_Ui.silentAim.maxDistanceM);
    I("combat.batch4", "silentFov", g_Ui.silentAim.fovPx);
    I("combat.batch4", "silentBone", g_Ui.silentAim.bone);
    I("combat.batch4", "silentKey", g_Ui.silentAim.key);
    B("combat.batch4", "silentBypass25", g_Ui.silentAim.bypass25m);
    B("combat.batch4", "silentDrawFov", g_Ui.silentAim.drawFov);
    B("combat.batch4", "silentDrawLock", g_Ui.silentAim.drawLock);
    B("combat.batch4", "silentPlayers", g_Ui.silentAim.players);
    B("combat.batch4", "silentZombies", g_Ui.silentAim.zombies);
    B("combat.batch4", "triggerbot", g_Ui.triggerbot.enabled);
    I("combat.batch4", "triggerDelay", g_Ui.triggerbot.delayMs);
    I("combat.batch4", "triggerDist", g_Ui.triggerbot.maxDistanceM);
    I("combat.batch4", "triggerDz", g_Ui.triggerbot.deadzonePx);
    B("combat.batch4", "triggerAds", g_Ui.triggerbot.requireAds);
    B("combat.batch4", "triggerLos", g_Ui.triggerbot.requireLos);
    B("combat.batch4", "triggerPlayers", g_Ui.triggerbot.players);
    B("combat.batch4", "triggerZombies", g_Ui.triggerbot.zombies);
    B("combat.batch4", "noRecoil", g_Ui.recoil.noRecoil);
    B("combat.batch4", "noSway", g_Ui.recoil.noSway);
    I("combat.batch4", "recoilPct", g_Ui.recoil.recoilPct);
    I("combat.batch4", "swayPct", g_Ui.recoil.swayPct);
    B("combat.batch4", "autoFire", g_Ui.aimbotB4.autoFireWhenLocked);
    I("combat.batch4", "aimAssistKey", g_Ui.aimbotB4.aimAssistKey);
    I("bind", "silentAim", g_Ui.bindSilentAim);
    I("bind", "aimAssist", g_Ui.bindAimAssist);
    B("exploits", "warp", g_Ui.exploits.warp);
    I("exploits", "warpDist", (int)g_Ui.exploits.warpDistanceM);
    I("exploits", "warpKey", g_Ui.exploits.warpKey);
    I("exploits", "warpCd", g_Ui.exploits.warpCooldownMs);
    I("exploits", "warpMode", g_Ui.exploits.warpMode);
    B("exploits", "warpFlat", g_Ui.exploits.warpFlat);
    B("exploits", "warpChain", g_Ui.exploits.warpHoldChain);
    B("exploits", "grenadeWalls", g_Ui.exploits.grenadeThroughWalls);
    B("exploits", "doorUnlock", g_Ui.exploits.doorUnlock);

    I("bind", "esp", g_Ui.bindEsp);
    I("bind", "aimbot", g_Ui.bindAimbot);
    I("bind", "magic", g_Ui.bindMagicBullet);
    I("bind", "fullbright", g_Ui.bindFullbright);
    I("bind", "despawn", g_Ui.bindMiddleClickDespawn);
    I("bind", "lootMagnet", g_Ui.bindLootMagnet);
    I("bind", "contMagnet", g_Ui.bindContainerMagnet);
    I("bind", "daytime", g_Ui.bindDaytimeLock);
    I("bind", "overlays", g_Ui.bindDisableOverlays);
    I("bind", "panic", g_Ui.bindPanic);
    I("bind", "addWp", g_Ui.bindAddWaypoint);
    I("bind", "copyCoords", g_Ui.bindCopyCoords);
    I("bind", "steamNames", g_Ui.bindSteamNames);
    I("bind", "pullBase", g_Ui.bindPullBasePart);
    I("bind", "freecam", g_Ui.bindFreecam);

    I("wp", "count", g_Ui.waypointCount);
    for (int i = 0; i < g_Ui.waypointCount && i < OAK_WAYPOINT_MAX; i++)
    {
        char k[32];
        wsprintfA(k, "x%d", i); OakWriteInt(path, "wp", k, (int)(g_Ui.waypointX[i] * 100.f));
        wsprintfA(k, "y%d", i); OakWriteInt(path, "wp", k, (int)(g_Ui.waypointY[i] * 100.f));
        wsprintfA(k, "z%d", i); OakWriteInt(path, "wp", k, (int)(g_Ui.waypointZ[i] * 100.f));
        wsprintfA(k, "n%d", i); WritePrivateProfileStringA("wp", k, g_Ui.waypointName[i], path);
        wsprintfA(k, "c%d", i);
        OakWriteFloat4Arr(path, "wp", k, g_Ui.waypointColor[i]);
    }

    I("friends", "count", g_Ui.friendCount);
    for (int i = 0; i < g_Ui.friendCount && i < 16; i++)
    {
        char k[32], sid[32];
        wsprintfA(k, "n%d", i); WritePrivateProfileStringA("friends", k, g_Ui.friendName[i], path);
        wsprintfA(k, "id%d", i);
        wsprintfA(sid, "%llu", g_Ui.friendSteamId[i]);
        WritePrivateProfileStringA("friends", k, sid, path);
    }

    B("loot", "weapons", g_Ui.lootShowWeapons);
    B("loot", "ammo", g_Ui.lootShowAmmo);
    B("loot", "medical", g_Ui.lootShowMedical);
    B("loot", "food", g_Ui.lootShowFood);
    B("loot", "clothing", g_Ui.lootShowClothing);
    B("loot", "tools", g_Ui.lootShowTools);
    B("loot", "other", g_Ui.lootShowOther);

    OakWritePerfSection(path, g_Ui.perf);
    OakWriteCorpseTrapSections(path);
    OakWriteLootStyleSections(path);
    OakWriteFeaturesSections(path);

    OakUiLayoutSave(path);

    OakWriteFloat4(path, "color", "playerBox", g_Ui.colorPlayerBox);
    OakWriteFloat4(path, "color", "playerSkel", g_Ui.colorPlayerSkeleton);
    OakWriteFloat4(path, "color", "playerName", g_Ui.colorPlayerName);
    OakWriteFloat4(path, "color", "playerChams", g_Ui.colorPlayerChams);
    OakWriteFloat4(path, "color", "zombieBox", g_Ui.colorZombieBox);
    OakWriteFloat4(path, "color", "crosshair", g_Ui.colorCrosshair);
    OakWriteFloat4(path, "color", "tracer", g_Ui.colorTracer);
    OakWriteFloat4(path, "color", "impact", g_Ui.colorImpact);
    OakWriteFloat4(path, "color", "grenade", g_Ui.colorGrenade);
    OakWriteFloat4(path, "color", "itemBox", g_Ui.colorItemBox);
    OakWriteFloat4(path, "color", "itemName", g_Ui.colorItemName);
    OakWriteFloat4(path, "color", "container", g_Ui.colorContainer);
    OakWriteFloat4(path, "color", "corpse", g_Ui.colorCorpse);
    OakWriteFloat4(path, "color", "trap", g_Ui.colorTrap);

    WritePrivateProfileStringA("meta", "version", "3", path);
    char bak[MAX_PATH] = {};
    OakConfigPathBackup(bak, MAX_PATH);
    if (bak[0])
        CopyFileA(path, bak, FALSE);
    ImGuiMenu_Log("config: saved");
}

void ImGuiMenu_LoadConfig()
{
    char path[MAX_PATH] = {};
    OakConfigPath(path, MAX_PATH);
    if (!path[0]) return;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        char bak[MAX_PATH] = {};
        OakConfigPathBackup(bak, MAX_PATH);
        if (!bak[0] || GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES)
            return;
        lstrcpynA(path, bak, MAX_PATH);
    }

    auto B = [&](const char* sec, const char* key, bool* v) {
        *v = GetPrivateProfileIntA(sec, key, *v ? 1 : 0, path) != 0;
    };
    auto I = [&](const char* sec, const char* key, int* v, int lo, int hi) {
        int x = GetPrivateProfileIntA(sec, key, *v, path);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        *v = x;
    };

    B("esp", "enabled", &g_Ui.espEnabled);
    B("esp", "players", &g_Ui.espPlayers);
    B("esp", "zombies", &g_Ui.espZombies);
    B("esp", "animals", &g_Ui.espAnimals);
    B("esp", "items", &g_Ui.espItems);
    B("esp", "vehicles", &g_Ui.espVehicles);
    B("esp", "skeleton", &g_Ui.espSkeleton);
    B("esp", "chams", &g_Ui.espChams);
    B("esp", "healthBars", &g_Ui.healthBars);
    B("esp", "weaponEsp", &g_Ui.weaponEsp);
    B("esp", "barHealth", &g_Ui.barHealth);
    B("esp", "barBlood", &g_Ui.barBlood);
    B("esp", "barShock", &g_Ui.barShock);
    B("esp", "barStamina", &g_Ui.barStamina);
    B("esp", "barHunger", &g_Ui.barHunger);
    B("esp", "barThirst", &g_Ui.barThirst);
    B("esp", "localVitals", &g_Ui.localVitalsHud);
    B("esp", "localAmmo", &g_Ui.localWeaponAmmo);
    B("esp", "containers", &g_Ui.espContainers);
    B("esp", "corpses", &g_Ui.espCorpses);
    B("esp", "traps", &g_Ui.espTraps);
    B("esp", "playerBox", &g_Ui.playerBox);
    B("esp", "playerName", &g_Ui.playerName);
    B("esp", "playerDist", &g_Ui.playerDistance);
    B("esp", "zombieBox", &g_Ui.zombieBox);
    B("esp", "zombieName", &g_Ui.zombieName);
    B("esp", "zombieDist", &g_Ui.zombieDistance);
    B("esp", "drawLocal", &g_Ui.drawLocalPlayer);
    B("esp", "itemBox", &g_Ui.itemBox);
    B("esp", "itemName", &g_Ui.itemName);
    B("esp", "itemDist", &g_Ui.itemDistance);
    I("esp", "playerMax", &g_Ui.playerMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "zombieMax", &g_Ui.zombieMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "animalMax", &g_Ui.animalMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "itemMax", &g_Ui.itemMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "vehicleMax", &g_Ui.vehicleMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "containerMax", &g_Ui.containerMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "corpseMax", &g_Ui.corpseMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "trapMax", &g_Ui.trapMaxDistance, 0, OAK_CAP_DIST_M);
    I("esp", "tracerMs", &g_Ui.tracerLifetimeMs, 200, 8000);
    I("esp", "impactMs", &g_Ui.impactLifetimeMs, 100, 5000);

    B("combat", "aimbot", &g_Ui.aimbotEnabled);
    B("combat", "magic", &g_Ui.magicBullet);
    B("combat", "mbAutoFire", &g_Ui.magicBulletAutoFire);
    B("combat", "mbChain", &g_Ui.magicBulletChain);
    B("combat", "mbDrawFov", &g_Ui.magicBulletDrawFov);
    B("combat", "aimPlayers", &g_Ui.aimbotPlayers);
    B("combat", "aimZombies", &g_Ui.aimbotZombies);
    {
        char tf[16] = {};
        GetPrivateProfileStringA("combat.aimbot", "targetFilter", "", tf, sizeof(tf), path);
        if (tf[0])
        {
            if (_stricmp(tf, "zombies") == 0) g_Ui.aimTargetFilter = OakAimZombies;
            else if (_stricmp(tf, "both") == 0) g_Ui.aimTargetFilter = OakAimBoth;
            else g_Ui.aimTargetFilter = OakAimPlayers;
        }
        else
            g_Ui.aimTargetFilter = OakDeriveAimTargetFilter(g_Ui.aimbotPlayers, g_Ui.aimbotZombies);
        OakSyncAimbotBoolsFromFilter(g_Ui.aimTargetFilter, &g_Ui.aimbotPlayers, &g_Ui.aimbotZombies);
    }
    B("combat", "drawFov", &g_Ui.aimbotDrawFov);
    B("combat", "fastBullets", &g_Ui.fastBullets);
    B("combat", "noDisp", &g_Ui.noDispersion);
    B("combat", "perfectBal", &g_Ui.perfectBallistics);
    B("combat", "tracers", &g_Ui.bulletTracers);
    B("combat", "impacts", &g_Ui.impactMarkers);
    B("combat", "shotInd", &g_Ui.shotIndicators);
    B("combat", "hitMarkers", &g_Ui.hitMarkers);
    B("combat", "crosshair", &g_Ui.crosshair);
    B("combat", "grenade", &g_Ui.grenadeTrajectory);
    I("combat", "fov", &g_Ui.aimbotFov, 20, 800);
    I("combat", "smooth", &g_Ui.aimbotSmooth, 1, 30);
    I("combat", "bone", &g_Ui.aimbotBone, 0, 1);
    I("combat", "key", &g_Ui.aimbotKey, 1, 255);
    I("combat", "aimDist", &g_Ui.aimbotMaxDistance, 50, OAK_CAP_DIST_M);
    I("combat", "mbDist", &g_Ui.magicBulletMaxDistance, 100, OAK_CAP_DIST_M);
    I("combat", "mbFov", &g_Ui.magicBulletFov, 20, 800);
    I("combat", "xhStyle", &g_Ui.crosshairStyle, 0, 4);
    I("combat", "xhSize", &g_Ui.crosshairSize, 2, 32);
    I("combat", "xhGap", &g_Ui.crosshairGap, 0, 20);
    I("combat", "xhThick", &g_Ui.crosshairThickness, 1, 8);

    B("misc", "fullbright", &g_Ui.fullbright);
    I("misc", "fbBright", &g_Ui.fullbrightBrightness, 5, 100);
    B("misc", "steamNames", &g_Ui.miscSteamNames);
    B("misc", "steamAvatars", &g_Ui.miscSteamAvatars);
    B("misc", "daytime", &g_Ui.miscDaytimeLock);
    B("misc", "overlays", &g_Ui.miscDisableOverlays);
    B("misc", "despawn", &g_Ui.miscMiddleClickDespawn);
    B("misc", "lootMagnet", &g_Ui.miscLootMagnet);
    B("misc", "containerMagnet", &g_Ui.miscContainerMagnet);
    B("misc", "drawWp", &g_Ui.miscDrawWaypoints);
    I("misc", "lootMagRange", &g_Ui.miscLootMagnetRange, 1, OAK_CAP_MAGNET_M);
    I("misc", "contMagRange", &g_Ui.miscContainerMagnetRange, 1, OAK_CAP_MAGNET_M);
    I("misc", "despawnRange", &g_Ui.miscDespawnRange, 5, OAK_CAP_MAGNET_M);

    B("misc", "freecam", &g_Ui.miscFreecam);
    B("misc", "freecamBody", &g_Ui.miscFreecamMoveBody);
    I("misc", "freecamSpeed", &g_Ui.miscFreecamSpeed, 2, OAK_CAP_FREECAM_SPEED);
    B("misc", "noGrass", &g_Ui.miscNoGrass);
    I("misc", "menuKey", &g_Ui.menuKey, 1, 255);
    B("misc", "streamProof", &g_Ui.streamProof);
    // Internal overlay cannot use DisplayAffinity — it blanks DayZ. Always force off.
    g_Ui.streamProof = false;
    g_Ui.worldMisc.streamProof = false;

    B("worldMisc", "fov", &g_Ui.worldMisc.fovChanger);
    B("worldMisc", "fovKeepAds", &g_Ui.worldMisc.fovKeepWhileAds);
    {
        int fovDeg = GetPrivateProfileIntA("worldMisc", "fovDeg", (int)g_Ui.worldMisc.horizontalFov, path);
        g_Ui.worldMisc.horizontalFov = (float)fovDeg;
    }
    B("worldMisc", "timeLock", &g_Ui.worldMisc.timeLock);
    {
        int hour = GetPrivateProfileIntA("worldMisc", "lockHour", (int)g_Ui.worldMisc.lockHour, path);
        g_Ui.worldMisc.lockHour = (float)hour;
    }
    B("worldMisc", "clearWx", &g_Ui.worldMisc.clearWeather);
    B("worldMisc", "thirdPerson", &g_Ui.worldMisc.thirdPerson);
    {
        int d = GetPrivateProfileIntA("worldMisc", "thirdPersonDistCm", (int)(g_Ui.worldMisc.thirdPersonDistanceM * 100.f), path);
        g_Ui.worldMisc.thirdPersonDistanceM = d / 100.f;
        int h = GetPrivateProfileIntA("worldMisc", "thirdPersonHeightCm", (int)(g_Ui.worldMisc.thirdPersonHeightM * 100.f), path);
        g_Ui.worldMisc.thirdPersonHeightM = h / 100.f;
    }
    B("worldMisc", "streamProof", &g_Ui.worldMisc.streamProof);
    g_Ui.streamProof = false;
    g_Ui.worldMisc.streamProof = false;
    B("worldMisc", "wireframe", &g_Ui.worldMisc.wireframe);
    B("combat.batch4", "silentAim", &g_Ui.silentAim.enabled);
    I("combat.batch4", "silentDist", &g_Ui.silentAim.maxDistanceM, 20, OAK_CAP_DIST_M);
    I("combat.batch4", "silentFov", &g_Ui.silentAim.fovPx, 20, 800);
    I("combat.batch4", "silentBone", &g_Ui.silentAim.bone, 0, 1);
    I("combat.batch4", "silentKey", &g_Ui.silentAim.key, 0, 255);
    B("combat.batch4", "silentBypass25", &g_Ui.silentAim.bypass25m);
    B("combat.batch4", "silentDrawFov", &g_Ui.silentAim.drawFov);
    B("combat.batch4", "silentDrawLock", &g_Ui.silentAim.drawLock);
    B("combat.batch4", "silentPlayers", &g_Ui.silentAim.players);
    B("combat.batch4", "silentZombies", &g_Ui.silentAim.zombies);
    B("combat.batch4", "triggerbot", &g_Ui.triggerbot.enabled);
    I("combat.batch4", "triggerDelay", &g_Ui.triggerbot.delayMs, 0, 500);
    I("combat.batch4", "triggerDist", &g_Ui.triggerbot.maxDistanceM, 20, OAK_CAP_DIST_M);
    I("combat.batch4", "triggerDz", &g_Ui.triggerbot.deadzonePx, 4, 120);
    B("combat.batch4", "triggerAds", &g_Ui.triggerbot.requireAds);
    B("combat.batch4", "triggerLos", &g_Ui.triggerbot.requireLos);
    B("combat.batch4", "triggerPlayers", &g_Ui.triggerbot.players);
    B("combat.batch4", "triggerZombies", &g_Ui.triggerbot.zombies);
    B("combat.batch4", "noRecoil", &g_Ui.recoil.noRecoil);
    B("combat.batch4", "noSway", &g_Ui.recoil.noSway);
    I("combat.batch4", "recoilPct", &g_Ui.recoil.recoilPct, 0, 100);
    I("combat.batch4", "swayPct", &g_Ui.recoil.swayPct, 0, 100);
    B("combat.batch4", "autoFire", &g_Ui.aimbotB4.autoFireWhenLocked);
    I("combat.batch4", "aimAssistKey", &g_Ui.aimbotB4.aimAssistKey, 0, 255);
    I("bind", "silentAim", &g_Ui.bindSilentAim, 0, 255);
    I("bind", "aimAssist", &g_Ui.bindAimAssist, 0, 255);
    B("exploits", "warp", &g_Ui.exploits.warp);
    {
        int warpM = GetPrivateProfileIntA("exploits", "warpDist", (int)g_Ui.exploits.warpDistanceM, path);
        g_Ui.exploits.warpDistanceM = (float)warpM;
    }
    I("exploits", "warpKey", &g_Ui.exploits.warpKey, 0, 255);
    I("exploits", "warpCd", &g_Ui.exploits.warpCooldownMs, 200, 8000);
    I("exploits", "warpMode", &g_Ui.exploits.warpMode, 0, 2);
    B("exploits", "warpFlat", &g_Ui.exploits.warpFlat);
    B("exploits", "warpChain", &g_Ui.exploits.warpHoldChain);
    B("exploits", "grenadeWalls", &g_Ui.exploits.grenadeThroughWalls);
    B("exploits", "doorUnlock", &g_Ui.exploits.doorUnlock);

    I("bind", "esp", &g_Ui.bindEsp, 0, 255);
    I("bind", "aimbot", &g_Ui.bindAimbot, 0, 255);
    I("bind", "magic", &g_Ui.bindMagicBullet, 0, 255);
    I("bind", "fullbright", &g_Ui.bindFullbright, 0, 255);
    I("bind", "despawn", &g_Ui.bindMiddleClickDespawn, 0, 255);
    I("bind", "lootMagnet", &g_Ui.bindLootMagnet, 0, 255);
    I("bind", "contMagnet", &g_Ui.bindContainerMagnet, 0, 255);
    I("bind", "daytime", &g_Ui.bindDaytimeLock, 0, 255);
    I("bind", "overlays", &g_Ui.bindDisableOverlays, 0, 255);
    I("bind", "panic", &g_Ui.bindPanic, 0, 255);
    I("bind", "addWp", &g_Ui.bindAddWaypoint, 0, 255);
    I("bind", "copyCoords", &g_Ui.bindCopyCoords, 0, 255);
    I("bind", "steamNames", &g_Ui.bindSteamNames, 0, 255);
    I("bind", "pullBase", &g_Ui.bindPullBasePart, 0, 255);
    I("bind", "freecam", &g_Ui.bindFreecam, 0, 255);

    I("wp", "count", &g_Ui.waypointCount, 0, OAK_WAYPOINT_MAX);
    for (int i = 0; i < g_Ui.waypointCount && i < OAK_WAYPOINT_MAX; i++)
    {
        char k[32];
        int xi = 0, yi = 0, zi = 0;
        wsprintfA(k, "x%d", i); xi = GetPrivateProfileIntA("wp", k, 0, path);
        wsprintfA(k, "y%d", i); yi = GetPrivateProfileIntA("wp", k, 0, path);
        wsprintfA(k, "z%d", i); zi = GetPrivateProfileIntA("wp", k, 0, path);
        g_Ui.waypointX[i] = xi / 100.f;
        g_Ui.waypointY[i] = yi / 100.f;
        g_Ui.waypointZ[i] = zi / 100.f;
        wsprintfA(k, "n%d", i);
        GetPrivateProfileStringA("wp", k, g_Ui.waypointName[i], g_Ui.waypointName[i], 32, path);
        wsprintfA(k, "c%d", i);
        OakReadFloat4Arr(path, "wp", k, g_Ui.waypointColor[i]);
    }

    I("friends", "count", &g_Ui.friendCount, 0, 16);
    for (int i = 0; i < g_Ui.friendCount && i < 16; i++)
    {
        char k[32], sid[40] = {};
        wsprintfA(k, "n%d", i);
        GetPrivateProfileStringA("friends", k, "", g_Ui.friendName[i], 64, path);
        wsprintfA(k, "id%d", i);
        GetPrivateProfileStringA("friends", k, "0", sid, sizeof(sid), path);
        g_Ui.friendSteamId[i] = _strtoui64(sid, nullptr, 10);
    }

    B("loot", "weapons", &g_Ui.lootShowWeapons);
    B("loot", "ammo", &g_Ui.lootShowAmmo);
    B("loot", "medical", &g_Ui.lootShowMedical);
    B("loot", "food", &g_Ui.lootShowFood);
    B("loot", "clothing", &g_Ui.lootShowClothing);
    B("loot", "tools", &g_Ui.lootShowTools);
    B("loot", "other", &g_Ui.lootShowOther);

    OakReadPerfSection(path, &g_Ui.perf);
    OakReadCorpseTrapSections(path);
    OakReadLootStyleSections(path);
    OakReadFeaturesSections(path);

    OakUiLayoutLoad(path);

    OakForceDeletedModulesOff();

    OakReadFloat4(path, "color", "playerBox", &g_Ui.colorPlayerBox);
    OakReadFloat4(path, "color", "playerSkel", &g_Ui.colorPlayerSkeleton);
    OakReadFloat4(path, "color", "playerName", &g_Ui.colorPlayerName);
    OakReadFloat4(path, "color", "playerChams", &g_Ui.colorPlayerChams);
    OakReadFloat4(path, "color", "zombieBox", &g_Ui.colorZombieBox);
    OakReadFloat4(path, "color", "crosshair", &g_Ui.colorCrosshair);
    OakReadFloat4(path, "color", "tracer", &g_Ui.colorTracer);
    OakReadFloat4(path, "color", "impact", &g_Ui.colorImpact);
    OakReadFloat4(path, "color", "grenade", &g_Ui.colorGrenade);
    OakReadFloat4(path, "color", "itemBox", &g_Ui.colorItemBox);
    OakReadFloat4(path, "color", "itemName", &g_Ui.colorItemName);
    OakReadFloat4(path, "color", "container", &g_Ui.colorContainer);
    OakReadFloat4(path, "color", "corpse", &g_Ui.colorCorpse);
    OakReadFloat4(path, "color", "trap", &g_Ui.colorTrap);
    // F8 is hardcoded QA dump — never leave freecam on the same key.
    // Keep freecam off shared binds (F6 wp / F7 coords / F8 QA / F9 panic).
    // Default/toggle key is U. Migrate old F10 default + never strip U.
    if (g_Ui.bindFreecam == 0 || g_Ui.bindFreecam == VK_F6 || g_Ui.bindFreecam == VK_F7 ||
        g_Ui.bindFreecam == VK_F8 || g_Ui.bindFreecam == VK_F9 || g_Ui.bindFreecam == VK_F10)
        g_Ui.bindFreecam = 'U';
    ImGuiMenu_Log("config: loaded");
}

void ImGuiMenu_SaveNamedProfile(const char* name)
{
    if (!name || !name[0]) return;
    ImGuiMenu_SaveConfig();
    char src[MAX_PATH] = {}, dst[MAX_PATH] = {};
    OakConfigPath(src, MAX_PATH);
    OakNamedProfilePath(dst, MAX_PATH, name);
    if (src[0] && dst[0])
        CopyFileA(src, dst, FALSE);
    char msg[128];
    wsprintfA(msg, "profile: saved named '%s'", name);
    ImGuiMenu_Log(msg);
}

void ImGuiMenu_LoadNamedProfile(const char* name)
{
    if (!name || !name[0]) return;
    char src[MAX_PATH] = {}, dst[MAX_PATH] = {};
    OakNamedProfilePath(src, MAX_PATH, name);
    if (!src[0] || GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES)
        return;
    OakConfigPath(dst, MAX_PATH);
    if (!dst[0]) return;
    CopyFileA(src, dst, FALSE);
    ImGuiMenu_LoadConfig();
    char msg[128];
    wsprintfA(msg, "profile: loaded named '%s'", name);
    ImGuiMenu_Log(msg);
}

void ImGuiMenu_ExportConfig()
{
    char src[MAX_PATH] = {}, dst[MAX_PATH] = {};
    ImGuiMenu_SaveConfig();
    OakConfigPath(src, MAX_PATH);
    OakExportConfigPath(dst, MAX_PATH);
    if (src[0] && dst[0])
        CopyFileA(src, dst, FALSE);
    ImGuiMenu_Log("config: exported to oak_config_export.ini");
}

void ImGuiMenu_ImportConfig()
{
    char src[MAX_PATH] = {}, dst[MAX_PATH] = {};
    OakExportConfigPath(src, MAX_PATH);
    if (!src[0] || GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES)
        return;
    OakConfigPath(dst, MAX_PATH);
    if (!dst[0]) return;
    CopyFileA(src, dst, FALSE);
    ImGuiMenu_LoadConfig();
    ImGuiMenu_Log("config: imported from oak_config_export.ini");
}

void ImGuiMenu_ResetAll()
{
    ResetUiDefaults();
    ImGuiMenu_Log("config: reset all");
}

void ImGuiMenu_ResetSection(const char* section)
{
    if (!section || !section[0]) return;
    if (_stricmp(section, "performance") == 0)
    {
        g_Ui.perf = OakPerfLimits{};
        OakClampPerfLimits(&g_Ui.perf);
    }
    else if (_stricmp(section, "loot") == 0)
    {
        OakInitLootCatDefaults();
        g_Ui.lootSortMode = OakLootSortDistance;
        g_Ui.lootFilterBlacklistCount = 0;
        g_Ui.lootFilterWhitelistCount = 0;
        OakSyncLootShowFromCats();
    }
    else if (_stricmp(section, "esp") == 0)
    {
        g_Ui.playerStyle = OakEspStyleSettings{};
        g_Ui.zombieStyle = OakEspStyleSettings{};
        g_Ui.playerStyle.boxStyle = OakBoxCorner;
        g_Ui.zombieStyle.boxStyle = OakBoxCorner;
    }
    else if (_stricmp(section, "combat") == 0)
    {
        g_Ui.aimExtras = OakAimbotExtras{};
        g_Ui.aimTargetFilter = OakAimPlayers;
        OakSyncAimbotBoolsFromFilter(g_Ui.aimTargetFilter, &g_Ui.aimbotPlayers, &g_Ui.aimbotZombies);
    }
    else if (_stricmp(section, "corpses") == 0)
        g_Ui.corpseEsp = OakCorpseEspSettings{};
    else if (_stricmp(section, "traps") == 0)
        g_Ui.trapEsp = OakTrapEspSettings{};
    else
        return;
    char msg[96];
    wsprintfA(msg, "config: reset section %s", section);
    ImGuiMenu_Log(msg);
}

void ImGuiMenu_SetPlayerPos(float x, float y, float z)
{
    g_Ui.posX = x;
    g_Ui.posY = y;
    g_Ui.posZ = z;
}

static void OakUnlockOsCursor()
{
    ClipCursor(nullptr);
    ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {}
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW
}

static float g_MenuCursorX = 0.f;
static float g_MenuCursorY = 0.f;
static bool g_MenuCursorInit = false;
static volatile LONG g_MenuRawDx = 0;
static volatile LONG g_MenuRawDy = 0;
static volatile LONG g_ConfigSaveQueued = 0;

void ImGuiMenu_OnRawMouse(int dx, int dy)
{
    if (!g_MenuOpen)
        return;
    if (dx) InterlockedAdd(&g_MenuRawDx, (LONG)dx);
    if (dy) InterlockedAdd(&g_MenuRawDy, (LONG)dy);
}

static DWORD WINAPI OakConfigSaveThread(LPVOID)
{
    ImGuiMenu_SaveConfig();
    InterlockedExchange(&g_ConfigSaveQueued, 0);
    return 0;
}

static void OakQueueConfigSave()
{
    // WritePrivateProfile* on the Present thread freezes DayZ for hundreds of ms.
    if (InterlockedCompareExchange(&g_ConfigSaveQueued, 1, 0) != 0)
        return;
    HANDLE h = CreateThread(nullptr, 0, OakConfigSaveThread, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
    else
    {
        InterlockedExchange(&g_ConfigSaveQueued, 0);
        ImGuiMenu_SaveConfig();
    }
}

static void OakSetGameRawMouse(bool enable)
{
    // DayZ uses raw input + cursor recenter for look. While the menu is open,
    // keep raw mouse registered so we can drive a virtual ImGui cursor, and
    // swallow WM_INPUT in the message hook so the game does not look/recenter.
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02; // mouse
    if (!enable)
    {
        // Menu open: own raw mouse (INPUTSINK) — do not REMOVE or we get no deltas.
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = g_Hwnd;
    }
    else
    {
        // Best-effort restore — DayZ may re-register itself on next focus.
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = g_Hwnd;
    }
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        char buf[96];
        wsprintfA(buf, "rawmouse enable=%d err=%u", enable ? 1 : 0, GetLastError());
        ImGuiMenu_Log(buf);
    }
}

static DWORD WINAPI OakSoftCursorThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_SoftRun, 0, 0) != 0)
    {
        if (InterlockedCompareExchange(&g_SoftActive, 0, 0) != 0)
            OakUnlockOsCursor();
        Sleep(8);
    }
    return 0;
}

static void OakEnsureSoftCursorThread()
{
    if (g_SoftThread)
        return;
    InterlockedExchange(&g_SoftRun, 1);
    g_SoftThread = CreateThread(nullptr, 0, OakSoftCursorThread, nullptr, 0, nullptr);
}

static void OakStopSoftCursorThread()
{
    InterlockedExchange(&g_SoftActive, 0);
    InterlockedExchange(&g_SoftRun, 0);
    if (g_SoftThread)
    {
        WaitForSingleObject(g_SoftThread, 2000);
        CloseHandle(g_SoftThread);
        g_SoftThread = nullptr;
    }
}

static void OakSetMenuInputActive(bool active)
{
    if (active)
    {
        OakEnsureSoftCursorThread();
        OakSetGameRawMouse(false);
        OakUnlockOsCursor();
        InterlockedExchange(&g_SoftActive, 1);
        InterlockedExchange(&g_MenuRawDx, 0);
        InterlockedExchange(&g_MenuRawDy, 0);
        g_MenuCursorInit = false;
        ImGuiMenu_Log("menu input: mouse unlocked");
    }
    else
    {
        InterlockedExchange(&g_SoftActive, 0);
        OakSetGameRawMouse(true);
        InterlockedExchange(&g_MenuRawDx, 0);
        InterlockedExchange(&g_MenuRawDy, 0);
        g_MenuCursorInit = false;
        ImGuiMenu_Log("menu input: mouse restored");
    }
}

// No WndProc subclass (CFG kills manual-map). Poll Win32 input into ImGui each frame.
static void OakPollImGuiInput()
{
    if (!g_Hwnd)
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_MenuOpen;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Menu closed: skip full VK/mouse sweep (was ~80 GetAsyncKeyState/frame). Menu key
    // is polled in BeginFrame; gameplay binds use their own KeyHeld paths.
    if (!g_MenuOpen)
    {
        io.AddMousePosEvent(-100000.f, -100000.f);
        return;
    }

    OakUnlockOsCursor();

    RECT rc = {};
    GetClientRect(g_Hwnd, &rc);
    const float maxX = (float)(rc.right > 1 ? rc.right - 1 : 1);
    const float maxY = (float)(rc.bottom > 1 ? rc.bottom - 1 : 1);

    if (!g_MenuCursorInit)
    {
        POINT pt = {};
        if (GetCursorPos(&pt) && ScreenToClient(g_Hwnd, &pt))
        {
            g_MenuCursorX = (float)pt.x;
            g_MenuCursorY = (float)pt.y;
        }
        else
        {
            g_MenuCursorX = maxX * 0.5f;
            g_MenuCursorY = maxY * 0.5f;
        }
        g_MenuCursorInit = true;
    }

    // DayZ recenters the OS cursor every frame — drive ImGui from raw deltas instead.
    const LONG dx = InterlockedExchange(&g_MenuRawDx, 0);
    const LONG dy = InterlockedExchange(&g_MenuRawDy, 0);
    if (dx || dy)
    {
        g_MenuCursorX += (float)dx;
        g_MenuCursorY += (float)dy;
    }
    else
    {
        // Fallback if raw path is quiet (e.g. before first WM_INPUT).
        POINT pt = {};
        if (GetCursorPos(&pt) && ScreenToClient(g_Hwnd, &pt))
        {
            // Only accept OS position when it actually moved (not a recenter snap to mid).
            const float ox = (float)pt.x, oy = (float)pt.y;
            const float midX = maxX * 0.5f, midY = maxY * 0.5f;
            const float adx = (ox >= midX) ? (ox - midX) : (midX - ox);
            const float ady = (oy >= midY) ? (oy - midY) : (midY - oy);
            const bool nearCenter = (adx < 3.f && ady < 3.f);
            if (!nearCenter)
            {
                g_MenuCursorX = ox;
                g_MenuCursorY = oy;
            }
        }
    }

    if (g_MenuCursorX < 0.f) g_MenuCursorX = 0.f;
    if (g_MenuCursorY < 0.f) g_MenuCursorY = 0.f;
    if (g_MenuCursorX > maxX) g_MenuCursorX = maxX;
    if (g_MenuCursorY > maxY) g_MenuCursorY = maxY;
    io.AddMousePosEvent(g_MenuCursorX, g_MenuCursorY);

    // Mouse buttons
    static bool s_MouseDown[5] = {};
    const int mouseVk[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    for (int i = 0; i < 5; i++)
    {
        bool down = (GetAsyncKeyState(mouseVk[i]) & 0x8000) != 0;
        if (down != s_MouseDown[i])
        {
            io.AddMouseButtonEvent(i, down);
            s_MouseDown[i] = down;
        }
    }

    // Mods
    io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);

    // Keyboard — edge-detect a useful VK set
    static const int s_Keys[] = {
        VK_TAB, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT, VK_HOME, VK_END,
        VK_INSERT, VK_DELETE, VK_BACK, VK_SPACE, VK_RETURN, VK_ESCAPE,
        VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7,
        VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_PLUS,
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        '0','1','2','3','4','5','6','7','8','9',
        'A','B','C','D','E','F','G','H','I','J','K','L','M',
        'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    };
    static bool s_KeyDown[sizeof(s_Keys) / sizeof(s_Keys[0])] = {};
    for (int i = 0; i < (int)(sizeof(s_Keys) / sizeof(s_Keys[0])); i++)
    {
        int vk = s_Keys[i];
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down == s_KeyDown[i])
            continue;
        s_KeyDown[i] = down;
        ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey((WPARAM)vk, 0);
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, down);

        if (down && g_MenuOpen)
        {
            BYTE state[256];
            if (GetKeyboardState(state))
            {
                WCHAR chars[4] = {};
                int n = ToUnicode((UINT)vk, MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC), state, chars, 4, 0);
                for (int c = 0; c < n; c++)
                {
                    if (chars[c] >= 32 && chars[c] != 127)
                        io.AddInputCharacterUTF16((ImWchar16)chars[c]);
                }
            }
        }
    }
}

void ImGuiMenu_SetBatch4Status(const char* door)
{
    if (door) lstrcpynA(g_Batch4StatusDoor, door, 72);
}

void ImGuiMenu_SetWarpStatus(const char* note)
{
    if (note && note[0])
        lstrcpynA(g_Batch4StatusWarp, note, 72);
}

static char g_PerfBoostStatus[120] = "idle";

void ImGuiMenu_SetPerfBoostStatus(const char* note)
{
    if (!note) return;
    lstrcpynA(g_PerfBoostStatus, note, 120);
}

const char* ImGuiMenu_GetPerfBoostStatus()
{
    return g_PerfBoostStatus;
}

void ImGuiMenu_SetExploitWarp(bool on)
{
    g_Ui.exploits.warp = on;
}

void ImGuiMenu_SetRecoilStatus(bool ok, const char* note)
{
    g_RecoilStatusOk = ok;
    if (note && note[0])
        lstrcpynA(g_RecoilStatusNote, note, 96);
}

const char* ImGuiMenu_RecoilNote()
{
    return g_RecoilStatusNote;
}

bool ImGuiMenu_RecoilOk()
{
    return g_RecoilStatusOk;
}

bool ImGuiMenu_BeginFrame()
{
    if (!g_ImGuiReady || !g_ImGuiWarmed)
        return false;

    const bool wasOpen = g_MenuOpen;
    if (GetAsyncKeyState(g_Ui.menuKey) & 1)
    {
        g_MenuOpen = !g_MenuOpen;
        ImGuiMenu_Log(g_MenuOpen ? "menu: OPEN" : "menu: CLOSED");
    }

    if (g_MenuOpen != wasOpen)
    {
        OakSetMenuInputActive(g_MenuOpen);
        if (!g_MenuOpen)
            OakQueueConfigSave();
    }

    PollBindCapture();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    OakPollImGuiInput();
    ImGui::NewFrame();
    return true;
}

ImDrawList* ImGuiMenu_EspDrawList()
{
    if (!g_ImGuiReady || !g_ImGuiWarmed)
        return nullptr;
    return ImGui::GetBackgroundDrawList();
}

void ImGuiMenu_EndFrame(ID3D11RenderTargetView* rtv)
{
    if (!g_ImGuiReady || !g_ImGuiWarmed || !g_Ctx || !rtv)
        return;

    if (g_MenuOpen)
        DrawMenu();

    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    g_Ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(dd);

    g_PresentFrames++;
    if (dd && dd->TotalVtxCount > 0)
        g_WasDrawn = true;

    // Beacon for the launcher: kernel inject is not enough — wait until we
    // actually submit ImGui/ESP vertices.
    if (g_PresentFrames == 1 || (g_WasDrawn && g_PresentFrames == 30))
    {
        const char* paths[2] = { "C:\\oak\\dayz\\overlay_ready.flag", "C:\\oak\\overlay_ready.flag" };
        for (int i = 0; i < 2; i++)
        {
            HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
                char msg[64];
                wsprintfA(msg, "present=%u vtx=%d\r\n",
                    g_PresentFrames, dd ? dd->TotalVtxCount : 0);
                DWORD w = 0;
                WriteFile(hf, msg, (DWORD)lstrlenA(msg), &w, NULL);
                CloseHandle(hf);
            }
        }
    }

    if (g_PresentFrames == 1 || g_PresentFrames == 60 || (g_PresentFrames % 3600) == 0
        || (g_MenuOpen && (g_PresentFrames % 10) == 0))
    {
        char buf[160];
        wsprintfA(buf, "present#%u open=%d vtx=%d idx=%d",
            g_PresentFrames,
            g_MenuOpen ? 1 : 0,
            dd ? dd->TotalVtxCount : 0,
            dd ? dd->TotalIdxCount : 0);
        ImGuiMenu_Log(buf);
    }
}

void ImGuiMenu_OnPresent(ID3D11RenderTargetView* rtv)
{
    if (!ImGuiMenu_BeginFrame())
        return;
    ImGuiMenu_EndFrame(rtv);
}

void ImGuiMenu_Shutdown()
{
    ImGuiMenu_Log("ImGuiMenu_Shutdown");
    OakStopSoftCursorThread();
    if (!g_ImGuiReady)
        return;

    if (g_Hwnd && g_OrigWndProc)
    {
        SetWindowLongPtrW(g_Hwnd, GWLP_WNDPROC, (LONG_PTR)g_OrigWndProc);
        g_OrigWndProc = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    g_ImGuiReady = false;
    g_UiReady = false;
    g_Hwnd = nullptr;
    g_Ctx = nullptr;
}

bool ImGuiMenu_IsOpen()
{
    return g_MenuOpen;
}

void ImGuiMenu_SetOpen(bool open)
{
    if (open == g_MenuOpen)
        return;
    g_MenuOpen = open;
    if (g_ImGuiWarmed)
        OakSetMenuInputActive(open);
}
