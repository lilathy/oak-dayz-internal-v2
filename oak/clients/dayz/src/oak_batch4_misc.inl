// Batch 4 â€” Phase C (movement/world) + Phase D (exploits, best-effort).
// Included from main.cpp after esp_addons_impl.inl + misc_impl.inl.
// Settings globals: g_WorldMisc, g_Exploits (main.cpp)

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static char g_Batch4StatusDoor[72] = "idle";
static char g_Batch4QaFov[56] = "off";
static char g_Batch4QaSpeed[56] = "off";
static char g_Batch4QaStam[56] = "off";

static float g_Batch4LastFovScale = 0.f;
static DWORD g_Batch4LastFovTick = 0;
static DWORD g_Batch4LastStamTick = 0;
static DWORD g_Batch4LastSpeedMoveTick = 0;
static DWORD g_Batch4LastQaTick = 0;
static float g_Batch4SavedTick = 0.f;
static bool g_Batch4TickSaved = false;
static uintptr_t g_Batch4TickWriteAddr = 0;

static bool Batch4TickLooksSane(float v)
{
    return v == v && v > 0.000001f && v < 2.f;
}

static bool Batch4ProbeSimTick(uintptr_t mod, uintptr_t& outWriteAddr, float& outTick)
{
    outWriteAddr = 0;
    outTick = 0.f;

    const uintptr_t tickSlots[] = {
        oak_offsets::modbase::Tick,
        (uintptr_t)0xF19418 // legacy candidate from older builds
    };

    for (int si = 0; si < 2; si++)
    {
        uintptr_t tickSlot = mod + tickSlots[si];
        float direct = Read<float>(tickSlot);
        if (Batch4TickLooksSane(direct))
        {
            outWriteAddr = tickSlot;
            outTick = direct;
            return true;
        }

        uintptr_t p = Read<uintptr_t>(tickSlot);
        if (IsValidPtr(p) && p > 0x100000000)
        {
            float indirect = Read<float>(p);
            if (Batch4TickLooksSane(indirect))
            {
                outWriteAddr = p;
                outTick = indirect;
                return true;
            }
        }
    }
    return false;
}

static float Batch4ReadEmbedFovScale(uintptr_t mod)
{
    uintptr_t embed = mod + oak_offsets::modbase::FOV_Context + oak_offsets::modbase::FovBase;
    float s = Read<float>(embed);
    if (s == s && s > 0.01f && s < 10.f)
        return s;

    uintptr_t fovCtx = Read<uintptr_t>(mod + oak_offsets::modbase::FOV_Context);
    if (IsValidPtr(fovCtx) && fovCtx > 0x100000000)
    {
        s = Read<float>(fovCtx + oak_offsets::modbase::FovBase);
        if (s == s && s > 0.01f && s < 10.f)
            return s;
    }
    return -1.f;
}

static float Batch4ScaleToFovDeg(float scale)
{
    if (!(scale == scale) || scale < 0.01f)
        return -1.f;
    return 75.f / scale;
}

// Live probe (2026-08-07): camera+0xD0 / +0xE0 are tan(half-FOV) projection scales
// (projXâ‰ˆ1.047, projYâ‰ˆ0.589 @ ~93Â° horiz). Game rebuilds them every camera update.
// VT trampoline hung Present â€” use a dedicated writer thread (same pattern as fullbright).

static float g_FovWantHorizDeg = 0.f;
static volatile long g_FovWorkerRun = 0;
static HANDLE g_FovWorker = NULL;
static uintptr_t g_FovWorkerCam = 0;
static volatile long g_StamPauseRefill = 0; // 1 = self-test owns stamina writes
static volatile long g_StamSelfTestPass = -1; // -1 unknown, 0 fail, 1 pass
static volatile long g_SpeedSelfTestPass = -1;

// Real sprint stamina lives in StaminaHandler + PlayerStat "Stamina".
// Entity+0x6A4 alone sticks in memory while the HUD / CanSprint path still drains.
struct OakStamCache {
    uintptr_t entity = 0;
    uintptr_t statRec = 0;
    uintptr_t statOff = 0;
    float     statFull = 100.f; // 100 or 1 depending on record scale
    uintptr_t handler = 0;
    uintptr_t meterOff[8] = {};
    int       meterN = 0;
    uintptr_t depleteOff = 0;   // m_StaminaDepletion-like float
    uintptr_t depletedOff = 0;  // m_StaminaDepleted bool
    DWORD     lastDiscover = 0;
    int       discoverOk = 0;
};
static OakStamCache g_StamCache = {};

static bool OakStamLooksMeter(float v)
{
    return v == v && v >= 0.f && v <= 100.05f;
}

static void OakStamClearCache()
{
    g_StamCache = OakStamCache{};
}

// Collect candidate float addrs on/near local player (Present only).
static int OakStamCollectCandidates(uintptr_t lp, uintptr_t* addrs, float* vals, int maxN)
{
    int n = 0;
    if (!IsValidPtr(lp) || !addrs || !vals || maxN < 4)
        return 0;
    auto push = [&](uintptr_t a, float v) {
        if (n >= maxN) return;
        if (!OakStamLooksMeter(v) && !(v == v && v >= 0.f && v <= 1.05f)) return;
        for (int i = 0; i < n; i++) if (addrs[i] == a) return;
        addrs[n] = a;
        vals[n] = v;
        n++;
    };

    push(lp + oak_offsets::entity::Stamina, Read<float>(lp + oak_offsets::entity::Stamina));

    // Wider direct float band on entity
    for (uintptr_t fo = 0x600; fo <= 0x780; fo += 4)
        push(lp + fo, Read<float>(lp + fo));

    // Child objects â€” prefer Man back-pointer, but also accept meter-rich objects
    for (uintptr_t off = 0x200; off <= 0xC00; off += 8)
    {
        uintptr_t p = Read<uintptr_t>(lp + off);
        if (!IsHeapObj(p) || p == lp) continue;
        bool back = false;
        for (uintptr_t bo = 0x08; bo <= 0x100; bo += 8)
        {
            if (Read<uintptr_t>(p + bo) == lp) { back = true; break; }
        }
        int meterHits = 0;
        int nearMax = 0;
        for (uintptr_t fo = 0x08; fo <= 0xD0; fo += 4)
        {
            float v = Read<float>(p + fo);
            if (OakStamLooksMeter(v)) { meterHits++; if (v >= 90.f) nearMax++; }
        }
        if (!back && !(meterHits >= 3 && nearMax >= 1))
            continue;
        for (uintptr_t fo = 0x08; fo <= 0xD0; fo += 4)
            push(p + fo, Read<float>(p + fo));
    }

    // PlayerStat Stamina if label walk works
    float sv = -1.f;
    if (OakReadPlayerStat(lp, "Stamina", sv) && sv == sv)
    {
        if (g_StatWriteCache.rec && g_StatWriteCache.valOff)
            push(g_StatWriteCache.rec + g_StatWriteCache.valOff, sv);
    }
    return n;
}

static void OakStamDiscover(uintptr_t lp)
{
    if (!IsValidPtr(lp) || lp < 0x100000000ULL)
        return;
    DWORD now = GetTickCount();
    if (g_StamCache.entity == lp && g_StamCache.discoverOk &&
        g_StamCache.lastDiscover && (now - g_StamCache.lastDiscover) < 2000)
        return;

    // --- Sprint-delta probe: find floats that actually drain while sprinting ---
    static int s_Phase = 0; // 0 idle, 1 snapshot, 2 wait, 3 done-session
    static DWORD s_SnapT = 0;
    static uintptr_t s_Addrs[96];
    static float s_Vals0[96];
    static int s_N = 0;
    static uintptr_t s_ProbeEntity = 0;

    int sprint = ((GetAsyncKeyState('W') & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) ? 1 : 0;

    if (s_ProbeEntity != lp)
    {
        s_Phase = 0;
        s_ProbeEntity = lp;
    }

    if (!g_StamCache.discoverOk || g_StamCache.entity != lp)
    {
        if (s_Phase == 0 && sprint)
        {
            InterlockedExchange(&g_StamPauseRefill, 1); // let real drain happen
            s_N = OakStamCollectCandidates(lp, s_Addrs, s_Vals0, 96);
            s_SnapT = now;
            s_Phase = 1;
            char b[96];
            wsprintfA(b, "stam: sprint-probe snap n=%d", s_N);
            Log(b);
        }
        else if (s_Phase == 1 && sprint && (now - s_SnapT) >= 1800 && s_N > 0)
        {
            uintptr_t hit[8] = {};
            float hitDrop[8] = {};
            int hn = 0;
            float bestDrain = 0.f;
            float bestAny = 0.f;
            float bestRise = 0.f;
            uintptr_t riseAddr = 0;
            for (int i = 0; i < s_N; i++)
            {
                float v1 = Read<float>(s_Addrs[i]);
                if (!(v1 == v1)) continue;
                float d = s_Vals0[i] - v1; // positive = drained
                float rise = v1 - s_Vals0[i];
                float ad = (d > rise) ? d : rise;
                if (ad > bestAny) bestAny = ad;
                if (rise > bestRise && rise >= 0.5f && s_Vals0[i] < 60.f)
                {
                    bestRise = rise;
                    riseAddr = s_Addrs[i];
                }
                bool meaningful = false;
                if (s_Vals0[i] <= 1.05f)
                    meaningful = (d >= 0.03f);
                else
                    meaningful = (d >= 1.5f);
                if (!meaningful) continue;
                if (hn < 8)
                {
                    hit[hn] = s_Addrs[i];
                    hitDrop[hn] = d;
                    hn++;
                }
                if (d > bestDrain) bestDrain = d;
            }
            InterlockedExchange(&g_StamPauseRefill, 0);

            if (hn == 0 && riseAddr && bestRise >= 1.f)
            {
                hit[hn] = riseAddr;
                hitDrop[hn] = -bestRise;
                hn++;
            }

            s_Phase = 2;
            char b[220];
            wsprintfA(b, "stam: sprint-probe hits=%d bestDrainx100=%d bestAnyx100=%d bestRisex100=%d",
                hn, (int)(bestDrain * 100.f + 0.5f), (int)(bestAny * 100.f + 0.5f),
                (int)(bestRise * 100.f + 0.5f));
            Log(b);

            g_StamCache = OakStamCache{};
            g_StamCache.entity = lp;
            g_StamCache.lastDiscover = now;
            g_StamCache.handler = 1;
            g_StamCache.meterN = 0;
            g_StamCache.depleteOff = 0;
            for (int i = 0; i < hn && g_StamCache.meterN < 8; i++)
            {
                if (hitDrop[i] < 0.f)
                {
                    g_StamCache.depleteOff = hit[i];
                    continue;
                }
                g_StamCache.meterOff[g_StamCache.meterN++] = hit[i];
            }
            float sv = -1.f;
            if (OakReadPlayerStat(lp, "Stamina", sv) && g_StatWriteCache.rec)
            {
                g_StamCache.statRec = g_StatWriteCache.rec;
                g_StamCache.statOff = g_StatWriteCache.valOff;
                g_StamCache.statFull = (sv <= 1.05f) ? 1.f : 100.f;
            }
            g_StamCache.discoverOk = (g_StamCache.meterN > 0 || g_StamCache.depleteOff || g_StamCache.statRec) ? 1 : 0;
            if (!g_StamCache.discoverOk)
            {
                Log("stam: sprint-probe found no drain targets â€” will retry");
                s_Phase = 0;
            }
            else
            {
                char b2[140];
                wsprintfA(b2, "stam: locked meters=%d depleteAbs=%d stat=%d",
                    g_StamCache.meterN, g_StamCache.depleteOff ? 1 : 0, g_StamCache.statRec ? 1 : 0);
                Log(b2);
            }
            return;
        }
        else if (s_Phase == 1 && !sprint)
        {
            InterlockedExchange(&g_StamPauseRefill, 0);
            s_Phase = 0; // need continuous sprint
        }
    }

    // Lightweight PlayerStat only â€” structural handler guesses are not written.
    // Real lock comes from sprint-delta probe above.
    if (g_StamCache.entity == lp && g_StamCache.discoverOk && g_StamCache.handler == 1)
        return;

    float sv = -1.f;
    if (OakReadPlayerStat(lp, "Stamina", sv) && sv == sv && sv >= 0.f && g_StatWriteCache.rec)
    {
        // Cache for QA reads; do NOT set discoverOk â€” sprint probe must confirm drain.
        if (!(g_StamCache.discoverOk && g_StamCache.handler == 1))
        {
            g_StamCache.entity = lp;
            g_StamCache.statRec = g_StatWriteCache.rec;
            g_StamCache.statOff = g_StatWriteCache.valOff;
            g_StamCache.statFull = (sv <= 1.05f) ? 1.f : 100.f;
            g_StamCache.lastDiscover = now;
        }
    }
    else
    {
        static DWORD s_LastMiss = 0;
        if (!s_LastMiss || (now - s_LastMiss) > 8000)
        {
            Log("stam: waiting â€” hold W+Shift ~1s to lock real drain targets");
            s_LastMiss = now;
        }
    }
}

static void Batch4ForceStaminaFull(uintptr_t lp)
{
    (void)lp;
    // Permanently retired â€” entity mirror / guessed handler writes are the known lie path.
}

static void Batch4ForceFovContext(float horizDeg)
{
    (void)horizDeg;
    // RETIRED 2026-08-07 crash: wrote float scale into mod+FOV_Context+FovBase /
    // mod+ScopeFovCtx+FovBase. Those globals are POINTER slots (ledger: old=00007FF7
    // â†’ new=3F22B63D), which corrupted deref chains â†’ ACCESS_VIOLATION.
    static int s_Once = 0;
    if (!s_Once)
    {
        Log("fov: FOV_Context/ScopeFovCtx module writes DISABLED (pointer corruption)");
        s_Once = 1;
    }
}

static void Batch4ForceCameraProj(uintptr_t camera, float horizDeg); // defined below

// ---------------------------------------------------------------------------
// Game-thread FOV hook â€” ENTRY of Camera update (DayZ+0x7A0EC0).
// That function reads cam+0x194/+0x198 then builds render matrices AND W2S
// tan caches (D0/E0). Patching only D0/E0 after the stores warps ESP boxes
// but not the world view. Override +194/+198 at entry so the full rebuild
// (including render path / vt calls) uses our FOV.
// ---------------------------------------------------------------------------
enum { kOakFovStolen = 15 }; // prologue: mov rax,rsp; mov [rax+8],rbx; mov [rax+10],rsi; mov [rax+18],rdi
static BYTE* g_FovHookTarget = nullptr;
static BYTE g_FovHookOrig[16] = {};
static BYTE* g_FovTrampoline = nullptr;
static volatile LONG g_FovHookInstalled = 0;
static volatile LONG g_FovHookHits = 0;

extern "C" void OakFovOnCamEntry(uintptr_t camera)
{
    InterlockedIncrement(&g_FovHookHits);
    if (InterlockedCompareExchange(&g_SessionQuiesced, 0, 0))
        return;
    if (!g_WorldMisc.fovChanger || g_MiscFreecam || g_PanicHidden)
        return;
    if (!OakLab_IsWriteAllowed(OAK_LAB_FOV))
        return;
    if (!IsValidPtr(camera) || camera < 0x100000000ULL)
        return;

    // Only the gameplay world camera â€” inventory/character/hotbar previews are
    // other Camera* objects that share this update; forcing FOV on them blows up UI meshes.
    uintptr_t worldCam = g_FovWorkerCam;
    if (!worldCam || camera != worldCam)
        return;

    float deg = g_FovWantHorizDeg;
    if (deg < 60.f) deg = g_WorldMisc.horizontalFov;
    if (deg < 70.f) deg = 70.f;
    if (deg > 110.f) deg = 110.f;

    float aspect = 16.f / 9.f;
    float vpX = 0.f, vpY = 0.f;
    __try
    {
        Vec3 vp = Read<Vec3>(camera + offsets::camera::ViewPortSize);
        vpX = vp.x;
        vpY = vp.y;
        if (vp.x > 1.f && vp.y > 1.f)
            aspect = vp.x / vp.y;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (aspect < 1.0f || aspect > 3.0f)
        aspect = 16.f / 9.f;
    // Tiny viewports = UI preview cameras (belt & braces if ptr check ever races)
    if (vpX > 1.f && vpY > 1.f && (vpX < 640.f || vpY < 360.f))
        return;

    const float kPi = 3.14159265f;
    // DayZ OPTIONS_FIELD_OF_VIEW range (vertical radians) â€” keeps terrain/near-plane sane.
    const float kVMin = 0.752427f;
    const float kVMax = 1.303220f;
    float hRad = deg * (kPi / 180.f);
    float vRad = 2.f * atanf(tanf(hRad * 0.5f) / aspect);
    if (vRad < kVMin) vRad = kVMin;
    if (vRad > kVMax) vRad = kVMax;

    const float halfV = vRad * 0.5f;
    const float halfH = atanf(tanf(halfV) * aspect);
    const float projX = tanf(halfH);
    const float projY = tanf(halfV);
    if (!(projX == projX) || !(projY == projY) || projX < 0.2f || projX > 3.5f)
        return;

    // 7A1D80 already wrote this frame's intended FOV (hipfire OR ADS/optics).
    // Default: if game wants narrower FOV, keep zoom. Optional: lock hipfire FOV through ADS.
    if (!g_WorldMisc.fovKeepWhileAds)
    {
        float natX = 0.f;
        __try { natX = Read<float>(camera + 0x194); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (natX == natX && natX > 0.05f && natX < projX * 0.92f)
            return;
    }

    Write<float>(camera + 0x194, projX);
    Write<float>(camera + 0x198, projY);
}

static BYTE* OakFovFindHookSite(uintptr_t modBase, uintptr_t modEnd)
{
    // Camera update entry (DayZ 1.29.0.163709): loads cam+0x194 at entry+0x1F.
    // Old hardcoded 0x7A0EC0 is mid-function junk â€” never hook that.
    static const BYTE kPat[] = {
        0x48, 0x8B, 0xC4,                   // mov rax, rsp
        0x48, 0x89, 0x58, 0x08,             // mov [rax+8], rbx
        0x48, 0x89, 0x70, 0x10,             // mov [rax+10], rsi
        0x48, 0x89, 0x78, 0x18              // mov [rax+18], rdi
    };
    const size_t patN = sizeof(kPat);
    constexpr uintptr_t kCamUpdateRva = 0x7A0330ull;

    auto siteOk = [&](BYTE* fb) -> bool {
        if (!fb || (uintptr_t)fb + 64 >= modEnd)
            return false;
        if (memcmp(fb, kPat, patN) != 0 || fb[15] != 0x55) // push rbp
            return false;
        // Must load [rcx+0x194] shortly after entry (movss xmm1, [rcx+194h]).
        for (int k = 0; k < 64; k++)
        {
            if (fb[k] == 0xF3 && fb[k + 1] == 0x0F && fb[k + 2] == 0x10 &&
                fb[k + 3] == 0x89 && fb[k + 4] == 0x94 && fb[k + 5] == 0x01)
                return true;
        }
        return false;
    };

    __try
    {
        if (modEnd > modBase + kCamUpdateRva + 64)
        {
            BYTE* preferred = (BYTE*)(modBase + kCamUpdateRva);
            if (siteOk(preferred))
                return preferred;
        }

        // Narrow scan only â€” full-image scan previously hooked wrong lookalikes and AVd.
        uintptr_t scanLo = modBase + 0x790000ull;
        uintptr_t scanHi = modBase + 0x7B0000ull;
        if (scanLo < modBase + 0x1000)
            scanLo = modBase + 0x1000;
        if (scanHi > modEnd - 64)
            scanHi = modEnd - 64;
        for (uintptr_t p = scanLo; p + patN + 64 < scanHi; p++)
        {
            if (*(const BYTE*)p != 0x48)
                continue;
            BYTE* cand = (BYTE*)p;
            if (siteOk(cand))
                return cand;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static void OakFovReleaseCamHook()
{
    if (!InterlockedCompareExchange(&g_FovHookInstalled, 0, 0))
        return;
    __try
    {
        if (g_FovHookTarget)
        {
            DWORD old = 0;
            if (VirtualProtect(g_FovHookTarget, kOakFovStolen, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(g_FovHookTarget, g_FovHookOrig, kOakFovStolen);
                FlushInstructionCache(GetCurrentProcess(), g_FovHookTarget, kOakFovStolen);
                VirtualProtect(g_FovHookTarget, kOakFovStolen, old, &old);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    InterlockedExchange(&g_FovHookInstalled, 0);
    g_FovHookTarget = nullptr;
    if (g_FovTrampoline)
    {
        VirtualFree(g_FovTrampoline, 0, MEM_RELEASE);
        g_FovTrampoline = nullptr;
    }
    Log("fov: game-thread entry hook removed");
}

static void OakFovEnsureCamHook(uintptr_t camera)
{
    (void)camera;
    if (InterlockedCompareExchange(&g_FovHookInstalled, 0, 0))
        return;
    if (!g_GameModule)
        return;

    uintptr_t base = (uintptr_t)g_GameModule;
    uintptr_t end = base;
    __try
    {
        auto dos = (IMAGE_DOS_HEADER*)g_GameModule;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                end = base + nt->OptionalHeader.SizeOfImage;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    BYTE* site = OakFovFindHookSite(base, end);
    if (!site)
    {
        static int s_Miss = 0;
        if (!s_Miss) { Log("fov: entry hook site NOT FOUND"); s_Miss = 1; }
        return;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return;

    BYTE* w = tramp;
    // At function entry RCX = Camera*. Save it, call OakFovOnCamEntry(rcx), then stolen prologue.
    *w++ = 0x9C; // pushfq
    *w++ = 0x50; // push rax
    *w++ = 0x51; // push rcx
    *w++ = 0x52; // push rdx
    *w++ = 0x41; *w++ = 0x50;
    *w++ = 0x41; *w++ = 0x51;
    *w++ = 0x41; *w++ = 0x52;
    *w++ = 0x41; *w++ = 0x53;
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xEC; *w++ = 0x28; // sub rsp,28
    // rcx already Camera*
    *w++ = 0x48; *w++ = 0xB8;
    UINT64 fn = (UINT64)&OakFovOnCamEntry;
    memcpy(w, &fn, 8); w += 8;
    *w++ = 0xFF; *w++ = 0xD0; // call rax
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xC4; *w++ = 0x28;
    *w++ = 0x41; *w++ = 0x5B;
    *w++ = 0x41; *w++ = 0x5A;
    *w++ = 0x41; *w++ = 0x59;
    *w++ = 0x41; *w++ = 0x58;
    *w++ = 0x5A;
    *w++ = 0x59;
    *w++ = 0x58;
    *w++ = 0x9D;
    memcpy(g_FovHookOrig, site, kOakFovStolen);
    memcpy(w, site, kOakFovStolen);
    w += kOakFovStolen;
    *w++ = 0xFF; *w++ = 0x25; *w++ = 0; *w++ = 0; *w++ = 0; *w++ = 0;
    UINT64 back = (UINT64)(site + kOakFovStolen);
    memcpy(w, &back, 8);

    DWORD old = 0;
    if (!VirtualProtect(site, kOakFovStolen, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return;
    }
    BYTE jmp[16] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x90, 0x90 };
    UINT64 dest = (UINT64)tramp;
    memcpy(jmp + 6, &dest, 8);
    memcpy(site, jmp, kOakFovStolen);
    FlushInstructionCache(GetCurrentProcess(), site, kOakFovStolen);
    VirtualProtect(site, kOakFovStolen, old, &old);

    g_FovHookTarget = site;
    g_FovTrampoline = tramp;
    InterlockedExchange(&g_FovHookInstalled, 1);
    InterlockedExchange(&g_FovHookHits, 0);

    char b[140];
    wsprintfA(b, "fov: ENTRY hook OK site=%p (override +194 before matrix rebuild)", site);
    Log(b);
}

static void Batch4ForceCameraProj(uintptr_t camera, float horizDeg)
{
    // Present/worker path: write the camera's FOV source (+0x194/+0x198) AND the
    // derived W2S tan caches (D0/E0). Entry mid-hook at the old 0x7A0EC0 site is
    // mid-function on 1.29.0.163709, so this is the live ownership path.
    if (!IsValidPtr(camera) || camera < 0x100000000ULL)
        return;
    if (!OakLab_IsWriteAllowed(OAK_LAB_FOV))
        return;

    float deg = horizDeg;
    if (deg < 70.f) deg = 70.f;
    if (deg > 110.f) deg = 110.f;

    float aspect = 16.f / 9.f;
    float vpX = 0.f, vpY = 0.f;
    __try
    {
        Vec3 vp = Read<Vec3>(camera + offsets::camera::ViewPortSize);
        vpX = vp.x;
        vpY = vp.y;
        if (vp.x > 1.f && vp.y > 1.f)
            aspect = vp.x / vp.y;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (aspect < 1.0f || aspect > 3.0f)
        aspect = 16.f / 9.f;
    // Tiny viewports = UI preview cameras â€” never touch those.
    if (vpX > 1.f && vpY > 1.f && (vpX < 640.f || vpY < 360.f))
        return;

    const float kPi = 3.14159265f;
    const float kVMin = 0.752427f;
    const float kVMax = 1.303220f;
    float hRad = deg * (kPi / 180.f);
    float vRad = 2.f * atanf(tanf(hRad * 0.5f) / aspect);
    if (vRad < kVMin) vRad = kVMin;
    if (vRad > kVMax) vRad = kVMax;

    const float halfV = vRad * 0.5f;
    const float halfH = atanf(tanf(halfV) * aspect);
    const float projX = tanf(halfH);
    const float projY = tanf(halfV);
    if (!(projX == projX) || !(projY == projY) || projX < 0.2f || projX > 3.5f)
        return;

    // Preserve ADS zoom unless user opted into fovKeepWhileAds.
    if (!g_WorldMisc.fovKeepWhileAds)
    {
        float natX = 0.f;
        __try { natX = Read<float>(camera + 0x194); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (natX == natX && natX > 0.05f && natX < projX * 0.92f)
            return;
    }

    __try
    {
        // Source FOV the camera update rebuilds from.
        Write<float>(camera + 0x194, projX);
        Write<float>(camera + 0x198, projY);
        // Derived projection caches used by W2S / some render paths.
        Write<float>(camera + offsets::camera::GetProjectionD1, projX);
        Write<float>(camera + offsets::camera::GetProjectionD2 + 4, projY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void OakSpeedStickSelfTestOnce(uintptr_t lp); // defined after Batch4WriteVsPos

static void OakStaminaSelfTestOnce(uintptr_t lp)
{
    // Present-thread only. Proof requires PlayerStat and/or StaminaHandler â€” not entity+0x6A4 alone.
    if (!IsValidPtr(lp) || lp < 0x100000000ULL)
        return;
    if (!g_StamCache.statRec && !g_StamCache.handler)
    {
        InterlockedExchange(&g_StamSelfTestPass, 0);
        Log("verify[stam-cycle] PASS=0 reason=no-real-target");
        return;
    }

    InterlockedExchange(&g_StamPauseRefill, 1);

    auto readReal = [&](float& outE, float& outP, float& outH) {
        outE = Read<float>(lp + oak_offsets::entity::Stamina);
        outP = -1.f;
        outH = -1.f;
        OakStamCache c = g_StamCache;
        if (c.statRec && c.statOff && IsHeapObj(c.statRec))
            outP = Read<float>(c.statRec + c.statOff);
        if (c.handler == 1 && c.meterN > 0)
            outH = Read<float>(c.meterOff[0]);
        else if (c.handler > 1 && c.meterN > 0 && IsHeapObj(c.handler))
            outH = Read<float>(c.handler + c.meterOff[0]);
    };

    float e0, p0, h0;
    readReal(e0, p0, h0);
    InterlockedExchange(&g_StamPauseRefill, 0);

    DWORD t0 = GetTickCount();
    int ok = 0;
    float pRec = p0, hRec = h0;
    for (int i = 0; i < 200; i++)
    {
        Batch4ForceStaminaFull(lp);
        float e, p, h;
        readReal(e, p, h);
        pRec = p; hRec = h;
        bool pOk = (p >= 0.f) && ((g_StamCache.statFull <= 1.05f) ? (p >= 0.85f) : (p >= 85.f));
        bool hOk = (h < 0.f) || (h >= 85.f);
        bool eOk = (e == e && e >= 0.85f);
        if (eOk && (pOk || hOk))
        {
            ok = 1;
            break;
        }
        Sleep(0);
    }
    DWORD ms = GetTickCount() - t0;

    for (int i = 0; i < 60; i++)
        Batch4ForceStaminaFull(lp);

    float eS, pS, hS;
    readReal(eS, pS, hS);
    bool stickP = (pS >= 0.f) && ((g_StamCache.statFull <= 1.05f) ? (pS >= 0.85f) : (pS >= 85.f));
    bool stickH = (hS < 0.f) || (hS >= 85.f);
    bool stickE = (eS == eS && eS >= 0.85f);
    int haveReal = (g_StamCache.statRec || g_StamCache.handler) ? 1 : 0;
    int pass = (ok && stickE && (stickP || stickH) && haveReal) ? 1 : 0;
    InterlockedExchange(&g_StamSelfTestPass, pass);

    char b[280];
    wsprintfA(b,
        "verify[stam-cycle] e0=%d p0=%d h0=%d refillP=%d refillH=%d stickP=%d stickH=%d ms=%u have=%d PASS=%d",
        (int)(e0 * 100.f + 0.5f), (int)(p0 + 0.5f), (int)(h0 + 0.5f),
        (int)(pRec + 0.5f), (int)(hRec + 0.5f),
        stickP ? 1 : 0, stickH ? 1 : 0, (unsigned)ms, haveReal, pass);
    Log(b);
}

static DWORD WINAPI OakFovWorkerMain(LPVOID)
{
    Log("fov: writer thread started (cam-proj only; no FOV_Context)");
    while (InterlockedCompareExchange(&g_FovWorkerRun, 1, 1) == 1 && !g_ShuttingDown)
    {
        if (!OakLab_IsWriteAllowed(OAK_LAB_FOV))
        {
            Sleep(16);
            continue;
        }
        float deg = g_FovWantHorizDeg;
        uintptr_t cam = g_FovWorkerCam;
        if (deg >= 60.f && IsValidPtr(cam) && cam > 0x100000000ULL && !g_MiscFreecam && !g_PanicHidden)
            Batch4ForceCameraProj(cam, deg);
        Sleep(16); // was 1msÃ—4 â€” fought sim rebuild and spiked AV risk
    }
    Log("fov: writer thread exit");
    return 0;
}

static void OakFovEnsureWorker()
{
    if (g_FovWorker)
        return;
    InterlockedExchange(&g_FovWorkerRun, 1);
    g_FovWorker = CreateThread(nullptr, 0, OakFovWorkerMain, nullptr, 0, nullptr);
}

static void OakFovStopWorker()
{
    InterlockedExchange(&g_FovWorkerRun, 0);
}

void OakApplyCameraFov(uintptr_t camera)
{
    if (!g_WorldMisc.fovChanger || g_MiscFreecam || g_PanicHidden)
    {
        OakFovReleaseCamHook();
        OakFovStopWorker();
        lstrcpynA(g_Batch4QaFov, "off", 56);
        return;
    }
    if (!OakLab_IsWriteAllowed(OAK_LAB_FOV))
    {
        lstrcpynA(g_Batch4QaFov, "lab-deny", 56);
        return;
    }

    float deg = g_WorldMisc.horizontalFov;
    if (deg < 70.f) deg = 70.f;
    if (deg > 110.f) deg = 110.f;
    g_FovWantHorizDeg = deg;
    g_FovWorkerCam = camera;

    // Game-thread entry hook @ 0x7A0330 writes +194 before the camera rebuild
    // reads it â€” that is what actually owns the visual FOV. Present/worker
    // cam-proj writes stay as a fallback echo for W2S.
    if (IsValidPtr(camera) && camera > 0x100000000ULL)
    {
        OakFovEnsureCamHook(camera);
        Batch4ForceCameraProj(camera, deg);
        OakFovEnsureWorker();
        char b[56];
        wsprintfA(b, "worker-proj tgt=%d", (int)(deg + 0.5f));
        lstrcpynA(g_Batch4QaFov, b, 56);
    }
    else
        lstrcpynA(g_Batch4QaFov, "no-cam", 56);
}

static bool g_Batch4StreamProofApplied = false;
static bool g_Batch4DoorLogged = false;
static char g_Batch4StatusWarp[72] = "idle";

// Lag Switch: outbound packet hold (see oak_lag_switch.inl). Old freecam-warp removed.
// g_WarpDesyncActive / g_WarpCommitting / g_WarpGhostPos / g_WarpGhostValid live in main.cpp (cleared on tick)

static bool Batch4Vec3Finite(const Vec3& v)
{
    return v.x == v.x && v.y == v.y && v.z == v.z
        && fabsf(v.x) < 50000.f && fabsf(v.y) < 50000.f && fabsf(v.z) < 50000.f;
}

static Vec3 Batch4CameraForward(uintptr_t camera)
{
    Vec3 fwd = Read<Vec3>(camera + offsets::camera::InvertedViewForward);
    float fl = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (fl > 1e-4f)
    {
        fwd.x /= fl;
        fwd.y /= fl;
        fwd.z /= fl;
    }
    else
        fwd = { 0.f, 0.f, 1.f };
    return fwd;
}

static void Batch4WriteVsPos(uintptr_t entity, const Vec3& pos)
{
    if (!IsValidPtr(entity) || entity < 0x100000000ULL)
        return;
    if (!Batch4Vec3Finite(pos))
        return;
    __try
    {
        uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
        if (!IsValidPtr(vs) || vs < 0x100000000ULL)
            return;
        Write<float>(vs + 0x2C, pos.x);
        Write<float>(vs + 0x30, pos.y);
        Write<float>(vs + 0x34, pos.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void Batch4WriteVsPosBurst(uintptr_t entity, const Vec3& pos, int times)
{
    if (times < 1) times = 1;
    if (times > 8) times = 8;
    for (int i = 0; i < times; i++)
        Batch4WriteVsPos(entity, pos);
}
#include "oak_lag_switch.inl"



static void Batch4ApplyTimeWeather(uintptr_t worldPtr); // defined below

static void Batch4Warp(uintptr_t worldPtr, uintptr_t localPlayer)
{
    (void)localPlayer;
    // Kill leftover freecam-desync state from older builds (once).
    if (g_WarpDesyncActive || g_WarpCommitting || g_WarpGhostValid)
    {
        g_WarpDesyncActive = false;
        g_WarpCommitting = false;
        g_WarpGhostValid = false;
        if (g_MiscFreecam)
        {
            g_MiscFreecam = false;
            MiscFreecamShutdown();
            MiscPushUi();
        }
    }
    LagSwitchTick();
    // Weather applied once in OakBatch4UpdateMisc â€” avoid double Write storm here.
    if (IsValidPtr(worldPtr) && worldPtr > 0x100000000ULL)
    {
        static bool s_WxIni = false;
        if (!s_WxIni)
        {
            s_WxIni = true;
            char ini[MAX_PATH] = {};
            const char* app = getenv("LOCALAPPDATA");
            if (app && app[0])
            {
                wsprintfA(ini, "%s\\DayZ\\oak_config.ini", app);
                if (GetPrivateProfileIntA("worldMisc", "clearWx", 0, ini))
                    g_WorldMisc.clearWeather = true;
                if (GetPrivateProfileIntA("worldMisc", "timeLock", 0, ini))
                    g_WorldMisc.timeLock = true;
            }
        }
    }
    lstrcpynA(g_Batch4StatusWarp, g_LagStatus, 72);
}

void OakBatch4DrawWarpOverlay()
{
    LagSwitchDrawOverlay();
}

static void OakSpeedStickSelfTestOnce(uintptr_t lp)
{
    if (!IsValidPtr(lp) || lp < 0x100000000ULL)
        return;
    Vec3 p0 = {};
    if (!GetEntityPosition(lp, p0))
        return;
    Vec3 p1 = p0;
    p1.x += 2.0f;
    for (int i = 0; i < 16; i++)
        Batch4WriteVsPos(lp, p1);
    Sleep(50);
    Vec3 p2 = {};
    GetEntityPosition(lp, p2);
    float dx = p2.x - p0.x;
    float dz = p2.z - p0.z;
    float dist = sqrtf(dx * dx + dz * dz);
    Batch4WriteVsPos(lp, p0);
    int pass = (dist >= 0.75f) ? 1 : 0;
    InterlockedExchange(&g_SpeedSelfTestPass, pass);
    OakLab_ContractSpeedStick((int)(dist * 100.f + 0.5f));
    char b[160];
    wsprintfA(b, "verify[speed-stick] distCm=%d PASS=%d", (int)(dist * 100.f + 0.5f), pass);
    Log(b);
    if (!pass)
    {
        lstrcpynA(g_Batch4QaSpeed, "vs-rubberband", 56);
        // Proven non-functional on this build â€” stop wasting Present on move nudges.
        g_WorldMisc.speedHack = false;
    }
}

static void Batch4ApplyFov(uintptr_t worldPtr, uintptr_t localPlayer)
{
    (void)localPlayer;
    if (!g_WorldMisc.fovChanger)
    {
        lstrcpynA(g_Batch4QaFov, "off", 56);
        return;
    }
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000ULL)
        return;
    uintptr_t cam = Read<uintptr_t>(worldPtr + offsets::world::Camera);
    OakApplyCameraFov(cam);
}

static void Batch4ApplyTimeWeather(uintptr_t worldPtr)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;
    // Idle path: no heartbeat / scan when both modules off.
    if (!g_WorldMisc.clearWeather && !g_WorldMisc.timeLock)
        return;

    __try
    {
        if (g_WorldMisc.timeLock)
        {
            float hour = g_WorldMisc.lockHour;
            if (hour < 0.f) hour = 0.f;
            if (hour > 24.f) hour = 24.f;
            Write<float>(worldPtr + oak_offsets::world::Hour, hour);
        }

        if (!g_WorldMisc.clearWeather)
            return;

        // Maintain clear-weather at ~4 Hz once settled (engine fights slowly).
        static DWORD s_WxApply = 0;
        static int s_WxStable = 0;
        DWORD nowWx = GetTickCount();
        const bool forceWx = !s_WxApply || (nowWx - s_WxApply) > 250;
        if (!forceWx && s_WxStable >= 3)
            return;
        s_WxApply = nowWx;

        uintptr_t wc = Read<uintptr_t>(worldPtr + oak_offsets::world::WeatherController);
        // Resolve WC if stock offset is stale/null â€” scan nearby world slots once.
        if (!IsValidPtr(wc) || wc < 0x100000000ULL)
        {
            static uintptr_t s_WcResolved = 0;
            static uintptr_t s_WcOff = 0;
            if (s_WcResolved && IsValidPtr(s_WcResolved))
            {
                wc = s_WcResolved;
            }
            else
            {
                int bestScore = 0;
                uintptr_t best = 0;
                uintptr_t bestOff = 0;
                for (uintptr_t off = 0x6800; off <= 0x8200; off += 8)
                {
                    uintptr_t cand = Read<uintptr_t>(worldPtr + off);
                    if (!IsValidPtr(cand) || cand < 0x100000000ULL) continue;
                    int score = 0;
                    for (uintptr_t poff = 0x08; poff <= 0x80; poff += 8)
                    {
                        uintptr_t phen = Read<uintptr_t>(cand + poff);
                        if (!IsValidPtr(phen) || phen < 0x100000000ULL) continue;
                        for (uintptr_t foff = 0x08; foff <= 0x30; foff += 4)
                        {
                            float v = Read<float>(phen + foff);
                            if (v == v && v >= 0.f && v <= 1.01f) score++;
                        }
                    }
                    for (uintptr_t foff = 0x08; foff <= 0x60; foff += 4)
                    {
                        float v = Read<float>(cand + foff);
                        if (v == v && v > 0.05f && v <= 1.01f) score++;
                    }
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = cand;
                        bestOff = off;
                    }
                }
                if (best && bestScore >= 2)
                {
                    s_WcResolved = best;
                    s_WcOff = bestOff;
                    wc = best;
                    char fb[96];
                    wsprintfA(fb, "weather: resolved WC off=0x%X score=%d ptr=%p",
                        (unsigned)bestOff, bestScore, (void*)best);
                    Log(fb);
                }
                else
                {
                    static int s_Miss = 0;
                    if (s_Miss < 3)
                    {
                        char mb[80];
                        wsprintfA(mb, "weather: WC scan miss bestScore=%d", bestScore);
                        Log(mb);
                        s_Miss++;
                    }
                }
            }
            (void)s_WcOff;
        }
        static DWORD s_WxLog = 0;
        DWORD now = GetTickCount();
        if (!IsValidPtr(wc) || wc < 0x100000000ULL)
        {
            if (!s_WxLog || (now - s_WxLog) > 4000)
            {
                s_WxLog = now;
                Log("weather: clear ON but WeatherController null/invalid");
            }
            return;
        }

        int wrote = 0;
        int touched = 0;
        auto tryZeroFloat = [&](uintptr_t addr) {
            float v = Read<float>(addr);
            if (v != v) return; // NaN
            // Phenomenon actual/forecast values live in [0,1].
            if (v >= 0.f && v <= 1.01f)
            {
                touched++;
                if (v > 0.0001f)
                {
                    Write<float>(addr, 0.f);
                    wrote++;
                }
            }
        };

        // Direct floats on Weather controller.
        for (uintptr_t off = 0x08; off <= 0x120; off += 4)
            tryZeroFloat(wc + off);

        // Nested WeatherPhenomenon objects (Overcast/Rain/Fog/Snowâ€¦): zero
        // their early floats (GetActual / GetForecast live near object head).
        for (uintptr_t poff = 0x08; poff <= 0xA0; poff += 8)
        {
            uintptr_t phen = Read<uintptr_t>(wc + poff);
            if (!IsValidPtr(phen) || phen < 0x100000000ULL) continue;
            if (phen == wc) continue;
            for (uintptr_t foff = 0x08; foff <= 0x40; foff += 4)
                tryZeroFloat(phen + foff);
        }

        if (!s_WxLog || (now - s_WxLog) > 8000)
        {
            s_WxLog = now;
            char b[140];
            wsprintfA(b, "weather: clear wc=%p wrote=%d touched=%d", (void*)wc, wrote, touched);
            Log(b);
        }
        if (wrote == 0)
            s_WxStable++;
        else
            s_WxStable = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void Batch4ApplyThirdPerson()
{
    // Vanilla unlock is MiscApplyThirdPerson: Network+0x9C=1 on Network object
    // (NOT ManagerNetworkClient+0x50 â€” that path is BAN_NET).
}

static void Batch4GuideGrenades(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!g_Exploits.grenadeThroughWalls)
        return;
    // Grenade Teleporter owns VS writes when enabled â€” don't fight camera-forward steer.
    if (g_GrenadeTeleport.enabled)
        return;
    if (!IsValidPtr(worldPtr))
        return;

    uintptr_t bulletData = 0;
    int bulletCount = 0;
    bool ptrArray = true;
    if (!ResolveBulletArray(worldPtr, bulletData, bulletCount, ptrArray))
        return;

    uintptr_t cam = Read<uintptr_t>(worldPtr + oak_offsets::world::Camera);
    if (!IsValidPtr(cam))
        return;

    Vec3 origin = g_CameraValid ? g_CameraPos : g_LocalPlayerPos;
    Vec3 fwd = Batch4CameraForward(cam);
    Vec3 target = {
        origin.x + fwd.x * 40.f,
        origin.y + fwd.y * 40.f,
        origin.z + fwd.z * 40.f
    };

    for (int i = 0; i < bulletCount && i < 64; i++)
    {
        uintptr_t bullet = ptrArray
            ? Read<uintptr_t>(bulletData + (uintptr_t)i * 8)
            : bulletData + (uintptr_t)i * 0x100;
        if (!IsValidPtr(bullet) || bullet == localPlayer)
            continue;
        if (!EntityLooksLikeGrenade(bullet))
            continue;
        CombatWriteEntityWorldPos(bullet, target);
    }
}

// DayZ 1.29 Building door natives (Enforce registration / IsDoorLocked body).
// Door table: [building+0x6B8] stride 0x38; locked = byte0 & 1. Count @ +0x6C4.
// Open/Unlock thin trampolines jmp VT[+0x930]/[+0x948].
static constexpr uintptr_t kDoorRvaIsLocked = 0x7A99F0;
static constexpr uintptr_t kDoorRvaOpen = 0x7AB130;
static constexpr uintptr_t kDoorRvaUnlock = 0x7AB140;
static constexpr uintptr_t kDoorRvaLock = 0x7AB120;
static constexpr uintptr_t kBuildingDoorArray = 0x6B8;
static constexpr uintptr_t kBuildingDoorCount = 0x6C4;
static constexpr uintptr_t kDoorStride = 0x38;

typedef bool(__fastcall* Fn_BuildingIsDoorLocked)(void* building, int doorIndex);
typedef void(__fastcall* Fn_BuildingDoorOp)(void* building, int doorIndex);
// LockDoor(index, force) / UnlockDoor(index, animate) â€” third arg in r8.
typedef void(__fastcall* Fn_BuildingDoorOpBool)(void* building, int doorIndex, unsigned char flag);

static Fn_BuildingIsDoorLocked g_FnIsDoorLocked = nullptr;
static Fn_BuildingDoorOp g_FnOpenDoor = nullptr;
static Fn_BuildingDoorOpBool g_FnUnlockDoor = nullptr;
static Fn_BuildingDoorOpBool g_FnLockDoor = nullptr;
static bool g_DoorNativesOk = false;
static DWORD g_DoorUnlockLastTick = 0;
static bool g_DoorSelfTestDone = false;
static bool g_DoorServerFlagOn = false;

static void Batch4DoorServerFlag(bool enable)
{
    if (enable == g_DoorServerFlagOn)
        return;
    g_DoorServerFlagOn = enable;
    // Same-machine dedicated server reads $profile:oak_door_unlock.on (oak_profiles).
    CreateDirectoryA("C:\\oak", nullptr);
    CreateDirectoryA("C:\\oak\\dayz", nullptr);
    const char* paths[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZServer\\oak_profiles\\oak_door_unlock.on",
        "C:\\oak\\dayz\\oak_door_unlock.on",
        nullptr
    };
    for (int i = 0; paths[i]; i++)
    {
        if (enable)
        {
            HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE)
            {
                DWORD w = 0;
                WriteFile(hf, "1\n", 2, &w, nullptr);
                CloseHandle(hf);
            }
        }
        else
        {
            DeleteFileA(paths[i]);
        }
    }
    Log(enable ? "door: server flag ON ($profile:oak_door_unlock.on)" : "door: server flag OFF");
}

static bool Batch4ResolveDoorNatives()
{
    if (g_DoorNativesOk)
        return true;
    if (!g_GameModule)
        return false;
    uintptr_t base = (uintptr_t)g_GameModule;
    BYTE* isLocked = (BYTE*)(base + kDoorRvaIsLocked);
    BYTE* open = (BYTE*)(base + kDoorRvaOpen);
    BYTE* unlock = (BYTE*)(base + kDoorRvaUnlock);
    BYTE* lock = (BYTE*)(base + kDoorRvaLock);
    __try
    {
        if (!(isLocked[0] == 0x8B && isLocked[1] == 0xC2 && isLocked[2] == 0x48 && isLocked[3] == 0x6B))
            return false;
        if (!(open[0] == 0x48 && open[1] == 0x8B && open[2] == 0x01 && open[3] == 0x48 && open[4] == 0xFF))
            return false;
        if (!(unlock[0] == 0x48 && unlock[1] == 0x8B && unlock[2] == 0x01 && unlock[3] == 0x48 && unlock[4] == 0xFF))
            return false;
        if (!(lock[0] == 0x48 && lock[1] == 0x8B && lock[2] == 0x01 && lock[3] == 0x48 && lock[4] == 0xFF))
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // A byte pattern can also match mid-function; require real function entries too.
    if (!OakNativeEntryOk((uintptr_t)isLocked, "Building::IsDoorLocked") ||
        !OakNativeEntryOk((uintptr_t)open, "Building::OpenDoor") ||
        !OakNativeEntryOk((uintptr_t)unlock, "Building::UnlockDoor") ||
        !OakNativeEntryOk((uintptr_t)lock, "Building::LockDoor"))
        return false;

    g_FnIsDoorLocked = (Fn_BuildingIsDoorLocked)isLocked;
    g_FnOpenDoor = (Fn_BuildingDoorOp)open;
    g_FnUnlockDoor = (Fn_BuildingDoorOpBool)unlock;
    g_FnLockDoor = (Fn_BuildingDoorOpBool)lock;
    g_DoorNativesOk = true;
    Log("door: natives resolved IsLocked/Open/Unlock/Lock");
    return true;
}

static bool Batch4BuildingDoorTableOk(uintptr_t ent, int& outCount)
{
    outCount = 0;
    if (!IsValidPtr(ent) || ent < 0x100000000)
        return false;
    int count = 0;
    uintptr_t arr = 0;
    __try
    {
        count = Read<int>(ent + kBuildingDoorCount);
        arr = Read<uintptr_t>(ent + kBuildingDoorArray);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (count < 1 || count > 24)
        return false;
    if (!IsValidPtr(arr) || arr < 0x100000000)
        return false;
    outCount = count;
    return true;
}

static bool Batch4DoorIsLocked(uintptr_t building, int doorIndex)
{
    if (g_FnIsDoorLocked)
    {
        __try { return g_FnIsDoorLocked((void*)building, doorIndex); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try
    {
        uintptr_t arr = Read<uintptr_t>(building + kBuildingDoorArray);
        if (!IsValidPtr(arr)) return false;
        unsigned char flags = Read<unsigned char>(arr + (uintptr_t)doorIndex * kDoorStride);
        return (flags & 1) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void Batch4DoorClearLockBit(uintptr_t building, int doorIndex)
{
    __try
    {
        uintptr_t arr = Read<uintptr_t>(building + kBuildingDoorArray);
        if (!IsValidPtr(arr)) return;
        uintptr_t slot = arr + (uintptr_t)doorIndex * kDoorStride;
        unsigned char flags = Read<unsigned char>(slot);
        if (flags & 1)
            Write<unsigned char>(slot, (unsigned char)(flags & ~1u));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool Batch4DoorUnlockOne(uintptr_t building, int doorIndex)
{
    Batch4DoorClearLockBit(building, doorIndex);
    bool ok = false;
    __try
    {
        if (g_FnUnlockDoor)
            g_FnUnlockDoor((void*)building, doorIndex, 1);
        if (g_FnOpenDoor)
            g_FnOpenDoor((void*)building, doorIndex);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

static void Batch4DoorUnlock(uintptr_t worldPtr, uintptr_t localPlayer)
{
    OAK_MARK("door");
    if (!g_Exploits.doorUnlock || g_PanicHidden || g_ShuttingDown)
    {
        Batch4DoorServerFlag(false);
        lstrcpynA(g_Batch4StatusDoor, "idle", 72);
        g_Batch4DoorLogged = false;
        return;
    }
    Batch4DoorServerFlag(true);
    if (!Batch4ResolveDoorNatives())
    {
        lstrcpynA(g_Batch4StatusDoor, "natives missing (build?)", 72);
        return;
    }
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;

    DWORD now = GetTickCount();
    if (now - g_DoorUnlockLastTick < 80)
        return;
    g_DoorUnlockLastTick = now;

    Vec3 origin = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    uintptr_t cam = Read<uintptr_t>(worldPtr + oak_offsets::world::Camera);
    Vec3 fwd = IsValidPtr(cam) ? Batch4CameraForward(cam) : Vec3{ 0.f, 0.f, 1.f };

    // Buildings are often flag!=1 and live in Slow + Item lists (same as occ Land_ scan).
    const float maxDist = 25.f;

    struct Cand {
        uintptr_t ent;
        float dist;
        float dot;
        int doorCount;
        int lockedMask;
        char name[48];
    };
    Cand best{};
    best.dist = 1.0e9f;
    int seenBuild = 0, seenLocked = 0, landNear = 0;

    auto consider = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            return;
        char cfg[64] = {}, tn[64] = {};
        __try {
            ReadEntityConfigName(ent, cfg, 64);
            ReadEntityTypeName(ent, tn, 64);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        bool landish = StrContainsI(tn, "Land_") || StrContainsI(cfg, "Land_") ||
            StrContainsI(tn, "House") || StrContainsI(cfg, "House") ||
            StrContainsI(tn, "Building") || StrContainsI(cfg, "Building") ||
            StrContainsI(cfg, "house");

        int doorCount = 0;
        bool tableOk = Batch4BuildingDoorTableOk(ent, doorCount);
        if (!tableOk && !landish)
            return;

        Vec3 pos{};
        if (!GetEntityPosition(ent, pos) || !Batch4Vec3Finite(pos))
            return;
        float dist = Distance3D(origin, pos);
        if (dist > maxDist)
            return;

        if (landish)
            landNear++;

        if (!tableOk)
        {
            // Land_ without door table at +0x6B8 â€” still log once for discovery.
            static int s_BadLand = 0;
            if (landish && s_BadLand < 12)
            {
                uintptr_t arr = Read<uintptr_t>(ent + kBuildingDoorArray);
                int rawC = Read<int>(ent + kBuildingDoorCount);
                char b[160];
                wsprintfA(b, "door: land-no-table '%s' dist=%d arr=%08X cnt=%d",
                    tn[0] ? tn : cfg, (int)dist, (DWORD)arr, rawC);
                Log(b);
                s_BadLand++;
            }
            return;
        }

        seenBuild++;
        int lockedMask = 0;
        int lockedN = 0;
        for (int d = 0; d < doorCount && d < 16; d++)
        {
            if (Batch4DoorIsLocked(ent, d))
            {
                lockedMask |= (1 << d);
                lockedN++;
            }
        }
        if (lockedN)
            seenLocked++;

        Vec3 to{ pos.x - origin.x, pos.y - origin.y, pos.z - origin.z };
        float tl = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
        float dot = 1.f;
        if (tl > 1e-3f)
        {
            to.x /= tl; to.y /= tl; to.z /= tl;
            dot = to.x * fwd.x + to.y * fwd.y + to.z * fwd.z;
        }

        // Prefer locked + in view; else nearest building with door table.
        float score = dist;
        if (lockedN > 0) score -= 100.f;
        if (dot > 0.35f) score -= 5.f;
        if (score < best.dist)
        {
            best.dist = score;
            best.ent = ent;
            best.dot = dot;
            best.doorCount = doorCount;
            best.lockedMask = lockedMask;
            const char* s = tn[0] ? tn : (cfg[0] ? cfg : "Building");
            lstrcpynA(best.name, s, 48);
        }
    };

    auto scanList = [&](uintptr_t dataOff, uintptr_t sizeOff, bool useValidCount, int hardCap) {
        uintptr_t data = Read<uintptr_t>(worldPtr + dataOff);
        int count = Read<int>(worldPtr + sizeOff);
        if (useValidCount)
        {
            int valid = Read<int>(worldPtr + oak_offsets::world::SlowEntValidCount);
            if (valid > count && valid < 30000)
                count = valid;
        }
        if (!IsValidPtr(data) || data < 0x100000000 || count <= 0 || count > 200000)
            return;
        int n = count > hardCap ? hardCap : count;
        for (int i = 0; i < n; i++)
        {
            uintptr_t entry = data + (uintptr_t)i * 0x18;
            WORD flag = 0;
            uintptr_t ent = 0;
            __try {
                flag = Read<WORD>(entry);
                ent = Read<uintptr_t>(entry + 8);
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (flag == 0) continue; // match occ scanner â€” not only flag==1
            consider(ent);
        }
    };

    scanList(oak_offsets::world::SlowEntList, oak_offsets::world::SlowTableSize, true, 800);
    scanList(oak_offsets::world::ItemList, oak_offsets::world::ItemListSize, false, 800);

    static DWORD s_Census = 0;
    if (now - s_Census > 5000)
    {
        s_Census = now;
        char b[192];
        wsprintfA(b, "door: census landNear=%d buildTable=%d locked=%d best='%s' distScore=%d lockMask=%X",
            landNear, seenBuild, seenLocked, best.name[0] ? best.name : "-",
            best.ent ? (int)best.dist : -1, best.lockedMask);
        Log(b);
    }

    if (!best.ent || best.doorCount <= 0)
    {
        wsprintfA(g_Batch4StatusDoor, "scan land=%d table=%d (no target)", landNear, seenBuild);
        return;
    }

    // One-shot self-test: LockDoor then UnlockDoor+OpenDoor on nearest building.
    // Proves client natives round-trip; server flag bridges dedicated authority.
    if (!g_DoorSelfTestDone && g_FnLockDoor)
    {
        g_DoorSelfTestDone = true;
        __try { g_FnLockDoor((void*)best.ent, 0, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        bool lockedNow = Batch4DoorIsLocked(best.ent, 0);
        bool unlocked = false;
        if (lockedNow)
            unlocked = Batch4DoorUnlockOne(best.ent, 0);
        char b[160];
        wsprintfA(b, "door: SELFTEST '%s' lock=%d unlock=%d", best.name, lockedNow ? 1 : 0, unlocked ? 1 : 0);
        Log(b);
        wsprintfA(g_Batch4StatusDoor, "selftest lock=%d unlock=%d", lockedNow ? 1 : 0, unlocked ? 1 : 0);
        return;
    }

    // Normal mode: only act on doors that are actually locked.
    if (best.lockedMask == 0)
    {
        wsprintfA(g_Batch4StatusDoor, "near '%s' â€” no locked door", best.name);
        return;
    }

    for (int d = 0; d < best.doorCount; d++)
    {
        if ((best.lockedMask & (1 << d)) == 0)
            continue;
        if (!Batch4DoorUnlockOne(best.ent, d))
            continue;
        char b[140];
        wsprintfA(b, "door: UNLOCKED '%s' idx=%d", best.name, d);
        Log(b);
        wsprintfA(g_Batch4StatusDoor, "UNLOCKED %s #%d", best.name, d);
        return;
    }
    wsprintfA(g_Batch4StatusDoor, "fail '%s' mask=%X", best.name, best.lockedMask);
}

static void Batch4ApplyStreamProof(HWND hwnd)
{
    // Internal overlay shares DayZ's HWND. WDA_EXCLUDEFROMCAPTURE on that window
    // blanks the game for the local user (Win10/11 compositor). Never set it here —
    // only clear any leftover affinity from older builds.
    static bool s_ClearedOnce = false;
    auto clearOne = [](HWND h) {
        if (h) SetWindowDisplayAffinity(h, WDA_NONE);
    };
    if (!s_ClearedOnce || (g_WorldMisc.streamProof || g_StreamProofUi))
    {
        clearOne(hwnd);
        HWND game = FindWindowA("DayZ", NULL);
        if (!game) game = FindWindowA(NULL, "DayZ");
        clearOne(game);
        s_ClearedOnce = true;
        g_Batch4StreamProofApplied = false;
    }
    if (g_WorldMisc.streamProof || g_StreamProofUi)
    {
        static DWORD s_Last = 0;
        DWORD now = GetTickCount();
        if (!s_Last || (now - s_Last) > 15000)
        {
            Log("streamproof: disabled for internal (would blank DayZ window)");
            s_Last = now;
        }
    }
}

void OakBatch4SyncFromSharedConfig()
{
    if (!g_SharedConfig || g_SharedConfig->magic != 0x4F414B00)
        return;

    if (g_SharedConfig->noRecoil)
        g_Recoil.noRecoil = true;
    if (g_SharedConfig->noSway)
        g_Recoil.noSway = true;
}

void OakBatch4PushSharedConfig()
{
    if (!g_SharedConfig || g_SharedConfig->magic != 0x4F414B00)
        return;

    g_SharedConfig->speedHack = false;
    g_SharedConfig->speedMultiplier = 1.f;
    g_SharedConfig->infiniteStamina = false;
    g_SharedConfig->noRecoil = g_Recoil.noRecoil;
    g_SharedConfig->noSway = g_Recoil.noSway;
    g_SharedConfig->streamProof = g_WorldMisc.streamProof || g_StreamProofUi;
}

void OakBatch4UpdateMisc(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (g_PanicHidden)
        return;

    uintptr_t lp = g_ResolvedLocalPlayer;
    if (!IsValidPtr(lp) || lp < 0x100000000)
        lp = localPlayer;

    OakBatch4SyncFromSharedConfig();

    __try
    {
        Batch4ApplyTimeWeather(worldPtr);
        // Batch4Warp runs every Present from OakUpdateGameplay (needs high rate).
        if (OakLab_IsWriteAllowed(OAK_LAB_GRENADE) && g_Exploits.grenadeThroughWalls)
            Batch4GuideGrenades(worldPtr, lp);
        Batch4DoorUnlock(worldPtr, lp);
        OakBatch4PushSharedConfig();
        OakBatch4LiveVerify(worldPtr, lp);
        ImGuiMenu_SetBatch4Status(g_Batch4StatusDoor);
        ImGuiMenu_SetWarpStatus(g_Batch4StatusWarp);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int s_Ex = 0;
        if ((s_Ex++ % 120) == 0)
            Log("batch4: exception caught (safe continue)");
    }
}

void OakBatch4OnPresent(HWND hwnd)
{
    __try
    {
        Batch4ApplyStreamProof(hwnd);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void OakBatch4LogLiveQa()
{
    char qb[160];
    wsprintfA(qb, "liveqa[batch4] fov=%s speed=%s stam=%s",
        g_Batch4QaFov, g_Batch4QaSpeed, g_Batch4QaStam);
    Log(qb);
}

void OakBatch4LiveVerify(uintptr_t worldPtr, uintptr_t localPlayer)
{
    // Measurement-only, throttled. PASS = memory matches physical effect source.
    if (!g_GameModule || g_PanicHidden)
        return;
    static DWORD s_Last = 0;
    DWORD now = GetTickCount();
    if (s_Last && (now - s_Last) < 1500)
        return;
    s_Last = now;

    uintptr_t lp = g_ResolvedLocalPlayer;
    if (!IsValidPtr(lp) || lp < 0x100000000ULL)
        lp = localPlayer;

    __try
    {
        if (g_WorldMisc.fovChanger && IsValidPtr(worldPtr))
        {
            uintptr_t cam = Read<uintptr_t>(worldPtr + offsets::world::Camera);
            float fov = g_WorldMisc.horizontalFov;
            if (fov < 70.f) fov = 70.f;
            if (fov > 110.f) fov = 110.f;
            float want = tanf((fov * 0.5f) * (3.14159265f / 180.f));
            float got = IsValidPtr(cam) ? Read<float>(cam + offsets::camera::GetProjectionD1) : -1.f;
            float src = IsValidPtr(cam) ? Read<float>(cam + 0x194) : -1.f;
            int err = (want > 0.01f && got == got) ? (int)(fabsf(got - want) / want * 100.f + 0.5f) : 999;
            int srcErr = (want > 0.01f && src == src) ? (int)(fabsf(src - want) / want * 100.f + 0.5f) : 999;
            int contract = OakLab_ContractFovMemory(want, got, (float)err);
            // Prefer src(+194) match for PASS signal â€” that's what the rebuild reads.
            // Entry hook hits mean we own the value before matrix rebuild (visual path).
            long hits = InterlockedCompareExchange(&g_FovHookHits, 0, 0);
            int hooked = InterlockedCompareExchange(&g_FovHookInstalled, 0, 0) ? 1 : 0;
            char b[220];
            wsprintfA(b, "verify[fov] tgt=%d tanWant=%d tanGot=%d src194=%d errPct=%d srcErr=%d contract=%d hook=%d hits=%d",
                (int)(fov + 0.5f),
                (int)(want * 1000.f + 0.5f),
                (int)(got * 1000.f + 0.5f),
                (int)(src * 1000.f + 0.5f),
                err, srcErr, contract, hooked, (int)hits);
            Log(b);
        }



    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
