// Crash matrix — in-process module soak driven by C:\oak\dayz\oak_crash_matrix.flag
// Included from main.cpp (Present / gameplay path).
//
// External harness drops the flag; this runner cycles every shipped module,
// logs crashmatrix[step] breadcrumbs, and attributes VEH/gates to the active step.

struct OakCrashMatrixStep
{
    const char* id;
    DWORD holdMs;
};

static const OakCrashMatrixStep kOakCrashMatrixSteps[] = {
    { "baseline", 2500 },
    { "esp_players_full", 5000 },
    { "esp_zombies_full", 5000 },
    { "esp_items", 4000 },
    { "esp_world", 4000 },
    { "ammo_fast", 4000 },
    { "ammo_nodisp", 4000 },
    { "ammo_perfect", 4000 },
    { "ammo_all", 5000 },
    { "recoil", 5000 },
    { "fov", 5000 },
    { "thirdperson", 5000 },
    { "streamproof", 4000 },
    { "fullbright", 4000 },
    { "weather_time", 4000 },
    { "combat_visuals", 4000 },
    { "aimbot", 5000 },
    { "magic_bullet", 5000 },
    { "silent_aim", 4000 },
    { "door_unlock", 4000 },
    { "loot_magnet", 4000 },
    { "grenade_tp", 4000 },
    { "lag_switch", 4000 },
    { "looking_inv", 4000 },
    { "frame_boost", 4000 },
    { "freecam", 6000 },
    { "thrash_esp", 6000 },
    { "combo_esp", 6000 },
    { "combo_combat", 7000 },
    { "combo_world", 6000 },
    { "combo_kitchen_sink", 10000 },
    { "baseline", 3000 },
};

enum { kOakCrashMatrixStepCount = (int)(sizeof(kOakCrashMatrixSteps) / sizeof(kOakCrashMatrixSteps[0])) };

static bool g_OakCrashMatrixWasArmed = false;
static bool g_OakCrashMatrixDone = false;
static int g_OakCrashMatrixIdx = -1;
static DWORD g_OakCrashMatrixStepStart = 0;
static DWORD g_OakCrashMatrixLastBeat = 0;
static DWORD g_OakCrashMatrixThrash = 0;
static bool g_OakCrashMatrixThrashOn = true;
static long g_OakCrashMatrixVehAtStep = 0;
static long g_OakCrashMatrixGateAtStep = 0;
static char g_OakCrashMatrixLastId[48] = "none";

static void OakCrashMatrixLog(const char* phase, const char* id, int idx)
{
    char b[220];
    wsprintfA(b,
        "crashmatrix[%s] i=%d/%d id=%s veh=%d gates=%d fatal=%d",
        phase ? phase : "?",
        idx + 1, kOakCrashMatrixStepCount,
        id ? id : "?",
        (int)OakCrashHunt_VehCount(),
        (int)OakCrashHunt_GateCount(),
        (int)OakCrashHunt_FatalCount());
    Log(b);
}

static void OakCrashMatrix_Tick()
{
    const bool armed = ImGuiMenu_CrashMatrixActive();
    if (!armed)
    {
        if (g_OakCrashMatrixWasArmed)
        {
            OakCrashMatrixLog("disarmed", g_OakCrashMatrixLastId, g_OakCrashMatrixIdx >= 0 ? g_OakCrashMatrixIdx : 0);
            ImGuiMenu_ApplyCrashMatrixStep("baseline");
            g_OakCrashMatrixWasArmed = false;
            g_OakCrashMatrixDone = false;
            g_OakCrashMatrixIdx = -1;
            Log("crashmatrix: flag cleared — restored baseline UI");
        }
        return;
    }

    if (!g_LocalPlayerValid || !g_CameraValid)
    {
        static DWORD s_WaitLog = 0;
        DWORD now = GetTickCount();
        if (!s_WaitLog || (now - s_WaitLog) > 3000)
        {
            s_WaitLog = now;
            Log("crashmatrix: waiting for local+camera before steps");
        }
        return;
    }

    if (!g_OakCrashMatrixWasArmed)
    {
        g_OakCrashMatrixWasArmed = true;
        g_OakCrashMatrixDone = false;
        g_OakCrashMatrixIdx = -1;
        g_OakCrashMatrixStepStart = 0;
        Log("crashmatrix: ARMED — cycling all modules (do not touch menu)");
        OakCrashHunt_LogSummary();
    }

    if (g_OakCrashMatrixDone)
        return;

    DWORD now = GetTickCount();

    if (g_OakCrashMatrixIdx < 0 ||
        (g_OakCrashMatrixStepStart &&
         (now - g_OakCrashMatrixStepStart) >= kOakCrashMatrixSteps[g_OakCrashMatrixIdx].holdMs))
    {
        if (g_OakCrashMatrixIdx >= 0)
        {
            const long vehNow = OakCrashHunt_VehCount();
            const long gateNow = OakCrashHunt_GateCount();
            if (vehNow > g_OakCrashMatrixVehAtStep || gateNow > g_OakCrashMatrixGateAtStep)
            {
                char fb[180];
                wsprintfA(fb,
                    "crashmatrix[FAULT] id=%s dVeh=%d dGate=%d (module may be unsafe)",
                    g_OakCrashMatrixLastId,
                    (int)(vehNow - g_OakCrashMatrixVehAtStep),
                    (int)(gateNow - g_OakCrashMatrixGateAtStep));
                Log(fb);
            }
            OakCrashMatrixLog("pass", g_OakCrashMatrixLastId, g_OakCrashMatrixIdx);
        }

        g_OakCrashMatrixIdx++;
        if (g_OakCrashMatrixIdx >= kOakCrashMatrixStepCount)
        {
            OakCrashMatrixLog("COMPLETE", "done", kOakCrashMatrixStepCount - 1);
            OakCrashHunt_LogSummary();
            ImGuiMenu_ApplyCrashMatrixStep("baseline");
            g_OakCrashMatrixDone = true;
            lstrcpynA(g_OakCrashMatrixLastId, "COMPLETE", 48);
            return;
        }

        const OakCrashMatrixStep& st = kOakCrashMatrixSteps[g_OakCrashMatrixIdx];
        if (!ImGuiMenu_ApplyCrashMatrixStep(st.id))
        {
            char ub[96];
            wsprintfA(ub, "crashmatrix[skip] unknown id=%s", st.id);
            Log(ub);
        }
        lstrcpynA(g_OakCrashMatrixLastId, st.id, 48);
        g_OakCrashMatrixStepStart = now;
        g_OakCrashMatrixVehAtStep = OakCrashHunt_VehCount();
        g_OakCrashMatrixGateAtStep = OakCrashHunt_GateCount();
        g_OakCrashMatrixThrash = now;
        g_OakCrashMatrixThrashOn = true;
        OakCrashMatrixLog("arm", st.id, g_OakCrashMatrixIdx);
    }

    if (g_OakCrashMatrixIdx >= 0 && g_OakCrashMatrixIdx < kOakCrashMatrixStepCount &&
        lstrcmpiA(kOakCrashMatrixSteps[g_OakCrashMatrixIdx].id, "thrash_esp") == 0)
    {
        if ((now - g_OakCrashMatrixThrash) > 350)
        {
            g_OakCrashMatrixThrash = now;
            g_OakCrashMatrixThrashOn = !g_OakCrashMatrixThrashOn;
            ImGuiMenu_ApplyCrashMatrixStep(g_OakCrashMatrixThrashOn ? "thrash_esp" : "esp_off");
        }
    }

    if (!g_OakCrashMatrixLastBeat || (now - g_OakCrashMatrixLastBeat) > 2000)
    {
        g_OakCrashMatrixLastBeat = now;
        if (g_OakCrashMatrixIdx >= 0 && g_OakCrashMatrixIdx < kOakCrashMatrixStepCount)
            OakCrashMatrixLog("hold", g_OakCrashMatrixLastId, g_OakCrashMatrixIdx);
    }
}
