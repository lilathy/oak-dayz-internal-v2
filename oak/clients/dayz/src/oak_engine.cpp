#include "oak_engine.h"

#include <string.h>

static HMODULE g_GameMod = nullptr;
static uintptr_t g_GameBase = 0;
static uintptr_t g_GameEnd = 0;
static char g_LastCensus[256] = {};
static uintptr_t g_HotWorld = 0;
static uintptr_t g_HotLp = 0;
static uintptr_t g_HotCam = 0;

void OakEngine_SetHot(uintptr_t world, uintptr_t localPlayer, uintptr_t camera)
{
    // Preserve existing hot slots when caller passes 0 (recoil microscope only has local).
    if (world) g_HotWorld = world;
    if (localPlayer) g_HotLp = localPlayer;
    if (camera) g_HotCam = camera;
}

void OakEngine_CensusHot(void)
{
    OakEngine_Census(g_HotWorld, g_HotLp, g_HotCam);
}

uintptr_t OakEngine_GetHotCamera(void) { return g_HotCam; }
uintptr_t OakEngine_GetHotLocal(void) { return g_HotLp; }

static void EngLog(const char* line)
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

static int EngPageReadable(const void* p, unsigned n)
{
    if (!p || !n)
        return 0;
    uintptr_t addr = (uintptr_t)p;
    // Reject non-canonical / clearly bogus before VirtualQuery.
    if (addr < 0x10000ull || addr > 0x00007FFFFFFFFFFFULL)
        return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == 0 || prot == PAGE_NOACCESS || prot == PAGE_EXECUTE || prot == PAGE_GUARD)
        return 0;
    // Ensure the whole span stays in this region.
    uintptr_t end = addr + (uintptr_t)n;
    uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (end < addr || end > regionEnd)
        return 0;
    return 1;
}

static int EngSafeCopy(void* dst, const void* src, unsigned n)
{
    if (!dst || !src || !n) return 0;
    // VirtualQuery BEFORE memcpy — CRT memcpy AVs on unmapped pages still hit VEH
    // and were killing Present during auto-census / RTTI walks.
    if (!EngPageReadable(src, n))
        return 0;
    __try {
        memcpy(dst, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int EngInGameImage(uintptr_t p)
{
    return g_GameBase && p >= g_GameBase && p < g_GameEnd;
}

static int EngLooksHeap(uintptr_t p)
{
    return p > 0x100000000ULL && p < 0x00007FFFFFFFFFFFULL;
}

static void EngDemangleRtti(const char* mangled, char* out, int outMax)
{
    if (!out || outMax < 2) return;
    out[0] = 0;
    if (!mangled || !mangled[0]) return;

    const char* s = mangled;
    // ".?AVClass@@" / ".?AUStruct@@"
    if (s[0] == '.' && s[1] == '?' && s[2] == 'A' && (s[3] == 'V' || s[3] == 'U'))
        s += 4;

    // Take up to first '@' for simple names; keep nested as A@B → B::A light form
    char tmp[128];
    int n = 0;
    while (*s && *s != '@' && n < (int)sizeof(tmp) - 1)
        tmp[n++] = *s++;
    tmp[n] = 0;

    // If namespace follows Class@Ns@@, prefer Class
    if (tmp[0])
        lstrcpynA(out, tmp, outMax);
    else
        lstrcpynA(out, mangled, outMax);
}

static int EngNameFromCol(uintptr_t col, char* out, int outMax)
{
    if (!out || outMax < 2) return 0;
    out[0] = 0;
    if (!EngInGameImage(col) && !EngLooksHeap(col))
        return 0;

    // x64 RTTICompleteObjectLocator — fields after signature are RVAs from image base
    DWORD sig = 0, typeRva = 0;
    if (!EngSafeCopy(&sig, (const void*)col, 4))
        return 0;
    if (!EngSafeCopy(&typeRva, (const void*)(col + 0x0C), 4))
        return 0;

    uintptr_t typeDesc = 0;
    if (sig == 1)
    {
        // x64: RVA
        if (!typeRva || !g_GameBase)
            return 0;
        typeDesc = g_GameBase + typeRva;
    }
    else
    {
        // x86-style absolute (rare here)
        if (!EngSafeCopy(&typeDesc, (const void*)(col + 0x0C), sizeof(typeDesc)))
            return 0;
    }

    if (!EngInGameImage(typeDesc))
        return 0;

    char mangled[128] = {};
    // TypeDescriptor: void* vftable, void* spare, then name at +0x10 on x64
    if (!EngSafeCopy(mangled, (const void*)(typeDesc + 0x10), sizeof(mangled) - 1))
        return 0;
    mangled[sizeof(mangled) - 1] = 0;
    if (mangled[0] != '.' && mangled[0] != '?')
        return 0;

    EngDemangleRtti(mangled, out, outMax);
    return out[0] ? 1 : 0;
}

int OakEngine_NameVtable(uintptr_t vtable, char* out, int outMax)
{
    if (!out || outMax < 2) return 0;
    out[0] = 0;
    if (!EngLooksHeap(vtable) && !EngInGameImage(vtable))
        return 0;

    uintptr_t col = 0;
    if (!EngSafeCopy(&col, (const void*)(vtable - sizeof(uintptr_t)), sizeof(col)))
        return 0;
    return EngNameFromCol(col, out, outMax);
}

int OakEngine_NameObject(uintptr_t obj, char* out, int outMax)
{
    if (!out || outMax < 2) return 0;
    out[0] = 0;
    if (!EngLooksHeap(obj))
        return 0;

    uintptr_t vtable = 0;
    if (!EngSafeCopy(&vtable, (const void*)obj, sizeof(vtable)))
        return 0;
    if (!EngInGameImage(vtable) && !EngLooksHeap(vtable))
        return 0;
    // Vtables usually live in the module image (.rdata)
    return OakEngine_NameVtable(vtable, out, outMax);
}

// ---- write-change watches (Present-polled; stack = who was on Present when change seen) ----
enum { kEngWatchMax = 8 };

struct EngWatch {
    int used;
    uintptr_t addr;
    unsigned size;
    unsigned char prev[16];
    char tag[32];
    DWORD changes;
};

static EngWatch g_Watch[kEngWatchMax] = {};

int OakEngine_WatchCount(void)
{
    int n = 0;
    for (int i = 0; i < kEngWatchMax; i++)
        if (g_Watch[i].used) n++;
    return n;
}

int OakEngine_Watch(uintptr_t addr, unsigned size, const char* tag)
{
    if (!EngLooksHeap(addr) || size < 1 || size > 16)
        return -1;
    for (int i = 0; i < kEngWatchMax; i++)
    {
        if (g_Watch[i].used) continue;
        ZeroMemory(&g_Watch[i], sizeof(g_Watch[i]));
        g_Watch[i].used = 1;
        g_Watch[i].addr = addr;
        g_Watch[i].size = size;
        lstrcpynA(g_Watch[i].tag, tag ? tag : "watch", 32);
        EngSafeCopy(g_Watch[i].prev, (const void*)addr, size);
        char b[160];
        wsprintfA(b, "engine[watch] slot=%d addr=%p sz=%u tag=%s", i, (void*)addr, size, g_Watch[i].tag);
        EngLog(b);
        return i;
    }
    EngLog("engine[watch] full");
    return -1;
}

void OakEngine_Unwatch(int slot)
{
    if (slot < 0 || slot >= kEngWatchMax) return;
    if (g_Watch[slot].used)
    {
        char b[80];
        wsprintfA(b, "engine[unwatch] slot=%d", slot);
        EngLog(b);
    }
    g_Watch[slot].used = 0;
}

void OakEngine_UnwatchAll(void)
{
    for (int i = 0; i < kEngWatchMax; i++)
        g_Watch[i].used = 0;
    EngLog("engine[unwatch] all");
}

static void EngLogStack(const char* tag)
{
    void* frames[10];
    USHORT n = CaptureStackBackTrace(1, 10, frames, nullptr);
    for (USHORT i = 0; i < n; i++)
    {
        char line[96];
        const char* where = EngInGameImage((uintptr_t)frames[i]) ? "game" : "other";
        wsprintfA(line, "engine[stack] %s #%u %p (%s)", tag, (unsigned)i, frames[i], where);
        EngLog(line);
    }
}

static void EngPollWatches(void)
{
    for (int i = 0; i < kEngWatchMax; i++)
    {
        if (!g_Watch[i].used) continue;
        unsigned char now[16] = {};
        if (!EngSafeCopy(now, (const void*)g_Watch[i].addr, g_Watch[i].size))
            continue;
        if (memcmp(now, g_Watch[i].prev, g_Watch[i].size) == 0)
            continue;

        g_Watch[i].changes++;
        unsigned oldBits = 0, newBits = 0;
        memcpy(&oldBits, g_Watch[i].prev, g_Watch[i].size < 4 ? g_Watch[i].size : 4);
        memcpy(&newBits, now, g_Watch[i].size < 4 ? g_Watch[i].size : 4);
        char b[200];
        wsprintfA(b, "engine[change] tag=%s addr=%p old=%08X new=%08X n=%u (Present-observed)",
            g_Watch[i].tag, (void*)g_Watch[i].addr, oldBits, newBits, g_Watch[i].changes);
        EngLog(b);
        EngLogStack(g_Watch[i].tag);
        memcpy(g_Watch[i].prev, now, g_Watch[i].size);
    }
}

static void EngNameLog(const char* label, uintptr_t obj)
{
    char name[96] = {};
    if (!obj)
    {
        char b[80];
        wsprintfA(b, "engine[type] %s=null", label);
        EngLog(b);
        return;
    }
    if (OakEngine_NameObject(obj, name, sizeof(name)))
    {
        char b[160];
        wsprintfA(b, "engine[type] %s=%p name=%s", label, (void*)obj, name);
        EngLog(b);
    }
    else
    {
        char b[120];
        wsprintfA(b, "engine[type] %s=%p name=?", label, (void*)obj);
        EngLog(b);
    }
}

int OakEngine_FindNamedChild(uintptr_t parent, const char* nameSubstr, uintptr_t offLo, uintptr_t offHi);

void OakEngine_Census(uintptr_t world, uintptr_t localPlayer, uintptr_t camera)
{
    EngLog("engine[census] begin");
    EngNameLog("world", world);
    EngNameLog("localPlayer", localPlayer);
    EngNameLog("camera", camera);

    if (localPlayer)
    {
        uintptr_t vs = 0, fvs = 0;
        EngSafeCopy(&vs, (const void*)(localPlayer + 0x1C8), sizeof(vs)); // VisualState
        EngSafeCopy(&fvs, (const void*)(localPlayer + 0x120), sizeof(fvs)); // FutureVisualState
        EngNameLog("local.VS", vs);
        EngNameLog("local.FVS", fvs);

        int typed = 0;
        for (uintptr_t off = 0x40; off <= 0xA00 && typed < 24; off += 8)
        {
            uintptr_t p = 0;
            if (!EngSafeCopy(&p, (const void*)(localPlayer + off), sizeof(p)))
                continue;
            if (!EngLooksHeap(p) || p == localPlayer) continue;
            char name[96] = {};
            if (!OakEngine_NameObject(p, name, sizeof(name)))
                continue;
            if (!name[0] || name[0] == '?') continue;
            char b[160];
            wsprintfA(b, "engine[type] local+0x%X=%p name=%s", (unsigned)off, (void*)p, name);
            EngLog(b);
            typed++;
        }

        // Targeted hunts from script ground truth
        OakEngine_FindNamedChild(localPlayer, "Stamina", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "PlayerStat", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "Inventory", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "Input", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "Aiming", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "Recoil", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "ItemAccessor", 0x40, 0xC00);
        OakEngine_FindNamedChild(localPlayer, "CharacterCtrl", 0x40, 0xC00);
    }

    if (camera)
    {
        uintptr_t vt = 0;
        EngSafeCopy(&vt, (const void*)camera, sizeof(vt));
        char vn[96] = {};
        if (OakEngine_NameVtable(vt, vn, sizeof(vn)))
        {
            char b[120];
            wsprintfA(b, "engine[type] camera.vt=%p name=%s", (void*)vt, vn);
            EngLog(b);
        }
    }

    wsprintfA(g_LastCensus, "census done watches=%d", OakEngine_WatchCount());
    EngLog("engine[census] end");
}

int OakEngine_FindNamedChild(uintptr_t parent, const char* nameSubstr, uintptr_t offLo, uintptr_t offHi)
{
    if (!EngLooksHeap(parent) || !nameSubstr || !nameSubstr[0])
        return 0;
    int hits = 0;
    for (uintptr_t off = offLo; off <= offHi; off += 8)
    {
        uintptr_t p = 0;
        if (!EngSafeCopy(&p, (const void*)(parent + off), sizeof(p)))
            continue;
        if (!EngLooksHeap(p) || p == parent) continue;
        char name[96] = {};
        if (!OakEngine_NameObject(p, name, sizeof(name)))
            continue;
        // case-insensitive substring
        int match = 0;
        for (int i = 0; name[i]; i++)
        {
            int j = 0;
            while (nameSubstr[j] && name[i + j])
            {
                char a = name[i + j];
                char b = nameSubstr[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
                j++;
            }
            if (!nameSubstr[j]) { match = 1; break; }
        }
        if (!match) continue;
        char line[160];
        wsprintfA(line, "engine[find] substr=%s off=0x%X ptr=%p name=%s",
            nameSubstr, (unsigned)off, (void*)p, name);
        EngLog(line);
        hits++;
        if (hits >= 8) break;
    }
    if (!hits)
    {
        char line[96];
        wsprintfA(line, "engine[find] substr=%s hits=0", nameSubstr);
        EngLog(line);
    }
    return hits;
}

const char* OakEngine_LastCensusLine(void)
{
    return g_LastCensus;
}

// ---- float snapshot / diff (settings FOV hunt) ----
enum { kEngSnapMax = 128 };
static float g_SnapVals[kEngSnapMax];
static uintptr_t g_SnapObj = 0;
static unsigned g_SnapBytes = 0;
static char g_SnapTag[32] = {};

void OakEngine_FloatSnap(uintptr_t obj, unsigned bytes, const char* tag)
{
    if (!EngLooksHeap(obj) || bytes < 16)
        return;
    if (bytes > kEngSnapMax * 4)
        bytes = kEngSnapMax * 4;
    g_SnapObj = obj;
    g_SnapBytes = bytes;
    lstrcpynA(g_SnapTag, tag ? tag : "snap", 32);
    ZeroMemory(g_SnapVals, sizeof(g_SnapVals));
    EngSafeCopy(g_SnapVals, (const void*)obj, bytes);
    char b[120];
    wsprintfA(b, "engine[fovsnap] tag=%s obj=%p bytes=%u — change settings FOV then DIFF",
        g_SnapTag, (void*)obj, bytes);
    EngLog(b);
}

void OakEngine_FloatDiff(uintptr_t obj, unsigned bytes, const char* tag)
{
    if (!g_SnapObj)
    {
        EngLog("engine[fovdiff] no prior SNAP");
        return;
    }
    if (!obj) obj = g_SnapObj;
    if (!bytes) bytes = g_SnapBytes;
    if (bytes > kEngSnapMax * 4) bytes = kEngSnapMax * 4;

    float now[kEngSnapMax];
    ZeroMemory(now, sizeof(now));
    if (!EngSafeCopy(now, (const void*)obj, bytes))
    {
        EngLog("engine[fovdiff] read-fail");
        return;
    }

    int hits = 0;
    int userHits = 0;
    unsigned n = bytes / 4;
    char name[96] = {};
    OakEngine_NameObject(obj, name, sizeof(name));
    char hdr[140];
    wsprintfA(hdr, "engine[fovdiff] tag=%s obj=%p rtti=%s",
        tag ? tag : g_SnapTag, (void*)obj, name[0] ? name : "?");
    EngLog(hdr);

    // DayZ scripts: OPTIONS_FIELD_OF_VIEW_MIN/MAX ≈ 0.752 .. 1.303 (m_UserFOV)
    auto isUserFov = [](float v) -> int {
        return (v >= 0.74f && v <= 1.32f) ? 1 : 0;
    };
    // Camera W2S / projection caches — ignore for auto-HWBP (already proven)
    auto isCamProjOff = [](unsigned off) -> int {
        return (off == 0x70 || off == 0x74 || off == 0x78 || off == 0x7C ||
                off == 0x80 || off == 0x84 || off == 0x88 || off == 0x8C ||
                off == 0xA0 || off == 0xB0 || off == 0xD0 || off == 0xE0 ||
                off == 0x194 || off == 0x198 || off == 0x1A8 || off == 0x1AC) ? 1 : 0;
    };

    int bestUserIdx = -1;
    int bestFovIdx = -1;

    for (unsigned i = 0; i < n; i++)
    {
        float a = g_SnapVals[i];
        float b = now[i];
        if (!(a == a) || !(b == b)) continue;
        float d = b - a;
        if (d < 0.f) d = -d;
        int looksFov = ((b > 0.25f && b < 2.0f) || (b > 40.f && b < 130.f)) ? 1 : 0;
        int userFov = isUserFov(b) && isUserFov(a);
        if (d < 0.0005f) continue;
        if (!looksFov && !userFov && d < 0.01f) continue;

        unsigned off = i * 4;
        int ax = (int)(a * 1000.f);
        int bx = (int)(b * 1000.f);
        char line[180];
        wsprintfA(line, "engine[fovdiff] +0x%X oldx1000=%d newx1000=%d fovish=%d userfov=%d",
            off, ax, bx, looksFov, userFov);
        EngLog(line);
        hits++;
        if (userFov && !isCamProjOff(off))
        {
            userHits++;
            if (bestUserIdx < 0) bestUserIdx = (int)i;
        }
        else if (looksFov && !isCamProjOff(off) && bestFovIdx < 0)
            bestFovIdx = (int)i;
        if (hits >= 32) break;
    }
    char sum[100];
    wsprintfA(sum, "engine[fovdiff] changes=%d userfovHits=%d", hits, userHits);
    EngLog(sum);

    // Prefer m_UserFOV-range float outside known Camera projection slots
    int armIdx = bestUserIdx >= 0 ? bestUserIdx : bestFovIdx;
    if (armIdx >= 0)
    {
        unsigned off = (unsigned)armIdx * 4;
        OakEngine_HwbpWatch(obj + off, 4, userHits ? "userfov-write" : "fov-settings-write", 1);
        char arm[120];
        wsprintfA(arm, "engine[fovdiff] HWBP +0x%X (%s) — tweak Settings FOV once more",
            off, userHits ? "userfov-range" : "fovish-nonproj");
        EngLog(arm);
    }
    else if (hits > 0)
        EngLog("engine[fovdiff] no non-projection FOV candidate — try SNAP on local player");
}

// ---- UserFOV graph hunt (DayZGame.m_UserFOV ≈ 0.75..1.30) ----
enum { kUserFovCand = 384 };
struct EngUserFovCand {
    uintptr_t addr;
    float v;
};
static EngUserFovCand g_UFov[kUserFovCand];
static int g_UFovN = 0;
enum { kUserFovTargets = 8 };
static uintptr_t g_UFovTargets[kUserFovTargets] = {};
static int g_UFovTargetN = 0;

static int EngIsUserFovRange(float v)
{
    return (v >= 0.74f && v <= 1.32f) ? 1 : 0;
}

static void EngUserFovAdd(uintptr_t addr, float v)
{
    if (!addr || !EngIsUserFovRange(v) || g_UFovN >= kUserFovCand)
        return;
    // de-dupe
    for (int i = 0; i < g_UFovN; i++)
        if (g_UFov[i].addr == addr)
            return;
    g_UFov[g_UFovN].addr = addr;
    g_UFov[g_UFovN].v = v;
    g_UFovN++;
}

static void EngUserFovScanObj(uintptr_t obj, unsigned bytes)
{
    if (!EngLooksHeap(obj) || bytes < 16)
        return;
    if (bytes > 0xC00) bytes = 0xC00;
    // Skip Camera — already proven W2S-only
    if (obj == g_HotCam)
        return;

    unsigned char buf[0xC00];
    if (!EngSafeCopy(buf, (const void*)obj, bytes))
        return;
    unsigned n = bytes / 4;
    for (unsigned i = 0; i < n; i++)
    {
        float f = 0.f;
        memcpy(&f, buf + i * 4, 4);
        if (EngIsUserFovRange(f))
            EngUserFovAdd(obj + i * 4, f);
    }
}

static void EngUserFovScanGraph(uintptr_t root, unsigned rootBytes, unsigned childBytes, int maxChildren)
{
    if (!EngLooksHeap(root))
        return;
    EngUserFovScanObj(root, rootBytes);

    int kids = 0;
    unsigned lim = rootBytes < 0x1000 ? rootBytes : 0x1000;
    for (unsigned off = 0x08; off + 8 <= lim && kids < maxChildren; off += 8)
    {
        uintptr_t p = 0;
        if (!EngSafeCopy(&p, (const void*)(root + off), sizeof(p)))
            continue;
        if (!EngLooksHeap(p) || p == root || p == g_HotCam)
            continue;
        EngUserFovScanObj(p, childBytes);
        kids++;
    }
}

void OakEngine_UserFovSnap(uintptr_t fovContextObj)
{
    g_UFovN = 0;

    if (!EngLooksHeap(fovContextObj) && g_GameBase)
    {
        uintptr_t ctx = 0;
        if (EngSafeCopy(&ctx, (const void*)(g_GameBase + 0x1007C70), sizeof(ctx)) && EngLooksHeap(ctx))
            fovContextObj = ctx;
    }

    EngUserFovScanGraph(g_HotWorld, 0x800, 0x200, 48);
    EngUserFovScanGraph(g_HotLp, 0x400, 0x180, 32);
    if (EngLooksHeap(fovContextObj))
        EngUserFovScanGraph(fovContextObj, 0xA00, 0x100, 24);

    // RTTI hunt: World children named *Game*
    if (EngLooksHeap(g_HotWorld))
    {
        for (uintptr_t off = 0x40; off <= 0x3000; off += 8)
        {
            uintptr_t p = 0;
            if (!EngSafeCopy(&p, (const void*)(g_HotWorld + off), sizeof(p)))
                continue;
            if (!EngLooksHeap(p)) continue;
            char name[96] = {};
            if (!OakEngine_NameObject(p, name, sizeof(name)) || !name[0])
                continue;
            int hit = 0;
            for (int i = 0; name[i]; i++)
            {
                char c0 = name[i];
                if (c0 >= 'A' && c0 <= 'Z') c0 = (char)(c0 - 'A' + 'a');
                if (c0 == 'g' && name[i + 1] && name[i + 2] && name[i + 3])
                {
                    char c1 = name[i + 1], c2 = name[i + 2], c3 = name[i + 3];
                    if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 - 'A' + 'a');
                    if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 - 'A' + 'a');
                    if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 - 'A' + 'a');
                    if (c1 == 'a' && c2 == 'm' && c3 == 'e') { hit = 1; break; }
                }
            }
            if (!hit) continue;
            char line[140];
            wsprintfA(line, "engine[userfov] game-like off=0x%X ptr=%p name=%s",
                (unsigned)off, (void*)p, name);
            EngLog(line);
            EngUserFovScanObj(p, 0x800);
        }
    }

    char sum[120];
    wsprintfA(sum, "engine[userfov] SNAP candidates=%d — change Settings FOV then HUNT DIFF",
        g_UFovN);
    EngLog(sum);
}

void OakEngine_UserFovDiff(void)
{
    if (g_UFovN <= 0)
    {
        EngLog("engine[userfov] no SNAP candidates");
        return;
    }

    g_UFovTargetN = 0;
    int changed = 0;
    uintptr_t firstAddr = 0;
    for (int i = 0; i < g_UFovN; i++)
    {
        float now = 0.f;
        if (!EngSafeCopy(&now, (const void*)g_UFov[i].addr, sizeof(now)))
            continue;
        if (!(now == now)) continue;
        float d = now - g_UFov[i].v;
        if (d < 0.f) d = -d;
        if (d < 0.001f) continue;

        int still = EngIsUserFovRange(now);
        int oldk = (int)(g_UFov[i].v * 1000.f);
        int newk = (int)(now * 1000.f);
        char name[96] = {};
        for (int back = 0; back < 0x200; back += 0x10)
        {
            if (OakEngine_NameObject(g_UFov[i].addr - (uintptr_t)back, name, sizeof(name)) && name[0])
                break;
            name[0] = 0;
        }
        char line[200];
        wsprintfA(line, "engine[userfov] DIFF addr=%p oldx1000=%d newx1000=%d still=%d rtti~%s",
            (void*)g_UFov[i].addr, oldk, newk, still, name[0] ? name : "?");
        EngLog(line);
        if (still)
        {
            if (!firstAddr)
                firstAddr = g_UFov[i].addr;
            if (g_UFovTargetN < kUserFovTargets)
            {
                g_UFovTargets[g_UFovTargetN] = g_UFov[i].addr;
                g_UFovTargetN++;
            }
        }
        changed++;
        if (changed >= 24) break;
    }

    char sum[100];
    wsprintfA(sum, "engine[userfov] DIFF changes=%d targets=%d", changed, g_UFovTargetN);
    EngLog(sum);

    if (firstAddr)
    {
        OakEngine_HwbpWatch(firstAddr, 4, "userfov-write", 1);
        EngLog("engine[userfov] HWBP armed — tweak Settings FOV once more");
    }
    else if (changed == 0)
        EngLog("engine[userfov] no candidate moved — DayZGame not in scanned graph");
}

int OakEngine_UserFovTargetCount(void)
{
    return g_UFovTargetN;
}

uintptr_t OakEngine_UserFovTarget(int index)
{
    if (index < 0 || index >= g_UFovTargetN)
        return 0;
    return g_UFovTargets[index];
}

// ---- (recoil hunt removed — feature retired) ----

// ---- Hardware breakpoints (true write attribution) ----
#include <tlhelp32.h>

enum { kHwbpMax = 4 };

struct EngHwbp {
    int used;
    uintptr_t addr;
    unsigned size; // 1,2,4,8
    char tag[32];
    int oneshot;
    volatile LONG hits;
};

static EngHwbp g_Hwbp[kHwbpMax] = {};
static PVOID g_HwbpVeh = nullptr;
static CRITICAL_SECTION g_HwbpCs;
static volatile LONG g_HwbpCsReady = 0;
static volatile LONG64 g_LastHwbpRip = 0;
static char g_LastHwbpTag[32] = {};

static unsigned EngHwbpLenCode(unsigned size)
{
    // Dr7 LEN: 00=1, 01=2, 11=4, 10=8
    if (size >= 8) return 2;
    if (size >= 4) return 3;
    if (size >= 2) return 1;
    return 0;
}

static void EngHwbpFillContext(CONTEXT* ctx)
{
    ctx->Dr0 = ctx->Dr1 = ctx->Dr2 = ctx->Dr3 = 0;
    // Keep lower reserved bits clear; rebuild enables from slots.
    DWORD64 dr7 = 0;
    for (int i = 0; i < kHwbpMax; i++)
    {
        if (!g_Hwbp[i].used || !g_Hwbp[i].addr)
            continue;
        uintptr_t a = g_Hwbp[i].addr;
        if (i == 0) ctx->Dr0 = a;
        else if (i == 1) ctx->Dr1 = a;
        else if (i == 2) ctx->Dr2 = a;
        else ctx->Dr3 = a;
        // Local enable bit
        dr7 |= (1ULL << (i * 2));
        // Condition = write (01), LEN from size
        unsigned cond = 1; // write
        unsigned len = EngHwbpLenCode(g_Hwbp[i].size);
        dr7 |= ((DWORD64)cond << (16 + i * 4));
        dr7 |= ((DWORD64)len << (18 + i * 4));
    }
    ctx->Dr6 = 0;
    ctx->Dr7 = dr7;
}

static void EngHwbpApplyAllThreads(void)
{
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != pid)
                continue;
            DWORD access = THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION;
            HANDLE th = OpenThread(access, FALSE, te.th32ThreadID);
            if (!th)
                continue;

            int suspended = 0;
            if (te.th32ThreadID != self)
            {
                if (SuspendThread(th) != (DWORD)-1)
                    suspended = 1;
            }

            CONTEXT ctx;
            ZeroMemory(&ctx, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx))
            {
                EngHwbpFillContext(&ctx);
                SetThreadContext(th, &ctx);
            }

            if (suspended)
                ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void EngDumpObjectImpl(uintptr_t obj, unsigned bytes, const char* tag)
{
    if (!obj || bytes < 16 || bytes > 512)
        return;
    if (!EngLooksHeap(obj) && !EngInGameImage(obj))
        return;

    char name[96] = {};
    OakEngine_NameObject(obj, name, sizeof(name));
    char hdr[160];
    wsprintfA(hdr, "engine[dump] tag=%s obj=%p bytes=%u rtti=%s",
        tag ? tag : "obj", (void*)obj, bytes, name[0] ? name : "?");
    EngLog(hdr);

    unsigned char buf[512];
    if (!EngSafeCopy(buf, (const void*)obj, bytes))
    {
        EngLog("engine[dump] read-fail");
        return;
    }

    for (unsigned off = 0; off + 16 <= bytes; off += 16)
    {
        char line[220];
        char* p = line;
        p += wsprintfA(p, "engine[dump] +%03X ", off);
        for (int i = 0; i < 16; i++)
            p += wsprintfA(p, "%02X", buf[off + i]);
        // annotate first 8 bytes as ptr + float if plausible
        uintptr_t maybePtr = 0;
        float maybeF = 0.f;
        memcpy(&maybePtr, buf + off, sizeof(maybePtr));
        memcpy(&maybeF, buf + off, sizeof(maybeF));
        if (EngLooksHeap(maybePtr) || EngInGameImage(maybePtr))
        {
            char cn[64] = {};
            if (OakEngine_NameObject(maybePtr, cn, sizeof(cn)) && cn[0])
                wsprintfA(p, " ptr=%p(%s)", (void*)maybePtr, cn);
            else
                wsprintfA(p, " ptr=%p", (void*)maybePtr);
        }
        else if (maybeF == maybeF && maybeF > -1e6f && maybeF < 1e6f &&
                 (maybeF != 0.f || (buf[off] | buf[off + 1] | buf[off + 2] | buf[off + 3])))
        {
            // skip printing float with wsprintf %f — use scaled int
            int fx1000 = (int)(maybeF * 1000.f);
            wsprintfA(p, " f~%d/1000", fx1000);
        }
        EngLog(line);
    }
}

void OakEngine_DumpObject(uintptr_t obj, unsigned bytes, const char* tag)
{
    if (bytes < 16) bytes = 16;
    if (bytes > 512) bytes = 512;
    EngDumpObjectImpl(obj, bytes, tag);
}

void OakEngine_DumpAround(uintptr_t addr, unsigned before, unsigned after, const char* tag)
{
    if (!addr) return;
    if (before > 128) before = 128;
    if (after > 256) after = 256;
    uintptr_t base = addr - before;
    EngDumpObjectImpl(base, before + after, tag);
}

uintptr_t OakEngine_LastHwbpRip(void)
{
    return (uintptr_t)InterlockedCompareExchange64(&g_LastHwbpRip, 0, 0);
}

const char* OakEngine_LastHwbpTag(void)
{
    return g_LastHwbpTag;
}

static LONG CALLBACK EngHwbpVeh(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD64 dr6 = ep->ContextRecord->Dr6;
    int hitSlot = -1;
    for (int i = 0; i < kHwbpMax; i++)
    {
        if ((dr6 & (1ULL << i)) && g_Hwbp[i].used)
        {
            hitSlot = i;
            break;
        }
    }
    if (hitSlot < 0)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    InterlockedExchange64(&g_LastHwbpRip, (LONG64)rip);
    lstrcpynA(g_LastHwbpTag, g_Hwbp[hitSlot].tag, 32);
    LONG n = InterlockedIncrement(&g_Hwbp[hitSlot].hits);

    char line[220];
    wsprintfA(line, "engine[hwbp] HIT tag=%s addr=%p rip=%p hits=%d tid=%u %s",
        g_Hwbp[hitSlot].tag, (void*)g_Hwbp[hitSlot].addr, (void*)rip, (int)n,
        GetCurrentThreadId(),
        EngInGameImage(rip) ? "GAME-THREAD" : "other-module");
    EngLog(line);

    // GPRs at write — rcx often = object base for [rcx+disp] stores
    {
        CONTEXT* c = ep->ContextRecord;
        char r[220];
        wsprintfA(r, "engine[hwbp-reg] rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p r8=%p r9=%p",
            (void*)c->Rax, (void*)c->Rbx, (void*)c->Rcx, (void*)c->Rdx,
            (void*)c->Rsi, (void*)c->Rdi, (void*)c->R8, (void*)c->R9);
        EngLog(r);
        // XMM0 low float (common movss source)
        float x0 = 0.f;
        memcpy(&x0, &c->Xmm0, sizeof(float));
        int x0k = (int)(x0 * 1000.f);
        char xf[80];
        wsprintfA(xf, "engine[hwbp-xmm] xmm0x1000=%d", x0k);
        EngLog(xf);
    }

    // Real stack of the writing thread
    void* frames[10];
    USHORT nf = CaptureStackBackTrace(0, 10, frames, nullptr);
    for (USHORT i = 0; i < nf; i++)
    {
        char s[96];
        const char* where = EngInGameImage((uintptr_t)frames[i]) ? "game" : "other";
        wsprintfA(s, "engine[hwbp-stack] #%u %p (%s)", (unsigned)i, frames[i], where);
        EngLog(s);
    }

    // Dump 64 bytes before / 128 after the watched field for structure rebuild
    OakEngine_DumpAround(g_Hwbp[hitSlot].addr, 64, 128, g_Hwbp[hitSlot].tag);

    if (g_Hwbp[hitSlot].oneshot)
    {
        g_Hwbp[hitSlot].used = 0;
        EngLog("engine[hwbp] oneshot cleared — re-arm after analyzing dump");
    }

    // Clear Dr6 hit bits; rebuild Dr7 from remaining slots into this thread's context
    ep->ContextRecord->Dr6 = 0;
    EngHwbpFillContext(ep->ContextRecord);
    // Resume without re-firing immediately
    ep->ContextRecord->EFlags |= 0x10000; // RF

    return EXCEPTION_CONTINUE_EXECUTION;
}

static void EngHwbpEnsureVeh(void)
{
    if (g_HwbpVeh)
        return;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto add = k32
        ? (PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER))GetProcAddress(k32, "AddVectoredExceptionHandler")
        : nullptr;
    if (!add)
    {
        EngLog("engine[hwbp] AddVectoredExceptionHandler missing");
        return;
    }
    g_HwbpVeh = add(1, EngHwbpVeh); // first — ahead of crash-hunt
    EngLog(g_HwbpVeh ? "engine[hwbp] VEH installed" : "engine[hwbp] VEH install failed");
}

int OakEngine_HwbpCount(void)
{
    int n = 0;
    for (int i = 0; i < kHwbpMax; i++)
        if (g_Hwbp[i].used) n++;
    return n;
}

int OakEngine_HwbpWatch(uintptr_t addr, unsigned size, const char* tag, int oneshot)
{
    if (!addr || addr < 0x10000)
        return -1;
    if (!(size == 1 || size == 2 || size == 4 || size == 8))
        size = 4;

    EngHwbpEnsureVeh();
    if (!g_HwbpVeh)
        return -1;

    if (!g_HwbpCsReady)
    {
        InitializeCriticalSection(&g_HwbpCs);
        InterlockedExchange(&g_HwbpCsReady, 1);
    }
    EnterCriticalSection(&g_HwbpCs);

    int slot = -1;
    for (int i = 0; i < kHwbpMax; i++)
    {
        if (!g_Hwbp[i].used) { slot = i; break; }
    }
    if (slot < 0)
    {
        LeaveCriticalSection(&g_HwbpCs);
        EngLog("engine[hwbp] full (max 4)");
        return -1;
    }

    ZeroMemory(&g_Hwbp[slot], sizeof(g_Hwbp[slot]));
    g_Hwbp[slot].used = 1;
    g_Hwbp[slot].addr = addr;
    g_Hwbp[slot].size = size;
    g_Hwbp[slot].oneshot = oneshot ? 1 : 0;
    lstrcpynA(g_Hwbp[slot].tag, tag ? tag : "hwbp", 32);

    EngHwbpApplyAllThreads();
    LeaveCriticalSection(&g_HwbpCs);

    char b[180];
    wsprintfA(b, "engine[hwbp] arm slot=%d addr=%p sz=%u tag=%s oneshot=%d (all threads)",
        slot, (void*)addr, size, g_Hwbp[slot].tag, g_Hwbp[slot].oneshot);
    EngLog(b);
    return slot;
}

void OakEngine_HwbpClear(int slot)
{
    if (slot < 0 || slot >= kHwbpMax) return;
    if (g_HwbpCsReady) EnterCriticalSection(&g_HwbpCs);
    g_Hwbp[slot].used = 0;
    EngHwbpApplyAllThreads();
    if (g_HwbpCsReady) LeaveCriticalSection(&g_HwbpCs);
    char b[64];
    wsprintfA(b, "engine[hwbp] clear slot=%d", slot);
    EngLog(b);
}

void OakEngine_HwbpClearAll(void)
{
    if (g_HwbpCsReady) EnterCriticalSection(&g_HwbpCs);
    for (int i = 0; i < kHwbpMax; i++)
        g_Hwbp[i].used = 0;
    EngHwbpApplyAllThreads();
    if (g_HwbpCsReady) LeaveCriticalSection(&g_HwbpCs);
    EngLog("engine[hwbp] clear all");
}

void OakEngine_Init(HMODULE gameModule)
{
    g_GameMod = gameModule;
    g_GameBase = (uintptr_t)gameModule;
    g_GameEnd = g_GameBase;
    EngLog("engine: init enter");
    if (gameModule)
    {
        __try
        {
            auto dos = (IMAGE_DOS_HEADER*)gameModule;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE)
            {
                auto nt = (IMAGE_NT_HEADERS*)((BYTE*)gameModule + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                    g_GameEnd = g_GameBase + nt->OptionalHeader.SizeOfImage;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            EngLog("engine: PE header read fault — using base only");
        }
    }
    // Avoid bulk ZeroMemory on manual-map BSS under BattlEye (observed AV during init).
    // Slots start unused (static / loader zero); clear used flags only.
    for (int i = 0; i < kEngWatchMax; i++)
        g_Watch[i].used = 0;
    for (int i = 0; i < kHwbpMax; i++)
        g_Hwbp[i].used = 0;
    g_LastCensus[0] = 0;
    g_LastHwbpTag[0] = 0;
    InterlockedExchange64(&g_LastHwbpRip, 0);
    char b[140];
    wsprintfA(b, "engine: init base=%p end=%p (RTTI+poll-watch+HWBP ready)",
        (void*)g_GameBase, (void*)g_GameEnd);
    EngLog(b);
}

void OakEngine_Shutdown(void)
{
    OakEngine_HwbpClearAll();
    OakEngine_UnwatchAll();
    if (g_HwbpVeh)
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        auto rem = k32
            ? (ULONG(WINAPI*)(PVOID))GetProcAddress(k32, "RemoveVectoredExceptionHandler")
            : nullptr;
        if (rem) rem(g_HwbpVeh);
        g_HwbpVeh = nullptr;
    }
    if (g_HwbpCsReady)
    {
        DeleteCriticalSection(&g_HwbpCs);
        InterlockedExchange(&g_HwbpCsReady, 0);
    }
    g_GameMod = nullptr;
    g_GameBase = g_GameEnd = 0;
}

void OakEngine_OnPresent(void)
{
    EngPollWatches();

    // Re-apply HWBP to newly created threads periodically (cheap when none armed).
    static DWORD s_LastHwbpRefresh = 0;
    if (OakEngine_HwbpCount() > 0)
    {
        DWORD now = GetTickCount();
        if (!s_LastHwbpRefresh || (now - s_LastHwbpRefresh) > 2000)
        {
            EngHwbpApplyAllThreads();
            s_LastHwbpRefresh = now;
        }
    }

    // Auto-census disabled: walking local+0x40..0xA00 with RTTI NameObject on every
    // attach AVd Present (memcpy into garbage) and crashed DayZ mid-connect.
    (void)0;
}
