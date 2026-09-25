// Oak Frame Boost — Present unlock + tearing + QPC FPS cap + OS/GPU high-perf.
// Included from main.cpp. Does NOT busy-burn CPU to fake utilization.
#pragma once

#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <timeapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "winmm.lib")

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200u
#endif
#ifndef DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 0x800u
#endif

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

struct OakPerfBoostRt
{
    bool fpsCapEnabled = false;
    int fpsCap = 240;
    bool unlockPresent = false;
    bool allowTearing = true;
    bool highPerfMode = false;
    bool patchDayzCfg = false;
    int maxFrameLatency = 0; // 0 = auto (2 when unlock)

    float measuredFps = 0.f;
    float paceSleepMs = 0.f;
    UINT lastSyncIn = 0;
    UINT lastSyncOut = 0;
    int tearingTried = 0;
    int tearingOk = 0;
    int swapEffect = -1;      // DXGI_SWAP_EFFECT
    int swapFlags = 0;
    int windowed = -1;        // 1 windowed/borderless, 0 exclusive
    int refreshHz = 0;
    int tearArmed = 0;        // swap chain has ALLOW_TEARING
    int gpuPrio = 0;
    char status[140] = "idle";
};

static OakPerfBoostRt g_PerfBoost = {};

static bool g_PerfTimerPeriodOn = false;
static bool g_PerfPriorityRaised = false;
static bool g_PerfThrottleCleared = false;
static DWORD g_PerfSavedPriority = NORMAL_PRIORITY_CLASS;
static bool g_PerfCfgPatched = false;
static int g_PerfAppliedLatency = -1;
static int g_PerfTearingCached = -1; // -1 unknown, 0 no, 1 yes
static int g_PerfTearResizeTried = 0;
static int g_PerfGpuPrioApplied = 0;
static HANDLE g_PerfMmcss = nullptr;
static DWORD g_PerfMmTaskIndex = 0;

// Force flip+tearing on any NEW swap chains the game creates after we hook the factory.
// DayZ typically uses Factory2::CreateSwapChainForHwnd — hook BOTH entry points.
struct OakPerfInlineHook
{
    BYTE* target = nullptr;
    BYTE orig[14] = {};
    BYTE hook[14] = {};
    bool hooked = false;
};

static OakPerfInlineHook g_PerfHookCreateSc = {};
static OakPerfInlineHook g_PerfHookCreateScHwnd = {};
static bool g_PerfFactoryHooksDone = false;
static int g_PerfBreakTried = 0;
static bool g_PerfForcedExclusive = false;
static bool g_PerfBreakRefresh = true;
static int g_PerfNvidiaPpeCached = -1; // -1 unknown, 0 absent, 1 nvppex/overlay PPE present
static float g_PerfPresentMs = 0.f;
static float g_PerfOverlayMs = 0.f;

static bool OakPerfBoost_IsUncappedLight()
{
    return g_PerfBoost.unlockPresent && !g_PerfBoost.fpsCapEnabled;
}

static float g_PerfOverlayMsMax = 0.f;

static void OakPerfBoost_SetOverlayMs(float ms)
{
    g_PerfOverlayMs = ms;
    if (ms > g_PerfOverlayMsMax)
        g_PerfOverlayMsMax = ms;
}

static HRESULT(STDMETHODCALLTYPE* g_PerfOrigCreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**) = nullptr;
static HRESULT(STDMETHODCALLTYPE* g_PerfOrigCreateSwapChainForHwnd)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**) = nullptr;

static void OakPerfBoost_PatchDescForFlip(DXGI_SWAP_CHAIN_DESC* sd)
{
    if (!sd) return;
    if (sd->SampleDesc.Count > 1)
    {
        sd->SampleDesc.Count = 1;
        sd->SampleDesc.Quality = 0;
    }
    if (sd->BufferCount < 2)
        sd->BufferCount = 2;
    sd->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
}

static void OakPerfBoost_PatchDesc1ForFlip(DXGI_SWAP_CHAIN_DESC1* sd)
{
    if (!sd) return;
    if (sd->SampleDesc.Count > 1)
    {
        sd->SampleDesc.Count = 1;
        sd->SampleDesc.Quality = 0;
    }
    if (sd->BufferCount < 2)
        sd->BufferCount = 2;
    sd->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
}

static HRESULT STDMETHODCALLTYPE OakPerfBoost_hkCreateSwapChain(
    IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** out)
{
    if (sd && g_PerfBoost.unlockPresent)
    {
        OakPerfBoost_PatchDescForFlip(sd);
        Log("perf: CreateSwapChain -> FLIP_DISCARD+TEARING");
    }
    if (!g_PerfHookCreateSc.hooked || !g_PerfHookCreateSc.target || !g_PerfOrigCreateSwapChain)
        return E_FAIL;
    PresentPatchLock();
    memcpy(g_PerfHookCreateSc.target, g_PerfHookCreateSc.orig, 14);
    HRESULT hr = g_PerfOrigCreateSwapChain(self, device, sd, out);
    memcpy(g_PerfHookCreateSc.target, g_PerfHookCreateSc.hook, 14);
    PresentPatchUnlock();
    return hr;
}

static HRESULT STDMETHODCALLTYPE OakPerfBoost_hkCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* restrictOut, IDXGISwapChain1** out)
{
    DXGI_SWAP_CHAIN_DESC1 local = {};
    const DXGI_SWAP_CHAIN_DESC1* use = desc;
    if (desc && g_PerfBoost.unlockPresent)
    {
        local = *desc;
        OakPerfBoost_PatchDesc1ForFlip(&local);
        use = &local;
        Log("perf: CreateSwapChainForHwnd -> FLIP_DISCARD+TEARING");
    }
    if (!g_PerfHookCreateScHwnd.hooked || !g_PerfHookCreateScHwnd.target || !g_PerfOrigCreateSwapChainForHwnd)
        return E_FAIL;
    PresentPatchLock();
    memcpy(g_PerfHookCreateScHwnd.target, g_PerfHookCreateScHwnd.orig, 14);
    HRESULT hr = g_PerfOrigCreateSwapChainForHwnd(self, device, hwnd, use, fsDesc, restrictOut, out);
    memcpy(g_PerfHookCreateScHwnd.target, g_PerfHookCreateScHwnd.hook, 14);
    PresentPatchUnlock();
    return hr;
}

static void OakPerfBoost_InstallOne(BYTE* target, void* detour, OakPerfInlineHook& slot, const char* tag)
{
    if (!target || !detour || slot.hooked) return;
    InstallInlineHook(target, detour, slot.orig, slot.hook, &slot.hooked, tag);
    if (slot.hooked)
        slot.target = target;
}

static void OakPerfBoost_HookFactoryCreate(IDXGISwapChain* sc)
{
    if (g_PerfFactoryHooksDone || !sc)
        return;
    g_PerfFactoryHooksDone = true;

    ID3D11Device* d3d = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d3d)) || !d3d)
        return;
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev)
    {
        d3d->Release();
        return;
    }
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter)
    {
        dxgiDev->Release();
        d3d->Release();
        return;
    }
    IDXGIFactory* factory = nullptr;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory)) || !factory)
    {
        adapter->Release();
        dxgiDev->Release();
        d3d->Release();
        return;
    }

    void** vt = *(void***)factory;
    // IDXGIFactory::CreateSwapChain = vtable[10]
    g_PerfOrigCreateSwapChain =
        (HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**))vt[10];
    OakPerfBoost_InstallOne((BYTE*)vt[10], (void*)&OakPerfBoost_hkCreateSwapChain,
        g_PerfHookCreateSc, "dxgi.CreateSwapChain");

    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2)
    {
        void** vt2 = *(void***)f2;
        // IDXGIFactory2::CreateSwapChainForHwnd = vtable[15]
        g_PerfOrigCreateSwapChainForHwnd =
            (HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**))vt2[15];
        OakPerfBoost_InstallOne((BYTE*)vt2[15], (void*)&OakPerfBoost_hkCreateSwapChainForHwnd,
            g_PerfHookCreateScHwnd, "dxgi.CreateSwapChainForHwnd");
        f2->Release();
    }

    if (g_PerfHookCreateSc.hooked || g_PerfHookCreateScHwnd.hooked)
        Log("perf: factory CreateSwapChain hooks installed (flip force on recreate)");

    factory->Release();
    adapter->Release();
    dxgiDev->Release();
    d3d->Release();
}

static LARGE_INTEGER g_PerfQpcFreq = {};
static LARGE_INTEGER g_PerfNextFrame = {};
static LARGE_INTEGER g_PerfLastPresent = {};

static void OakInvalidateOverlayRtv(); // main.cpp

static void OakPerfBoost_SetStatus(const char* s)
{
    if (!s) return;
    lstrcpynA(g_PerfBoost.status, s, (int)sizeof(g_PerfBoost.status));
    ImGuiMenu_SetPerfBoostStatus(s);
}

static void OakPerfBoost_HeartbeatLog()
{
    static DWORD s_Last = 0;
    DWORD now = GetTickCount();
    if (s_Last && (now - s_Last) < 2000)
        return;
    s_Last = now;
    if (!g_PerfBoost.fpsCapEnabled && !g_PerfBoost.unlockPresent && !g_PerfBoost.highPerfMode)
        return;
    char b[180];
    wsprintfA(b, "perf: fps=%d ovMs=%d ovMax=%d presentMs=%d sleepMs=%d sync %u->%u cap=%d@%d unlock=%d tear=%d/%d win=%d hi=%d",
        (int)(g_PerfBoost.measuredFps + 0.5f),
        (int)(g_PerfOverlayMs + 0.5f),
        (int)(g_PerfOverlayMsMax + 0.5f),
        (int)(g_PerfPresentMs + 0.5f),
        (int)(g_PerfBoost.paceSleepMs + 0.5f),
        (unsigned)g_PerfBoost.lastSyncIn,
        (unsigned)g_PerfBoost.lastSyncOut,
        g_PerfBoost.fpsCapEnabled ? 1 : 0,
        g_PerfBoost.fpsCap,
        g_PerfBoost.unlockPresent ? 1 : 0,
        g_PerfBoost.tearingOk,
        g_PerfBoost.tearingTried,
        g_PerfBoost.windowed,
        g_PerfPriorityRaised ? 1 : 0);
    Log(b);
    g_PerfOverlayMsMax = 0.f;
}

static void OakPerfBoost_SyncFromLimits(const OakPerfLimits& p)
{
    const bool on = p.frameBoost;
    const bool wantCap = on && p.fpsCapEnabled;
    const bool wantUnlock = on && p.unlockPresent;
    const bool wantHi = on && p.highPerfMode;
    const bool wantTear = on && p.unlockPresent && p.allowTearing;
    const bool wantBreak = on && p.unlockPresent && p.breakRefreshLock;
    const int wantCapFps = p.fpsCap;
    // Latency: exclusive FS benefits from 2; keep auto-2 on unlock.
    const int wantLat = on ? (p.maxFrameLatency > 0 ? p.maxFrameLatency : (wantUnlock ? 2 : 0)) : 0;
    const bool wantPatch = on && p.patchDayzCfg;

    const bool unlockChanged =
        g_PerfBoost.unlockPresent != wantUnlock ||
        g_PerfBoost.allowTearing != wantTear;
    const bool breakChanged = g_PerfBreakRefresh != wantBreak;

    const bool changed =
        g_PerfBoost.fpsCapEnabled != wantCap ||
        g_PerfBoost.fpsCap != wantCapFps ||
        unlockChanged ||
        g_PerfBoost.highPerfMode != wantHi ||
        breakChanged ||
        g_PerfBoost.maxFrameLatency != wantLat;

    g_PerfBoost.fpsCapEnabled = wantCap;
    g_PerfBoost.fpsCap = wantCapFps;
    g_PerfBoost.unlockPresent = wantUnlock;
    g_PerfBoost.allowTearing = wantTear;
    g_PerfBoost.highPerfMode = wantHi;
    g_PerfBoost.patchDayzCfg = wantPatch;
    g_PerfBoost.maxFrameLatency = wantLat;
    g_PerfBreakRefresh = wantBreak;
    // Do NOT clear g_PerfCfgPatched here — uncap auto-patches cfg once; resetting
    // every frame caused GetFileAttributes/Log spam on Present (FPS killer).
    if (changed)
    {
        g_PerfNextFrame.QuadPart = 0;
        if (wantLat != g_PerfAppliedLatency)
            g_PerfAppliedLatency = -1;
    }
    // Only re-arm tear/break attempts when unlock/break toggles — not on FPS slider spam.
    if (unlockChanged && wantUnlock)
        g_PerfTearResizeTried = 0;
    if (breakChanged)
        g_PerfBreakTried = 0;

    if (changed)
    {
        static DWORD s_LastLog = 0;
        DWORD now = GetTickCount();
        if (!s_LastLog || (now - s_LastLog) > 400)
        {
            s_LastLog = now;
            char b[160];
            wsprintfA(b, "perf: boost on=%d cap=%d@%d unlock=%d tear=%d break=%d hi=%d lat=%d",
                on ? 1 : 0, wantCap ? 1 : 0, wantCapFps, wantUnlock ? 1 : 0,
                wantTear ? 1 : 0, wantBreak ? 1 : 0, wantHi ? 1 : 0, wantLat);
            Log(b);
        }
    }
}

static void OakPerfBoost_EnsureQpc()
{
    if (!g_PerfQpcFreq.QuadPart)
        QueryPerformanceFrequency(&g_PerfQpcFreq);
}

static void OakPerfBoost_ApplyTimerPeriod(bool on)
{
    if (on && !g_PerfTimerPeriodOn)
    {
        if (timeBeginPeriod(1) == TIMERR_NOERROR)
            g_PerfTimerPeriodOn = true;
    }
    else if (!on && g_PerfTimerPeriodOn)
    {
        timeEndPeriod(1);
        g_PerfTimerPeriodOn = false;
    }
}

static void OakPerfBoost_ApplyPriority(bool on)
{
    HANDLE proc = GetCurrentProcess();
    if (on && !g_PerfPriorityRaised)
    {
        g_PerfSavedPriority = GetPriorityClass(proc);
        if (!g_PerfSavedPriority)
            g_PerfSavedPriority = NORMAL_PRIORITY_CLASS;
        // HIGH beats ABOVE_NORMAL for contested cores; REALTIME is too dangerous.
        if (SetPriorityClass(proc, HIGH_PRIORITY_CLASS))
            g_PerfPriorityRaised = true;
        else if (SetPriorityClass(proc, ABOVE_NORMAL_PRIORITY_CLASS))
            g_PerfPriorityRaised = true;
    }
    else if (!on && g_PerfPriorityRaised)
    {
        SetPriorityClass(proc, g_PerfSavedPriority ? g_PerfSavedPriority : NORMAL_PRIORITY_CLASS);
        g_PerfPriorityRaised = false;
    }

    // Present thread: lift while boosting (MMCSS below does more when available).
    if (on)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

// DayZ is heavily main-thread bound — overall CPU% looks low while 1–2 cores are hot.
// Sweep threads and disable EcoQoS so Windows doesn't park the render/sim threads.
static void OakPerfBoost_BoostAllThreads(bool on)
{
    static bool s_Done = false;
    if (!on)
    {
        s_Done = false;
        return;
    }
    if (s_Done)
        return;
    s_Done = true;

    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    typedef BOOL(WINAPI* tSetThreadInfo)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    static tSetThreadInfo s_setTi = nullptr;
    static int s_resolv = 0;
    if (!s_resolv)
    {
        s_resolv = 1;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            s_setTi = (tSetThreadInfo)GetProcAddress(k, "SetThreadInformation");
    }

    struct LocalThreadPts
    {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    int n = 0;
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
                continue;
            HANDLE th = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            // Don't demote time-critical system threads; lift normals.
            int cur = GetThreadPriority(th);
            if (cur >= THREAD_PRIORITY_NORMAL && cur < THREAD_PRIORITY_HIGHEST)
                SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL);

            if (s_setTi)
            {
                LocalThreadPts pts = {};
                pts.Version = 1; // THREAD_POWER_THROTTLING_CURRENT_VERSION
                pts.ControlMask = 0x1; // THREAD_POWER_THROTTLING_EXECUTION_SPEED
                pts.StateMask = 0; // disable throttling
                // ThreadPowerThrottling == 9
                s_setTi(th, (THREAD_INFORMATION_CLASS)9, &pts, sizeof(pts));
            }
            CloseHandle(th);
            ++n;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    char b[80];
    wsprintfA(b, "perf: boosted %d threads (EcoQoS off)", n);
    Log(b);
}

static HANDLE g_PerfPowerRequest = nullptr;
static void OakPerfBoost_ApplyPowerRequests(bool on)
{
    if (on && !g_PerfPowerRequest)
    {
        REASON_CONTEXT ctx = {};
        ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
        ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        ctx.Reason.SimpleReasonString = L"Oak Frame Boost";
        HANDLE req = PowerCreateRequest(&ctx);
        if (req && req != INVALID_HANDLE_VALUE)
        {
            // Keep system/execution from parking cores while boosting.
            PowerSetRequest(req, PowerRequestExecutionRequired);
            PowerSetRequest(req, PowerRequestDisplayRequired);
            g_PerfPowerRequest = req;
            Log("perf: PowerRequest Execution+Display");
        }
    }
    else if (!on && g_PerfPowerRequest)
    {
        PowerClearRequest(g_PerfPowerRequest, PowerRequestExecutionRequired);
        PowerClearRequest(g_PerfPowerRequest, PowerRequestDisplayRequired);
        CloseHandle(g_PerfPowerRequest);
        g_PerfPowerRequest = nullptr;
    }
}

static void OakPerfBoost_ApplyMmCss(bool on)
{
    typedef HANDLE(WINAPI* tAvSet)(const char*, DWORD*);
    typedef BOOL(WINAPI* tAvRevert)(HANDLE);
    static tAvSet s_set = nullptr;
    static tAvRevert s_rev = nullptr;
    static int s_resolv = 0;
    if (!s_resolv)
    {
        s_resolv = 1;
        HMODULE av = LoadLibraryA("avrt.dll");
        if (av)
        {
            s_set = (tAvSet)GetProcAddress(av, "AvSetMmThreadCharacteristicsA");
            s_rev = (tAvRevert)GetProcAddress(av, "AvRevertMmThreadCharacteristics");
        }
    }
    if (on && !g_PerfMmcss && s_set)
    {
        DWORD idx = 0;
        HANDLE h = s_set("Games", &idx);
        if (h)
        {
            g_PerfMmcss = h;
            g_PerfMmTaskIndex = idx;
        }
    }
    else if (!on && g_PerfMmcss && s_rev)
    {
        s_rev(g_PerfMmcss);
        g_PerfMmcss = nullptr;
        g_PerfMmTaskIndex = 0;
    }
}

static void OakPerfBoost_ApplyPowerThrottle(bool disableThrottle)
{
    typedef BOOL(WINAPI* tSetProcInfo)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    static tSetProcInfo s_fn = nullptr;
    static int s_resolv = 0;
    if (!s_resolv)
    {
        s_resolv = 1;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            s_fn = (tSetProcInfo)GetProcAddress(k, "SetProcessInformation");
    }
    if (!s_fn)
        return;

    struct LocalPts
    {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    LocalPts pts = {};
    pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    pts.StateMask = disableThrottle ? 0
        : (PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);

    const PROCESS_INFORMATION_CLASS kPower = (PROCESS_INFORMATION_CLASS)4;

    if (disableThrottle && !g_PerfThrottleCleared)
    {
        if (s_fn(GetCurrentProcess(), kPower, &pts, sizeof(pts)))
            g_PerfThrottleCleared = true;
    }
    else if (!disableThrottle && g_PerfThrottleCleared)
    {
        s_fn(GetCurrentProcess(), kPower, &pts, sizeof(pts));
        g_PerfThrottleCleared = false;
    }
}

static bool OakPerfBoost_ResolveDayzCfg(char* out, int outN)
{
    if (!out || outN < 64) return false;
    out[0] = 0;
    char up[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("USERPROFILE", up, MAX_PATH))
        return false;

    const char* tails[] = {
        "\\OneDrive\\Documents\\DayZ\\DayZ.cfg",
        "\\Documents\\DayZ\\DayZ.cfg",
        "\\OneDrive\\Documentos\\DayZ\\DayZ.cfg",
    };
    for (int i = 0; i < 3; ++i)
    {
        wsprintfA(out, "%s%s", up, tails[i]);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    // Default create path (OneDrive Documents is where this machine keeps it).
    wsprintfA(out, "%s\\OneDrive\\Documents\\DayZ\\DayZ.cfg", up);
    return false;
}

static void OakPerfBoost_TryPatchDayzCfg()
{
    // Auto-patch on uncap: DayZ uses cfg `refresh=` as an FPS ceiling for many players.
    if ((!g_PerfBoost.patchDayzCfg && !g_PerfBoost.unlockPresent) || g_PerfCfgPatched)
        return;

    char path[MAX_PATH] = {};
    const bool exists = OakPerfBoost_ResolveDayzCfg(path, MAX_PATH);

    char buf[200 * 1024];
    DWORD rd = 0;
    if (exists)
    {
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            g_PerfCfgPatched = true;
            return;
        }
        DWORD sz = GetFileSize(h, nullptr);
        if (sz == INVALID_FILE_SIZE || sz > sizeof(buf) - 8)
        {
            CloseHandle(h);
            g_PerfCfgPatched = true;
            return;
        }
        if (sz > 0 && (!ReadFile(h, buf, sz, &rd, nullptr) || rd == 0))
        {
            CloseHandle(h);
            return;
        }
        CloseHandle(h);
        buf[rd] = 0;
    }
    else
    {
        buf[0] = 0;
        // Ensure directory exists best-effort.
        char dir[MAX_PATH] = {};
        lstrcpynA(dir, path, MAX_PATH);
        char* slash = strrchr(dir, '\\');
        if (slash) { *slash = 0; CreateDirectoryA(dir, nullptr); }
    }

    char out[200 * 1024];
    size_t outN = 0;
    auto append = [&](const char* s) {
        size_t L = strlen(s);
        if (outN + L + 4 >= sizeof(out)) return;
        memcpy(out + outN, s, L);
        outN += L;
    };

    bool sawVsync = false, sawRefresh = false, sawWindowed = false;
    bool changed = false;
    char* line = buf;
    while (line && *line)
    {
        char* next = line;
        while (*next && *next != '\n') ++next;
        char saved = *next;
        *next = 0;

        char tmp[512];
        lstrcpynA(tmp, line, 512);
        size_t tl = strlen(tmp);
        if (tl && tmp[tl - 1] == '\r') tmp[tl - 1] = 0;

        if (_strnicmp(tmp, "vsync=", 6) == 0 || _strnicmp(tmp, "VSync=", 6) == 0)
        {
            append("VSync=0;\r\n");
            sawVsync = true;
            if (_strnicmp(tmp, "VSync=0", 7) != 0 && _strnicmp(tmp, "vsync=0", 7) != 0)
                changed = true;
        }
        else if (_strnicmp(tmp, "refresh=", 8) == 0)
        {
            append("refresh=1000;\r\n");
            sawRefresh = true;
            if (_strnicmp(tmp, "refresh=1000", 12) != 0)
                changed = true;
        }
        else if (_strnicmp(tmp, "Windowed=", 9) == 0 || _strnicmp(tmp, "windowed=", 9) == 0)
        {
            // Keep player's window mode in cfg — exclusive is applied live via DXGI.
            append(tmp);
            append("\r\n");
            sawWindowed = true;
        }
        else if (_strnicmp(tmp, "MSAA=", 5) == 0 || _strnicmp(tmp, "FSAA=", 5) == 0
            || _strnicmp(tmp, "AToC=", 5) == 0)
        {
            // HWAA/MSAA + NVIDIA Freestyle crashes DayZ in nvppex (ppeGetVersion).
            // Force off so filters can stay enabled.
            char key[8] = {};
            lstrcpynA(key, tmp, 6); // "MSAA=" / "FSAA=" / "AToC="
            char lineOut[32];
            wsprintfA(lineOut, "%s0;\r\n", key);
            append(lineOut);
            if (tmp[5] != '0')
                changed = true;
        }
        else if (tmp[0])
        {
            append(tmp);
            append("\r\n");
        }
        *next = saved;
        if (!saved) break;
        line = next + 1;
    }
    if (!sawVsync) { append("VSync=0;\r\n"); changed = true; }
    if (!sawRefresh) { append("refresh=1000;\r\n"); changed = true; }

    if (changed || !exists)
    {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD wr = 0;
            WriteFile(h, out, (DWORD)outN, &wr, nullptr);
            CloseHandle(h);
            char b[160];
            wsprintfA(b, "perf: patched DayZ.cfg refresh=1000 VSync=0 (%s)", path);
            Log(b);
            OakPerfBoost_SetStatus("cfg: refresh=1000 — RESTART DayZ to apply");
        }
    }
    else
    {
        static bool s_LoggedOk = false;
        if (!s_LoggedOk)
        {
            s_LoggedOk = true;
            Log("perf: DayZ.cfg already uncapped (refresh/VSync)");
        }
    }

    g_PerfCfgPatched = true;
}

static void OakPerfBoost_ApplyFrameLatency(IDXGISwapChain* sc)
{
    const int want = g_PerfBoost.maxFrameLatency;
    if (want <= 0 || !sc)
        return;
    if (g_PerfAppliedLatency == want)
        return;

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return;
    IDXGIDevice1* dxgiDev = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev)) && dxgiDev)
    {
        if (SUCCEEDED(dxgiDev->SetMaximumFrameLatency((UINT)want)))
        {
            g_PerfAppliedLatency = want;
            char b[64];
            wsprintfA(b, "perf: maxFrameLatency=%d", want);
            Log(b);
        }
        dxgiDev->Release();
    }

    // SwapChain2 latency (waitable object path) when available.
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) && sc2)
    {
        sc2->SetMaximumFrameLatency((UINT)want);
        sc2->Release();
    }
    dev->Release();
}

static void OakPerfBoost_ApplyGpuPriority(IDXGISwapChain* sc, bool on)
{
    if (!sc) return;
    if (on && g_PerfGpuPrioApplied)
        return;
    if (!on && !g_PerfGpuPrioApplied)
        return;

    ID3D11Device* d3d = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d3d)) || !d3d)
        return;
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev)
    {
        // Range -7..7. Max helps contested GPU scheduling.
        INT prio = on ? 7 : 0;
        if (SUCCEEDED(dxgiDev->SetGPUThreadPriority(prio)))
        {
            g_PerfGpuPrioApplied = on ? 1 : 0;
            g_PerfBoost.gpuPrio = prio;
            if (on)
                Log("perf: GPU thread priority=7");
        }
        dxgiDev->Release();
    }
    d3d->Release();
}

static bool OakPerfBoost_TearingSupported(IDXGISwapChain* sc)
{
    if (g_PerfTearingCached >= 0)
        return g_PerfTearingCached == 1;
    g_PerfTearingCached = 0;
    if (!sc) return false;

    ID3D11Device* d3d = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d3d)) || !d3d)
        return false;
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev)
    {
        d3d->Release();
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter)
    {
        dxgiDev->Release();
        d3d->Release();
        return false;
    }
    IDXGIFactory* factory = nullptr;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory)) || !factory)
    {
        adapter->Release();
        dxgiDev->Release();
        d3d->Release();
        return false;
    }
    IDXGIFactory5* f5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5)) && f5)
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow)
            g_PerfTearingCached = 1;
        f5->Release();
    }
    factory->Release();
    adapter->Release();
    dxgiDev->Release();
    d3d->Release();
    return g_PerfTearingCached == 1;
}

static void OakPerfBoost_CacheSwapInfo(IDXGISwapChain* sc)
{
    if (!sc) return;
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(sc->GetDesc(&desc)))
        return;
    g_PerfBoost.swapEffect = (int)desc.SwapEffect;
    g_PerfBoost.swapFlags = (int)desc.Flags;
    g_PerfBoost.windowed = desc.Windowed ? 1 : 0;
    g_PerfBoost.refreshHz = (desc.BufferDesc.RefreshRate.Denominator > 0)
        ? (int)(desc.BufferDesc.RefreshRate.Numerator / desc.BufferDesc.RefreshRate.Denominator)
        : 0;
    g_PerfBoost.tearArmed = (desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? 1 : 0;
}

// NVIDIA Freestyle (nvppex) + DayZ DXGI fullscreen toggles = ACCESS_VIOLATION.
// Detect once so Frame Boost can uncap without SetFullscreenState thrashing.
static bool OakPerfBoost_NvidiaPpePresent()
{
    if (g_PerfNvidiaPpeCached >= 0)
        return g_PerfNvidiaPpeCached == 1;

    g_PerfNvidiaPpeCached = 0;
    const wchar_t* kMods[] = {
        L"nvppex.dll",
        L"nvspcap64.dll",
        L"nvEncodeAPI64.dll",
    };
    for (int i = 0; i < 3; ++i)
    {
        if (GetModuleHandleW(kMods[i]))
        {
            g_PerfNvidiaPpeCached = 1;
            Log("perf: NVIDIA PPE/overlay module present — skip exclusive FS toggles (filters safe)");
            break;
        }
    }
    return g_PerfNvidiaPpeCached == 1;
}

// Present(ALLOW_TEARING) needs flip + ALLOW_TEARING flag. Bitblt borderless is
// DWM-paced ≈ monitor Hz even with SyncInterval=0 — break via exclusive FS.
static void OakPerfBoost_BreakRefreshLock(IDXGISwapChain* sc)
{
    if (!sc)
        return;

    // Restore windowed when user turns break/unlock off.
    if (!g_PerfBoost.unlockPresent || !g_PerfBreakRefresh)
    {
        if (g_PerfForcedExclusive)
        {
            HRESULT hr = sc->SetFullscreenState(FALSE, nullptr);
            char b[96];
            wsprintfA(b, "perf: restore windowed hr=0x%08X", (unsigned)hr);
            Log(b);
            g_PerfForcedExclusive = false;
            OakInvalidateOverlayRtv();
            OakPerfBoost_CacheSwapInfo(sc);
        }
        return;
    }

    if (g_PerfBreakTried)
        return;
    g_PerfBreakTried = 1;

    OakPerfBoost_CacheSwapInfo(sc);

    // Already exclusive — Present(0) should be uncapped on blit.
    if (g_PerfBoost.windowed == 0)
    {
        g_PerfForcedExclusive = true;
        Log("perf: already exclusive FS — refresh lock bypassed");
        return;
    }

    const bool flip = g_PerfBoost.swapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
        || g_PerfBoost.swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Flip + tearing armed: no need for exclusive.
    if (flip && (g_PerfBoost.tearArmed || g_PerfBoost.allowTearing) && OakPerfBoost_TearingSupported(sc))
    {
        // Still try ResizeBuffers arm below via TryArmTearing; skip exclusive.
        Log("perf: flip chain — skip exclusive, use tearing path");
        return;
    }

    // Keep NVIDIA Freestyle alive: do not force exclusive while PPE is hooked in.
    if (OakPerfBoost_NvidiaPpePresent())
    {
        Log("perf: BreakRefreshLock skipped (NVIDIA filters) — use flip/tearing or native exclusive");
        return;
    }

    // Bitblt (or flip without tear): force exclusive fullscreen to leave DWM.
    OakInvalidateOverlayRtv();
    HRESULT hr = sc->SetFullscreenState(TRUE, nullptr);
    char b[120];
    wsprintfA(b, "perf: BreakRefreshLock SetFullscreenState(TRUE) hr=0x%08X", (unsigned)hr);
    Log(b);
    if (SUCCEEDED(hr))
    {
        g_PerfForcedExclusive = true;
        // Required after fullscreen transition on many DXGI paths.
        sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, (UINT)g_PerfBoost.swapFlags);
        // Push a high refresh mode — DayZ often treats display refresh as an FPS ceiling.
        DXGI_SWAP_CHAIN_DESC cur = {};
        if (SUCCEEDED(sc->GetDesc(&cur)))
        {
            DXGI_MODE_DESC mode = cur.BufferDesc;
            mode.RefreshRate.Numerator = 1000;
            mode.RefreshRate.Denominator = 1;
            HRESULT hrMode = sc->ResizeTarget(&mode);
            char mb[96];
            wsprintfA(mb, "perf: ResizeTarget refresh=1000 hr=0x%08X", (unsigned)hrMode);
            Log(mb);
        }
        OakPerfBoost_CacheSwapInfo(sc);
        wsprintfA(b, "perf: after exclusive win=%d swapEffect=%d hz=%d",
            g_PerfBoost.windowed, g_PerfBoost.swapEffect, g_PerfBoost.refreshHz);
        Log(b);
    }
}

static void OakPerfBoost_TryArmTearing(IDXGISwapChain* sc)
{
    if (!sc)
        return;

    OakPerfBoost_HookFactoryCreate(sc);
    OakPerfBoost_BreakRefreshLock(sc);

    if (!g_PerfBoost.unlockPresent || !g_PerfBoost.allowTearing)
        return;
    if (g_PerfTearResizeTried)
        return;

    OakPerfBoost_CacheSwapInfo(sc);
    // Exclusive FS: ALLOW_TEARING Present is illegal — sync0 only (already uncapped).
    if (g_PerfBoost.windowed == 0)
    {
        g_PerfTearResizeTried = 1;
        return;
    }
    if (!OakPerfBoost_TearingSupported(sc))
    {
        g_PerfTearResizeTried = 1;
        return;
    }
    if (g_PerfBoost.tearArmed)
    {
        g_PerfTearResizeTried = 1;
        return;
    }

    const bool flip = g_PerfBoost.swapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD
        || g_PerfBoost.swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (!flip)
    {
        g_PerfTearResizeTried = 1;
        return;
    }

    g_PerfTearResizeTried = 1;
    OakInvalidateOverlayRtv();
    const UINT newFlags = (UINT)g_PerfBoost.swapFlags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    HRESULT hr = sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, newFlags);
    if (SUCCEEDED(hr))
    {
        OakPerfBoost_CacheSwapInfo(sc);
        Log("perf: ResizeBuffers armed ALLOW_TEARING");
    }
    else
    {
        char b[96];
        wsprintfA(b, "perf: ResizeBuffers tear-arm failed hr=0x%08X", (unsigned)hr);
        Log(b);
    }
}

static void OakPerfBoost_TickOsHints(IDXGISwapChain* sc)
{
    const bool needTimer = g_PerfBoost.highPerfMode || g_PerfBoost.fpsCapEnabled || g_PerfBoost.unlockPresent;
    const bool wantHi = g_PerfBoost.highPerfMode || g_PerfBoost.unlockPresent;
    OakPerfBoost_ApplyTimerPeriod(needTimer);
    OakPerfBoost_ApplyPriority(wantHi);
    OakPerfBoost_BoostAllThreads(wantHi);
    OakPerfBoost_ApplyPowerRequests(wantHi);
    OakPerfBoost_ApplyMmCss(wantHi);
    OakPerfBoost_ApplyPowerThrottle(wantHi);
    OakPerfBoost_ApplyGpuPriority(sc, wantHi);
    OakPerfBoost_TryPatchDayzCfg();
    OakPerfBoost_ApplyFrameLatency(sc);
    OakPerfBoost_TryArmTearing(sc);
    OakPerfBoost_HeartbeatLog();
}

static void OakPerfBoost_PaceFrame()
{
    if (!g_PerfBoost.fpsCapEnabled)
    {
        g_PerfBoost.paceSleepMs = 0.f;
        g_PerfNextFrame.QuadPart = 0;
        return;
    }
    int fps = g_PerfBoost.fpsCap;
    if (fps < OAK_CAP_FPS_LIMIT_MIN) fps = OAK_CAP_FPS_LIMIT_MIN;
    if (fps > OAK_CAP_FPS_LIMIT) fps = OAK_CAP_FPS_LIMIT;

    OakPerfBoost_EnsureQpc();
    if (g_PerfQpcFreq.QuadPart <= 0)
        return;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    const LONGLONG ticksPerFrame = g_PerfQpcFreq.QuadPart / (LONGLONG)fps;
    if (ticksPerFrame <= 0)
        return;

    if (g_PerfNextFrame.QuadPart == 0)
    {
        g_PerfNextFrame.QuadPart = now.QuadPart + ticksPerFrame;
        return;
    }

    if (now.QuadPart >= g_PerfNextFrame.QuadPart)
    {
        LONGLONG late = now.QuadPart - g_PerfNextFrame.QuadPart;
        if (late > ticksPerFrame * 2)
            g_PerfNextFrame.QuadPart = now.QuadPart + ticksPerFrame;
        else
            g_PerfNextFrame.QuadPart += ticksPerFrame;
        g_PerfBoost.paceSleepMs = 0.f;
        return;
    }

    const double remainMs =
        (double)(g_PerfNextFrame.QuadPart - now.QuadPart) * 1000.0 / (double)g_PerfQpcFreq.QuadPart;
    g_PerfBoost.paceSleepMs = (float)remainMs;

    if (remainMs > 1.25)
    {
        DWORD sleepMs = (DWORD)(remainMs - 1.0);
        if (sleepMs > 25) sleepMs = 25;
        if (sleepMs > 0)
            Sleep(sleepMs);
    }

    for (;;)
    {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= g_PerfNextFrame.QuadPart)
            break;
        YieldProcessor();
    }
    g_PerfNextFrame.QuadPart += ticksPerFrame;
}

static void OakPerfBoost_UpdateMeasuredFps()
{
    OakPerfBoost_EnsureQpc();
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (g_PerfLastPresent.QuadPart > 0 && g_PerfQpcFreq.QuadPart > 0)
    {
        double dt = (double)(now.QuadPart - g_PerfLastPresent.QuadPart) / (double)g_PerfQpcFreq.QuadPart;
        if (dt > 0.0001 && dt < 1.0)
        {
            float inst = (float)(1.0 / dt);
            if (g_PerfBoost.measuredFps <= 1.f)
                g_PerfBoost.measuredFps = inst;
            else
                g_PerfBoost.measuredFps = g_PerfBoost.measuredFps * 0.85f + inst * 0.15f;
        }
    }
    g_PerfLastPresent = now;
}

static void OakPerfBoost_RefreshStatusLineSafe()
{
    if (g_PerfBoost.status[0] == 'c' && g_PerfBoost.status[1] == 'f' && g_PerfBoost.status[2] == 'g')
        return;
    char b[160];
    const char* mode = (g_PerfBoost.windowed == 0) ? "excl" : "win";
    // presentMs≈0 + moderate fps ⇒ DayZ main-thread/scene cost, not VSync.
    const bool engineBound = g_PerfBoost.unlockPresent
        && g_PerfPresentMs < 0.75f
        && g_PerfOverlayMs < 3.f
        && g_PerfBoost.measuredFps > 30.f
        && g_PerfBoost.measuredFps < 200.f;
    if (engineBound)
    {
        wsprintfA(b, "fps=%d %s | engine-bound (Present free, Oak %.0fms)",
            (int)(g_PerfBoost.measuredFps + 0.5f), mode, g_PerfOverlayMs);
    }
    else
    {
        const char* tear =
            !g_PerfBoost.unlockPresent ? "off"
            : (g_PerfBoost.tearingOk > 0 ? "ok"
               : (g_PerfForcedExclusive ? "excl-uncap"
                  : (g_PerfBoost.tearArmed ? "armed"
                     : (g_PerfBoost.swapEffect == 0 || g_PerfBoost.swapEffect == 1 ? "blit" : "blocked"))));
        wsprintfA(b, "fps=%d %s tear=%s%s",
            (int)(g_PerfBoost.measuredFps + 0.5f),
            mode,
            tear,
            g_PerfBoost.fpsCapEnabled ? " capped" : "");
    }
    OakPerfBoost_SetStatus(b);
}

using OakPerfPresentFn = HRESULT(*)(IDXGISwapChain*, UINT, UINT);
using OakPerfPresent1Fn = HRESULT(*)(IDXGISwapChain*, UINT, UINT, const void*);

static HRESULT OakPerfBoost_Present(IDXGISwapChain* sc, UINT syncIn, UINT flagsIn, OakPerfPresentFn callOrig)
{
    OakPerfBoost_TickOsHints(sc);
    g_PerfBoost.lastSyncIn = syncIn;

    UINT sync = syncIn;
    UINT flags = flagsIn;
    if (g_PerfBoost.unlockPresent)
    {
        sync = 0;
        // Tearing Present only valid windowed + armed swap chain.
        if (g_PerfBoost.allowTearing
            && g_PerfBoost.windowed != 0
            && OakPerfBoost_TearingSupported(sc)
            && (g_PerfBoost.tearArmed || (g_PerfBoost.swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)))
        {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
            g_PerfBoost.tearingTried++;
        }
    }
    g_PerfBoost.lastSyncOut = sync;

    OakPerfBoost_EnsureQpc();
    LARGE_INTEGER t0 = {}, t1 = {};
    QueryPerformanceCounter(&t0);
    HRESULT hr = callOrig(sc, sync, flags);
    QueryPerformanceCounter(&t1);
    if (g_PerfQpcFreq.QuadPart > 0)
        g_PerfPresentMs = (float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_PerfQpcFreq.QuadPart);
    if (FAILED(hr) && (flags & DXGI_PRESENT_ALLOW_TEARING))
    {
        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        hr = callOrig(sc, sync, flags);
    }
    else if (SUCCEEDED(hr) && (flags & DXGI_PRESENT_ALLOW_TEARING))
        g_PerfBoost.tearingOk++;

    OakPerfBoost_PaceFrame();
    OakPerfBoost_UpdateMeasuredFps();
    OakPerfBoost_RefreshStatusLineSafe();
    return hr;
}

static HRESULT OakPerfBoost_Present1(IDXGISwapChain* sc, UINT syncIn, UINT flagsIn, const void* params, OakPerfPresent1Fn callOrig)
{
    OakPerfBoost_TickOsHints(sc);
    g_PerfBoost.lastSyncIn = syncIn;

    UINT sync = syncIn;
    UINT flags = flagsIn;
    if (g_PerfBoost.unlockPresent)
    {
        sync = 0;
        if (g_PerfBoost.allowTearing
            && g_PerfBoost.windowed != 0
            && OakPerfBoost_TearingSupported(sc)
            && (g_PerfBoost.tearArmed || (g_PerfBoost.swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)))
        {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
            g_PerfBoost.tearingTried++;
        }
    }
    g_PerfBoost.lastSyncOut = sync;

    OakPerfBoost_EnsureQpc();
    LARGE_INTEGER t0 = {}, t1 = {};
    QueryPerformanceCounter(&t0);
    HRESULT hr = callOrig(sc, sync, flags, params);
    QueryPerformanceCounter(&t1);
    if (g_PerfQpcFreq.QuadPart > 0)
        g_PerfPresentMs = (float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_PerfQpcFreq.QuadPart);
    if (FAILED(hr) && (flags & DXGI_PRESENT_ALLOW_TEARING))
    {
        flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        hr = callOrig(sc, sync, flags, params);
    }
    else if (SUCCEEDED(hr) && (flags & DXGI_PRESENT_ALLOW_TEARING))
        g_PerfBoost.tearingOk++;

    OakPerfBoost_PaceFrame();
    OakPerfBoost_UpdateMeasuredFps();
    OakPerfBoost_RefreshStatusLineSafe();
    return hr;
}

static void OakPerfBoost_Shutdown()
{
    OakPerfBoost_ApplyPowerRequests(false);
    OakPerfBoost_BoostAllThreads(false);
    OakPerfBoost_ApplyMmCss(false);
    OakPerfBoost_ApplyTimerPeriod(false);
    OakPerfBoost_ApplyPriority(false);
    OakPerfBoost_ApplyPowerThrottle(false);
}
