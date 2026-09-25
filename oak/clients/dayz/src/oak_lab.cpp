#include "oak_lab.h"

#include <string.h>
#include <math.h>

static void LabLog(const char* line)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
        return;
    char file[MAX_PATH];
    wsprintfA(file, "%s\\DayZ\\oak_imgui.log", path);
    HANDLE hf = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    DWORD wr = 0;
    WriteFile(hf, line, (DWORD)lstrlenA(line), &wr, nullptr);
    WriteFile(hf, "\r\n", 2, &wr, nullptr);
    CloseHandle(hf);
}

static const char* const kModNames[OAK_LAB_COUNT] = {
    "none", "esp", "freecam", "fov", "stamina", "speed", "noclip", "warp",
    "thirdperson", "streamproof", "mb", "silent", "wallbypass",
    "ammo", "grenade", "triggerbot"
};

static int g_State[OAK_LAB_COUNT] = {};
static char g_LockReason[OAK_LAB_COUNT][96] = {};
static char g_Claim[OAK_LAB_COUNT][128] = {};
static int g_GameplayConfirm[OAK_LAB_COUNT] = {};
static volatile LONG g_Armed = OAK_LAB_NONE;
static volatile LONG g_SafeMode = 0; // PLAY by default — SAFE is opt-in
static volatile LONG g_Inited = 0;
static uintptr_t g_LocalPlayerHint = 0;

static DWORD g_ArmTick = 0;
static int g_SoakOk = 0;
static const DWORD kArmSoakMs = 20000; // crash-free window before Finalize can PASS

static int LabIsAlwaysSafeOverlay(int mod)
{
    // No game-memory ownership — always allowed (even SAFE / one-arm).
    return mod == OAK_LAB_ESP
        || mod == OAK_LAB_FREECAM
        || mod == OAK_LAB_STREAMPROOF
        || mod == OAK_LAB_NONE;
}

// Shipped gameplay features — work under PLAY and SAFE. Still respect LOCKED.
// One-arm Lab no longer bricks these (operator can soak another module while using them).
static int LabIsShippedFeature(int mod)
{
    return mod == OAK_LAB_SILENT
        || mod == OAK_LAB_MB
        || mod == OAK_LAB_FOV
        || mod == OAK_LAB_TRIGGERBOT
        || mod == OAK_LAB_STREAMPROOF;
}

const char* OakLab_ModuleName(int moduleId)
{
    if (moduleId < 0 || moduleId >= OAK_LAB_COUNT)
        return "?";
    return kModNames[moduleId];
}

int OakLab_GetState(int moduleId)
{
    if (moduleId < 0 || moduleId >= OAK_LAB_COUNT)
        return OAK_LAB_OFF;
    return g_State[moduleId];
}

int OakLab_GetArmedModule(void)
{
    return (int)InterlockedCompareExchange(&g_Armed, 0, 0);
}

int OakLab_IsWriteAllowed(int moduleId)
{
    if (moduleId < 0 || moduleId >= OAK_LAB_COUNT)
        return 0;
    if (g_State[moduleId] == OAK_LAB_LOCKED)
        return 0;
    if (LabIsAlwaysSafeOverlay(moduleId))
        return 1;
    // Verified shipped features ignore SAFE/one-arm bricks (toggle = on).
    if (LabIsShippedFeature(moduleId))
        return 1;
    // SAFE: kill experimental writers (crash triage / one-feature soak prep).
    if (InterlockedCompareExchange(&g_SafeMode, 0, 0) != 0)
        return 0;
    // ONE-ARM: only the armed module may write (Lab harness).
    const int armed = OakLab_GetArmedModule();
    if (armed > OAK_LAB_FREECAM && armed < OAK_LAB_COUNT)
        return (armed == moduleId && g_State[moduleId] == OAK_LAB_ARMED) ? 1 : 0;
    // PLAY: non-locked modules follow normal UI toggles (Lab does not brick gameplay).
    return 1;
}

int OakLab_NeedsProbeBeforeArm(int moduleId)
{
    // These failed because ownership was guessed without a drain/delta probe.
    // FOV uses hold-test (camera rebuild) instead — not sprint probe.
    return moduleId == OAK_LAB_STAMINA
        || moduleId == OAK_LAB_SPEED
        || moduleId == OAK_LAB_NOCLIP;
}

void OakLab_SetClaim(int moduleId, const char* claim)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return;
    lstrcpynA(g_Claim[moduleId], claim ? claim : "", 128);
    g_GameplayConfirm[moduleId] = 0;
    char b[200];
    wsprintfA(b, "lab[claim] module=%s text=%s", OakLab_ModuleName(moduleId),
        g_Claim[moduleId][0] ? g_Claim[moduleId] : "(cleared)");
    LabLog(b);
}

const char* OakLab_GetClaim(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return "";
    return g_Claim[moduleId];
}

void OakLab_ConfirmGameplay(int moduleId, int yes)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return;
    g_GameplayConfirm[moduleId] = yes ? 1 : 0;
    char b[120];
    wsprintfA(b, "lab[confirm] module=%s gameplay=%d", OakLab_ModuleName(moduleId), yes ? 1 : 0);
    LabLog(b);
}

int OakLab_HasGameplayConfirm(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return 0;
    return g_GameplayConfirm[moduleId];
}

void OakLab_Lock(int moduleId, const char* reason)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return;
    g_State[moduleId] = OAK_LAB_LOCKED;
    lstrcpynA(g_LockReason[moduleId], reason ? reason : "locked", 96);
    g_GameplayConfirm[moduleId] = 0;
    if (OakLab_GetArmedModule() == moduleId)
        InterlockedExchange(&g_Armed, OAK_LAB_NONE);
    char b[160];
    wsprintfA(b, "lab[lock] module=%s reason=%s", OakLab_ModuleName(moduleId),
        g_LockReason[moduleId]);
    LabLog(b);
}

void OakLab_Disarm(void)
{
    int a = OakLab_GetArmedModule();
    if (a > OAK_LAB_NONE && a < OAK_LAB_COUNT && g_State[a] == OAK_LAB_ARMED)
        g_State[a] = OAK_LAB_OFF;
    InterlockedExchange(&g_Armed, OAK_LAB_NONE);
    g_ArmTick = 0;
    g_SoakOk = 0;
    LabLog("lab[disarm]");
}

void OakLab_EnterSafeMode(const char* reason)
{
    InterlockedExchange(&g_SafeMode, 1);
    OakLab_Disarm();
    char b[160];
    wsprintfA(b, "lab[safe] reason=%s", reason ? reason : "safe");
    LabLog(b);
}

void OakLab_EnterPlayMode(const char* reason)
{
    InterlockedExchange(&g_SafeMode, 0);
    // Keep armed if any — leave one-arm until Disarm. Clearing safe alone is enough for PLAY.
    char b[160];
    wsprintfA(b, "lab[play] reason=%s", reason ? reason : "play");
    LabLog(b);
}

// ---- probe state (needed by Arm gate) ----
enum { kProbeMax = 96 };
struct ProbeSlot { uintptr_t addr; float v0; };
static ProbeSlot g_Probe[kProbeMax];
static int g_ProbeN = 0;
static DWORD g_ProbeT0 = 0;
static int g_ProbePhase = 0;
static char g_ProbeAction[24] = {};
static uintptr_t g_ProbeEntity = 0;
static int g_ProbeLastHits = 0;
static DWORD g_ProbeLastDoneTick = 0;

int OakLab_ProbeBusy(void) { return g_ProbePhase == 1 ? 1 : 0; }
int OakLab_ProbeLastHits(void) { return g_ProbeLastHits; }
DWORD OakLab_ProbeLastDoneTick(void) { return g_ProbeLastDoneTick; }

static int LabRecentProbeOk(void)
{
    if (!g_ProbeLastDoneTick)
        return 0;
    DWORD age = GetTickCount() - g_ProbeLastDoneTick;
    return age <= 120000; // 2 minutes
}

static int LabArmCommon(int moduleId, int force, const char* forceWhy)
{
    if (moduleId <= OAK_LAB_FREECAM || moduleId >= OAK_LAB_COUNT)
        return 0;
    if (g_State[moduleId] == OAK_LAB_LOCKED)
    {
        char b[160];
        wsprintfA(b, "lab[arm-deny] module=%s reason=%s", OakLab_ModuleName(moduleId),
            g_LockReason[moduleId][0] ? g_LockReason[moduleId] : "locked");
        LabLog(b);
        return 0;
    }
    if (!g_Claim[moduleId][0])
    {
        LabLog("lab[arm-deny] reason=no-claim (set a gameplay claim first — anti-guess)");
        return 0;
    }
    if (!force && OakLab_NeedsProbeBeforeArm(moduleId) && !LabRecentProbeOk())
    {
        LabLog("lab[arm-deny] reason=need-recent-probe (or ArmForce with why)");
        return 0;
    }
    if (force)
    {
        char b[160];
        wsprintfA(b, "lab[arm-force] module=%s why=%s", OakLab_ModuleName(moduleId),
            forceWhy ? forceWhy : "?");
        LabLog(b);
    }

    OakLab_Disarm();
    InterlockedExchange(&g_SafeMode, 0);
    g_State[moduleId] = OAK_LAB_ARMED;
    InterlockedExchange(&g_Armed, moduleId);
    g_ArmTick = GetTickCount();
    g_SoakOk = 0;
    g_GameplayConfirm[moduleId] = 0;

    char b[200];
    wsprintfA(b, "lab[arm] module=%s claim=%s soakMs=%u",
        OakLab_ModuleName(moduleId), g_Claim[moduleId], kArmSoakMs);
    LabLog(b);
    LabLog("lab[arm] PASS blocked until: soak OK + gameplay confirm + Finalize");
    return 1;
}

int OakLab_Arm(int moduleId)
{
    return LabArmCommon(moduleId, 0, nullptr);
}

int OakLab_ArmForce(int moduleId, const char* why)
{
    return LabArmCommon(moduleId, 1, why);
}

// ---- hold test ----
static int g_HoldPhase = 0; // 0 idle, 1 waiting
static int g_HoldMod = OAK_LAB_NONE;
static uintptr_t g_HoldAddr = 0;
static float g_HoldWant = 0.f;
static float g_HoldBefore = 0.f;
static DWORD g_HoldDeadline = 0;
static int g_HoldVerdict = OAK_LAB_HOLD_IDLE;

static int LabSafeCopy(void* dst, const void* src, unsigned n)
{
    if (!dst || !src || !n) return 0;
    __try {
        memcpy(dst, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static float LabReadF(uintptr_t a)
{
    float v = 0.f;
    if (!LabSafeCopy(&v, (const void*)a, sizeof(v)))
        return 0.f;
    return v;
}

int OakLab_HoldTestLastVerdict(void) { return g_HoldVerdict; }
uintptr_t OakLab_HoldTestLastAddr(void) { return g_HoldAddr; }
int OakLab_HoldTestBusy(void) { return g_HoldPhase == 1 ? 1 : 0; }

int OakLab_HoldTestStart(int moduleId, uintptr_t addr, float value, DWORD waitMs)
{
    if (!OakLab_IsWriteAllowed(moduleId))
    {
        LabLog("lab[hold] deny — module not armed");
        return 0;
    }
    if (addr < 0x10000 || waitMs < 50 || waitMs > 5000)
    {
        LabLog("lab[hold] deny — bad args");
        return 0;
    }
    g_HoldBefore = LabReadF(addr);
    if (!OakLab_WriteFloat(moduleId, addr, value, 0))
    {
        g_HoldVerdict = OAK_LAB_HOLD_FAULT;
        LabLog("lab[hold] write failed");
        return 0;
    }
    g_HoldMod = moduleId;
    g_HoldAddr = addr;
    g_HoldWant = value;
    g_HoldDeadline = GetTickCount() + waitMs;
    g_HoldPhase = 1;
    g_HoldVerdict = OAK_LAB_HOLD_BUSY;
    char b[160];
    wsprintfA(b, "lab[hold] start mod=%s addr=%p beforex1000=%d wantx1000=%d wait=%u",
        OakLab_ModuleName(moduleId), (void*)addr,
        (int)(g_HoldBefore * 1000.f + 0.5f), (int)(value * 1000.f + 0.5f), waitMs);
    LabLog(b);
    return 1;
}

void OakLab_HoldTestTick(void)
{
    if (g_HoldPhase != 1)
        return;
    if ((int)(GetTickCount() - g_HoldDeadline) < 0)
        return;

    float now = LabReadF(g_HoldAddr);
    float err = fabsf(now - g_HoldWant);
    float base = fabsf(g_HoldWant) > 0.01f ? fabsf(g_HoldWant) : 1.f;
    float errPct = (err / base) * 100.f;

    if (!(now == now))
    {
        g_HoldVerdict = OAK_LAB_HOLD_FAULT;
        LabLog("lab[hold] verdict=FAULT nan");
    }
    else if (errPct <= 8.f)
    {
        g_HoldVerdict = OAK_LAB_HOLD_HELD;
        LabLog("lab[hold] verdict=HELD (client-owned candidate — still need gameplay confirm)");
    }
    else
    {
        g_HoldVerdict = OAK_LAB_HOLD_OVERWRITTEN;
        char b[160];
        wsprintfA(b, "lab[hold] verdict=OVERWRITTEN gotx1000=%d errPct=%d (sim/server owned — do NOT claim PASS)",
            (int)(now * 1000.f + 0.5f), (int)(errPct + 0.5f));
        LabLog(b);
    }
    g_HoldPhase = 0;
}

int OakLab_Finalize(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return OAK_LAB_CONTRACT_FAIL;

    if (!g_Claim[moduleId][0])
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_FAIL, "no-claim");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (g_State[moduleId] == OAK_LAB_LOCKED)
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_FAIL, "locked");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (!g_SoakOk || OakLab_GetArmedModule() != moduleId)
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_PROBE, "need-armed-soak");
        return OAK_LAB_CONTRACT_PROBE;
    }
    if (!g_GameplayConfirm[moduleId])
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_PROBE, "need-gameplay-confirm");
        return OAK_LAB_CONTRACT_PROBE;
    }
    if (g_HoldVerdict == OAK_LAB_HOLD_OVERWRITTEN)
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_FAIL, "hold-overwritten-not-owned");
        return OAK_LAB_CONTRACT_FAIL;
    }
    // FOV: projection floats are a cache. HELD during continuous fight is weak;
    // OVERWRITTEN already failed above. Require gameplay confirm (already checked)
    // and at least one hold completed (not IDLE) so we didn't skip ownership.
    if (moduleId == OAK_LAB_FOV && g_HoldVerdict == OAK_LAB_HOLD_IDLE)
    {
        OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_PROBE, "fov-need-hold-test-first");
        return OAK_LAB_CONTRACT_PROBE;
    }
    if (moduleId == OAK_LAB_FOV && g_HoldVerdict == OAK_LAB_HOLD_HELD)
    {
        // HELD on cam+0xD0 while worker paused can still be a lie if visual unchanged —
        // gameplay confirm already required; log honesty note.
        LabLog("lab[finalize] FOV hold=HELD — still only PASS if CONFIRM SEEN was visual widen");
    }
    OakLab_LogContract(moduleId, OAK_LAB_CONTRACT_PASS, "claim+soak+gameplay-confirm");
    return OAK_LAB_CONTRACT_PASS;
}

void OakLab_GetPolicy(OakLabPolicy* out)
{
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    out->safeMode = InterlockedCompareExchange(&g_SafeMode, 0, 0) ? 1 : 0;
    out->armedModule = OakLab_GetArmedModule();
    out->soakOk = g_SoakOk;
    out->gameplayConfirmed = (out->armedModule > 0) ? g_GameplayConfirm[out->armedModule] : 0;
    out->lastHoldVerdict = g_HoldVerdict;
    out->probeHits = g_ProbeLastHits;
    out->allowFov = OakLab_IsWriteAllowed(OAK_LAB_FOV) ? 1 : 0;
    out->allowStamina = OakLab_IsWriteAllowed(OAK_LAB_STAMINA) ? 1 : 0;
    out->allowSpeed = OakLab_IsWriteAllowed(OAK_LAB_SPEED) ? 1 : 0;
    out->allowNoclip = OakLab_IsWriteAllowed(OAK_LAB_NOCLIP) ? 1 : 0;
    out->allowWarp = OakLab_IsWriteAllowed(OAK_LAB_WARP) ? 1 : 0;
    out->allowThirdPerson = OakLab_IsWriteAllowed(OAK_LAB_THIRDPERSON) ? 1 : 0;
    out->allowStreamProof = OakLab_IsWriteAllowed(OAK_LAB_STREAMPROOF) ? 1 : 0;
    out->allowMb = OakLab_IsWriteAllowed(OAK_LAB_MB) ? 1 : 0;
    out->allowSilent = OakLab_IsWriteAllowed(OAK_LAB_SILENT) ? 1 : 0;
    out->allowWallBypass = OakLab_IsWriteAllowed(OAK_LAB_WALLBYPASS) ? 1 : 0;
    out->allowAmmo = OakLab_IsWriteAllowed(OAK_LAB_AMMO) ? 1 : 0;
    out->allowGrenade = OakLab_IsWriteAllowed(OAK_LAB_GRENADE) ? 1 : 0;
    out->allowTriggerbot = OakLab_IsWriteAllowed(OAK_LAB_TRIGGERBOT) ? 1 : 0;
}

static volatile LONG g_ModStack[8] = {};
static volatile LONG g_ModDepth = 0;

void OakLab_PushModule(int moduleId)
{
    LONG d = InterlockedIncrement(&g_ModDepth) - 1;
    if (d >= 0 && d < 8)
        InterlockedExchange(&g_ModStack[d], moduleId);
}

void OakLab_PopModule(void)
{
    LONG d = InterlockedDecrement(&g_ModDepth);
    if (d < 0)
        InterlockedExchange(&g_ModDepth, 0);
}

int OakLab_CurrentModule(void)
{
    LONG d = InterlockedCompareExchange(&g_ModDepth, 0, 0);
    if (d <= 0 || d > 8)
        return OAK_LAB_NONE;
    return (int)InterlockedCompareExchange(&g_ModStack[d - 1], 0, 0);
}

enum { kLabLedgerN = 64 };
struct LabWriteRec {
    DWORD tick;
    int moduleId;
    uintptr_t addr;
    unsigned size;
    unsigned oldBits;
    unsigned newBits;
};
static LabWriteRec g_Ledger[kLabLedgerN] = {};
static volatile LONG g_LedgerIdx = 0;
static volatile LONG g_LedgerCount = 0;
static uintptr_t g_LastWriteAddr[OAK_LAB_COUNT] = {};
// No CRITICAL_SECTION — InitializeCriticalSection AVs under BattlEye + manual-map.
static volatile LONG g_LedgerLock = 0;
static int g_CsReady = 0;

static void LabLock()
{
    while (InterlockedCompareExchange(&g_LedgerLock, 1, 0) != 0)
        SwitchToThread();
}

static void LabUnlock()
{
    InterlockedExchange(&g_LedgerLock, 0);
}

static unsigned LabPackBits(const void* p, unsigned size)
{
    unsigned v = 0;
    if (!p || !size) return 0;
    unsigned n = size < 4 ? size : 4;
    memcpy(&v, p, n);
    return v;
}

void OakLab_NoteWrite(int moduleId, uintptr_t addr, unsigned size,
                      const void* oldBytes, const void* newBytes)
{
    if (!g_CsReady) return;
    LabLock();
    LONG i = InterlockedIncrement(&g_LedgerIdx) - 1;
    LabWriteRec& r = g_Ledger[(unsigned)i % kLabLedgerN];
    r.tick = GetTickCount();
    r.moduleId = moduleId;
    r.addr = addr;
    r.size = size;
    r.oldBits = LabPackBits(oldBytes, size);
    r.newBits = LabPackBits(newBytes, size);
    if (moduleId > OAK_LAB_NONE && moduleId < OAK_LAB_COUNT)
        g_LastWriteAddr[moduleId] = addr;
    InterlockedIncrement(&g_LedgerCount);
    LabUnlock();
}

uintptr_t OakLab_LastWriteAddr(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return 0;
    return g_LastWriteAddr[moduleId];
}

void OakLab_DumpWriteLedger(const char* why)
{
    char hdr[200];
    wsprintfA(hdr, "lab[ledger] dump why=%s armed=%s claim=%s safe=%d count=%d",
        why ? why : "?",
        OakLab_ModuleName(OakLab_GetArmedModule()),
        OakLab_GetClaim(OakLab_GetArmedModule()),
        (int)InterlockedCompareExchange(&g_SafeMode, 0, 0),
        (int)InterlockedCompareExchange(&g_LedgerCount, 0, 0));
    LabLog(hdr);

    if (g_CsReady)
    {
        LabLock();
        LONG idx = InterlockedCompareExchange(&g_LedgerIdx, 0, 0);
        int n = (int)InterlockedCompareExchange(&g_LedgerCount, 0, 0);
        if (n > kLabLedgerN) n = kLabLedgerN;
        for (int k = 0; k < n; k++)
        {
            unsigned slot = (unsigned)(idx - n + k) % kLabLedgerN;
            const LabWriteRec& r = g_Ledger[slot];
            if (!r.addr) continue;
            char line[200];
            wsprintfA(line, "lab[write] t=%u mod=%s addr=%p sz=%u old=%08X new=%08X",
                r.tick, OakLab_ModuleName(r.moduleId), (void*)r.addr, r.size,
                r.oldBits, r.newBits);
            LabLog(line);
        }
        LabUnlock();
    }

    int armed = OakLab_GetArmedModule();
    if (armed > OAK_LAB_FREECAM && why &&
        (strstr(why, "VEH") || strstr(why, "FATAL") || strstr(why, "AV")))
    {
        OakLab_Lock(armed, why);
        OakLab_EnterSafeMode(why);
    }
}

int OakLab_WriteBytes(int moduleId, uintptr_t addr, const void* data, unsigned size, unsigned flags)
{
    if (flags & (OAK_LAB_WF_BAN_FVS | OAK_LAB_WF_BAN_NET | OAK_LAB_WF_BAN_IC))
    {
        LabLog("lab[write-deny] banned-class-flag");
        return 0;
    }
    if (!data || !size || size > 64)
        return 0;
    if (!OakLab_IsWriteAllowed(moduleId))
    {
        static DWORD s_Last = 0;
        DWORD now = GetTickCount();
        if (!s_Last || (now - s_Last) > 2000)
        {
            char b[120];
            wsprintfA(b, "lab[write-deny] module=%s addr=%p", OakLab_ModuleName(moduleId), (void*)addr);
            LabLog(b);
            s_Last = now;
        }
        return 0;
    }
    if (addr < 0x10000)
        return 0;

    unsigned char oldBuf[64] = {};
    LabSafeCopy(oldBuf, (const void*)addr, size);

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
        return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    const int alreadyWritable =
        prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;

    int ok = 0;
    if (alreadyWritable)
    {
        ok = LabSafeCopy((void*)addr, data, size);
    }
    else
    {
        DWORD oldProt = 0;
        if (!VirtualProtect((LPVOID)addr, size, PAGE_EXECUTE_READWRITE, &oldProt))
            return 0;
        ok = LabSafeCopy((void*)addr, data, size);
        DWORD tmp = 0;
        VirtualProtect((LPVOID)addr, size, oldProt, &tmp);
    }
    if (ok)
        OakLab_NoteWrite(moduleId, addr, size, oldBuf, data);
    return ok;
}

int OakLab_WriteFloat(int moduleId, uintptr_t addr, float value, unsigned flags)
{
    return OakLab_WriteBytes(moduleId, addr, &value, sizeof(float), flags);
}

int OakLab_WriteU8(int moduleId, uintptr_t addr, unsigned char value, unsigned flags)
{
    return OakLab_WriteBytes(moduleId, addr, &value, 1, flags);
}

int OakLab_WriteVec3(int moduleId, uintptr_t addr, float x, float y, float z, unsigned flags)
{
    float v[3] = { x, y, z };
    return OakLab_WriteBytes(moduleId, addr, v, sizeof(v), flags);
}

int OakLab_ExperimentalArmed(void)
{
    int a = OakLab_GetArmedModule();
    return (a > OAK_LAB_FREECAM && a < OAK_LAB_COUNT) ? 1 : 0;
}

void OakLab_AssertNotNetWrite(const char* where)
{
    char b[120];
    wsprintfA(b, "lab[assert] BAN_NET tripped where=%s", where ? where : "?");
    LabLog(b);
}

void OakLab_AssertNotFvsWrite(const char* where)
{
    char b[120];
    wsprintfA(b, "lab[assert] BAN_FVS tripped where=%s", where ? where : "?");
    LabLog(b);
}

int OakLab_ApplyRecipe(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return 0;
    if (g_State[moduleId] == OAK_LAB_LOCKED)
    {
        char b[160];
        wsprintfA(b, "lab[recipe] deny locked module=%s reason=%s",
            OakLab_ModuleName(moduleId), g_LockReason[moduleId]);
        LabLog(b);
        return 0;
    }

    switch (moduleId)
    {
    case OAK_LAB_FOV:
        OakLab_SetClaim(moduleId, "rendered FOV widens/narrows with slider (not tan echo)");
        LabLog("lab[recipe] FOV: cam+0xD0/+0xE0 are sim-rebuilt tan(halfFOV) cache");
        LabLog("lab[recipe] FOV: hold OVERWRITTEN expected unless continuous fight; visual confirm required");
        LabLog("lab[recipe] FOV: never patch shared camera VT; FOV_Context layout still discovery-open");
        return 1;
    case OAK_LAB_MB:
        OakLab_SetClaim(moduleId, "local bullet VS snaps to bone and registers a hit");
        LabLog("lab[recipe] MB: write VisualState+0x2C/30/34 ONLY — never FutureVisualState");
        LabLog("lab[recipe] MB: hold-test low value (sim may overwrite VS); need hit confirm for PASS");
        LabLog("lab[recipe] MB: ammo InitSpeed boosts must use same Lab module (AmmoType heap AV class)");
        return 1;
    case OAK_LAB_SILENT:
        OakLab_SetClaim(moduleId, "silent path uses MB bone snap (not camera-silent) and hits");
        LabLog("lab[recipe] SILENT: same VS snap as MB — not camera aim write");
        return 1;
    case OAK_LAB_AMMO:
        OakLab_SetClaim(moduleId, "AmmoType InitSpeed/dispersion sticks and changes TOF/spread in-game");
        LabLog("lab[recipe] AMMO: shared AmmoType object — hold-test InitSpeed; watch heap AV");
        return 1;
    case OAK_LAB_GRENADE:
        OakLab_SetClaim(moduleId, "thrown grenade VS snaps to locked player feet");
        LabLog("lab[recipe] GRENADE: VS-only via CombatWriteEntityWorldPos under OAK_LAB_GRENADE");
        return 1;
    case OAK_LAB_STREAMPROOF:
        OakLab_SetClaim(moduleId, "overlay+game HWND excluded from capture");
        LabLog("lab[recipe] STREAMPROOF: SetWindowDisplayAffinity path — no game memory ownership");
        return 1;
    case OAK_LAB_TRIGGERBOT:
        OakLab_SetClaim(moduleId, "fires only when crosshair on valid target within delay");
        LabLog("lab[recipe] TRIGGERBOT: input synthesis — no float ownership probe");
        return 1;
    case OAK_LAB_WARP:
        OakLab_SetClaim(moduleId, "lag switch: hold outbound UDP, release flushes (anti-kick max hold)");
        LabLog("lab[recipe] WARP/LAG: HOLD key queues sendto/WSASendTo; release flushes; auto-cut by mode");
        LabLog("lab[recipe] WARP/LAG: skips Steam ports 27000-27100; overlapped sends pass through");
        return 1;
    case OAK_LAB_STAMINA:
    case OAK_LAB_SPEED:
    case OAK_LAB_NOCLIP:
    case OAK_LAB_WALLBYPASS:
        LabLog("lab[recipe] module permanently locked — see DAYZ_OWNERSHIP_MAP.md");
        return 0;
    case OAK_LAB_THIRDPERSON:
        OakLab_SetClaim(moduleId, "vanilla 3PP: Network+0x9C allow=1 on Network object (never +0x50 client)");
        LabLog("lab[recipe] THIRDPERSON: unlock V-key; World+0x2984 allow=1; Network ThirdPersonFlag=1");
        LabLog("lab[recipe] THIRDPERSON: NEVER follow ManagerNetworkClient+0x50 (garbage → CDP AV)");
        return 1;
    default:
        return 0;
    }
}

static int LabLooksMeter(float v)
{
    return v == v && v >= 0.f && v <= 100.05f;
}

static int LabIsHeap(uintptr_t p)
{
    return p > 0x100000000ULL && p < 0x00007FFFFFFFFFFFULL;
}

static uintptr_t LabReadP(uintptr_t a)
{
    uintptr_t p = 0;
    if (!LabSafeCopy(&p, (const void*)a, sizeof(p)))
        return 0;
    return p;
}

static void LabProbePush(uintptr_t a, float v)
{
    if (g_ProbeN >= kProbeMax) return;
    if (!(v == v)) return;
    if (!LabLooksMeter(v) && !(v >= 0.f && v <= 1.05f))
        return;
    for (int i = 0; i < g_ProbeN; i++)
        if (g_Probe[i].addr == a) return;
    g_Probe[g_ProbeN].addr = a;
    g_Probe[g_ProbeN].v0 = v;
    g_ProbeN++;
}

void OakLab_ProbeStart(uintptr_t localPlayer, const char* action)
{
    g_ProbeN = 0;
    g_ProbePhase = 0;
    g_ProbeEntity = localPlayer;
    g_ProbeT0 = 0;
    g_ProbeLastHits = 0;
    lstrcpynA(g_ProbeAction, action ? action : "idle", 24);
    if (!LabIsHeap(localPlayer))
    {
        LabLog("lab[probe] start FAIL bad localPlayer");
        return;
    }

    for (uintptr_t fo = 0x600; fo <= 0x780 && g_ProbeN < kProbeMax; fo += 4)
        LabProbePush(localPlayer + fo, LabReadF(localPlayer + fo));

    for (uintptr_t off = 0x200; off <= 0xC00 && g_ProbeN < kProbeMax - 8; off += 8)
    {
        uintptr_t p = LabReadP(localPlayer + off);
        if (!LabIsHeap(p) || p == localPlayer) continue;
        int back = 0;
        for (uintptr_t bo = 0x08; bo <= 0x100; bo += 8)
            if (LabReadP(p + bo) == localPlayer) { back = 1; break; }
        int meters = 0, nearMax = 0;
        for (uintptr_t fo = 0x08; fo <= 0xC0; fo += 4)
        {
            float v = LabReadF(p + fo);
            if (LabLooksMeter(v)) { meters++; if (v >= 90.f) nearMax++; }
        }
        if (!back && !(meters >= 3 && nearMax >= 1))
            continue;
        for (uintptr_t fo = 0x08; fo <= 0xC0 && g_ProbeN < kProbeMax; fo += 4)
            LabProbePush(p + fo, LabReadF(p + fo));
    }

    g_ProbePhase = 1;
    g_ProbeT0 = GetTickCount();
    char b[120];
    wsprintfA(b, "lab[probe] start action=%s n=%d (READ-ONLY)", g_ProbeAction, g_ProbeN);
    LabLog(b);
}

void OakLab_ProbeTick(uintptr_t localPlayer)
{
    if (g_ProbePhase != 1)
        return;
    if (localPlayer != g_ProbeEntity)
    {
        g_ProbePhase = 0;
        return;
    }
    DWORD now = GetTickCount();
    DWORD waitMs = 900;
    if (lstrcmpiA(g_ProbeAction, "sprint") == 0) waitMs = 1600;
    if (lstrcmpiA(g_ProbeAction, "fire") == 0) waitMs = 400;
    if ((now - g_ProbeT0) < waitMs)
        return;

    int hits = 0;
    float bestDrop = 0.f, bestRise = 0.f, bestAny = 0.f;
    uintptr_t bestDropA = 0, bestRiseA = 0;
    for (int i = 0; i < g_ProbeN; i++)
    {
        float v1 = LabReadF(g_Probe[i].addr);
        if (!(v1 == v1)) continue;
        float d = g_Probe[i].v0 - v1;
        float rise = v1 - g_Probe[i].v0;
        float ad = d > rise ? d : rise;
        if (ad > bestAny) bestAny = ad;
        int meaningful = 0;
        if (g_Probe[i].v0 <= 1.05f)
            meaningful = (d >= 0.03f) ? 1 : 0;
        else
            meaningful = (d >= 1.5f) ? 1 : 0;
        if (meaningful)
        {
            hits++;
            if (d > bestDrop) { bestDrop = d; bestDropA = g_Probe[i].addr; }
        }
        if (rise >= 1.f && g_Probe[i].v0 < 60.f && rise > bestRise)
        {
            bestRise = rise;
            bestRiseA = g_Probe[i].addr;
        }
    }

    g_ProbeLastHits = hits;
    g_ProbeLastDoneTick = GetTickCount();

    char b[220];
    wsprintfA(b,
        "lab[probe] done action=%s hits=%d bestDropx100=%d bestRisex100=%d bestAnyx100=%d dropA=%p riseA=%p",
        g_ProbeAction, hits,
        (int)(bestDrop * 100.f + 0.5f), (int)(bestRise * 100.f + 0.5f),
        (int)(bestAny * 100.f + 0.5f),
        (void*)bestDropA, (void*)bestRiseA);
    LabLog(b);

    if (hits == 0 && bestAny < 0.5f)
        LabLog("lab[probe] verdict=NO_CLIENT_DRAIN (likely server/sim owned — do not Arm writers guessing)");
    else if (hits > 0)
        LabLog("lab[probe] verdict=CLIENT_CANDIDATES (hold-test next — PROBE != DONE)");

    g_ProbePhase = 2;
}

static const char* LabContractName(int r)
{
    switch (r)
    {
    case OAK_LAB_CONTRACT_PASS: return "PASS";
    case OAK_LAB_CONTRACT_FAIL: return "FAIL";
    case OAK_LAB_CONTRACT_PROBE: return "PROBE";
    default: return "NONE";
    }
}

void OakLab_LogContract(int moduleId, int result, const char* reason)
{
    char b[220];
    wsprintfA(b, "lab[contract] module=%s result=%s reason=%s claim=%s",
        OakLab_ModuleName(moduleId), LabContractName(result), reason ? reason : "",
        OakLab_GetClaim(moduleId));
    LabLog(b);
}

int OakLab_ContractFovMemory(float wantTan, float gotTan, float errPct)
{
    if (!(wantTan > 0.01f) || !(gotTan == gotTan))
    {
        OakLab_LogContract(OAK_LAB_FOV, OAK_LAB_CONTRACT_FAIL, "invalid-tan");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (errPct <= 12.f)
    {
        OakLab_LogContract(OAK_LAB_FOV, OAK_LAB_CONTRACT_PROBE, "memory-echo-only-not-visual");
        return OAK_LAB_CONTRACT_PROBE;
    }
    OakLab_LogContract(OAK_LAB_FOV, OAK_LAB_CONTRACT_FAIL, "memory-miss");
    return OAK_LAB_CONTRACT_FAIL;
}

int OakLab_ContractStamina(int sprint, int haveRealTarget, int holdOk)
{
    (void)sprint;
    if (!haveRealTarget)
    {
        OakLab_LogContract(OAK_LAB_STAMINA, OAK_LAB_CONTRACT_FAIL, "no-real-drain-target");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (holdOk)
    {
        // Memory hold alone is never PASS — requires Finalize + gameplay confirm.
        OakLab_LogContract(OAK_LAB_STAMINA, OAK_LAB_CONTRACT_PROBE, "hold-ok-need-gameplay-confirm");
        return OAK_LAB_CONTRACT_PROBE;
    }
    OakLab_LogContract(OAK_LAB_STAMINA, OAK_LAB_CONTRACT_FAIL, "hold-fail");
    return OAK_LAB_CONTRACT_FAIL;
}

int OakLab_ContractSpeedStick(int distCm)
{
    if (distCm >= 75)
    {
        // Position stick is gameplay-shaped evidence, still needs Finalize for PASS.
        OakLab_LogContract(OAK_LAB_SPEED, OAK_LAB_CONTRACT_PROBE, "stick-ok-need-finalize");
        return OAK_LAB_CONTRACT_PROBE;
    }
    OakLab_LogContract(OAK_LAB_SPEED, OAK_LAB_CONTRACT_FAIL, "rubberband");
    return OAK_LAB_CONTRACT_FAIL;
}

int OakLab_ContractMbSnap(int guided, int boneErrCm, int usedFvs)
{
    if (usedFvs)
    {
        OakLab_LogContract(OAK_LAB_MB, OAK_LAB_CONTRACT_FAIL, "fvs-write-forbidden");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (guided <= 0)
    {
        OakLab_LogContract(OAK_LAB_MB, OAK_LAB_CONTRACT_FAIL, "no-guide");
        return OAK_LAB_CONTRACT_FAIL;
    }
    if (boneErrCm <= 15)
    {
        OakLab_LogContract(OAK_LAB_MB, OAK_LAB_CONTRACT_PROBE, "vs-near-bone-need-hit-confirm");
        return OAK_LAB_CONTRACT_PROBE;
    }
    OakLab_LogContract(OAK_LAB_MB, OAK_LAB_CONTRACT_FAIL, "bone-miss");
    return OAK_LAB_CONTRACT_FAIL;
}

void OakLab_Init(void)
{
    if (InterlockedCompareExchange(&g_Inited, 1, 0) != 0)
        return;
    // Intentionally no InitializeCriticalSection — AV under BE + manual-map.
    g_CsReady = 1;
    for (int i = 0; i < OAK_LAB_COUNT; i++)
    {
        g_State[i] = OAK_LAB_OFF;
        g_Claim[i][0] = 0;
        g_GameplayConfirm[i] = 0;
    }
    OakLab_Lock(OAK_LAB_STAMINA, "prior: 0 client drain targets / server-synced");
    OakLab_Lock(OAK_LAB_SPEED, "prior: VS rubberband distCm=0");
    OakLab_Lock(OAK_LAB_NOCLIP, "prior: VS rubberband");
    // WARP = lag switch (outbound packet hold). Freecam fake-body path removed.
    // THIRDPERSON: soft camera pullback (MiscApplyThirdPerson) — Network+0x9C stays banned.
    OakLab_Lock(OAK_LAB_WALLBYPASS, "prior: mid-air snap + FVS dual-write AV");
    // Default PLAY — Lab SAFE/ARM is opt-in. safe=1 was bricking Fast Bullets / MB / FOV.
    InterlockedExchange(&g_SafeMode, 0);
    InterlockedExchange(&g_Armed, OAK_LAB_NONE);
    LabLog("lab: init OK mode=PLAY (SAFE/ARM opt-in; locked modules stay denied)");
}

void OakLab_Shutdown(void)
{
    if (!g_CsReady) return;
    g_CsReady = 0;
    InterlockedExchange(&g_LedgerLock, 0);
    InterlockedExchange(&g_Inited, 0);
}

void OakLab_SetLocalPlayer(uintptr_t localPlayer)
{
    g_LocalPlayerHint = localPlayer;
}

uintptr_t OakLab_GetLocalPlayer(void)
{
    return g_LocalPlayerHint;
}

const char* OakLab_LockReason(int moduleId)
{
    if (moduleId <= OAK_LAB_NONE || moduleId >= OAK_LAB_COUNT)
        return "";
    return g_LockReason[moduleId];
}

void OakLab_OnPresent(void)
{
    if (!InterlockedCompareExchange(&g_Inited, 0, 0))
        return;

    uintptr_t lp = OakLab_GetLocalPlayer();
    if (lp)
        OakLab_ProbeTick(lp);
    OakLab_HoldTestTick();

    int armed = OakLab_GetArmedModule();
    if (armed > OAK_LAB_FREECAM && g_ArmTick && !g_SoakOk)
    {
        if ((GetTickCount() - g_ArmTick) >= kArmSoakMs)
        {
            g_SoakOk = 1;
            char b[120];
            wsprintfA(b, "lab[soak] OK module=%s ms=%u (still need gameplay confirm)",
                OakLab_ModuleName(armed), kArmSoakMs);
            LabLog(b);
        }
    }

    static DWORD s_LastSum = 0;
    DWORD now = GetTickCount();
    if (!s_LastSum || (now - s_LastSum) > 30000)
    {
        char b[200];
        wsprintfA(b, "lab[status] safe=%d armed=%s soak=%d confirm=%d hold=%d probeHits=%d ledger=%d",
            (int)InterlockedCompareExchange(&g_SafeMode, 0, 0),
            OakLab_ModuleName(armed),
            g_SoakOk,
            (armed > 0) ? g_GameplayConfirm[armed] : 0,
            g_HoldVerdict,
            g_ProbeLastHits,
            (int)InterlockedCompareExchange(&g_LedgerCount, 0, 0));
        LabLog(b);
        s_LastSum = now;
    }
}
