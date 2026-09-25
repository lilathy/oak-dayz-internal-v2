// Batch 4 — Phase A combat (shipped; see docs/remaining.txt for leftovers).
// Included from main.cpp after esp_addons_impl.inl (uses combat helpers there).

static CombatTarget g_SilentLock = {};
static bool g_Batch4AutoFireHeld = false;
static bool g_Batch4TriggerHeld = false;
static bool g_Batch4TriggerPendingUp = false;
static DWORD g_Batch4TriggerArmTick = 0;
static DWORD g_Batch4TriggerLastShot = 0;
static DWORD g_Batch4TriggerIgnoreUserLmbUntil = 0;
static uintptr_t g_Batch4TriggerTarget = 0;
static int g_Batch4TriggerConfirm = 0;

static int Batch4EffectiveAimKey()
{
    if (g_BindAimAssist > 0) return g_BindAimAssist;
    if (g_AimbotB4.aimAssistKey > 0) return g_AimbotB4.aimAssistKey;
    return g_AimbotKey;
}

static int Batch4EffectiveSilentKey()
{
    if (g_BindSilentAim > 0) return g_BindSilentAim;
    if (g_SilentAim.key > 0) return g_SilentAim.key;
    return 0;
}

static bool Batch4CombatGatesOk(uintptr_t localPlayer, bool* outReloading)
{
    if (outReloading) *outReloading = false;
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
        return false;
    if (ImGuiMenu_IsOpen() || g_PanicHidden || g_MiscFreecam)
        return false;

    uintptr_t hands = GetLocalHandsWeapon(localPlayer);
    if (!IsValidPtr(hands))
        return false;

    bool reloading = false;
    float prog = 0.f;
    OakProbeReloadState(hands, &reloading, &prog);
    if (outReloading) *outReloading = reloading;
    if (reloading)
        return false;

    int ammo = -1;
    char wpn[64] = {};
    if (!GetHandWeaponInfo(localPlayer, wpn, 64, &ammo))
        return IsValidPtr(hands);
    if (ammo == 0)
        return false;
    return true;
}

static void Batch4SendLmb(bool down)
{
    // DayZ eats some input paths — fire synthetic mouse + window messages.
    mouse_event(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &inp, sizeof(INPUT));
    HWND hwnd = FindWindowA("DayZ", NULL);
    if (!hwnd) hwnd = FindWindowA(NULL, "DayZ");
    if (hwnd)
    {
        if (down)
            PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        else
            PostMessageA(hwnd, WM_LBUTTONUP, 0, 0);
    }
}

static void Batch4ReleaseSyntheticLmb()
{
    if (g_Batch4AutoFireHeld || g_Batch4TriggerHeld || g_Batch4TriggerPendingUp)
        Batch4SendLmb(false);
    g_Batch4AutoFireHeld = false;
    g_Batch4TriggerHeld = false;
    g_Batch4TriggerPendingUp = false;
    g_Batch4TriggerArmTick = 0;
    g_Batch4TriggerTarget = 0;
    g_Batch4TriggerConfirm = 0;
    // Long enough that sticky GetAsyncKeyState after our UP can't soft-lock the bot.
    g_Batch4TriggerIgnoreUserLmbUntil = GetTickCount() + 200;
}

// ---------------------------------------------------------------------------
// T4-3 No recoil / sway — mid-hook AFTER native AimingModel script invoke.
// Live site (1.29): DayZPlayer::AimingModel @ cmp [rcx+0x2760],-1 → post-script
// epilogue RVA 0x4E9126 (was 0x4E9D16 on 163451). At that point:
//   RBX = DayZPlayer*, RDI = SDayZPlayerAimingModel* (r8 in), RAX = script retval
//   (NOT a float block — treating RAX/player+0x1C0 as floats corrupts and exits).
// Model kick floats: +0x10/14 CamXY, +0x18/1C HandsXY, +0x20/24 MouseXY, +0x28.. CamPos.
// Cold documented site 0x1A9037 still exists (vt+0x2D0 RAX float consume) but hits=0.
// NEVER write player+0x1C0 / InputController / VisualState / CharacterCtrl.
// ---------------------------------------------------------------------------
enum { kOakRecoilStolenMin = 14 };
enum { kOakRecoilStolenMax = 16 };
enum { kOakRecoilHookMax = 2 };
struct OakRecoilHookSlot {
    BYTE* site;
    BYTE* tramp;
    BYTE orig[24];
    int stolen;
};
static OakRecoilHookSlot g_RecoilHooks[kOakRecoilHookMax] = {};
static int g_RecoilHookN = 0;
static volatile LONG g_RecoilHookInstalled = 0;
static volatile LONG g_RecoilHookHits = 0;
static volatile LONG g_RecoilHookWrites = 0;
static DWORD g_RecoilLastLog = 0;
static UINT64 g_RecoilSavedRax = 0;

static float OakRecoilClampPct(int pct)
{
    float k = (float)pct / 100.f;
    if (k < 0.f) k = 0.f;
    if (k > 1.f) k = 1.f;
    return k;
}

// Sway = hands breathing/noise (+0x18/1C). Recoil = mouse shift + cam kick + camPos punch.
static float OakRecoilKeepRecoil()
{
    if (!g_Recoil.noRecoil) return 1.f;
    return OakRecoilClampPct(g_Recoil.recoilPct);
}
static float OakRecoilKeepSway()
{
    if (!g_Recoil.noSway) return 1.f;
    return OakRecoilClampPct(g_Recoil.swayPct);
}
static float OakRecoilKeepFactor() // status display: strictest active suppress
{
    float k = 1.f;
    if (g_Recoil.noRecoil)
    {
        float kr = OakRecoilKeepRecoil();
        if (kr < k) k = kr;
    }
    if (g_Recoil.noSway)
    {
        float ks = OakRecoilKeepSway();
        if (ks < k) k = ks;
    }
    return k;
}

static volatile LONG g_RecoilPeakMouseX1000 = 0;
static volatile LONG g_RecoilPeakHandsX1000 = 0;
static volatile LONG g_RecoilPeakCamX1000 = 0;
static volatile LONG g_RecoilMouseWrites = 0;
static volatile LONG g_RecoilMouseYWrites = 0;
static uintptr_t g_RecoilLastModel = 0;
static uintptr_t g_RecoilAimImpl = 0;
static DWORD g_RecoilAimScan = 0;
static uintptr_t g_RecoilAimOff = 0; // player+off → aiming-like object

static void OakRecoilNotePeak(volatile LONG* slot, float v)
{
    float a = v < 0.f ? -v : v;
    LONG m = (LONG)(a * 1000000.f);
    if (m < 0) m = 0x7FFFFFFF;
    for (;;)
    {
        LONG cur = InterlockedCompareExchange(slot, 0, 0);
        if (m <= cur) break;
        if (InterlockedCompareExchange(slot, m, cur) == cur) break;
    }
}

static int OakRecoilScaleField(uintptr_t model, uintptr_t fo, float keep, float maxAbs)
{
    float cur = *(volatile float*)(model + fo);
    if (!(cur == cur))
        return 0;
    float a = cur < 0.f ? -cur : cur;
    if (a > maxAbs)
        return 0;
    float want = (keep <= 0.01f) ? 0.f : (cur * keep);
    if (want == cur)
        return 0;
    *(volatile float*)(model + fo) = want;
    return 1;
}

static int OakRecoilPtrOk(uintptr_t m)
{
    return m >= 0x100000000ULL && m <= 0x00007FFFFFFFFFFFULL;
}

static int OakRecoilPageReadable(uintptr_t p)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)p, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_GUARD)
        return 0;
    return 1;
}

static int OakRecoilLooksLikeVtable(uintptr_t m)
{
    // VirtualQuery BEFORE any deref — probing garbage player+off slots AVs the Present
    // thread even with SEH (DayZ/NVIDIA VEH can still tear down the process).
    if (!OakRecoilPtrOk(m) || (m & 7ull))
        return 0;
    if (!OakRecoilPageReadable(m))
        return 0;
    __try
    {
        uintptr_t v = *(uintptr_t*)m;
        if (!OakRecoilPtrOk(v) || (v & 0xFull))
            return 0;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)v, &mbi, sizeof(mbi)))
            return 0;
        if (mbi.State != MEM_COMMIT)
            return 0;
        return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int OakRecoilScoreFloats(float mx, float my, float hx, float hy, float cx, float cy)
{
    auto finiteKick = [](float v) { return v == v && fabsf(v) < 500.f; };
    if (!finiteKick(mx) || !finiteKick(my) || !finiteKick(hx) || !finiteKick(hy) || !finiteKick(cx) || !finiteKick(cy))
        return -1;
    auto hot = [](float v) { return v == v && fabsf(v) > 1e-5f && fabsf(v) < 500.f; };
    int s = 0;
    if (hot(mx) || hot(my)) s += 4;
    if (hot(hx) || hot(hy)) s += 2;
    if (hot(cx) || hot(cy)) s += 2;
    return s; // 0 = plausible but cold — do not write
}

// consume=0: object layout (+0x10 cam / +0x18 hands / +0x20 mouse)
// consume=1: RAX float block the epilogue copies (cam@0, hands@8, mouse@0x10)
static int OakRecoilScoreLayout(uintptr_t m, int consume)
{
    if (!OakRecoilPtrOk(m) || (m & 3ull))
        return -1;
    if (!OakRecoilPageReadable(m))
        return -1;
    __try
    {
        const uintptr_t off = consume ? 0ull : 0x10ull;
        float cx = *(volatile float*)(m + off + 0x00);
        float cy = *(volatile float*)(m + off + 0x04);
        float hx = *(volatile float*)(m + off + 0x08);
        float hy = *(volatile float*)(m + off + 0x0C);
        float mx = *(volatile float*)(m + off + 0x10);
        float my = *(volatile float*)(m + off + 0x14);
        return OakRecoilScoreFloats(mx, my, hx, hy, cx, cy);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int OakRecoilApplyLayout(uintptr_t model, int consume, float keepR, float keepHands)
{
    if (!OakRecoilPtrOk(model) || (model & 3ull))
        return 0;
    const uintptr_t off = consume ? 0ull : 0x10ull;
    float cx, cy, hx, hy, mx, my;
    __try
    {
    cx = *(volatile float*)(model + off + 0x00);
    cy = *(volatile float*)(model + off + 0x04);
    hx = *(volatile float*)(model + off + 0x08);
    hy = *(volatile float*)(model + off + 0x0C);
    mx = *(volatile float*)(model + off + 0x10);
    my = *(volatile float*)(model + off + 0x14);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    OakRecoilNotePeak(&g_RecoilPeakCamX1000, cx);
    OakRecoilNotePeak(&g_RecoilPeakCamX1000, cy);
    OakRecoilNotePeak(&g_RecoilPeakHandsX1000, hx);
    OakRecoilNotePeak(&g_RecoilPeakHandsX1000, hy);
    OakRecoilNotePeak(&g_RecoilPeakMouseX1000, mx);
    OakRecoilNotePeak(&g_RecoilPeakMouseX1000, my);

    int wrote = 0;
    if (g_Recoil.noRecoil)
    {
        const unsigned last = consume ? 0x2Cu : 0x30u;
        for (unsigned fo = (unsigned)off; fo <= last; fo += 4)
        {
            int w = OakRecoilScaleField(model, fo, keepR, 500.f);
            if (!w)
                continue;
            wrote += w;
            const unsigned rel = fo - (unsigned)off;
            if (rel == 0x10 || rel == 0x14)
                InterlockedIncrement(&g_RecoilMouseWrites);
            if (rel == 0x14)
                InterlockedIncrement(&g_RecoilMouseYWrites);
        }
    }
    if (g_Recoil.noSway || g_Recoil.noRecoil)
    {
        wrote += OakRecoilScaleField(model, off + 0x08, keepHands, 500.f);
        wrote += OakRecoilScaleField(model, off + 0x0C, keepHands, 500.f);
    }
    return wrote;
}

static void OakRecoilCensus(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t rbx)
{
    static LONG s_N = 0;
    if (InterlockedIncrement(&s_N) > 8)
        return;
    auto milli = [](uintptr_t p, unsigned off) -> int {
        __try
        {
            if (!OakRecoilPtrOk(p) || (p & 3ull))
                return 888888;
            float v = *(volatile float*)(p + off);
            if (!(v == v) || v > 1e6f || v < -1e6f)
                return 999999;
            return (int)(v * 1000.f);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 777777; }
    };
    char buf[280];
    wsprintfA(buf, "recoil: census a=%p c=%p rbx=%p a0=%d a10=%d a18=%d a20=%d c0=%d c10=%d c18=%d c20=%d rbx1c0=%d rbx1d0=%d rbx1e0=%d",
        (void*)a, (void*)c, (void*)rbx,
        milli(a, 0), milli(a, 0x10), milli(a, 0x18), milli(a, 0x20),
        milli(c, 0), milli(c, 0x10), milli(c, 0x18), milli(c, 0x20),
        milli(rbx, 0x1C0), milli(rbx, 0x1D0), milli(rbx, 0x1E0));
    Log(buf);
}

static int OakRecoilPageWritable(uintptr_t p)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)p, &mbi, sizeof(mbi)))
        return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    return (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY) ? 1 : 0;
}

// r8 = RDI = SDayZPlayerAimingModel* (AimingModel arg). Do not touch RAX/player+0x1C0.
extern "C" void __fastcall OakRecoilAfterAimingModel(uintptr_t /*a*/, uintptr_t /*b*/, uintptr_t c, uintptr_t /*rbx*/)
{
    InterlockedIncrement(&g_RecoilHookHits);
    if (!g_Recoil.noRecoil && !g_Recoil.noSway)
        return;

    const float keepR = OakRecoilKeepRecoil();
    const float keepHands = g_Recoil.noSway ? OakRecoilKeepSway() : keepR;
    // Model is often a stack SDayZPlayerAimingModel* (may be < 4GB) — don't use heap-only PtrOk.
    uintptr_t model = c;
    if (model < 0x10000ull || model > 0x00007FFFFFFFFFFFULL || (model & 3ull))
        return;
    if (!OakRecoilPageWritable(model))
        return;

    const int scObj = OakRecoilScoreLayout(model, 0);
    const int scRaw = OakRecoilScoreLayout(model, 1);
    static LONG s_Census;
    if (InterlockedIncrement(&s_Census) <= 10)
    {
        char b[200];
        auto milli = [](uintptr_t p, unsigned off) -> int {
            __try {
                float v = *(volatile float*)(p + off);
                if (!(v == v) || v > 1e5f || v < -1e5f) return 999999;
                return (int)(v * 1000.f);
            } __except (EXCEPTION_EXECUTE_HANDLER) { return 777777; }
        };
        wsprintfA(b, "recoil: model census scObj=%d scRaw=%d +10=%d +18=%d +20=%d +28=%d +30=%d +34=%d",
            scObj, scRaw,
            milli(model, 0x10), milli(model, 0x18), milli(model, 0x20),
            milli(model, 0x28), milli(model, 0x30), milli(model, 0x34));
        Log(b);
    }

    int wrote = 0;
    // Refuse writes on unrecognized layouts — bad RDI here corrupts heap and AVs the game.
    bool layoutOk = (scObj >= 2 || scRaw >= 2);

    // If the classic +0x10 layout is cold/wrong, scan the model for hot kick floats and
    // zero any finite kick-sized field (Present/ADS ownership without a fixed recipe).
    if (!layoutOk && (g_Recoil.noRecoil || g_Recoil.noSway))
    {
        int hotFields = 0;
        __try
        {
            for (unsigned fo = 0x08; fo <= 0x78; fo += 4)
            {
                // +0x14 is an int flag on some layouts — skip obvious intish patterns later.
                float cur = *(volatile float*)(model + fo);
                if (!(cur == cur) || fabsf(cur) < 1e-5f || fabsf(cur) > 80.f)
                    continue;
                hotFields++;
                OakRecoilNotePeak(&g_RecoilPeakMouseX1000, cur);
                if (g_Recoil.noRecoil)
                {
                    float want = (keepR <= 0.01f) ? 0.f : (cur * keepR);
                    if (want != cur)
                    {
                        *(volatile float*)(model + fo) = want;
                        wrote++;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (hotFields >= 1)
            layoutOk = true; // treat as validated dynamic layout for this fire
        static LONG s_HotOnce;
        if (hotFields > 0 && InterlockedIncrement(&s_HotOnce) <= 8)
        {
            char b[96];
            wsprintfA(b, "recoil: dynamic hot-fields=%d writes=%d", hotFields, wrote);
            Log(b);
        }
    }

    static LONG s_BadLayout;
    if (!layoutOk)
    {
        if (InterlockedIncrement(&s_BadLayout) == 25)
            Log("recoil: AimingModel layout never validated — mid-hook waits for ADS/fire kick floats");
        // Still remember the pointer so Present wipe can hammer it once it goes hot.
        g_RecoilLastModel = model;
        g_RecoilAimImpl = model;
        return;
    }
    InterlockedExchange(&s_BadLayout, 0);

    if (wrote == 0)
    {
        __try { wrote += OakRecoilApplyLayout(model, 0, keepR, keepHands); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (g_Recoil.noRecoil || g_Recoil.noSway)
    {
        __try
        {
            const uintptr_t offsObj[] = {
                0x10, 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38
            };
            for (int i = 0; i < 10; i++)
            {
                // +0x14 is an int flag (mov [rdi+0x14], eax) — never treat as float.
                float cur = *(volatile float*)(model + offsObj[i]);
                if (cur == cur && fabsf(cur) < 500.f)
                {
                    OakRecoilNotePeak(
                        (offsObj[i] == 0x18 || offsObj[i] == 0x1C) ? &g_RecoilPeakHandsX1000 :
                        (offsObj[i] == 0x20 || offsObj[i] == 0x24) ? &g_RecoilPeakMouseX1000 :
                        &g_RecoilPeakCamX1000,
                        cur);
                }
                const float keep = (offsObj[i] == 0x18 || offsObj[i] == 0x1C) ? keepHands : keepR;
                int w = OakRecoilScaleField(model, offsObj[i], keep, 500.f);
                wrote += w;
                if (w && (offsObj[i] == 0x20 || offsObj[i] == 0x24))
                    InterlockedIncrement(&g_RecoilMouseWrites);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_RecoilLastModel = model;
    g_RecoilAimImpl = model;
    if (wrote > 0)
        InterlockedExchangeAdd(&g_RecoilHookWrites, wrote);
}

static int OakRecoilStolenLen(const BYTE* site)
{
    // Live post-script (no rel CALL inside): mov rcx,[rax]; cmp; lea; setne = 14
    if (site[0] == 0x48 && site[1] == 0x8B && site[2] == 0x08 &&
        site[3] == 0x83 && site[4] == 0x39 && site[5] == 0x00 &&
        site[6] == 0x48 && site[7] == 0x8D && site[11] == 0x0F && site[12] == 0x95)
        return 14;
    // 0x1A9037: mov dword [rsp+0x2C],0 ; mov rcx,rbx ; movaps xmm0,[rsp+0x20] = 16
    if (site[0] == 0xC7 && site[1] == 0x44 && site[2] == 0x24 && site[3] == 0x2C &&
        site[8] == 0x48 && site[9] == 0x8B && site[10] == 0xCB &&
        site[11] == 0x0F && site[12] == 0x28)
        return 16;
    return 0;
}

static int OakRecoilInstallOne(BYTE* site)
{
    if (!site || g_RecoilHookN >= kOakRecoilHookMax)
        return 0;
    for (int i = 0; i < g_RecoilHookN; i++)
        if (g_RecoilHooks[i].site == site)
            return 1;

    const int stolen = OakRecoilStolenLen(site);
    if (stolen < kOakRecoilStolenMin || stolen > kOakRecoilStolenMax)
        return 0;

    // Rel CALL/JCC cannot be relocated into the trampoline.
    for (int i = 0; i < stolen; i++)
    {
        if (site[i] == 0xE8 || site[i] == 0xE9)
            return 0;
        if (site[i] == 0x0F && i + 1 < stolen && (site[i + 1] & 0xF0) == 0x80)
            return 0;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;

    // RAX must survive the callback for the following mov rcx,[rax].
    // Save it to an absolute slot; RDI is nonvolatile and stays the model*.
    BYTE* w = tramp;
    *w++ = 0x48; *w++ = 0xA3; // mov [&g_RecoilSavedRax], rax
    { UINT64 a = (UINT64)&g_RecoilSavedRax; memcpy(w, &a, 8); w += 8; }
    *w++ = 0x51; // push rcx
    *w++ = 0x52; // push rdx
    *w++ = 0x41; *w++ = 0x50; // push r8
    *w++ = 0x41; *w++ = 0x51; // push r9
    *w++ = 0x41; *w++ = 0x52; // push r10
    *w++ = 0x41; *w++ = 0x53; // push r11
    *w++ = 0x53; // push rbx
    *w++ = 0x56; // push rsi
    // 8 pushes => keep 16-byte alignment if entry rsp was aligned
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xEC; *w++ = 0x20; // sub rsp, 0x20
    *w++ = 0x48; *w++ = 0x31; *w++ = 0xC9; // xor rcx, rcx
    *w++ = 0x48; *w++ = 0x31; *w++ = 0xD2; // xor rdx, rdx
    *w++ = 0x49; *w++ = 0x89; *w++ = 0xF8; // mov r8, rdi
    *w++ = 0x49; *w++ = 0x89; *w++ = 0xD9; // mov r9, rbx
    *w++ = 0x48; *w++ = 0xB8; // mov rax, fn
    { UINT64 fn = (UINT64)&OakRecoilAfterAimingModel; memcpy(w, &fn, 8); w += 8; }
    *w++ = 0xFF; *w++ = 0xD0; // call rax
    *w++ = 0x48; *w++ = 0x83; *w++ = 0xC4; *w++ = 0x20; // add rsp, 0x20
    *w++ = 0x5E; // pop rsi
    *w++ = 0x5B; // pop rbx
    *w++ = 0x41; *w++ = 0x5B;
    *w++ = 0x41; *w++ = 0x5A;
    *w++ = 0x41; *w++ = 0x59;
    *w++ = 0x41; *w++ = 0x58;
    *w++ = 0x5A;
    *w++ = 0x59;
    *w++ = 0x48; *w++ = 0xA1; // mov rax, [&g_RecoilSavedRax]
    { UINT64 a = (UINT64)&g_RecoilSavedRax; memcpy(w, &a, 8); w += 8; }

    memcpy(g_RecoilHooks[g_RecoilHookN].orig, site, (size_t)stolen);
    memcpy(w, site, (size_t)stolen);
    w += stolen;
    *w++ = 0xFF; *w++ = 0x25; *w++ = 0; *w++ = 0; *w++ = 0; *w++ = 0;
    { UINT64 back = (UINT64)(site + stolen); memcpy(w, &back, 8); }

    DWORD old = 0;
    if (!VirtualProtect(site, stolen, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    UINT64 dest = (UINT64)tramp;
    memcpy(jmp + 6, &dest, 8);
    memcpy(site, jmp, 14);
    if (stolen > 14)
        memset(site + 14, 0x90, (size_t)(stolen - 14));
    FlushInstructionCache(GetCurrentProcess(), site, stolen);
    VirtualProtect(site, stolen, old, &old);

    g_RecoilHooks[g_RecoilHookN].site = site;
    g_RecoilHooks[g_RecoilHookN].tramp = tramp;
    g_RecoilHooks[g_RecoilHookN].stolen = stolen;
    g_RecoilHookN++;
    return 1;
}

static BYTE* OakRecoilFindPostScriptSite(uintptr_t fnStart, uintptr_t fnEnd)
{
    // After script invoke: mov rcx,[rax]; cmp [rcx],0; lea rcx,[rsp+0x50]; setne bl
    // These 14 bytes are RIP-displacement-free (safe to relocate into trampoline).
    static const BYTE kTail[] = {
        0x48, 0x8B, 0x08, 0x83, 0x39, 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x50, 0x0F, 0x95, 0xC3
    };
    __try
    {
        for (uintptr_t p = fnStart; p + sizeof(kTail) + 8 < fnEnd; p++)
        {
            if (*(const BYTE*)p != 0x48) continue;
            if (memcmp((const void*)p, kTail, sizeof(kTail)) != 0)
                continue;
            for (int back = 1; back <= 8; back++)
            {
                if (*(const BYTE*)(p - back) == 0xE8)
                    return (BYTE*)p;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static void OakRecoilCollectSites(uintptr_t modBase, uintptr_t modEnd, BYTE** out, int* outN, int maxOut)
{
    *outN = 0;
    if (maxOut <= 0 || !modBase)
        return;

    // Live path: AimingModel method-id gate then post-script epilogue.
    // Documented 0x1A9037 matches bytes but is cold on 1.29 RelWithDebInfo (hits stay 0).
    static const BYTE kAimId[] = { 0x83, 0xB9, 0x60, 0x27, 0x00, 0x00, 0xFF };
    uintptr_t scanEnd = modEnd > 0x100 ? modEnd - 0x100 : modBase;
    if (scanEnd > modBase + 0xC00000)
        scanEnd = modBase + 0xC00000;
    __try
    {
        for (uintptr_t p = modBase + 0x1000; p + 0xA0 < scanEnd && *outN < maxOut; p++)
        {
            if (*(const BYTE*)p != 0x83) continue;
            if (memcmp((const void*)p, kAimId, sizeof(kAimId)) != 0)
                continue;
            BYTE* site = OakRecoilFindPostScriptSite(p, p + 0xA0);
            if (!site)
                continue;
            out[(*outN)++] = site;
            {
                char b[120];
                wsprintfA(b, "recoil: AimingModel post-script site RVA 0x%X (steal %d)",
                    (unsigned)((uintptr_t)site - modBase), OakRecoilStolenLen(site));
                Log(b);
            }
            return; // one site only
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Fallback: mid-hook after call [vt+0x2D0] @ 0x1A9031 → site 0x1A9037
    BYTE* known = (BYTE*)(modBase + 0x1A9037ULL);
    __try
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(known, &mbi, sizeof(mbi)) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
            known[0] == 0xC7 && known[1] == 0x44 && known[2] == 0x24 &&
            known[-6] == 0xFF && known[-5] == 0x90)
        {
            out[(*outN)++] = known;
            Log("recoil: fallback documented site RVA 0x1A9037 (steal 16)");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void OakRecoilReleaseHook()
{
    if (!InterlockedCompareExchange(&g_RecoilHookInstalled, 0, 0))
        return;
    for (int i = 0; i < g_RecoilHookN; i++)
    {
        __try
        {
            BYTE* site = g_RecoilHooks[i].site;
            if (!site) continue;
            const int stolen = g_RecoilHooks[i].stolen > 0 ? g_RecoilHooks[i].stolen : 14;
            DWORD old = 0;
            if (VirtualProtect(site, stolen, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(site, g_RecoilHooks[i].orig, stolen);
                FlushInstructionCache(GetCurrentProcess(), site, stolen);
                VirtualProtect(site, stolen, old, &old);
            }
            if (g_RecoilHooks[i].tramp)
                VirtualFree(g_RecoilHooks[i].tramp, 0, MEM_RELEASE);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_RecoilHooks[i] = OakRecoilHookSlot{};
    }
    g_RecoilHookN = 0;
    InterlockedExchange(&g_RecoilHookInstalled, 0);
    Log("recoil: AimingModel native hook removed");
}

static uintptr_t OakRecoilFindAimingImpl(uintptr_t player)
{
    const DWORD now = GetTickCount();
    if (g_RecoilAimImpl && (now - g_RecoilAimScan) < 800u)
    {
        if (OakRecoilLooksLikeVtable(g_RecoilAimImpl))
            return g_RecoilAimImpl;
        g_RecoilAimImpl = 0;
    }
    g_RecoilAimScan = now;
    if (!IsValidPtr(player))
        return 0;

    if (g_RecoilAimOff)
    {
        uintptr_t p = 0;
        __try { p = Read<uintptr_t>(player + g_RecoilAimOff); }
        __except (EXCEPTION_EXECUTE_HANDLER) { p = 0; }
        if (OakRecoilLooksLikeVtable(p) && OakRecoilScoreLayout(p, 0) >= 2)
        {
            g_RecoilAimImpl = p;
            return p;
        }
        g_RecoilAimOff = 0;
    }

    // Prefer RTTI name when MSVC locator exists (often missing on Enforce).
    for (uintptr_t o = 0x40; o <= 0xA00; o += 8)
    {
        uintptr_t p = 0;
        __try { p = Read<uintptr_t>(player + o); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!OakRecoilLooksLikeVtable(p))
            continue;
        char n[80] = {};
        if (!OakEngine_NameObject(p, n, 80) || !n[0])
            continue;
        if (!StrContainsI(n, "Aiming"))
            continue;
        if (StrContainsI(n, "Input") || StrContainsI(n, "Action"))
            continue;
        g_RecoilAimOff = o;
        g_RecoilAimImpl = p;
        char b[140];
        wsprintfA(b, "recoil: aiming impl '%s' @ player+0x%X", n, (unsigned)o);
        Log(b);
        return p;
    }

    // Layout hunt — while ADS accept weaker scores so we pin the model early.
    // Without ADS, still accept score>=2 (hands/cam hot from prior kick) so Present
    // wipe can latch the model before the next shot.
    const bool ads = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    int best = ads ? 0 : 1;
    uintptr_t hit = 0;
    uintptr_t hitOff = 0;
    for (uintptr_t o = 0x40; o <= 0xA00; o += 8)
    {
        uintptr_t p = 0;
        __try { p = Read<uintptr_t>(player + o); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!OakRecoilLooksLikeVtable(p))
            continue;
        const int sc = OakRecoilScoreLayout(p, 0);
        if (sc > best)
        {
            best = sc;
            hit = p;
            hitOff = o;
        }
    }
    if (hit)
    {
        g_RecoilAimOff = hitOff;
        g_RecoilAimImpl = hit;
        char b[120];
        wsprintfA(b, "recoil: aiming layout score=%d @ player+0x%X", best, (unsigned)hitOff);
        Log(b);
        return hit;
    }
    return 0;
}

static void OakRecoilEnsureHook()
{
    if (InterlockedCompareExchange(&g_RecoilHookInstalled, 0, 0))
        return;
    if (!g_GameModule)
        return;

    uintptr_t modBase = (uintptr_t)g_GameModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)modBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(modBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    uintptr_t modEnd = modBase + nt->OptionalHeader.SizeOfImage;

    BYTE* sites[4] = {};
    int n = 0;

    // Prefer the live-validated post-script epilogue (OFFSET_MIGRATION / FULL_DUMP_REPORT).
    BYTE* known = (BYTE*)(modBase + 0x4E9126ull);
    __try
    {
        if (OakRecoilStolenLen(known) == 14)
        {
            sites[n++] = known;
            Log("recoil: using validated AimingModel site RVA 0x4E9126");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!n)
        OakRecoilCollectSites(modBase, modEnd, sites, &n, 4);

    int installed = 0;
    for (int i = 0; i < n; i++)
    {
        if (OakRecoilInstallOne(sites[i]))
            installed++;
    }
    if (installed > 0)
    {
        InterlockedExchange(&g_RecoilHookInstalled, 1);
        char b[96];
        wsprintfA(b, "recoil: mid-hook installed sites=%d (writes only when layout score>=2)", installed);
        Log(b);
    }
    else
    {
        static LONG s_Once;
        if (!InterlockedCompareExchange(&s_Once, 1, 0))
            Log("recoil: mid-hook site missing — Present wipe only");
    }
}

static void Batch4ApplyNoRecoil(uintptr_t localPlayer)
{
    OAK_MARK("recoil");
    if (g_Recoil.noRecoil || g_Recoil.noSway)
        OakRecoilEnsureHook();

    if (!g_Recoil.noRecoil && !g_Recoil.noSway)
    {
        ImGuiMenu_SetRecoilStatus(false, "off");
        return;
    }

    const float keepR = OakRecoilKeepRecoil();
    const float keepHands = g_Recoil.noSway ? OakRecoilKeepSway() : keepR;
    int w = 0;
    const bool ads = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool fire = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const long hits = InterlockedCompareExchange(&g_RecoilHookHits, 0, 0);

    if (IsValidPtr(localPlayer))
    {
        // Present-path wipe: always (re)resolve AimingModel while the feature is on.
        // Waiting for ADS/fire left aim=0 forever when the mid-hook is off.
        uintptr_t aim = g_RecoilLastModel;
        if (!(aim && aim >= 0x10000ull && OakRecoilPageWritable(aim) && OakRecoilScoreLayout(aim, 0) >= 0))
        {
            aim = OakRecoilFindAimingImpl(localPlayer);
            if (aim)
                g_RecoilLastModel = aim;
        }
        if (aim && aim >= 0x10000ull && OakRecoilPageWritable(aim))
        {
            __try { w += OakRecoilApplyLayout(aim, 0, keepR, keepHands); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            // Also wipe the consume/float-block layout some builds copy from.
            __try { w += OakRecoilApplyLayout(aim, 1, keepR, keepHands); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (w > 0)
            InterlockedExchangeAdd(&g_RecoilHookWrites, w);
    }

    const long writes = InterlockedCompareExchange(&g_RecoilHookWrites, 0, 0);
    char note[96];
    wsprintfA(note, "wipe hits=%ld writes=%ld ads=%d keep=%d%%",
        hits, writes, ads ? 1 : 0, (int)(OakRecoilKeepFactor() * 100.f));
    ImGuiMenu_SetRecoilStatus(writes > 0 || g_RecoilAimImpl != 0, note);

    DWORD now = GetTickCount();
    if (!g_RecoilLastLog || (now - g_RecoilLastLog) > 2000u)
    {
        g_RecoilLastLog = now;
        const long peakM = InterlockedExchange(&g_RecoilPeakMouseX1000, 0);
        const long peakH = InterlockedExchange(&g_RecoilPeakHandsX1000, 0);
        const long peakC = InterlockedExchange(&g_RecoilPeakCamX1000, 0);
        const long mouseW = InterlockedExchange(&g_RecoilMouseWrites, 0);
        const long mouseYW = InterlockedExchange(&g_RecoilMouseYWrites, 0);
        const long wTot = InterlockedExchange(&g_RecoilHookWrites, 0);
        const long hTot = InterlockedExchange(&g_RecoilHookHits, 0);
        // Present wipe is the live path — PASS when we found AimingModel and wrote,
        // or when peaks prove the layout was hot under ADS/fire.
        const int pass = (g_RecoilAimImpl && (wTot > 0 || peakH > 0 || peakM > 0 || peakC > 0))
            || (hTot > 0 && (wTot > 0 || peakH > 0 || peakM > 0)) ? 1 : 0;
        char b[240];
        wsprintfA(b, "verify[recoil] wipe=1 hits=%ld writes=%ld mouseW=%ld mouseYW=%ld keep=%d peakM=%ld peakH=%ld peakC=%ld aim=%d PASS=%d",
            hTot, wTot, mouseW, mouseYW, (int)(OakRecoilKeepFactor() * 100.f),
            peakM, peakH, peakC, g_RecoilAimImpl ? 1 : 0, pass);
        Log(b);
    }
}

static void Batch4ZeroRecoilInput(uintptr_t localPlayer)
{
    Batch4ApplyNoRecoil(localPlayer);
}

static void Batch4SilentBoostAmmo(uintptr_t localPlayer, float worldDist)
{
    if (!g_SilentAim.bypass25m)
        return;

    uintptr_t ammoType = 0;
    if (!ResolveHeldAmmoType(localPlayer, ammoType))
        return;
    if (!AmmoTypeLooksValid(ammoType))
        return;
    if (!CaptureAmmoBackup(ammoType))
        return;

    float speed = worldDist * 100.f;
    if (speed < 800.f) speed = 800.f;
    if (speed > 6000.f) speed = 6000.f;

    __try
    {
        if (!OakLab_IsWriteAllowed(OAK_LAB_SILENT) && !OakLab_IsWriteAllowed(OAK_LAB_AMMO))
            return;
        const int labMod = OakLab_IsWriteAllowed(OAK_LAB_SILENT) ? OAK_LAB_SILENT : OAK_LAB_AMMO;
        OakLab_PushModule(labMod);
        OakLab_WriteFloat(labMod, ammoType + 0x38C, speed, 0); // InitSpeed
        OakLab_PopModule();
        g_AmmoBackup.dirty = true;
        static DWORD s_BoostLog = 0;
        DWORD now = GetTickCount();
        if (!s_BoostLog || (now - s_BoostLog) > 1500)
        {
            s_BoostLog = now;
            char b[128];
            wsprintfA(b, "silent: bypass25m InitSpeed=%d dist=%d at=%p",
                (int)speed, (int)worldDist, (void*)ammoType);
            Log(b);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { OakLab_PopModule(); }
}

static bool Batch4FindSilentTarget(uintptr_t worldPtr, uintptr_t localPlayer, CombatTarget& out, bool allowOffscreen)
{
    int savedFov = g_AimbotFov;
    int savedAimDist = g_AimbotMaxDistance;
    int savedBone = g_AimbotBone;
    bool savedP = g_AimbotPlayers;
    bool savedZ = g_AimbotZombies;
    int savedMbDist = g_MagicBulletMaxDistance;

    g_AimbotFov = OakClampInt(g_SilentAim.fovPx, 20, 800);
    g_AimbotMaxDistance = OakClampInt(g_SilentAim.maxDistanceM, 20, OAK_CAP_DIST_M);
    g_AimbotBone = (g_SilentAim.bone == 1) ? 1 : 0;
    g_AimbotPlayers = g_SilentAim.players;
    g_AimbotZombies = g_SilentAim.zombies;
    g_MagicBulletMaxDistance = g_AimbotMaxDistance;

    bool playersOnly = g_SilentAim.players && !g_SilentAim.zombies;
    bool ok = CombatFindBestTarget(worldPtr, localPlayer, out, allowOffscreen, playersOnly);

    g_AimbotFov = savedFov;
    g_AimbotMaxDistance = savedAimDist;
    g_AimbotBone = savedBone;
    g_AimbotPlayers = savedP;
    g_AimbotZombies = savedZ;
    g_MagicBulletMaxDistance = savedMbDist;
    return ok;
}

static void Batch4SilentPresentDraw()
{
    if (!g_SilentAim.enabled || g_PanicHidden || g_MagicBullet)
        return;
    if (!OakLab_IsWriteAllowed(OAK_LAB_SILENT))
        return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;

    if (g_SilentAim.drawFov)
    {
        float r = (float)OakClampInt(g_SilentAim.fovPx, 20, 800);
        bool locked = g_SilentLock.valid;
        ImU32 col = locked ? IM_COL32(80, 255, 140, 200) : IM_COL32(120, 220, 160, 110);
        dl->AddCircle(ImVec2(cx, cy), r, col, 64, 1.6f);
    }

    if (g_SilentAim.drawLock && g_SilentLock.valid)
    {
        Vec2 scr;
        if (WorldToScreen(g_SilentLock.bonePos, scr))
        {
            ImU32 col = IM_COL32(80, 255, 140, 230);
            dl->AddCircle(ImVec2(scr.x, scr.y), 8.f, col, 20, 2.2f);
            dl->AddCircleFilled(ImVec2(scr.x, scr.y), 2.4f, col, 8);
            dl->AddLine(ImVec2(cx, cy), ImVec2(scr.x, scr.y), IM_COL32(80, 255, 140, 160), 1.3f);
        }
    }
}

static void Batch4UpdateSilentAim(uintptr_t worldPtr, uintptr_t localPlayer)
{
    // Memory writes (bullet VS) — needs PLAY mode or Silent armed in Lab.
    if (!g_SilentAim.enabled || g_MagicBullet)
    {
        g_SilentLock = {};
        return;
    }
    if (!OakLab_IsWriteAllowed(OAK_LAB_SILENT))
    {
        g_SilentLock = {};
        return;
    }
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer))
        return;

    int silentKey = Batch4EffectiveSilentKey();
    // key/bind 0 = always active while module enabled
    bool silentHeld = (silentKey <= 0) ? true : ((GetAsyncKeyState(silentKey) & 0x8000) != 0);
    if (!silentHeld)
    {
        g_SilentLock = {};
        return;
    }

    bool reloading = false;
    if (!Batch4CombatGatesOk(localPlayer, &reloading))
    {
        // Keep last lock briefly so FOV draw stays useful; don't guide while gated.
        return;
    }

    float fovPx = (float)OakClampInt(g_SilentAim.fovPx, 20, 800);
    bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    // Triggerbot hold also counts as firing for silent guides.
    if (!firing && g_Batch4TriggerHeld)
        firing = true;

    int savedBone = g_AimbotBone;
    g_AimbotBone = (g_SilentAim.bone == 1) ? 1 : 0;

    CombatTarget saTarget = {};
    auto polishBone = [&](CombatTarget& t) {
        if (!t.valid) return;
        Vec3 bone;
        if (CombatGetMagicBone(t.entity, bone, t.isPlayer))
        {
            t.bonePos = CombatLeadMagicBone(t.entity, bone, t.worldDist);
            Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
            t.worldDist = Distance3D(localPos, t.bonePos);
        }
    };

    bool playersOnly = g_SilentAim.players && !g_SilentAim.zombies;

    if (firing && CombatTargetStillValid(localPlayer, g_SilentLock, fovPx, false, playersOnly))
    {
        saTarget = g_SilentLock;
        polishBone(saTarget);
        g_SilentLock = saTarget;
    }
    else if (Batch4FindSilentTarget(worldPtr, localPlayer, saTarget, firing /*offscreen OK while shooting*/))
    {
        polishBone(saTarget);
        g_SilentLock = saTarget;
    }
    else if (firing && g_SilentLock.valid)
    {
        // Sticky while spraying — re-polish bone even if briefly off FOV.
        saTarget = g_SilentLock;
        polishBone(saTarget);
        if (!CombatTargetStillValid(localPlayer, saTarget, fovPx * 2.5f, false, playersOnly))
        {
            g_SilentLock = {};
            saTarget = {};
        }
        else
            g_SilentLock = saTarget;
    }
    else
    {
        g_SilentLock = {};
        g_AimbotBone = savedBone;
        return;
    }

    g_AimbotBone = savedBone;

    if (!saTarget.valid)
        return;

    // Only guide bullets while actually shooting.
    if (!firing)
        return;

    // Silent path: VS bone seat + optional InitSpeed bypass25m (dist*100).
    if (g_SilentAim.bypass25m)
        Batch4SilentBoostAmmo(localPlayer, saTarget.worldDist);
    CombatSnapLocalBulletsToBone(worldPtr, localPlayer, saTarget.bonePos);
}

// True if crosshair sits on the entity's body (screen AABB from feet→head), not just
// "nearest bone in a wide FOV" — that caused random shots at off-center targets.
static bool Batch4CrosshairOnEntityBody(uintptr_t entity, bool isPlayer, int padPx, float* outScreenDist)
{
    if (outScreenDist) *outScreenDist = 1e9f;
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    Vec3 origin = {};
    if (!GetEntityPosition(entity, origin))
        return false;

    Vec3 head = origin;
    head.y += isPlayer ? 1.70f : 1.60f;
    Vec3 chest = origin;
    chest.y += isPlayer ? 1.15f : 1.05f;

    Vec2 pts[4];
    int n = 0;
    Vec2 s{};
    if (WorldToScreenLoose(origin, s)) pts[n++] = s;
    if (WorldToScreenLoose(head, s)) pts[n++] = s;
    if (WorldToScreenLoose(chest, s)) pts[n++] = s;
    if (n < 1)
        return false;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    float pad = (float)(padPx > 0 ? padPx : 24);

    float best = 1e12f;
    float minX = pts[0].x, maxX = pts[0].x, minY = pts[0].y, maxY = pts[0].y;
    for (int i = 0; i < n; i++)
    {
        float dx = pts[i].x - cx;
        float dy = pts[i].y - cy;
        float d2 = dx * dx + dy * dy;
        if (d2 < best) best = d2;
        if (pts[i].x < minX) minX = pts[i].x;
        if (pts[i].x > maxX) maxX = pts[i].x;
        if (pts[i].y < minY) minY = pts[i].y;
        if (pts[i].y > maxY) maxY = pts[i].y;
    }
    float sd = sqrtf(best);
    if (outScreenDist) *outScreenDist = sd;

    // Circle deadzone around any projected body point.
    if (sd <= pad)
        return true;

    float extra = pad * 0.45f;
    minX -= extra; maxX += extra;
    minY -= extra * 0.25f; maxY += extra * 0.15f;
    if (maxX - minX < 18.f)
    {
        float m = (minX + maxX) * 0.5f;
        minX = m - 9.f; maxX = m + 9.f;
    }
    if (maxY - minY < 18.f)
    {
        float m = (minY + maxY) * 0.5f;
        minY = m - 9.f; maxY = m + 9.f;
    }
    return cx >= minX && cx <= maxX && cy >= minY && cy <= maxY;
}

static bool Batch4FindTriggerTarget(uintptr_t worldPtr, uintptr_t localPlayer, CombatTarget& out, int padPx)
{
    out = {};
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer))
        return false;
    if (!g_LocalPlayerValid && !g_CameraValid)
        return false;

    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    float maxDist = (float)OakClampInt(g_Triggerbot.maxDistanceM, 5, OAK_CAP_DIST_M);

    CombatTarget best = {};
    float bestScore = 1e12f;

    auto consider = [&](uintptr_t entity) {
        if (!IsValidPtr(entity) || entity < 0x100000000 || entity == localPlayer)
            return;
        if (EntityIsDead(entity))
            return;
        if (CombatIsFriendEntity(entity))
            return;

        bool isP = false, isZ = false;
        if (!CombatEntityIsPlayerOrZombie(entity, isP, isZ))
            return;
        if (!((isP && g_Triggerbot.players) || (isZ && g_Triggerbot.zombies)))
            return;

        Vec3 origin = {};
        if (!GetEntityPosition(entity, origin))
            return;
        float wdist = Distance3D(localPos, origin);
        // Allow point-blank (old 0.35m floor made close combat miss constantly).
        if (wdist > maxDist || wdist < 0.05f)
            return;

        float sd = 1e9f;
        bool on = Batch4CrosshairOnEntityBody(entity, isP, padPx, &sd);
        if (!on && wdist < 2.8f && g_W2S.valid)
        {
            Vec3 d = { origin.x - g_W2S.translation.x, origin.y - g_W2S.translation.y,
                       origin.z - g_W2S.translation.z };
            float vz = d.x * g_W2S.forward.x + d.y * g_W2S.forward.y + d.z * g_W2S.forward.z;
            float xz = sqrtf(d.x * d.x + d.z * d.z);
            if (vz > 0.20f && xz < 1.35f)
            {
                on = true;
                sd = 0.f;
            }
        }
        if (!on)
            return;

        // Prefer on-crosshair targets; break ties with closer world distance.
        float score = sd * 10.f + wdist;
        if (score >= bestScore)
            return;

        Vec3 bone = {};
        if (!CombatGetAimBone(entity, isP, bone))
            bone = origin;

        bestScore = score;
        best = {};
        best.entity = entity;
        best.bonePos = bone;
        best.worldDist = wdist;
        best.screenDist = sd;
        best.isPlayer = isP;
        best.onScreen = true;
        best.valid = true;
    };

    // Near list first — close combat lives here.
    const uintptr_t listOffs[2] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = 0;
        int count = 0;
        if (!ResolveEntityList(worldPtr, listOffs[li], 2000, data, count, nullptr))
        {
            data = Read<uintptr_t>(worldPtr + listOffs[li]);
            count = Read<int>(worldPtr + listOffs[li] + 8);
            if (li == 1)
            {
                int farC = Read<int>(worldPtr + oak_offsets::world::FarTableSize);
                if (farC > 0 && farC < 2000) count = farC;
            }
            if (!IsValidPtr(data) || data < 0x100000000 || count <= 0 || count > 2000)
                continue;
        }
        int maxI = count > 300 ? 300 : count;
        for (int i = 0; i < maxI; i++)
            consider(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }

    if (g_Triggerbot.zombies)
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 2000)
        {
            int maxI = count > 300 ? 300 : count;
            for (int i = 0; i < maxI; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                consider(Read<uintptr_t>(entry + 0x8));
            }
        }
    }

    if (!best.valid)
        return false;
    out = best;
    return true;
}

static bool Batch4TriggerLosClear(uintptr_t worldPtr, uintptr_t localPlayer, uintptr_t targetEnt, const Vec3& aimPos)
{
    // Shared with ESP visibility colors (oak_batch4_esp.inl).
    return OakBatch4LosClear(worldPtr, localPlayer, targetEnt, aimPos);
}

static void Batch4UpdateTriggerbot(uintptr_t worldPtr, uintptr_t localPlayer)
{
    // Input-only — do NOT require OakLab_IsWriteAllowed (SAFE/one-arm would soft-kill it).
    if (!g_Triggerbot.enabled)
        return;
    if (OakLab_GetState(OAK_LAB_TRIGGERBOT) == OAK_LAB_LOCKED)
        return;
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer))
        return;

    DWORD now = GetTickCount();
    auto skipLog = [&](const char* why) {
        static DWORD s_Skip = 0;
        static char s_Last[48] = {};
        if (lstrcmpA(s_Last, why) != 0 || !s_Skip || (now - s_Skip) > 1500)
        {
            s_Skip = now;
            lstrcpynA(s_Last, why, 48);
            char b[96];
            wsprintfA(b, "triggerbot: skip=%s", why);
            Log(b);
        }
    };

    if (g_Batch4TriggerPendingUp)
    {
        Batch4SendLmb(false);
        g_Batch4TriggerPendingUp = false;
        g_Batch4TriggerHeld = false;
        g_Batch4TriggerIgnoreUserLmbUntil = now + 200;
        return;
    }

    if (ImGuiMenu_IsOpen() || g_PanicHidden || g_MiscFreecam)
    {
        if (g_Batch4TriggerHeld)
            Batch4ReleaseSyntheticLmb();
        skipLog("menu");
        return;
    }

    const bool userLmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (userLmb && !g_Batch4TriggerHeld && now >= g_Batch4TriggerIgnoreUserLmbUntil)
        return;

    if (g_Triggerbot.requireAds && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
    {
        if (g_Batch4TriggerHeld)
            Batch4ReleaseSyntheticLmb();
        g_Batch4TriggerConfirm = 0;
        skipLog("ads");
        return;
    }

    // Do NOT use OakProbeReloadState / ammo==0 — those offsets false-positive
    // and permanently gate the bot. Weapon presence is optional.

    const int padPx = OakClampInt(g_Triggerbot.deadzonePx, 8, 160);
    CombatTarget tbTarget = {};
    const bool onBody = Batch4FindTriggerTarget(worldPtr, localPlayer, tbTarget, padPx);

    if (!onBody || !tbTarget.valid)
    {
        if (g_Batch4TriggerHeld)
            Batch4ReleaseSyntheticLmb();
        g_Batch4TriggerConfirm = 0;
        g_Batch4TriggerTarget = 0;
        skipLog("nobody");
        return;
    }

    if (g_Triggerbot.requireLos &&
        !Batch4TriggerLosClear(worldPtr, localPlayer, tbTarget.entity, tbTarget.bonePos))
    {
        if (g_Batch4TriggerHeld)
            Batch4ReleaseSyntheticLmb();
        g_Batch4TriggerConfirm = 0;
        skipLog("los");
        return;
    }

    if (tbTarget.entity != g_Batch4TriggerTarget)
    {
        g_Batch4TriggerTarget = tbTarget.entity;
        g_Batch4TriggerConfirm = 0;
        g_Batch4TriggerArmTick = now;
        if (g_Batch4TriggerHeld)
        {
            Batch4SendLmb(false);
            g_Batch4TriggerHeld = false;
            g_Batch4TriggerIgnoreUserLmbUntil = now + 200;
        }
    }
    g_Batch4TriggerConfirm++;

    int delay = OakClampInt(g_Triggerbot.delayMs, 0, 500);
    if (delay > 0 && !g_Batch4TriggerHeld && (now - g_Batch4TriggerArmTick) < (DWORD)delay)
        return;

    // Pulse click (down this frame, up next) — DayZ edge-detects fire more
    // reliably than a sticky synthetic hold.
    if ((now - g_Batch4TriggerLastShot) >= 45 || !g_Batch4TriggerHeld)
    {
        Batch4SendLmb(true);
        g_Batch4TriggerHeld = true;
        g_Batch4TriggerPendingUp = true;
        g_Batch4TriggerLastShot = now;
        static DWORD s_FireLog = 0;
        if (!s_FireLog || (now - s_FireLog) > 1000)
        {
            s_FireLog = now;
            char b[160];
            wsprintfA(b, "triggerbot: FIRE p=%d dist=%d sd=%d",
                tbTarget.isPlayer ? 1 : 0, (int)tbTarget.worldDist, (int)tbTarget.screenDist);
            Log(b);
        }
    }
}

static void Batch4UpdateAutoFire(uintptr_t localPlayer)
{
    if (!g_AimbotB4.autoFireWhenLocked || !g_AimbotEnabled)
    {
        if (g_Batch4AutoFireHeld)
            Batch4ReleaseSyntheticLmb();
        return;
    }

    int aimKey = Batch4EffectiveAimKey();
    bool aimHeld = aimKey > 0 && (GetAsyncKeyState(aimKey) & 0x8000) != 0;
    bool locked = g_CombatLock.valid && aimHeld;

    if (!locked || !Batch4CombatGatesOk(localPlayer, nullptr))
    {
        if (g_Batch4AutoFireHeld)
            Batch4ReleaseSyntheticLmb();
        return;
    }

    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    {
        if (g_Batch4AutoFireHeld)
            Batch4ReleaseSyntheticLmb();
        return;
    }

    if (!g_Batch4AutoFireHeld)
    {
        Batch4SendLmb(true);
        g_Batch4AutoFireHeld = true;
    }
}

void OakBatch4InitCombatFrame()
{
    if (ImGuiMenu_IsOpen() || g_PanicHidden || g_MiscFreecam)
        Batch4ReleaseSyntheticLmb();
}

void OakBatch4UpdateCombat(uintptr_t worldPtr, uintptr_t localPlayer)
{
    __try
    {
        Batch4ZeroRecoilInput(localPlayer);
        Batch4UpdateSilentAim(worldPtr, localPlayer);
        Batch4UpdateAutoFire(localPlayer);
        Batch4UpdateTriggerbot(worldPtr, localPlayer);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
