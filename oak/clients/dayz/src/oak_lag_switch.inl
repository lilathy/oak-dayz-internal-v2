// Real lag switch: DROP outbound packets while bind held.
// Inline-hook ws2_32 via trampolines (thread-safe — unhook/call/rehook races net threads).
//
// Never use persistent Windows Firewall rules. A mid-hold crash used to leave
// OakLagCut / OakLagCutTcp enabled and permanently blocked every DayZ connect.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

static volatile LONG g_LagHooksInstalled = 0;
static volatile LONG g_LagHolding = 0;
static DWORD g_LagHoldStart = 0;
static DWORD g_LagCooldownUntil = 0;
static volatile LONG g_LagSeenSend = 0;
static volatile LONG g_LagSeenSendto = 0;
static volatile LONG g_LagSeenWSASend = 0;
static volatile LONG g_LagSeenWSASendTo = 0;
static volatile LONG g_LagDroppedN = 0;
static char g_LagStatus[72] = "idle";
static volatile LONG g_LagFwSweepDone = 0;

typedef int (WSAAPI* t_sendto)(SOCKET, const char*, int, int, const sockaddr*, int);
typedef int (WSAAPI* t_send)(SOCKET, const char*, int, int);
typedef int (WSAAPI* t_WSASendTo)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
    const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int (WSAAPI* t_WSASend)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

static t_sendto o_sendto = nullptr;
static t_send o_send = nullptr;
static t_WSASendTo o_WSASendTo = nullptr;
static t_WSASend o_WSASend = nullptr;

enum { kLagTrampBytes = 14 };

static bool LagIsHoldActive()
{
    return InterlockedCompareExchange(&g_LagHolding, 1, 1) == 1;
}

static int LagMaxHoldMs()
{
    switch (g_Exploits.warpMode)
    {
    case 0: return 4000;
    case 2: return 12000;
    default: return 7000;
    }
}

static void LagStatus(const char* s)
{
    if (!s) return;
    if (lstrcmpA(g_LagStatus, s) == 0)
        return;
    lstrcpynA(g_LagStatus, s, 72);
    ImGuiMenu_SetWarpStatus(g_LagStatus);
}

static bool LagShouldCutDest(const sockaddr* to, int tolen)
{
    if (!to || tolen < (int)sizeof(sockaddr_in))
        return true;
    if (to->sa_family == AF_INET)
    {
        const u_short port = ntohs(((const sockaddr_in*)to)->sin_port);
        if (port >= 27000 && port <= 27100) return false;
        if (port == 80 || port == 443) return false;
        return true;
    }
    if (to->sa_family == AF_INET6)
    {
        const u_short port = ntohs(((const sockaddr_in6*)to)->sin6_port);
        if (port >= 27000 && port <= 27100) return false;
        if (port == 80 || port == 443) return false;
        return true;
    }
    return true;
}

static void LagCompleteOv(LPWSAOVERLAPPED ov, DWORD bytes)
{
    if (!ov) return;
    ov->Internal = 0;
    ov->InternalHigh = bytes;
    if (ov->hEvent && ov->hEvent != INVALID_HANDLE_VALUE)
        SetEvent(ov->hEvent);
}

static bool LagDropNow(const sockaddr* to, int tolen, LPWSAOVERLAPPED ov, LPDWORD sent, DWORD bytes)
{
    if (!LagIsHoldActive())
        return false;
    if (to && !LagShouldCutDest(to, tolen))
        return false;
    InterlockedIncrement(&g_LagDroppedN);
    if (sent) *sent = bytes;
    LagCompleteOv(ov, bytes);
    return true;
}

#include "oak_lag_lde.inl"

// Build trampoline: steal complete instructions (>=14), then JMP back to target+steal.
static void* LagMakeTrampoline(BYTE* target, int* stealOut)
{
    int steal = LagStealLen(target, kLagTrampBytes);
    if (steal < kLagTrampBytes)
        return nullptr;
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, (size_t)steal);
    int o = steal;
    tramp[o + 0] = 0xFF; tramp[o + 1] = 0x25;
    tramp[o + 2] = 0; tramp[o + 3] = 0; tramp[o + 4] = 0; tramp[o + 5] = 0;
    UINT64 back = (UINT64)(target + steal);
    memcpy(tramp + o + 6, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);
    if (stealOut) *stealOut = steal;
    return tramp;
}

static bool LagPatchAbsJmp(BYTE* target, void* detour, int steal)
{
    if (steal < kLagTrampBytes) return false;
    BYTE jmp[kLagTrampBytes] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    UINT64 d = (UINT64)detour;
    memcpy(jmp + 6, &d, 8);
    DWORD old = 0;
    if (!VirtualProtect(target, (SIZE_T)steal, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(target, jmp, kLagTrampBytes);
    // NOP the leftover stolen bytes so nothing mid-insn remains executable.
    for (int i = kLagTrampBytes; i < steal; i++)
        target[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, (SIZE_T)steal);
    DWORD tmp = 0;
    VirtualProtect(target, (SIZE_T)steal, old, &tmp);
    return true;
}
static int WSAAPI hk_sendto(SOCKET s, const char* buf, int len, int flags,
    const sockaddr* to, int tolen)
{
    InterlockedIncrement(&g_LagSeenSendto);
    if (LagDropNow(to, tolen, nullptr, nullptr, (DWORD)(len > 0 ? len : 0)))
        return len > 0 ? len : 0;
    return o_sendto ? o_sendto(s, buf, len, flags, to, tolen) : SOCKET_ERROR;
}

static int WSAAPI hk_send(SOCKET s, const char* buf, int len, int flags)
{
    InterlockedIncrement(&g_LagSeenSend);
    if (LagDropNow(nullptr, 0, nullptr, nullptr, (DWORD)(len > 0 ? len : 0)))
        return len > 0 ? len : 0;
    return o_send ? o_send(s, buf, len, flags) : SOCKET_ERROR;
}

static int WSAAPI hk_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD bufCnt, LPDWORD sent,
    DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED ov,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE comp)
{
    InterlockedIncrement(&g_LagSeenWSASendTo);
    DWORD bytes = (bufs && bufCnt) ? bufs[0].len : 0;
    if (!comp && LagDropNow(to, tolen, ov, sent, bytes))
        return 0;
    return o_WSASendTo ? o_WSASendTo(s, bufs, bufCnt, sent, flags, to, tolen, ov, comp) : SOCKET_ERROR;
}

static int WSAAPI hk_WSASend(SOCKET s, LPWSABUF bufs, DWORD bufCnt, LPDWORD sent,
    DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE comp)
{
    InterlockedIncrement(&g_LagSeenWSASend);
    DWORD bytes = (bufs && bufCnt) ? bufs[0].len : 0;
    if (!comp && LagDropNow(nullptr, 0, ov, sent, bytes))
        return 0;
    return o_WSASend ? o_WSASend(s, bufs, bufCnt, sent, flags, ov, comp) : SOCKET_ERROR;
}

static bool LagHookIat(HMODULE mod, const char* dll, const char* func, PVOID hook, PVOID* orig)
{
    if (!mod || !dll || !func || !hook) return false;
    BYTE* base = (BYTE*)mod;
    __try
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir->VirtualAddress) return false;
        IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
        for (; imp->Name; imp++)
        {
            const char* name = (const char*)(base + imp->Name);
            const char* a = name; const char* b = dll;
            bool match = true;
            while (*a && *b)
            {
                char c1 = *a++, c2 = *b++;
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) { match = false; break; }
            }
            if (!match || (*a && *a != '.') || (*b)) continue;

            IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA* origThunk = imp->OriginalFirstThunk
                ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : thunk;
            for (; origThunk->u1.AddressOfData; ++thunk, ++origThunk)
            {
                if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                    continue;
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                const char* fname = (const char*)ibn->Name;
                const char* f1 = fname; const char* f2 = func;
                while (*f1 && *f2 && *f1 == *f2) { f1++; f2++; }
                if (*f1 || *f2) continue;

                DWORD oldProt = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(PVOID), PAGE_READWRITE, &oldProt))
                    return false;
                if (orig && !*orig)
                    *orig = (PVOID)thunk->u1.Function;
                thunk->u1.Function = (ULONGLONG)hook;
                DWORD tmp = 0;
                VirtualProtect(&thunk->u1.Function, sizeof(PVOID), oldProt, &tmp);
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return false;
}

static bool LagHookOneExport(const char* name, void* detour, void** outOrig)
{
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    if (!ws) ws = LoadLibraryA("ws2_32.dll");
    if (!ws) return false;
    BYTE* target = (BYTE*)GetProcAddress(ws, name);
    if (!target) return false;
    if (target[0] == 0xFF && target[1] == 0x25)
        return false;

    int steal = 0;
    void* tramp = LagMakeTrampoline(target, &steal);
    if (!tramp)
    {
        char b[96];
        wsprintfA(b, "lag: LDE fail %s", name);
        Log(b);
        return false;
    }
    if (!LagPatchAbsJmp(target, detour, steal))
        return false;
    *outOrig = tramp;
    char b[120];
    wsprintfA(b, "lag: tramp %s steal=%d @ %p -> %p", name, steal, target, tramp);
    Log(b);
    return true;
}

static bool LagRunNetsh(const char* args)
{
    if (!args || !args[0]) return false;
    // Prefer runas so residual OakLagCut deletes actually work after a
    // non-elevated DayZ session (plain "open" silently fails without admin).
    SHELLEXECUTEINFOA si{};
    si.cbSize = sizeof(si);
    si.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    si.lpVerb = "runas";
    si.lpFile = "netsh.exe";
    si.lpParameters = args;
    si.nShow = SW_HIDE;
    if (!ShellExecuteExA(&si))
    {
        si.lpVerb = "open";
        if (!ShellExecuteExA(&si))
            return false;
    }
    if (si.hProcess)
    {
        WaitForSingleObject(si.hProcess, 8000);
        DWORD code = 1;
        GetExitCodeProcess(si.hProcess, &code);
        CloseHandle(si.hProcess);
        return code == 0;
    }
    return true;
}

// Best-effort delete of leftover OakLagCut* rules from older builds.
// Never adds block rules — those survived crashes and blocked every connect.
static void LagClearResidualFirewall()
{
    if (InterlockedCompareExchange(&g_LagFwSweepDone, 1, 0) != 0)
        return;
    LagRunNetsh("advfirewall firewall delete rule name=OakLagCut");
    LagRunNetsh("advfirewall firewall delete rule name=OakLagCutTcp");
    Log("lag: residual firewall sweep (delete-only)");
}

static DWORD WINAPI LagFwClearThread(LPVOID)
{
    LagClearResidualFirewall();
    return 0;
}

static void LagClearResidualFirewallAsync()
{
    if (InterlockedCompareExchange(&g_LagFwSweepDone, 0, 0) != 0)
        return;
    HANDLE th = CreateThread(nullptr, 0, LagFwClearThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else LagClearResidualFirewall();
}

static void LagInstallHooks()
{
    // Only claim installed after at least one trampoline succeeds.
    if (InterlockedCompareExchange(&g_LagHooksInstalled, 0, 0) == 1)
        return;

    static LONG s_Busy = 0;
    if (InterlockedCompareExchange(&s_Busy, 1, 0) != 0)
        return;

    Log("lag: install begin");

    int ok = 0;
    void* tramp = nullptr;
    if (LagHookOneExport("sendto", (void*)&hk_sendto, &tramp)) { o_sendto = (t_sendto)tramp; ok++; }
    tramp = nullptr;
    if (LagHookOneExport("send", (void*)&hk_send, &tramp)) { o_send = (t_send)tramp; ok++; }
    tramp = nullptr;
    if (LagHookOneExport("WSASendTo", (void*)&hk_WSASendTo, &tramp)) { o_WSASendTo = (t_WSASendTo)tramp; ok++; }
    tramp = nullptr;
    if (LagHookOneExport("WSASend", (void*)&hk_WSASend, &tramp)) { o_WSASend = (t_WSASend)tramp; ok++; }

    // IAT backup — set orig only if trampoline path didn't.
    HMODULE mods[256];
    DWORD needed = 0;
    int nIat = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    {
        int n = (int)(needed / sizeof(HMODULE));
        if (n > 256) n = 256;
        for (int i = 0; i < n; i++)
        {
            PVOID discard = nullptr;
            if (!o_sendto)
            {
                if (LagHookIat(mods[i], "ws2_32.dll", "sendto", (PVOID)hk_sendto, (PVOID*)&o_sendto)) nIat++;
            }
            else if (LagHookIat(mods[i], "ws2_32.dll", "sendto", (PVOID)hk_sendto, &discard)) nIat++;

            discard = nullptr;
            if (!o_send)
            {
                if (LagHookIat(mods[i], "ws2_32.dll", "send", (PVOID)hk_send, (PVOID*)&o_send)) nIat++;
            }
            else if (LagHookIat(mods[i], "ws2_32.dll", "send", (PVOID)hk_send, &discard)) nIat++;

            discard = nullptr;
            if (!o_WSASendTo)
            {
                if (LagHookIat(mods[i], "ws2_32.dll", "WSASendTo", (PVOID)hk_WSASendTo, (PVOID*)&o_WSASendTo)) nIat++;
            }
            else if (LagHookIat(mods[i], "ws2_32.dll", "WSASendTo", (PVOID)hk_WSASendTo, &discard)) nIat++;

            discard = nullptr;
            if (!o_WSASend)
            {
                if (LagHookIat(mods[i], "ws2_32.dll", "WSASend", (PVOID)hk_WSASend, (PVOID*)&o_WSASend)) nIat++;
            }
            else if (LagHookIat(mods[i], "ws2_32.dll", "WSASend", (PVOID)hk_WSASend, &discard)) nIat++;
        }
    }

    // Absolute last resort: direct GetProcAddress originals (no hook) so we don't null-call.
    HMODULE ws = GetModuleHandleA("ws2_32.dll");
    if (ws)
    {
        if (!o_sendto) o_sendto = (t_sendto)GetProcAddress(ws, "sendto");
        if (!o_send) o_send = (t_send)GetProcAddress(ws, "send");
        if (!o_WSASendTo) o_WSASendTo = (t_WSASendTo)GetProcAddress(ws, "WSASendTo");
        if (!o_WSASend) o_WSASend = (t_WSASend)GetProcAddress(ws, "WSASend");
    }

    char b[160];
    wsprintfA(b, "lag: install done tramp=%d iat=%d o(st=%d s=%d wt=%d w=%d)",
        ok, nIat,
        o_sendto ? 1 : 0, o_send ? 1 : 0, o_WSASendTo ? 1 : 0, o_WSASend ? 1 : 0);
    Log(b);

    if (ok > 0 || nIat > 0)
        InterlockedExchange(&g_LagHooksInstalled, 1);
    InterlockedExchange(&s_Busy, 0);
}

static void LagStartHold()
{
    // Install only at hold time — never patch ws2 while idle/connecting.
    LagInstallHooks();
    if (InterlockedCompareExchange(&g_LagHooksInstalled, 0, 0) == 0)
    {
        Log("lag: hold aborted — hooks not installed");
        LagStatus("hook install failed");
        return;
    }
    g_LagHoldStart = GetTickCount();
    InterlockedExchange(&g_LagDroppedN, 0);
    InterlockedExchange(&g_LagHolding, 1);
    Log("lag: hold START (drop outbound, hooks only)");
    LagStatus("CUT — dropping outbound");
}

static void LagStopHold()
{
    if (!LagIsHoldActive())
        return;
    InterlockedExchange(&g_LagHolding, 0);
    const DWORD held = g_LagHoldStart ? (GetTickCount() - g_LagHoldStart) : 0;
    const int dropped = (int)g_LagDroppedN;

    DWORD now = GetTickCount();
    int cd = g_Exploits.warpCooldownMs;
    if (cd < 200) cd = 200;
    if (cd > 8000) cd = 8000;
    g_LagCooldownUntil = now + (DWORD)cd;

    char b[140];
    wsprintfA(b, "lag: hold END dropped=%d ms=%u seen(st=%d wsaT=%d s=%d wsa=%d)",
        dropped, held, (int)g_LagSeenSendto, (int)g_LagSeenWSASendTo,
        (int)g_LagSeenSend, (int)g_LagSeenWSASend);
    Log(b);
    char st[72];
    wsprintfA(st, "restored %ums drop=%d", held, dropped);
    LagStatus(st);
}

static void LagSwitchShutdown()
{
    LagStopHold();
    LagClearResidualFirewallAsync();
}

// Mouse buttons VK 0x01-0x06 (incl. XBUTTON1=5) must never drive lag — warpKey=5
// was auto-firing holds and patching ws2 mid-session → AV in our image.
static bool LagKeyUsable(int vk)
{
    if (vk <= 0) return false;
    if (vk >= 0x01 && vk <= 0x06) return false; // mouse buttons
    if (vk == 0x07) return false; // undefined
    return true;
}

static void LagSwitchTick()
{
    LagClearResidualFirewallAsync();

    // Official / BattlEye: never touch ws2. Inline trampolines during CDP traffic
    // crash DayZ (fault RIP inside our mapped image at hold START).
    if (OakBeIsLaunch())
    {
        if (LagIsHoldActive())
            LagStopHold();
        if (g_Exploits.warp)
        {
            g_Exploits.warp = false;
            ImGuiMenu_SetExploitWarp(false);
        }
        LagStatus("disabled on BE/official");
        return;
    }

    if (OakLab_GetState(OAK_LAB_WARP) == OAK_LAB_LOCKED)
    {
        if (LagIsHoldActive())
            LagStopHold();
        LagStatus("lab LOCKED");
        return;
    }

    // Never auto-enable from a held key. Menu toggle only.
    if (!g_Exploits.warp)
    {
        if (LagIsHoldActive())
            LagStopHold();
        LagStatus("off — enable in menu first");
        return;
    }

    const int key = (g_Exploits.warpKey != 0) ? g_Exploits.warpKey : 0x54;
    if (!LagKeyUsable(key))
    {
        if (LagIsHoldActive())
            LagStopHold();
        LagStatus("bad bind — set keyboard key");
        return;
    }

    const bool keyDown = KeyHeld(key) && !ImGuiMenu_IsOpen();

    const bool spawnReady =
        IsValidPtr(g_ResolvedLocalPlayer) && g_ResolvedLocalPlayer > 0x100000000ULL &&
        InterlockedCompareExchange(&g_SessionQuiesced, 0, 0) == 0;

    static DWORD s_SpawnReadySince = 0;
    if (!spawnReady)
    {
        s_SpawnReadySince = 0;
        if (LagIsHoldActive())
            LagStopHold();
        LagStatus("wait spawn — no cut mid-connect");
        return;
    }

    // Extra soak after writers resume — CDP/scoreboard still churns briefly.
    DWORD now = GetTickCount();
    if (!s_SpawnReadySince)
        s_SpawnReadySince = now;
    if ((DWORD)(now - s_SpawnReadySince) < 15000)
    {
        if (LagIsHoldActive())
            LagStopHold();
        LagStatus("soak — lag armed in a few sec");
        return;
    }

    static DWORD s_IdleBeat = 0;
    static LONG s_LastSeen = 0;
    if (!LagIsHoldActive() && (!s_IdleBeat || (now - s_IdleBeat) >= 5000))
    {
        s_IdleBeat = now;
        const LONG seen = g_LagSeenSendto + g_LagSeenWSASendTo + g_LagSeenSend + g_LagSeenWSASend;
        const LONG delta = seen - s_LastSeen;
        s_LastSeen = seen;
        char b[96];
        wsprintfA(b, "lag: traffic delta=%d total=%d hooks=%d",
            (int)delta, (int)seen, (int)g_LagHooksInstalled);
        Log(b);
    }

    if (LagIsHoldActive())
    {
        const int maxMs = LagMaxHoldMs();
        const int held = (int)(now - g_LagHoldStart);
        if (!keyDown || held >= maxMs)
        {
            LagStopHold();
            if (held >= maxMs)
                LagStatus("auto-release (anti-kick)");
            return;
        }
        char b[72];
        wsprintfA(b, "CUT %d.%01ds drop=%d seen=%d",
            held / 1000, (held / 100) % 10,
            (int)g_LagDroppedN,
            (int)(g_LagSeenSendto + g_LagSeenWSASendTo + g_LagSeenSend + g_LagSeenWSASend));
        LagStatus(b);

        static DWORD s_LastBeat = 0;
        if (!s_LastBeat || (now - s_LastBeat) >= 1000)
        {
            s_LastBeat = now;
            char beat[160];
            wsprintfA(beat, "lag: beat hold=%dms drop=%d seen(st=%d wsaT=%d)",
                held, (int)g_LagDroppedN, (int)g_LagSeenSendto, (int)g_LagSeenWSASendTo);
            Log(beat);
        }
        return;
    }

    if (now < g_LagCooldownUntil)
    {
        char b[72];
        wsprintfA(b, "cooldown %dms", (int)(g_LagCooldownUntil - now));
        LagStatus(b);
        return;
    }

    if (keyDown)
    {
        LagStartHold();
        return;
    }

    LagStatus("ready — HOLD bind drops outbound");
}

static void LagSwitchDrawOverlay()
{
    if (!LagIsHoldActive() || g_PanicHidden)
        return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    char msg[96];
    const int held = (int)(GetTickCount() - g_LagHoldStart);
    wsprintfA(msg, "CONNECTION CUT  %d.%01ds  dropped=%d  — release to restore",
        held / 1000, (held / 100) % 10, (int)g_LagDroppedN);
    ImVec2 sz = ImGui::CalcTextSize(msg);
    float x = (ImGui::GetIO().DisplaySize.x - sz.x) * 0.5f;
    float y = 48.f;
    dl->AddRectFilled(ImVec2(x - 10.f, y - 4.f), ImVec2(x + sz.x + 10.f, y + sz.y + 4.f),
        IM_COL32(40, 0, 0, 180), 4.f);
    dl->AddText(ImVec2(x, y), IM_COL32(255, 80, 80, 255), msg);
}
