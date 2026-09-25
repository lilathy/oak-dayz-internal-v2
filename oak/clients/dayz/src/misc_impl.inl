// Included from main.cpp after ESP helpers.
// SAFETY RULE: only write offsets that are proven float fields (or AmmoType floats
// already used by Fast Bullets / No Dispersion). No adjacent spray, no TimeScale.

struct SteamTagCache {
    uintptr_t entity;
    int networkId;
    unsigned long long steamId;
    char name[64];
    DWORD lastOk;
    bool used;
};
static SteamTagCache g_SteamTags[64];

// Full server roster from DayZ scoreboard (Steam names + SteamIDs for everyone online).
enum { kMaxRoster = 128 };
struct ScoreboardRosterEntry {
    int networkId;
    unsigned long long steamId;
    char name[64];
    bool used;
};
static ScoreboardRosterEntry g_Roster[kMaxRoster];
static int g_RosterCount = 0;
static DWORD g_RosterRefreshTick = 0;
// Live-resolved NetworkClient (modbase::Network often points at a status-string blob).
static uintptr_t g_CachedNetworkClient = 0;
static DWORD g_CachedNetworkClientTick = 0;

static void MiscClearSteamTagCache()
{
    for (int i = 0; i < 64; i++)
    {
        g_SteamTags[i].used = false;
        g_SteamTags[i].networkId = 0;
        g_SteamTags[i].steamId = 0;
        g_SteamTags[i].entity = 0;
        g_SteamTags[i].name[0] = 0;
        g_SteamTags[i].lastOk = 0;
    }
}

// g_LocalSteamName / g_LocalSteamId live in main.cpp
// True = do NOT use this name for a REMOTE entity (callers skip clear when isLocal).
// Bug we fixed: entity+0x50 is NetworkClient — PlayerName@0xF8 is always LOCAL persona.
// Old logic accepted local name + local/missing SID, so every remote got your name.
static bool MiscNameIsFalselyLocal(const char* name, unsigned long long steamId)
{
    if (!name || !name[0] || !g_LocalSteamName[0]) return false;
    if (StrCmpI(name, g_LocalSteamName) != 0) return false;
    // Same display name as local: only OK for remotes if they have a DIFFERENT valid SID
    // (two people with identical persona names). Local SID or missing SID = polluted.
    if (g_LocalSteamId && steamId && steamId != g_LocalSteamId)
        return false;
    return true;
}

// Steamworks (already loaded by DayZ) — prefer flat SteamAPI_* exports (version-stable)
static void* g_SteamFriends = nullptr;
static void* g_SteamUtils = nullptr;
static void* g_SteamUser = nullptr;
static bool g_SteamTried = false;
static HMODULE g_SteamApi = nullptr;

// Flat export fns (resolved once)
typedef const char* (*Fn_GetPersonaName)(void*);
typedef const char* (*Fn_GetFriendPersonaName)(void*, unsigned long long);
typedef int (*Fn_GetFriendAvatar)(void*, unsigned long long);
typedef bool (*Fn_RequestUserInformation)(void*, unsigned long long, bool);
typedef bool (*Fn_GetImageSize)(void*, int, unsigned int*, unsigned int*);
typedef bool (*Fn_GetImageRGBA)(void*, int, unsigned char*, int);
typedef unsigned long long (*Fn_GetSteamID)(void*);
typedef void (*Fn_RunCallbacks)();

static Fn_GetPersonaName g_FnGetPersonaName = nullptr;
static Fn_GetFriendPersonaName g_FnGetFriendPersonaName = nullptr;
static Fn_GetFriendAvatar g_FnGetSmallAvatar = nullptr;
static Fn_GetFriendAvatar g_FnGetMediumAvatar = nullptr;
static Fn_GetFriendAvatar g_FnGetLargeAvatar = nullptr;
static Fn_RequestUserInformation g_FnRequestUserInfo = nullptr;
static Fn_GetImageSize g_FnGetImageSize = nullptr;
static Fn_GetImageRGBA g_FnGetImageRGBA = nullptr;
static Fn_GetSteamID g_FnGetSteamID = nullptr;
static Fn_RunCallbacks g_FnRunCallbacks = nullptr;

static void* SteamVCall(void* iface, int index)
{
    if (!iface) return nullptr;
    void** vt = *(void***)iface;
    return vt ? vt[index] : nullptr;
}

static void MiscInitSteamIfNeeded()
{
    if (g_SteamTried) return;
    g_SteamTried = true;
    g_SteamApi = GetModuleHandleA("steam_api64.dll");
    if (!g_SteamApi) g_SteamApi = GetModuleHandleA("steam_api.dll");
    if (!g_SteamApi)
    {
        Log("steam: steam_api64 not loaded");
        return;
    }

    g_FnGetPersonaName = (Fn_GetPersonaName)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_GetPersonaName");
    g_FnGetFriendPersonaName = (Fn_GetFriendPersonaName)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    g_FnGetSmallAvatar = (Fn_GetFriendAvatar)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_GetSmallFriendAvatar");
    g_FnGetMediumAvatar = (Fn_GetFriendAvatar)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    g_FnGetLargeAvatar = (Fn_GetFriendAvatar)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_GetLargeFriendAvatar");
    g_FnRequestUserInfo = (Fn_RequestUserInformation)GetProcAddress(g_SteamApi, "SteamAPI_ISteamFriends_RequestUserInformation");
    g_FnGetImageSize = (Fn_GetImageSize)GetProcAddress(g_SteamApi, "SteamAPI_ISteamUtils_GetImageSize");
    g_FnGetImageRGBA = (Fn_GetImageRGBA)GetProcAddress(g_SteamApi, "SteamAPI_ISteamUtils_GetImageRGBA");
    g_FnGetSteamID = (Fn_GetSteamID)GetProcAddress(g_SteamApi, "SteamAPI_ISteamUser_GetSteamID");
    g_FnRunCallbacks = (Fn_RunCallbacks)GetProcAddress(g_SteamApi, "SteamAPI_RunCallbacks");

    typedef int (*GetHSteamUserFn)();
    typedef void* (*FindIfaceFn)(int, const char*);
    auto getUser = (GetHSteamUserFn)GetProcAddress(g_SteamApi, "SteamAPI_GetHSteamUser");
    auto findIface = (FindIfaceFn)GetProcAddress(g_SteamApi, "SteamInternal_FindOrCreateUserInterface");
    if (!getUser || !findIface)
    {
        Log("steam: missing SteamInternal exports");
        return;
    }
    int hUser = getUser();
    if (!hUser)
    {
        Log("steam: GetHSteamUser=0");
        return;
    }

    const char* friendsVers[] = {
        "SteamFriends019", "SteamFriends018", "SteamFriends017", "SteamFriends015", "SteamFriends014", nullptr
    };
    for (int i = 0; friendsVers[i]; i++)
    {
        g_SteamFriends = findIface(hUser, friendsVers[i]);
        if (g_SteamFriends) break;
    }
    const char* utilsVers[] = {
        "SteamUtils010", "SteamUtils009", "SteamUtils008", "SteamUtils007", nullptr
    };
    for (int i = 0; utilsVers[i]; i++)
    {
        g_SteamUtils = findIface(hUser, utilsVers[i]);
        if (g_SteamUtils) break;
    }
    const char* userVers[] = {
        "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020", "SteamUser019", "SteamUser018", nullptr
    };
    for (int i = 0; userVers[i]; i++)
    {
        g_SteamUser = findIface(hUser, userVers[i]);
        if (g_SteamUser) break;
    }

    char b[160];
    wsprintfA(b, "steam: friends=%p utils=%p user=%p flatPersona=%d flatAvatar=%d flatReq=%d",
        g_SteamFriends, g_SteamUtils, g_SteamUser,
        g_FnGetPersonaName ? 1 : 0,
        (g_FnGetMediumAvatar || g_FnGetSmallAvatar) ? 1 : 0,
        g_FnRequestUserInfo ? 1 : 0);
    Log(b);
}

static bool MiscSteamIdLooksValid(unsigned long long sid)
{
    // Universe public (1) account type individual (1) — SteamID64 range for users
    return sid > 76561197960265728ULL && sid < 80000000000000000ULL;
}

static bool MiscSteamNameLooksValid(const char* n)
{
    if (!n || !n[0]) return false;
    if (n[0] == '?' && !n[1]) return false;
    if (n[0] == ' ') return false;
    if (StrContainsI(n, "Survivor")) return false;
    // Engine/shader junk seen in live roster dumps
    if (StrContainsI(n, "Colorization") || StrContainsI(n, "HeatHaze") ||
        StrContainsI(n, "texGen") || StrContainsI(n, "mainLight") ||
        StrContainsI(n, "TreeSN") || StrContainsI(n, "NonTL") ||
        StrCmpI(n, "explicit") == 0 || StrCmpI(n, "Super") == 0)
        return false;
    int len = lstrlenA(n);
    if (len < 2 || len > 63) return false;
    int printable = 0;
    bool hasSep = false;
    bool sawLower = false;
    bool camel = false;
    for (int i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)n[i];
        if (c < 32 || c == 127) return false;
        if (c == ' ' || c == '_' || c == '-' || c == '.' || c == '[' || c == ']')
            hasSep = true;
        if (c >= 'a' && c <= 'z') sawLower = true;
        if (sawLower && c >= 'A' && c <= 'Z') camel = true;
        if (c >= 33) printable++;
    }
    // Reject camelCase identifiers with no separators (treeColorization, mainLight)
    if (camel && !hasSep) return false;
    return printable >= 2;
}

static unsigned long long MiscReadIdentitySteamId(uintptr_t ident)
{
    if (!IsValidPtr(ident) || ident < 0x100000000) return 0;
    const uintptr_t offs[] = {
        oak_offsets::scoreboard_identity::SteamId, // 0xA0
        0x98, 0xA8, 0x88, 0x90, 0xB0, 0xB8, 0xC0, 0x70, 0x78, 0x80, 0xC8, 0xD0
    };
    for (int i = 0; i < 13; i++)
    {
        unsigned long long sid = Read<unsigned long long>(ident + offs[i]);
        if (MiscSteamIdLooksValid(sid))
            return sid;
    }
    return 0;
}

static void MiscSteamRequestUserInfo(unsigned long long steamId)
{
    if (!MiscSteamIdLooksValid(steamId) || !g_SteamFriends) return;
    // Short settle after inject before Steam network fetch
    static DWORD s_ReqWarm = 0;
    if (!s_ReqWarm) s_ReqWarm = GetTickCount();
    if ((GetTickCount() - s_ReqWarm) < 2500)
        return;

    static struct { unsigned long long sid; DWORD tick; } s_Req[64];
    DWORD now = GetTickCount();
    for (int i = 0; i < 64; i++)
    {
        if (s_Req[i].sid == steamId && (now - s_Req[i].tick) < 15000)
            return;
    }
    for (int i = 0; i < 64; i++)
    {
        if (s_Req[i].sid == 0 || s_Req[i].sid == steamId || (now - s_Req[i].tick) > 60000)
        {
            s_Req[i].sid = steamId;
            s_Req[i].tick = now;
            break;
        }
    }
    // bRequireNameOnly=false → also download avatar
    if (g_FnRequestUserInfo)
    {
        __try { g_FnRequestUserInfo(g_SteamFriends, steamId, false); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    typedef bool (*ReqFn)(void*, unsigned long long, bool);
    for (int idx = 36; idx <= 42; idx++)
    {
        auto fn = (ReqFn)SteamVCall(g_SteamFriends, idx);
        if (!fn) continue;
        bool called = false;
        __try { fn(g_SteamFriends, steamId, false); called = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { called = false; }
        if (called) break;
    }
}

// HTTP-downloaded Steam avatar RGBA (filled on harvest thread; Present uploads to D3D once).
enum { kHttpAvatars = 32 };
struct HttpAvatarRgba {
    unsigned long long sid;
    unsigned char* rgba;
    unsigned w;
    unsigned h;
    DWORD tick;
    bool ready;
    bool failed;
    bool gpuDone; // session: already uploaded to D3D — never re-download
    char url[256];
};
static HttpAvatarRgba g_HttpAvatars[kHttpAvatars];

static void MiscHttpAvatarFree(HttpAvatarRgba& a)
{
    if (a.rgba) { HeapFree(GetProcessHeap(), 0, a.rgba); a.rgba = nullptr; }
    a.ready = false;
    a.w = a.h = 0;
}

static HttpAvatarRgba* MiscHttpAvatarSlot(unsigned long long sid, bool create)
{
    HttpAvatarRgba* freeSlot = nullptr;
    HttpAvatarRgba* oldest = nullptr;
    DWORD oldestTick = 0xFFFFFFFFu;
    for (int i = 0; i < kHttpAvatars; i++)
    {
        if (g_HttpAvatars[i].sid == sid) return &g_HttpAvatars[i];
        if (!g_HttpAvatars[i].sid && !freeSlot) freeSlot = &g_HttpAvatars[i];
        // Never steal a slot that already made it to GPU this session
        if (g_HttpAvatars[i].sid && !g_HttpAvatars[i].gpuDone &&
            g_HttpAvatars[i].tick < oldestTick)
        {
            oldestTick = g_HttpAvatars[i].tick;
            oldest = &g_HttpAvatars[i];
        }
    }
    if (!create) return nullptr;
    HttpAvatarRgba* s = freeSlot ? freeSlot : oldest;
    if (!s) return nullptr;
    MiscHttpAvatarFree(*s);
    ZeroMemory(s, sizeof(*s));
    s->sid = sid;
    s->tick = GetTickCount();
    return s;
}

static bool MiscHttpDownloadUrl(const char* url, unsigned char** outBuf, DWORD* outLen)
{
    if (!url || !url[0] || !outBuf || !outLen) return false;
    *outBuf = nullptr;
    *outLen = 0;
    // Expect https://host/path
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)
        return false;
    bool https = (strncmp(url, "https://", 8) == 0);
    const char* hostStart = url + (https ? 8 : 7);
    const char* path = strchr(hostStart, '/');
    if (!path) return false;
    char host[128];
    int hostLen = (int)(path - hostStart);
    if (hostLen < 1 || hostLen >= 128) return false;
    CopyMemory(host, hostStart, (size_t)hostLen);
    host[hostLen] = 0;

    wchar_t whost[128];
    wchar_t wpath[384];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 128);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 384);

    bool ok = false;
    HINTERNET hSess = nullptr, hConn = nullptr, hReq = nullptr;
    unsigned char* buf = nullptr;
    DWORD cap = 0, total = 0;
    __try
    {
        hSess = WinHttpOpen(L"OakAvatar/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSess) __leave;
        WinHttpSetTimeouts(hSess, 3000, 3000, 5000, 5000);
        hConn = WinHttpConnect(hSess, whost, https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
        if (!hConn) __leave;
        hReq = WinHttpOpenRequest(hConn, L"GET", wpath, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!hReq) __leave;
        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            __leave;
        if (!WinHttpReceiveResponse(hReq, nullptr)) __leave;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
            if (total + avail > 512 * 1024) break; // avatar cap
            if (total + avail > cap)
            {
                DWORD ncap = cap ? cap * 2 : 16384;
                while (ncap < total + avail) ncap *= 2;
                unsigned char* nb = buf
                    ? (unsigned char*)HeapReAlloc(GetProcessHeap(), 0, buf, ncap)
                    : (unsigned char*)HeapAlloc(GetProcessHeap(), 0, ncap);
                if (!nb) break;
                buf = nb;
                cap = ncap;
            }
            DWORD got = 0;
            if (!WinHttpReadData(hReq, buf + total, avail, &got) || got == 0) break;
            total += got;
        }
        if (total > 64)
        {
            *outBuf = buf;
            *outLen = total;
            buf = nullptr;
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static bool MiscWicDecodeToRgba(const unsigned char* data, DWORD len, unsigned char** outRgba, unsigned* outW, unsigned* outH)
{
    if (!data || len < 16 || !outRgba || !outW || !outH) return false;
    *outRgba = nullptr;
    *outW = *outH = 0;
    static LONG s_Com = 0;
    if (InterlockedCompareExchange(&s_Com, 1, 0) == 0)
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    __try
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) || !factory)
            __leave;
        if (FAILED(factory->CreateStream(&stream)) || !stream) __leave;
        if (FAILED(stream->InitializeFromMemory((BYTE*)data, len))) __leave;
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
            __leave;
        if (FAILED(decoder->GetFrame(0, &frame)) || !frame) __leave;
        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 256 || h > 256) __leave;
        if (FAILED(factory->CreateFormatConverter(&conv)) || !conv) __leave;
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            __leave;
        const UINT bytes = w * h * 4;
        unsigned char* rgba = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!rgba) __leave;
        if (FAILED(conv->CopyPixels(nullptr, w * 4, bytes, rgba)))
        {
            HeapFree(GetProcessHeap(), 0, rgba);
            __leave;
        }
        *outRgba = rgba;
        *outW = w;
        *outH = h;
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

static bool MiscSteamAvatarGpuReady(unsigned long long sid); // defined with D3D cache below

static void MiscHttpAvatarEnsure(unsigned long long sid, const char* url)
{
    if (!MiscSteamIdLooksValid(sid) || !url || !url[0]) return;
    // Session cache: already on GPU — never touch the network again for this SteamID.
    if (MiscSteamAvatarGpuReady(sid)) return;
    HttpAvatarRgba* slot = MiscHttpAvatarSlot(sid, true);
    if (!slot) return;
    if (slot->gpuDone) return;
    if (slot->ready && slot->rgba) return;
    if (slot->failed && (GetTickCount() - slot->tick) < 60000) return;
    lstrcpynA(slot->url, url, 256);
    slot->tick = GetTickCount();

    unsigned char* jpg = nullptr;
    DWORD jpgLen = 0;
    if (!MiscHttpDownloadUrl(url, &jpg, &jpgLen))
    {
        slot->failed = true;
        return;
    }
    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    bool ok = MiscWicDecodeToRgba(jpg, jpgLen, &rgba, &w, &h);
    HeapFree(GetProcessHeap(), 0, jpg);
    if (!ok || !rgba)
    {
        slot->failed = true;
        return;
    }
    MiscHttpAvatarFree(*slot);
    slot->sid = sid;
    slot->rgba = rgba;
    slot->w = w;
    slot->h = h;
    slot->ready = true;
    slot->failed = false;
    slot->gpuDone = false;
    slot->tick = GetTickCount();
    char b[128];
    wsprintfA(b, "names: http avatar sid_lo=%u %ux%u", (unsigned)(sid & 0xFFFFFFFFu), w, h);
    Log(b);
}

static bool MiscXmlCdataTag(const char* body, const char* tag, char* out, int outMax)
{
    if (!body || !tag || !out || outMax < 2) return false;
    out[0] = 0;
    char open[48];
    wsprintfA(open, "<%s>", tag);
    const char* p = strstr(body, open);
    if (!p) return false;
    p += lstrlenA(open);
    if (strncmp(p, "<![CDATA[", 9) == 0) p += 9;
    char close1[48];
    wsprintfA(close1, "</%s>", tag);
    const char* end = strstr(p, "]]>");
    const char* end2 = strstr(p, close1);
    if (!end || (end2 && end2 < end)) end = end2;
    if (!end || end <= p) return false;
    int n = (int)(end - p);
    if (n >= outMax) n = outMax - 1;
    CopyMemory(out, p, (size_t)n);
    out[n] = 0;
    return out[0] != 0;
}

static bool MiscSteamHttpPersona(unsigned long long steamId, char* out, int outMax, bool allowNetwork)
{
    if (!out || outMax < 2 || !MiscSteamIdLooksValid(steamId)) return false;
    out[0] = 0;
    // Throttle HTTP lookups
    static struct { unsigned long long sid; DWORD tick; char name[64]; char avatarUrl[256]; } s_Http[32];
    DWORD now = GetTickCount();
    for (int i = 0; i < 32; i++)
    {
        if (s_Http[i].sid == steamId && s_Http[i].name[0] && (now - s_Http[i].tick) < 300000)
        {
            lstrcpynA(out, s_Http[i].name, outMax);
            // Only fetch JPEG once per session — GPU cache / gpuDone skips repeats.
            if (allowNetwork && s_Http[i].avatarUrl[0] && !MiscSteamAvatarGpuReady(steamId))
                MiscHttpAvatarEnsure(steamId, s_Http[i].avatarUrl);
            return true;
        }
        if (s_Http[i].sid == steamId && (now - s_Http[i].tick) < 20000)
            return false; // recently failed / in-flight
    }
    if (!allowNetwork)
        return false;

    bool ok = false;
    char avatarUrl[256] = {};
    HINTERNET hSess = nullptr, hConn = nullptr, hReq = nullptr;
    __try
    {
        hSess = WinHttpOpen(L"OakNames/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSess) __leave;
        WinHttpSetTimeouts(hSess, 3000, 3000, 5000, 5000);
        hConn = WinHttpConnect(hSess, L"steamcommunity.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConn) __leave;
        wchar_t path[96];
        wsprintfW(path, L"/profiles/%I64u/?xml=1", steamId);
        hReq = WinHttpOpenRequest(hConn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hReq) __leave;
        if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            __leave;
        if (!WinHttpReceiveResponse(hReq, nullptr)) __leave;

        char body[8192];
        int total = 0;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
            if (total + (int)avail >= (int)sizeof(body) - 1) avail = (DWORD)(sizeof(body) - 1 - total);
            if (avail == 0) break;
            DWORD got = 0;
            if (!WinHttpReadData(hReq, body + total, avail, &got) || got == 0) break;
            total += (int)got;
            if (total >= (int)sizeof(body) - 1) break;
        }
        body[total] = 0;

        char tmp[64] = {};
        if (MiscXmlCdataTag(body, "steamID", tmp, 64) &&
            MiscSteamNameLooksValid(tmp) && !MiscNameIsFalselyLocal(tmp, steamId))
        {
            lstrcpynA(out, tmp, outMax);
            ok = true;
        }
        if (!MiscXmlCdataTag(body, "avatarMedium", avatarUrl, 256))
            MiscXmlCdataTag(body, "avatarIcon", avatarUrl, 256);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

    if (hReq) WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    if (hSess) WinHttpCloseHandle(hSess);

    for (int i = 0; i < 32; i++)
    {
        if (s_Http[i].sid == 0 || s_Http[i].sid == steamId || (now - s_Http[i].tick) > 300000)
        {
            s_Http[i].sid = steamId;
            s_Http[i].tick = now;
            s_Http[i].name[0] = 0;
            s_Http[i].avatarUrl[0] = 0;
            if (ok) lstrcpynA(s_Http[i].name, out, 64);
            if (avatarUrl[0]) lstrcpynA(s_Http[i].avatarUrl, avatarUrl, 256);
            break;
        }
    }
    if (ok)
    {
        char b[128];
        wsprintfA(b, "names: http persona sid_lo=%u name='%s'",
            (unsigned)(steamId & 0xFFFFFFFFu), out);
        Log(b);
    }
    if (avatarUrl[0])
        MiscHttpAvatarEnsure(steamId, avatarUrl);
    return ok;
}

static bool MiscSteamPersonaNameEx(unsigned long long steamId, char* out, int outMax, bool allowHttp);

static bool MiscSteamPersonaName(unsigned long long steamId, char* out, int outMax)
{
    return MiscSteamPersonaNameEx(steamId, out, outMax, false);
}

static bool MiscSteamPersonaNameEx(unsigned long long steamId, char* out, int outMax, bool allowHttp)
{
    if (!out || outMax < 2 || !MiscSteamIdLooksValid(steamId)) return false;
    out[0] = 0;
    MiscInitSteamIfNeeded();
    if (!g_SteamFriends) return false;

    // Pump Steam callbacks so RequestUserInformation can populate persona cache.
    if (g_FnRunCallbacks)
    {
        __try { g_FnRunCallbacks(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Prefer flat export only — wrong vtable slots often call GetPersonaName (local) and
    // stamp Mister_Hacker_999 onto every player.
    if (g_FnGetFriendPersonaName)
    {
        const char* n = nullptr;
        __try { n = g_FnGetFriendPersonaName(g_SteamFriends, steamId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = nullptr; }
        if (MiscSteamNameLooksValid(n) && !MiscNameIsFalselyLocal(n, steamId))
        {
            lstrcpynA(out, n, outMax);
            return true;
        }
        // Non-friends often return empty until RequestUserInformation + RunCallbacks
        if (!MiscSteamNameLooksValid(n) || MiscNameIsFalselyLocal(n, steamId))
            MiscSteamRequestUserInfo(steamId);
        if (g_FnRunCallbacks)
        {
            __try { g_FnRunCallbacks(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        __try { n = g_FnGetFriendPersonaName(g_SteamFriends, steamId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = nullptr; }
        if (MiscSteamNameLooksValid(n) && !MiscNameIsFalselyLocal(n, steamId))
        {
            lstrcpynA(out, n, outMax);
            return true;
        }
    }
    else
    {
        MiscSteamRequestUserInfo(steamId);
    }
    // HTTP network only on harvest thread. Present may read HTTP cache only.
    if (MiscSteamHttpPersona(steamId, out, outMax, allowHttp))
        return true;
    return false;
}

static bool MiscSteamLocalPersona(char* out, int outMax, unsigned long long* sidOut)
{
    if (out) out[0] = 0;
    if (sidOut) *sidOut = 0;
    MiscInitSteamIfNeeded();

    // Local SteamID first
    if (sidOut && g_SteamUser)
    {
        if (g_FnGetSteamID)
        {
            unsigned long long sid = 0;
            __try { sid = g_FnGetSteamID(g_SteamUser); }
            __except (EXCEPTION_EXECUTE_HANDLER) { sid = 0; }
            if (MiscSteamIdLooksValid(sid))
                *sidOut = sid;
        }
        if (!*sidOut)
        {
            typedef unsigned long long (*GetSteamIDFn)(void*);
            for (int idx = 0; idx <= 3; idx++)
            {
                auto fn = (GetSteamIDFn)SteamVCall(g_SteamUser, idx);
                if (!fn) continue;
                unsigned long long sid = 0;
                __try { sid = fn(g_SteamUser); }
                __except (EXCEPTION_EXECUTE_HANDLER) { sid = 0; }
                if (MiscSteamIdLooksValid(sid)) { *sidOut = sid; break; }
            }
        }
    }

    // Local persona name
    if (g_SteamFriends && out)
    {
        if (g_FnGetPersonaName)
        {
            const char* n = nullptr;
            __try { n = g_FnGetPersonaName(g_SteamFriends); }
            __except (EXCEPTION_EXECUTE_HANDLER) { n = nullptr; }
            if (MiscSteamNameLooksValid(n))
                lstrcpynA(out, n, outMax);
        }
        if (!out[0])
        {
            typedef const char* (*GetPersonaNameFn)(void*);
            for (int idx = 0; idx <= 2; idx++)
            {
                auto fn = (GetPersonaNameFn)SteamVCall(g_SteamFriends, idx);
                if (!fn) continue;
                const char* n = nullptr;
                __try { n = fn(g_SteamFriends); }
                __except (EXCEPTION_EXECUTE_HANDLER) { n = nullptr; }
                if (MiscSteamNameLooksValid(n))
                {
                    lstrcpynA(out, n, outMax);
                    break;
                }
            }
        }
    }

    if (sidOut && *sidOut)
    {
        // Do NOT RequestUserInformation here — calling it from Present/roster init crashed DayZ.
        if (out && (!out[0] || !MiscSteamNameLooksValid(out)))
            MiscSteamPersonaName(*sidOut, out, outMax);
    }

    return out && MiscSteamNameLooksValid(out);
}

// ---------------------------------------------------------------------------
// Remote SteamID harvest (scoreboard / NetworkClient is dead on this build).
// Live dual proof: remotes ARE in NearEntList, but GetName/owner/scoreboard
// return empty. Binary SteamID64 + PlainId "7656..." ARE in process memory.
// Background scan → Steam GetFriendPersonaName → bind to unnamed remotes.
// ---------------------------------------------------------------------------
static int MiscReadNetworkId(uintptr_t entity);
static bool MiscCacheSteamTag(uintptr_t entity, int netId, unsigned long long steamId,
    const char* name, char* outName, int outMax);
enum { kMaxOrphans = 64 };
struct OrphanSteamSlot {
    unsigned long long steamId;
    uintptr_t hitAddr;
    int networkId;
    char name[64];
    DWORD lastSeen;
    bool used;
    bool fromPlainId; // DayZ SyncPlayer UID string — prefer over Steam friend-cache binary hits
};
static OrphanSteamSlot g_Orphans[kMaxOrphans];
static int g_OrphanCount = 0;
static HANDLE g_HarvestThread = nullptr;
static volatile LONG g_HarvestStop = 0;
static volatile LONG g_HarvestGen = 0;

struct RemotePlayerSnap {
    uintptr_t entity;
    int networkId;
};
static RemotePlayerSnap g_RemoteSnap[64];
static int g_RemoteSnapCount = 0;
static volatile LONG g_RemoteSnapLock = 0;

static bool MiscRosterUpsert(int netId, unsigned long long steamId, const char* name)
{
    if (!netId && !MiscSteamIdLooksValid(steamId))
        return false;
    char nbuf[64] = {};
    if (name && MiscSteamNameLooksValid(name) && !MiscNameIsFalselyLocal(name, steamId))
        lstrcpynA(nbuf, name, 64);
    if (!nbuf[0] && MiscSteamIdLooksValid(steamId))
        MiscSteamPersonaName(steamId, nbuf, 64);
    if (MiscNameIsFalselyLocal(nbuf, steamId))
        nbuf[0] = 0;

    for (int i = 0; i < g_RosterCount; i++)
    {
        if ((netId && g_Roster[i].networkId == netId) ||
            (steamId && g_Roster[i].steamId == steamId))
        {
            if (netId) g_Roster[i].networkId = netId;
            if (steamId) g_Roster[i].steamId = steamId;
            if (nbuf[0]) lstrcpynA(g_Roster[i].name, nbuf, 64);
            g_Roster[i].used = true;
            return true;
        }
    }
    if (g_RosterCount >= kMaxRoster) return false;
    ScoreboardRosterEntry& e = g_Roster[g_RosterCount++];
    e.used = true;
    e.networkId = netId;
    e.steamId = steamId;
    e.name[0] = 0;
    if (nbuf[0]) lstrcpynA(e.name, nbuf, 64);
    return true;
}

static void MiscOrphanNote(unsigned long long sid, uintptr_t hitAddr, bool fromPlainId)
{
    if (!MiscSteamIdLooksValid(sid)) return;
    if (g_LocalSteamId && sid == g_LocalSteamId) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (g_Orphans[i].used && g_Orphans[i].steamId == sid)
        {
            g_Orphans[i].lastSeen = now;
            if (hitAddr) g_Orphans[i].hitAddr = hitAddr;
            if (fromPlainId) g_Orphans[i].fromPlainId = true;
            return;
        }
    }
    // Prefer filling slots with PlainId UIDs; binary-only floods friend cache.
    int freeIdx = -1;
    int binaryVictim = -1;
    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (!g_Orphans[i].used) { freeIdx = i; break; }
        if (!g_Orphans[i].fromPlainId && !g_Orphans[i].networkId && binaryVictim < 0)
            binaryVictim = i;
    }
    int slot = freeIdx >= 0 ? freeIdx : (fromPlainId ? binaryVictim : -1);
    if (slot < 0) return;
    g_Orphans[slot].used = true;
    g_Orphans[slot].steamId = sid;
    g_Orphans[slot].hitAddr = hitAddr;
    g_Orphans[slot].networkId = 0;
    g_Orphans[slot].name[0] = 0;
    g_Orphans[slot].lastSeen = now;
    g_Orphans[slot].fromPlainId = fromPlainId;
    if (g_OrphanCount < kMaxOrphans) g_OrphanCount++;
    InterlockedIncrement(&g_HarvestGen);
}

static void MiscOrphanTryBindNetNearHit(OrphanSteamSlot& o, const int* nets, int netN)
{
    if (!o.hitAddr || !nets || netN <= 0 || o.networkId) return;
    __try
    {
        for (int off = -0x200; off <= 0x200; off += 4)
        {
            int v = *(int*)(o.hitAddr + (uintptr_t)(intptr_t)off);
            if (v == 0) continue;
            for (int n = 0; n < netN; n++)
            {
                if (nets[n] && nets[n] == v)
                {
                    o.networkId = v;
                    return;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void MiscCollectRemotePlayersForNames()
{
    if (!g_GameModule) return;
    RemotePlayerSnap snap[64];
    int n = 0;
    uintptr_t world = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::World);
    if (!IsValidPtr(world)) return;
    const uintptr_t lists[] = {
        oak_offsets::world::NearEntList,
        oak_offsets::world::FarEntList
    };
    for (int li = 0; li < 2 && n < 64; li++)
    {
        uintptr_t data = 0; int count = 0;
        if (!ResolveEntityList(world, lists[li], 256, data, count, nullptr))
            continue;
        if (!IsValidPtr(data) || count <= 0) continue;
        int lim = count < 96 ? count : 96;
        for (int i = 0; i < lim && n < 64; i++)
        {
            uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
            if (!IsValidPtr(ent)) continue;
            uintptr_t typ = Read<uintptr_t>(ent + oak_offsets::entity::Type);
            if (!IsValidPtr(typ)) continue;
            uintptr_t cfg = Read<uintptr_t>(typ + oak_offsets::entitytype::ConfigName);
            char cfgName[32] = {};
            if (!ReadEngineString(cfg, cfgName, 32) || StrCmpI(cfgName, "dayzplayer") != 0)
                continue;
            bool isLocal = (IsValidPtr(g_ResolvedLocalPlayer) && ent == g_ResolvedLocalPlayer);
            if (!isLocal)
            {
                uintptr_t lp = Read<uintptr_t>(world + oak_offsets::world::LocalPlayer);
                if (IsValidPtr(lp) && ent == lp) isLocal = true;
            }
            if (isLocal) continue;
            int netId = MiscReadNetworkId(ent);
            if (!netId) continue;
            bool dup = false;
            for (int j = 0; j < n; j++)
                if (snap[j].networkId == netId) { dup = true; break; }
            if (dup) continue;
            snap[n].entity = ent;
            snap[n].networkId = netId;
            n++;
        }
    }
    // Publish snapshot (Present-thread writer)
    while (InterlockedCompareExchange(&g_RemoteSnapLock, 1, 0) != 0)
        Sleep(0);
    CopyMemory(g_RemoteSnap, snap, sizeof(RemotePlayerSnap) * (SIZE_T)n);
    g_RemoteSnapCount = n;
    InterlockedExchange(&g_RemoteSnapLock, 0);
}

static DWORD WINAPI MiscSteamHarvestProc(LPVOID)
{
    BYTE* buf = (BYTE*)VirtualAlloc(nullptr, 1u << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;
    while (InterlockedCompareExchange(&g_HarvestStop, 0, 0) == 0)
    {
        Sleep(2000);
        MiscInitSteamIfNeeded();
        if (!g_LocalSteamId)
        {
            char ln[64] = {};
            unsigned long long sid = 0;
            MiscSteamLocalPersona(ln, 64, &sid);
            if (sid) g_LocalSteamId = sid;
            if (ln[0] && !g_LocalSteamName[0]) lstrcpynA(g_LocalSteamName, ln, 64);
        }
        if (!g_LocalSteamId)
            continue;

        // Snapshot remote netIds for co-locate
        int nets[64]; int netN = 0;
        while (InterlockedCompareExchange(&g_RemoteSnapLock, 1, 0) != 0)
            Sleep(0);
        for (int i = 0; i < g_RemoteSnapCount && netN < 64; i++)
            nets[netN++] = g_RemoteSnap[i].networkId;
        InterlockedExchange(&g_RemoteSnapLock, 0);

        MEMORY_BASIC_INFORMATION mbi = {};
        uintptr_t addr = 0;
        int hits = 0;
        // Only full-process scan when we have remotes that still need PlainIds.
        int namedOrphans = 0;
        for (int oi = 0; oi < kMaxOrphans; oi++)
        {
            if (g_Orphans[oi].used && g_Orphans[oi].fromPlainId)
                namedOrphans++;
        }
        const bool doScan = (netN > 0) && (namedOrphans < netN);
        while (doScan && hits < 64 && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
        {
            uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            DWORD prot = mbi.Protect & 0xFF;
            // Prefer private RW data — skip execute pages (code/JIT races with CRT memcpy).
            bool readable = mbi.State == MEM_COMMIT &&
                (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY) &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
            if (readable && mbi.RegionSize > 0 && mbi.RegionSize < 0x4000000ULL &&
                mbi.Type == MEM_PRIVATE)
            {
                BYTE* base = (BYTE*)mbi.BaseAddress;
                size_t left = (size_t)mbi.RegionSize;
                size_t off = 0;
                while (off + 8 < left && hits < 64)
                {
                    size_t chunk = left - off;
                    // Small chunks shrink the unmap race window vs a 1MB memcpy.
                    if (chunk > (64u * 1024u)) chunk = 64u * 1024u;
                    SIZE_T got = 0;
                    // RPM returns false on bad/guard pages without raising — no VEH spam.
                    if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf, chunk, &got) || got < 17)
                    {
                        off += chunk;
                        continue;
                    }
                    chunk = (size_t)got;

                    // ONLY PlainId ASCII ("7656119...") — binary SteamID scan floods junk
                    // and never yields GetFriendPersonaName. External dual probe proved
                    // remote PlainId is present (plainB=2) while display name is not.
                    for (size_t i = 0; i + 17 <= chunk && hits < 64; i++)
                    {
                        if (buf[i] != '7' || buf[i + 1] != '6' || buf[i + 2] != '5' ||
                            buf[i + 3] != '6' || buf[i + 4] != '1' || buf[i + 5] != '1' ||
                            buf[i + 6] != '9')
                            continue;
                        bool digits = true;
                        for (int d = 0; d < 17; d++)
                        {
                            if (buf[i + d] < '0' || buf[i + d] > '9') { digits = false; break; }
                        }
                        if (!digits) continue;
                        char tmp[20] = {};
                        CopyMemory(tmp, buf + i, 17);
                        unsigned long long sid = _strtoui64(tmp, nullptr, 10);
                        if (!MiscSteamIdLooksValid(sid) || sid == g_LocalSteamId) continue;
                        MiscOrphanNote(sid, (uintptr_t)(base + off + i), true);
                        hits++;
                        i += 16;
                    }
                    off += (chunk > 8) ? (chunk - 8) : chunk;
                }
            }
            if (next <= addr) break;
            addr = next;
            if (InterlockedCompareExchange(&g_HarvestStop, 0, 0) != 0) break;
        }

        // Resolve personas + co-locate networkIds (off Present thread — safe for RequestUserInfo)
        for (int i = 0; i < kMaxOrphans; i++)
        {
            if (!g_Orphans[i].used) continue;
            if ((GetTickCount() - g_Orphans[i].lastSeen) > 120000)
            {
                ZeroMemory(&g_Orphans[i], sizeof(g_Orphans[i]));
                continue;
            }
            MiscOrphanTryBindNetNearHit(g_Orphans[i], nets, netN);
            if (!MiscSteamNameLooksValid(g_Orphans[i].name))
            {
                char nm[64] = {};
                if (MiscSteamPersonaNameEx(g_Orphans[i].steamId, nm, 64, true) && nm[0])
                    lstrcpynA(g_Orphans[i].name, nm, 64);
            }
        }

        static int s_Log = 0;
        if ((s_Log++ % 8) == 0)
        {
            int alive = 0, named = 0, plain = 0;
            for (int i = 0; i < kMaxOrphans; i++)
            {
                if (!g_Orphans[i].used) continue;
                alive++;
                if (g_Orphans[i].fromPlainId) plain++;
                if (g_Orphans[i].name[0]) named++;
            }
            char b[160];
            wsprintfA(b, "names: harvest orphans=%d plain=%d named=%d remotes=%d hits=%d",
                alive, plain, named, netN, hits);
            Log(b);
        }
        Sleep(4000);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return 0;
}

static void MiscEnsureSteamHarvest()
{
    if (!g_MiscSteamAvatars)
    {
        InterlockedExchange(&g_HarvestStop, 1);
        if (g_HarvestThread)
        {
            DWORD code = 0;
            if (GetExitCodeThread(g_HarvestThread, &code) && code != STILL_ACTIVE)
            {
                CloseHandle(g_HarvestThread);
                g_HarvestThread = nullptr;
                InterlockedExchange(&g_HarvestStop, 0);
            }
        }
        return;
    }
    if (g_HarvestThread)
    {
        DWORD code = 0;
        if (GetExitCodeThread(g_HarvestThread, &code) && code != STILL_ACTIVE)
        {
            CloseHandle(g_HarvestThread);
            g_HarvestThread = nullptr;
            InterlockedExchange(&g_HarvestStop, 0);
        }
        else
            return;
    }
    InterlockedExchange(&g_HarvestStop, 0);
    HANDLE th = CreateThread(nullptr, 0, MiscSteamHarvestProc, nullptr, 0, nullptr);
    if (th)
    {
        g_HarvestThread = th;
        SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
        Log("names: steam harvest thread started");
    }
}

static void MiscPromoteOrphanSteams()
{
    MiscEnsureSteamHarvest();
    MiscCollectRemotePlayersForNames();

    RemotePlayerSnap rem[64];
    int remN = 0;
    while (InterlockedCompareExchange(&g_RemoteSnapLock, 1, 0) != 0)
        Sleep(0);
    remN = g_RemoteSnapCount < 64 ? g_RemoteSnapCount : 64;
    CopyMemory(rem, g_RemoteSnap, sizeof(RemotePlayerSnap) * (SIZE_T)remN);
    InterlockedExchange(&g_RemoteSnapLock, 0);

    // Direct netId matches
    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (!g_Orphans[i].used || !g_Orphans[i].networkId) continue;
        MiscRosterUpsert(g_Orphans[i].networkId, g_Orphans[i].steamId, g_Orphans[i].name);
        if (MiscSteamNameLooksValid(g_Orphans[i].name))
        {
            for (int r = 0; r < remN; r++)
            {
                if (rem[r].networkId == g_Orphans[i].networkId)
                    MiscCacheSteamTag(rem[r].entity, rem[r].networkId,
                        g_Orphans[i].steamId, g_Orphans[i].name, nullptr, 0);
            }
        }
    }

    // 1:1 fallback — dual clients: one remote entity + one orphan SteamID
    int unboundRem = 0, unboundRemIdx = -1;
    for (int r = 0; r < remN; r++)
    {
        bool have = false;
        for (int i = 0; i < g_RosterCount; i++)
        {
            if (g_Roster[i].used && g_Roster[i].networkId == rem[r].networkId &&
                MiscSteamNameLooksValid(g_Roster[i].name) &&
                !MiscNameIsFalselyLocal(g_Roster[i].name, g_Roster[i].steamId))
            {
                have = true;
                break;
            }
        }
        if (!have)
        {
            unboundRem++;
            unboundRemIdx = r;
        }
    }
    int unboundOrphan = 0, unboundOrphanIdx = -1;
    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (!g_Orphans[i].used) continue;
        if (g_Orphans[i].networkId) continue; // already matched
        // 1:1 only trusts DayZ PlainId UIDs (not Steam friends-list binary noise)
        if (!g_Orphans[i].fromPlainId) continue;
        // Skip if this steam already has a named roster row
        bool have = false;
        for (int r = 0; r < g_RosterCount; r++)
        {
            if (g_Roster[r].used && g_Roster[r].steamId == g_Orphans[i].steamId &&
                MiscSteamNameLooksValid(g_Roster[r].name))
            {
                have = true;
                break;
            }
        }
        if (have) continue;
        unboundOrphan++;
        unboundOrphanIdx = i;
    }
    // 1:1 / N:N refresh — DayZ networkIds churn often; always re-stamp the current
    // remote entity when orphan count matches remote count.
    int plainNamed = 0;
    int plainIdx[16];
    for (int i = 0; i < kMaxOrphans && plainNamed < 16; i++)
    {
        if (!g_Orphans[i].used || !g_Orphans[i].fromPlainId) continue;
        if (!MiscSteamNameLooksValid(g_Orphans[i].name))
            MiscSteamPersonaNameEx(g_Orphans[i].steamId, g_Orphans[i].name, 64, true);
        if (!MiscSteamNameLooksValid(g_Orphans[i].name)) continue;
        plainIdx[plainNamed++] = i;
    }
    if (remN == 1 && plainNamed == 1)
    {
        OrphanSteamSlot& o = g_Orphans[plainIdx[0]];
        o.networkId = rem[0].networkId;
        o.lastSeen = GetTickCount();
        MiscRosterUpsert(o.networkId, o.steamId, o.name);
        MiscCacheSteamTag(rem[0].entity, o.networkId, o.steamId, o.name, nullptr, 0);
        static DWORD s_BindLog = 0;
        DWORD now = GetTickCount();
        if (!s_BindLog || (now - s_BindLog) > 8000)
        {
            s_BindLog = now;
            char b[160];
            wsprintfA(b, "names: 1:1 bind net=%d sid_lo=%u name='%s'",
                o.networkId, (unsigned)(o.steamId & 0xFFFFFFFFu), o.name);
            Log(b);
        }
    }
    else if (unboundRem == 1 && unboundOrphan == 1 && unboundRemIdx >= 0 && unboundOrphanIdx >= 0)
    {
        OrphanSteamSlot& o = g_Orphans[unboundOrphanIdx];
        o.networkId = rem[unboundRemIdx].networkId;
        if (!MiscSteamNameLooksValid(o.name))
            MiscSteamPersonaName(o.steamId, o.name, 64);
        MiscRosterUpsert(o.networkId, o.steamId, o.name);
        if (MiscSteamNameLooksValid(o.name))
        {
            MiscCacheSteamTag(rem[unboundRemIdx].entity, o.networkId, o.steamId, o.name, nullptr, 0);
            char b[160];
            wsprintfA(b, "names: 1:1 bind net=%d sid_lo=%u name='%s'",
                o.networkId, (unsigned)(o.steamId & 0xFFFFFFFFu), o.name);
            Log(b);
        }
    }
}

static bool MiscTryResolveFromOrphan(uintptr_t entity, int netId, char* outName, int outMax,
    unsigned long long* steamOut)
{
    if (outName) outName[0] = 0;
    if (steamOut) *steamOut = 0;
    MiscPromoteOrphanSteams();

    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (!g_Orphans[i].used) continue;
        if (netId && g_Orphans[i].networkId == netId)
        {
            if (steamOut) *steamOut = g_Orphans[i].steamId;
            if (!MiscSteamNameLooksValid(g_Orphans[i].name))
                MiscSteamPersonaNameEx(g_Orphans[i].steamId, g_Orphans[i].name, 64, false);
            if (MiscSteamNameLooksValid(g_Orphans[i].name) && outName)
            {
                lstrcpynA(outName, g_Orphans[i].name, outMax);
                g_Orphans[i].lastSeen = GetTickCount();
                return true;
            }
            return false;
        }
    }

    // Dual / single-remote: one PlainId orphan name applies to the unnamed remote even
    // when DayZ has rotated networkIds since the last bind.
    int namedOrphans = 0, last = -1;
    for (int i = 0; i < kMaxOrphans; i++)
    {
        if (!g_Orphans[i].used || !g_Orphans[i].fromPlainId) continue;
        if (!MiscSteamNameLooksValid(g_Orphans[i].name))
            MiscSteamPersonaNameEx(g_Orphans[i].steamId, g_Orphans[i].name, 64, false);
        if (!MiscSteamNameLooksValid(g_Orphans[i].name)) continue;
        namedOrphans++;
        last = i;
    }
    if (namedOrphans == 1 && last >= 0 && g_RemoteSnapCount <= 1)
    {
        if (steamOut) *steamOut = g_Orphans[last].steamId;
        if (outName) lstrcpynA(outName, g_Orphans[last].name, outMax);
        if (netId) g_Orphans[last].networkId = netId; // refresh through netId churn
        g_Orphans[last].lastSeen = GetTickCount();
        MiscRosterUpsert(netId, g_Orphans[last].steamId, g_Orphans[last].name);
        MiscCacheSteamTag(entity, netId, g_Orphans[last].steamId, g_Orphans[last].name, nullptr, 0);
        return outName && outName[0];
    }
    return false;
}

// Session Steam avatar cache (GPU). Keyed by SteamID — upload once, reuse forever this inject.
enum { kMaxAvatars = 64 };
struct SteamAvatarSlot {
    unsigned long long steamId;
    ID3D11ShaderResourceView* srv;
    ID3D11Texture2D* tex;
    int fails;
    DWORD lastTry;
    bool ready;
    bool dead;
    bool pinned; // session: never evict
};
static SteamAvatarSlot g_Avatars[kMaxAvatars];
static int g_AvatarUploadsThisFrame = 0;
static int g_AvatarGlobalFails = 0;

static void MiscAvatarRelease(SteamAvatarSlot& s)
{
    if (s.pinned && s.ready && s.srv) return; // session pin — keep
    if (s.srv) { s.srv->Release(); s.srv = nullptr; }
    if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    s.ready = false;
    s.pinned = false;
}

static void MiscAvatarBeginFrame()
{
    g_AvatarUploadsThisFrame = 0;
}

static SteamAvatarSlot* MiscAvatarFind(unsigned long long sid)
{
    if (!MiscSteamIdLooksValid(sid)) return nullptr;
    for (int i = 0; i < kMaxAvatars; i++)
        if (g_Avatars[i].steamId == sid) return &g_Avatars[i];
    return nullptr;
}

static bool MiscSteamAvatarGpuReady(unsigned long long sid)
{
    SteamAvatarSlot* s = MiscAvatarFind(sid);
    return s && s->ready && s->srv != nullptr;
}

// Lookup only — never downloads / uploads. Safe for sticky ESP every frame.
static ID3D11ShaderResourceView* MiscSteamAvatarCached(unsigned long long sid)
{
    if (!g_MiscSteamAvatars) return nullptr;
    SteamAvatarSlot* s = MiscAvatarFind(sid);
    if (s && s->ready && s->srv) return s->srv;
    return nullptr;
}

static SteamAvatarSlot* MiscAvatarAlloc(unsigned long long sid)
{
    SteamAvatarSlot* hit = MiscAvatarFind(sid);
    if (hit) return hit;
    SteamAvatarSlot* freeSlot = nullptr;
    for (int i = 0; i < kMaxAvatars; i++)
    {
        SteamAvatarSlot& s = g_Avatars[i];
        if (!s.steamId) { freeSlot = &s; break; }
    }
    if (!freeSlot)
    {
        // Prefer unpinned / not-ready victims only — never steal a session-ready avatar.
        for (int i = 0; i < kMaxAvatars; i++)
        {
            SteamAvatarSlot& s = g_Avatars[i];
            if (s.pinned || s.ready) continue;
            freeSlot = &s;
            break;
        }
    }
    if (!freeSlot) return nullptr; // table full of live session avatars
    if (freeSlot->steamId)
        MiscAvatarRelease(*freeSlot);
    ZeroMemory(freeSlot, sizeof(*freeSlot));
    freeSlot->steamId = sid;
    return freeSlot;
}

static ID3D11ShaderResourceView* MiscAvatarCommitGpu(SteamAvatarSlot* slot, unsigned long long sid,
    ID3D11Device* dev, const unsigned char* rgba, unsigned w, unsigned h)
{
    if (!slot || !dev || !rgba || w == 0 || h == 0 || w > 256 || h > 256) return nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = rgba;
    sd.SysMemPitch = w * 4;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
    if (SUCCEEDED(hr) && tex)
        hr = dev->CreateShaderResourceView(tex, nullptr, &srv);
    if (FAILED(hr) || !srv)
    {
        if (tex) tex->Release();
        if (srv) srv->Release();
        return nullptr;
    }
    slot->pinned = false;
    if (slot->srv) { slot->srv->Release(); slot->srv = nullptr; }
    if (slot->tex) { slot->tex->Release(); slot->tex = nullptr; }
    slot->tex = tex;
    slot->srv = srv;
    slot->ready = true;
    slot->pinned = true;
    slot->fails = 0;
    slot->steamId = sid;
    g_AvatarUploadsThisFrame++;
    static int s_AvOk = 0;
    if (s_AvOk++ < 8)
    {
        char b[96];
        wsprintfA(b, "names: avatar cached sid_lo=%u %ux%u",
            (unsigned)(sid & 0xFFFFFFFFu), w, h);
        Log(b);
    }
    return slot->srv;
}

static bool MiscAvatarTryFriendsUpload(SteamAvatarSlot* slot, unsigned long long sid,
    ID3D11Device* dev, ID3D11ShaderResourceView** outSrv)
{
    if (outSrv) *outSrv = nullptr;
    if (!slot || !dev) return false;
    if (!g_SteamFriends || !g_SteamUtils || !g_FnGetSmallAvatar || !g_FnGetImageSize || !g_FnGetImageRGBA)
        return false;

    int img = 0;
    unsigned w = 0, h = 0;
    __try
    {
        img = g_FnGetSmallAvatar(g_SteamFriends, sid);
        if (img <= 0)
        {
            MiscSteamRequestUserInfo(sid);
            return false;
        }
        if (!g_FnGetImageSize(g_SteamUtils, img, &w, &h) || w == 0 || h == 0 || w > 256 || h > 256)
            return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        slot->fails++;
        g_AvatarGlobalFails++;
        if (slot->fails >= 6) slot->dead = true;
        return false;
    }

    const int bytes = (int)(w * h * 4);
    unsigned char* rgba = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes);
    if (!rgba) return false;
    bool okRgba = false;
    __try { okRgba = g_FnGetImageRGBA(g_SteamUtils, img, rgba, bytes); }
    __except (EXCEPTION_EXECUTE_HANDLER) { okRgba = false; }
    if (!okRgba)
    {
        HeapFree(GetProcessHeap(), 0, rgba);
        slot->fails++;
        return false;
    }
    ID3D11ShaderResourceView* srv = MiscAvatarCommitGpu(slot, sid, dev, rgba, w, h);
    HeapFree(GetProcessHeap(), 0, rgba);
    if (!srv)
    {
        slot->fails++;
        g_AvatarGlobalFails++;
        return false;
    }
    if (outSrv) *outSrv = srv;
    return true;
}

static ID3D11ShaderResourceView* MiscSteamAvatarSrv(unsigned long long sid)
{
    if (!g_MiscSteamAvatars || !MiscSteamIdLooksValid(sid))
        return nullptr;

    // Fast path: session cache hit — same SteamID always shares this SRV.
    if (ID3D11ShaderResourceView* cached = MiscSteamAvatarCached(sid))
        return cached;

    SteamAvatarSlot* slot = MiscAvatarFind(sid);
    if (slot && slot->dead) return nullptr;

    ID3D11Device* dev = ImGuiMenu_GetD3DDevice();
    if (!dev) return nullptr;

    if (!slot)
    {
        slot = MiscAvatarAlloc(sid);
        if (!slot) return nullptr;
    }

    DWORD now = GetTickCount();
    // Soft throttle only while waiting for first upload — never after ready.
    if (slot->lastTry && (now - slot->lastTry) < 800)
        return nullptr;
    if (g_AvatarUploadsThisFrame >= 2)
        return nullptr;
    slot->lastTry = now;

    // Prefer HTTP RGBA from harvest (Friends API often empty for non-friends).
    HttpAvatarRgba* http = MiscHttpAvatarSlot(sid, false);
    if (http && http->ready && http->rgba && http->w > 0 && http->h > 0 && !http->gpuDone)
    {
        ID3D11ShaderResourceView* srv = MiscAvatarCommitGpu(slot, sid, dev, http->rgba, http->w, http->h);
        if (srv)
        {
            MiscHttpAvatarFree(*http);
            http->sid = sid;
            http->gpuDone = true;
            http->ready = true;
            http->tick = GetTickCount();
            return srv;
        }
    }
    if (http && http->gpuDone)
        return MiscSteamAvatarCached(sid);

    ID3D11ShaderResourceView* friendsSrv = nullptr;
    if (MiscAvatarTryFriendsUpload(slot, sid, dev, &friendsSrv) && friendsSrv)
    {
        HttpAvatarRgba* mark = MiscHttpAvatarSlot(sid, true);
        if (mark)
        {
            MiscHttpAvatarFree(*mark);
            mark->sid = sid;
            mark->gpuDone = true;
            mark->ready = true;
            mark->tick = GetTickCount();
        }
        return friendsSrv;
    }
    return nullptr;
}

static bool MiscReadScoreboardName(uintptr_t identity, char* out, int outMax, unsigned long long* steamOut);
static int MiscReadNetworkId(uintptr_t entity);
static bool MiscTryDayZPlayerGetName(uintptr_t entity, char* out, int outMax);
static bool MiscCacheSteamTag(uintptr_t entity, int netId, unsigned long long steamId,
    const char* name, char* outName, int outMax);

// DayZ EngString is inline (WORD len @+8, chars @+0x10). Fields may store a pointer
// to that object OR embed the EngString at the field offset.
static bool MiscReadIdentityNameField(uintptr_t identity, uintptr_t off, char* out, int outMax)
{
    if (!out || outMax < 2 || !IsValidPtr(identity)) return false;
    out[0] = 0;
    uintptr_t p = Read<uintptr_t>(identity + off);
    if (IsValidPtr(p) && p > 0x100000000 && p <= 0x00007FFFFFFFFFFFULL)
    {
        if (ReadEngineString(p, out, outMax) && out[0])
            return true;
    }
    if (ReadEngineString(identity + off, out, outMax) && out[0])
        return true;
    return false;
}

// modbase::Network currently resolves to a status-string table ("NO_COMMAND", "GAME DIR").
static bool MiscLooksLikeStatusStringBlob(uintptr_t p)
{
    if (!IsValidPtr(p) || p < 0x100000000) return true;
    char buf[12] = {};
    __try { memcpy(buf, (const void*)(p + 0x10), 11); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
    int caps = 0, und = 0, letters = 0;
    for (int i = 0; i < 11 && buf[i]; i++)
    {
        unsigned char c = (unsigned char)buf[i];
        if (c >= 'A' && c <= 'Z') { caps++; letters++; }
        else if (c >= 'a' && c <= 'z') letters++;
        else if (c == '_') und++;
        else if (c == ' ') { /* GAME DIR */ }
        else return false;
    }
    // NO_COMMAND / NO_CLIENT / GAME DIR / MISSION SELECTED
    return (caps >= 5 && und >= 1) || (caps >= 4 && und == 0 && letters >= 6);
}

static bool MiscIngestIdentity(uintptr_t ident)
{
    if (!IsValidPtr(ident) || ident < 0x100000000) return false;

    // Prefer NetworkId at 0x30; also accept 0x24 / 0x28 / 0x34 (layout drift)
    int netId = 0;
    const uintptr_t netOffs[] = {
        oak_offsets::scoreboard_identity::NetworkId, 0x24, 0x28, 0x34, 0x2C
    };
    for (int i = 0; i < 5; i++)
    {
        int v = Read<int>(ident + netOffs[i]);
        // Server session IDs can be large (seen 12M+). Reject 0 and obvious float-bit junk.
        if (v == 0) continue;
        unsigned u = (unsigned)v;
        if (u >= 0x7F000000u) continue; // float-ish / sentinel
        // Reject values that look like 4 ASCII chars (shader/config pollution)
        unsigned char b0 = (unsigned char)(u & 0xFF);
        unsigned char b1 = (unsigned char)((u >> 8) & 0xFF);
        unsigned char b2 = (unsigned char)((u >> 16) & 0xFF);
        unsigned char b3 = (unsigned char)((u >> 24) & 0xFF);
        int asc = 0;
        if (b0 >= 0x20 && b0 < 0x7F) asc++;
        if (b1 >= 0x20 && b1 < 0x7F) asc++;
        if (b2 >= 0x20 && b2 < 0x7F) asc++;
        if (b3 >= 0x20 && b3 < 0x7F) asc++;
        if (asc >= 3) continue;
        netId = v;
        break;
    }
    if (netId == 0)
        return false;

    char name[64] = {};
    unsigned long long steamId = 0;
    MiscReadScoreboardName(ident, name, 64, &steamId);
    if (!MiscSteamNameLooksValid(name))
    {
        const uintptr_t nameOffs[] = {
            oak_offsets::network::PlayerName, 0xF0,
            oak_offsets::player_identity::Name, 0x68, 0x78, 0x80, 0xE8
        };
        for (int ni = 0; ni < 7 && !MiscSteamNameLooksValid(name); ni++)
            MiscReadIdentityNameField(ident, nameOffs[ni], name, 64);
    }
    if (!MiscSteamIdLooksValid(steamId))
        steamId = MiscReadIdentitySteamId(ident);
    if (!MiscSteamIdLooksValid(steamId))
        steamId = 0;
    // Prefer SteamID when present; allow name+netId when SteamID offset drifts
    // (shader junk already rejected by MiscSteamNameLooksValid).
    if (!steamId && !MiscSteamNameLooksValid(name))
        return false;
    if (MiscNameIsFalselyLocal(name, steamId))
        name[0] = 0;
    if ((!MiscSteamNameLooksValid(name) || StrContainsI(name, "Survivor")) && steamId)
        MiscSteamPersonaName(steamId, name, 64);
    if (MiscNameIsFalselyLocal(name, steamId))
        name[0] = 0;
    // Need a usable display key: name and/or steam
    if (!MiscSteamNameLooksValid(name) && !steamId)
        return false;

    // Dedup by networkId OR steamId
    for (int i = 0; i < g_RosterCount; i++)
    {
        if (g_Roster[i].networkId == netId ||
            (steamId && g_Roster[i].steamId == steamId))
        {
            if (netId) g_Roster[i].networkId = netId;
            g_Roster[i].steamId = steamId ? steamId : g_Roster[i].steamId;
            if (MiscSteamNameLooksValid(name)) lstrcpynA(g_Roster[i].name, name, 64);
            g_Roster[i].used = true;
            return true;
        }
    }
    if (g_RosterCount >= kMaxRoster) return false;
    ScoreboardRosterEntry& e = g_Roster[g_RosterCount++];
    e.used = true;
    e.networkId = netId;
    e.steamId = steamId;
    e.name[0] = 0;
    if (MiscSteamNameLooksValid(name)) lstrcpynA(e.name, name, 64);
    return true;
}

static int MiscScoreboardBestCount(uintptr_t client)
{
    // NetworkClient identity count is typically +0x1C (SDK). Updater Network::ScoreboardSize
    // is +0x24 and often reads as a small wrong value (e.g. 1), which truncated the roster.
    const uintptr_t offs[] = {
        oak_offsets::network::IdentityCount, // 0x1C
        oak_offsets::network::ScoreboardSize, // 0x24
        0x10, 0x28, 0x20, 0x14
    };
    int best = 0;
    for (int i = 0; i < 6; i++)
    {
        int c = Read<int>(client + offs[i]);
        if (c > best && c <= 128)
            best = c;
    }
    return best;
}

static void MiscTryIngestTable(uintptr_t table, int size)
{
    if (!IsValidPtr(table) || table < 0x100000000) return;
    if (size <= 0 || size > 128) return;
    for (int i = 0; i < size; i++)
    {
        uintptr_t ident = Read<uintptr_t>(table + (uintptr_t)i * 8);
        MiscIngestIdentity(ident);
    }
}

// Current DayZ builds store PlayerIdentity as a contiguous array (UC: stride 0x160/0x170),
// not always a pointer table. Walk both layouts.
static void MiscTryIngestContiguous(uintptr_t base, int size, uintptr_t stride)
{
    if (!IsValidPtr(base) || base < 0x100000000) return;
    if (size <= 0 || size > 128) return;
    if (stride < 0x100 || stride > 0x220) return;
    for (int i = 0; i < size; i++)
        MiscIngestIdentity(base + (uintptr_t)i * stride);
}

static int MiscCountSteamHits(uintptr_t board, int count, bool contiguous, uintptr_t stride)
{
    if (!IsValidPtr(board) || board < 0x100000000) return 0;
    if (count < 1 || count > 128) return 0;
    if (contiguous && (stride < 0x100 || stride > 0x220)) return 0;
    int hits = 0;
    for (int j = 0; j < count && j < 32; j++)
    {
        uintptr_t ident = contiguous
            ? (board + (uintptr_t)j * stride)
            : Read<uintptr_t>(board + (uintptr_t)j * 8);
        if (!IsValidPtr(ident) || ident < 0x100000000) continue;
        unsigned long long sid = MiscReadIdentitySteamId(ident);
        if (MiscSteamIdLooksValid(sid))
            hits++;
    }
    return hits;
}

static void MiscIngestBoardAllLayouts(uintptr_t board, int count)
{
    if (!IsValidPtr(board) || board < 0x100000000 || count < 1) return;
    MiscTryIngestTable(board, count);
    // UC Apr 2026: PLAYERIDENTITYSIZE / stride commonly 0x170 (also 0x160 / 0x158).
    const uintptr_t strides[] = { 0x170, 0x160, 0x158, 0x168, 0x180, 0x148 };
    for (int s = 0; s < 6; s++)
        MiscTryIngestContiguous(board, count, strides[s]);
}

// Find PlayerIdentity** arrays by looking for multiple valid SteamIDs at +0xA0.
static void MiscScanRootForScoreboard(uintptr_t root, int* outHits, uintptr_t* outTable, int* outSize)
{
    *outHits = 0; *outTable = 0; *outSize = 0;
    if (!IsValidPtr(root) || root < 0x100000000) return;

    const uintptr_t strides[] = { 0, 0x170, 0x160, 0x158, 0x168, 0x180 }; // 0 = pointer table

    for (uintptr_t off = 0; off <= 0x280; off += 8)
    {
        uintptr_t table = Read<uintptr_t>(root + off);
        if (!IsValidPtr(table) || table < 0x100000000) continue;

        int sizes[6] = {
            Read<int>(root + off + 8),
            Read<int>(root + off + 4),
            Read<int>(root + 0x1C),
            Read<int>(root + 0x24),
            Read<int>(root + 0x10),
            Read<int>(root + 0x28)
        };
        for (int si = 0; si < 6; si++)
        {
            int size = sizes[si];
            if (size < 1 || size > 128) continue;
            for (int st = 0; st < 6; st++)
            {
                bool contig = strides[st] != 0;
                int steamHits = MiscCountSteamHits(table, size, contig, strides[st]);
                if (steamHits > *outHits)
                {
                    *outHits = steamHits;
                    *outTable = table;
                    *outSize = size;
                }
            }
        }
    }
}

// Resolve NetworkClient — NetworkManager RVA is often stale (reads 0/garbage).
// UC: NetworkManager and Network share 0x100FBD0; client = *(net+0x50).
static uintptr_t MiscFindNetworkClient(uintptr_t net, uintptr_t nm)
{
    uintptr_t cands[32];
    int n = 0;
    auto add = [&](uintptr_t p) {
        if (!IsValidPtr(p) || p < 0x100000000 || p > 0x00007FFFFFFFFFFFULL) return;
        for (int i = 0; i < n; i++) if (cands[i] == p) return;
        if (n < 32) cands[n++] = p;
    };

    // Prefer Network pointer as the manager when NM RVA is dead (live logs: nm=0 / float junk).
    if (IsValidPtr(net) && net > 0x100000000)
    {
        add(net);
        add(Read<uintptr_t>(net + oak_offsets::network::ManagerNetworkClient)); // +0x50
        for (uintptr_t off = 0x40; off <= 0xC0; off += 8)
            add(Read<uintptr_t>(net + off));
    }
    if (IsValidPtr(nm) && nm > 0x100000000)
    {
        add(nm);
        add(Read<uintptr_t>(nm + oak_offsets::network::ManagerNetworkClient));
        for (uintptr_t off = 0x40; off <= 0xC0; off += 8)
            add(Read<uintptr_t>(nm + off));
    }

    uintptr_t best = 0;
    int bestHits = -1;
    int bestCount = 0;
    const uintptr_t strides[] = { 0, 0x170, 0x160, 0x158, 0x168, 0x180 };
    const uintptr_t boardOffs[] = {
        oak_offsets::network::ScoreboardPtr, 0x10, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x08
    };

    for (int i = 0; i < n; i++)
    {
        uintptr_t c = cands[i];
        int count = MiscScoreboardBestCount(c);
        if (count < 1) count = Read<int>(c + oak_offsets::network::IdentityCount);
        if (count < 1) count = Read<int>(c + oak_offsets::network::ScoreboardSize);
        if (count < 1 || count > 128) count = 1; // still probe solo

        for (int bi = 0; bi < 9; bi++)
        {
            uintptr_t board = Read<uintptr_t>(c + boardOffs[bi]);
            if (!IsValidPtr(board) || board < 0x100000000) continue;
            int useCount = count;
            int szPair = Read<int>(c + boardOffs[bi] + 8);
            if (szPair > useCount && szPair <= 128) useCount = szPair;

            for (int st = 0; st < 6; st++)
            {
                bool contig = strides[st] != 0;
                int hits = MiscCountSteamHits(board, useCount, contig, strides[st]);
                // Also accept EngString name hits when SteamID not yet filled
                if (hits == 0 && useCount >= 1)
                {
                    for (int j = 0; j < useCount && j < 8; j++)
                    {
                        uintptr_t ident = contig
                            ? (board + (uintptr_t)j * strides[st])
                            : Read<uintptr_t>(board + (uintptr_t)j * 8);
                        if (!IsValidPtr(ident) || ident < 0x100000000) continue;
                        if (MiscSteamIdLooksValid(MiscReadIdentitySteamId(ident)))
                            hits++;
                    }
                }
                if (hits > bestHits || (hits == bestHits && useCount > bestCount))
                {
                    bestHits = hits;
                    bestCount = useCount;
                    best = c;
                }
            }
        }
    }
    return best;
}

// Primary roster path matching steamthing.txt / common external cheats.
// Live log showed *(client+0x18)=0xBA on current build — scan for the real board.
static int MiscBoardNameHits(uintptr_t board, bool contiguous, uintptr_t stride, int slots)
{
    if (!IsValidPtr(board) || board < 0x100000000 || board > 0x00007FFFFFFFFFFFULL)
        return 0;
    if (slots < 1) slots = 1;
    if (slots > 32) slots = 32;
    if (contiguous && (stride < 0x100 || stride > 0x220)) return 0;
    int hits = 0;
    for (int i = 0; i < slots; i++)
    {
        uintptr_t ident = contiguous
            ? (board + (uintptr_t)i * stride)
            : Read<uintptr_t>(board + (uintptr_t)i * sizeof(uintptr_t));
        if (!IsValidPtr(ident) || ident < 0x100000000 || ident > 0x00007FFFFFFFFFFFULL)
            continue;
        int netId = Read<int>(ident + oak_offsets::scoreboard_identity::NetworkId);
        if (netId == 0 || (unsigned)netId >= 0x7F000000u) continue;
        if (MiscSteamIdLooksValid(MiscReadIdentitySteamId(ident)))
        {
            hits++;
            continue;
        }
        // SteamID offset may have drifted — accept validated persona EngStrings.
        char nbuf[64] = {};
        if (MiscReadIdentityNameField(ident, oak_offsets::network::PlayerName, nbuf, 64) ||
            MiscReadIdentityNameField(ident, oak_offsets::player_identity::Name, nbuf, 64) ||
            MiscReadIdentityNameField(ident, 0xF0, nbuf, 64))
        {
            if (MiscSteamNameLooksValid(nbuf))
                hits++;
        }
    }
    return hits;
}

// Find real NetworkClient by matching local Steam persona at PlayerName (+0xF8).
// IMPORTANT: never full-scan .data on the Present thread (that froze live QA).
static uintptr_t MiscDiscoverNetworkClientByLocalName()
{
    if (!g_GameModule || !g_LocalSteamName[0]) return 0;
    DWORD now = GetTickCount();
    if (g_CachedNetworkClient && (now - g_CachedNetworkClientTick) < 30000 &&
        IsValidPtr(g_CachedNetworkClient) &&
        !MiscLooksLikeStatusStringBlob(g_CachedNetworkClient))
        return g_CachedNetworkClient;

    uintptr_t base = (uintptr_t)g_GameModule;
    uintptr_t best = 0;
    int bestScore = -1;

    auto consider = [&](uintptr_t obj) {
        if (!IsValidPtr(obj) || obj < 0x100000000 || obj > 0x00007FFFFFFFFFFFULL)
            return;
        if (MiscLooksLikeStatusStringBlob(obj))
            return;
        char name[64] = {};
        if (!MiscReadIdentityNameField(obj, oak_offsets::network::PlayerName, name, 64) &&
            !MiscReadIdentityNameField(obj, 0xF0, name, 64) &&
            !MiscReadIdentityNameField(obj, oak_offsets::player_identity::Name, name, 64))
            return;
        if (StrCmpI(name, g_LocalSteamName) != 0)
            return;
        int score = 10;
        int count = MiscScoreboardBestCount(obj);
        if (count > 0 && count <= 128) score += count;
        const uintptr_t boardOffs[] = { 0x18, 0x10, 0x20, 0x28, 0x08, 0x30, 0x40 };
        for (int bi = 0; bi < 7; bi++)
        {
            uintptr_t board = Read<uintptr_t>(obj + boardOffs[bi]);
            int hits = MiscBoardNameHits(board, false, 0, count > 0 ? count : 8);
            if (hits > 0) score += hits * 5;
            hits = MiscBoardNameHits(board, true, 0x170, count > 0 ? count : 8);
            if (hits > 0) score += hits * 5;
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = obj;
        }
    };

    // 1) Narrow window around known Network RVAs (not a multi-MB .data walk).
    const uintptr_t rvaCenters[] = {
        oak_offsets::modbase::Network,
        oak_offsets::modbase::NetworkManager,
        0x100FBA0, 0x100FBC0, 0x100FBE0, 0x100FC00, 0x100FC20, 0x100FC60,
        0x100FC80, 0x100FCA0, 0x100FCC0, 0x100FD00, 0x100FD40, 0x100FD80,
        0x0FFF000, 0x1010000, 0x1010400, 0x0FF8000
    };
    for (int ci = 0; ci < 18; ci++)
    {
        uintptr_t center = rvaCenters[ci];
        for (int d = -0x200; d <= 0x200; d += 8)
        {
            uintptr_t rva = center + (uintptr_t)d;
            if (rva < 0x800000 || rva > 0x1800000) continue;
            uintptr_t obj = 0;
            __try { obj = *(uintptr_t*)(base + rva); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            consider(obj);
            if (IsValidPtr(obj))
            {
                // One hop: ManagerNetworkClient / nearby ptrs
                consider(Read<uintptr_t>(obj + 0x50));
                consider(Read<uintptr_t>(obj + 0x48));
                consider(Read<uintptr_t>(obj + 0x58));
                consider(Read<uintptr_t>(obj + 0x40));
            }
        }
    }

    // 2) World object nearby ptrs (script Game often holds Network/client refs).
    uintptr_t world = Read<uintptr_t>(base + oak_offsets::modbase::World);
    if (IsValidPtr(world) && world > 0x100000000)
    {
        for (uintptr_t off = 0x2800; off <= 0x2C00; off += 8)
            consider(Read<uintptr_t>(world + off));
        for (uintptr_t off = 0x7100; off <= 0x7300; off += 8)
            consider(Read<uintptr_t>(world + off));
    }

    if (best)
    {
        g_CachedNetworkClient = best;
        g_CachedNetworkClientTick = now;
        static int s_DiscLog = 0;
        if ((s_DiscLog++ % 4) == 0)
        {
            char b[160];
            wsprintfA(b, "roster: discovered NetworkClient=%p score=%d (by local name)",
                (void*)best, bestScore);
            Log(b);
        }
    }
    else
    {
        static int s_Miss = 0;
        if ((s_Miss++ % 8) == 0)
            Log("roster: NetworkClient discover miss (name scan)");
    }
    return best;
}

static int MiscSteamthingIngestClient(uintptr_t client)
{
    if (!IsValidPtr(client) || client < 0x100000000 || client > 0x00007FFFFFFFFFFFULL)
        return 0;

    // Prefer classic +0x18, but also discover when that field is junk (live: 0xBA).
    uintptr_t bestBoard = 0;
    int bestHits = 0;
    bool bestContig = false;
    uintptr_t bestStride = 0;
    int bestSlots = 128;

    int countHint = MiscScoreboardBestCount(client);
    if (countHint < 1) countHint = Read<int>(client + 0x10); // live saw 16 here
    if (countHint < 1 || countHint > 128) countHint = 64;

    const uintptr_t boardOffs[] = {
        0x18, 0x10, 0x20, 0x28, 0x08, 0x30, 0x38, 0x40, 0x48, 0x00, 0x50, 0x58, 0x60
    };
    const uintptr_t strides[] = { 0, 0x170, 0x160, 0x158, 0x168 };

    for (int bi = 0; bi < 13; bi++)
    {
        uintptr_t board = Read<uintptr_t>(client + boardOffs[bi]);
        if (!IsValidPtr(board) || board < 0x100000000 || board > 0x00007FFFFFFFFFFFULL)
            continue;
        for (int st = 0; st < 5; st++)
        {
            bool contig = strides[st] != 0;
            int hits = MiscBoardNameHits(board, contig, strides[st], countHint > 24 ? 24 : countHint);
            if (hits > bestHits)
            {
                bestHits = hits;
                bestBoard = board;
                bestContig = contig;
                bestStride = strides[st];
                bestSlots = countHint;
            }
        }
    }

    // Wider pointer scan on the client object when fixed offsets miss
    if (bestHits < 1)
    {
        for (uintptr_t off = 0; off <= 0x120; off += 8)
        {
            uintptr_t board = Read<uintptr_t>(client + off);
            if (!IsValidPtr(board) || board < 0x100000000 || board > 0x00007FFFFFFFFFFFULL)
                continue;
            for (int st = 0; st < 5; st++)
            {
                bool contig = strides[st] != 0;
                int hits = MiscBoardNameHits(board, contig, strides[st], 16);
                if (hits > bestHits)
                {
                    bestHits = hits;
                    bestBoard = board;
                    bestContig = contig;
                    bestStride = strides[st];
                    bestSlots = countHint;
                    static int s_Disc = 0;
                    if ((s_Disc++ % 5) == 0)
                    {
                        char b[160];
                        wsprintfA(b, "roster: discovered board off=0x%X hits=%d contig=%d stride=0x%X",
                            (unsigned)off, hits, contig ? 1 : 0, (unsigned)bestStride);
                        Log(b);
                    }
                }
            }
        }
    }

    if (bestHits < 1 || !bestBoard)
        return 0;

    int got = 0;
    g_RosterCount = 0;
    if (!bestContig)
    {
        for (int i = 0; i < 128; i++)
        {
            uintptr_t ident = Read<uintptr_t>(bestBoard + (uintptr_t)i * sizeof(uintptr_t));
            if (!IsValidPtr(ident) || ident < 0x100000000 || ident > 0x00007FFFFFFFFFFFULL)
                continue;
            if (MiscIngestIdentity(ident))
                got++;
        }
    }
    else
    {
        int slots = bestSlots;
        if (slots < 1 || slots > 128) slots = 64;
        for (int i = 0; i < slots; i++)
        {
            if (MiscIngestIdentity(bestBoard + (uintptr_t)i * bestStride))
                got++;
        }
    }

    if (got > 0)
    {
        static int s_Log = 0;
        if ((s_Log++ % 4) == 0)
        {
            char b[192];
            wsprintfA(b, "roster: steamthing client=%p board=%p hits=%d got=%d contig=%d",
                (void*)client, (void*)bestBoard, bestHits, got, bestContig ? 1 : 0);
            Log(b);
            for (int i = 0; i < g_RosterCount && i < 6; i++)
            {
                wsprintfA(b, "roster[%d] net=%d name='%s'",
                    i, g_Roster[i].networkId, g_Roster[i].name);
                Log(b);
            }
        }
    }
    return got;
}

static int MiscSteamthingRefresh()
{
    if (!g_GameModule) return 0;
    // Ensure local persona is available for NetworkClient discovery by name.
    if (!g_LocalSteamName[0])
    {
        char ln[64] = {};
        unsigned long long ls = 0;
        if (MiscSteamLocalPersona(ln, 64, &ls) && ln[0])
        {
            lstrcpynA(g_LocalSteamName, ln, 64);
            if (ls) g_LocalSteamId = ls;
        }
    }

    uintptr_t net = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::Network);
    if (MiscLooksLikeStatusStringBlob(net))
        net = 0;
    uintptr_t nm = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::NetworkManager);
    if (!IsValidPtr(nm) || nm < 0x100000000 || nm > 0x00007FFFFFFFFFFFULL ||
        MiscLooksLikeStatusStringBlob(nm))
        nm = net;

    uintptr_t discovered = MiscDiscoverNetworkClientByLocalName();

    // Prefer live-discovered client; never treat status-string blob as NetworkClient.
    uintptr_t clients[10] = {
        discovered,
        g_CachedNetworkClient,
        (IsValidPtr(net) && !MiscLooksLikeStatusStringBlob(net)) ? net : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + oak_offsets::network::ManagerNetworkClient) : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x48) : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x58) : 0,
        MiscFindNetworkClient(net, nm),
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x40) : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x60) : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x68) : 0,
    };

    int bestGot = 0;
    ScoreboardRosterEntry bestBuf[kMaxRoster];
    for (int c = 0; c < 10; c++)
    {
        if (!IsValidPtr(clients[c]) || clients[c] < 0x100000000 || clients[c] > 0x00007FFFFFFFFFFFULL)
            continue;
        if (MiscLooksLikeStatusStringBlob(clients[c]))
            continue;
        g_RosterCount = 0;
        int got = MiscSteamthingIngestClient(clients[c]);
        if (got > bestGot)
        {
            bestGot = got;
            CopyMemory(bestBuf, g_Roster, sizeof(ScoreboardRosterEntry) * (SIZE_T)g_RosterCount);
            if (!g_CachedNetworkClient)
            {
                g_CachedNetworkClient = clients[c];
                g_CachedNetworkClientTick = GetTickCount();
            }
        }
    }
    g_RosterCount = bestGot;
    if (bestGot > 0)
        CopyMemory(g_Roster, bestBuf, sizeof(ScoreboardRosterEntry) * (SIZE_T)bestGot);
    return bestGot;
}

static void MiscRefreshScoreboardRoster()
{
    if (!g_MiscSteamAvatars)
        return;
    DWORD now = GetTickCount();
    static DWORD s_RosterWarm = 0;
    if (!s_RosterWarm) s_RosterWarm = now;
    // Wait briefly after inject, then refresh — 8s keeps Present light
    if ((now - s_RosterWarm) < 2000)
        return;
    if (g_RosterRefreshTick && (now - g_RosterRefreshTick) < 8000)
        return;
    g_RosterRefreshTick = now;
    if (!g_GameModule) return;

    __try
    {
    int prev = g_RosterCount;
    // Do NOT restore a previous lobby-sized snapshot when the server list grows/shrinks.
    // That was the main "everyone is Mister_Hacker_999" bug after leaving lobby.

    g_RosterCount = 0;

    // 1) steamthing.txt primary path (Network → scoreboard pointer table → Name@0xF8)
    int bestGot = MiscSteamthingRefresh();
    ScoreboardRosterEntry bestBuf[kMaxRoster];
    if (bestGot > 0)
        CopyMemory(bestBuf, g_Roster, sizeof(ScoreboardRosterEntry) * (SIZE_T)bestGot);

    uintptr_t net = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::Network);
    if (MiscLooksLikeStatusStringBlob(net))
        net = 0;
    uintptr_t nm = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::NetworkManager);
    if (!IsValidPtr(nm) || nm < 0x100000000 || nm > 0x00007FFFFFFFFFFFULL ||
        MiscLooksLikeStatusStringBlob(nm))
        nm = net;

    // Only run heavy probes if steamthing found nothing
    uintptr_t client = 0;
    if (bestGot < 1)
    {
        client = g_CachedNetworkClient;
        if (!IsValidPtr(client) || MiscLooksLikeStatusStringBlob(client))
            client = MiscDiscoverNetworkClientByLocalName();
        if (!IsValidPtr(client) || MiscLooksLikeStatusStringBlob(client))
            client = MiscFindNetworkClient(net, nm);
        if (!IsValidPtr(client) || client < 0x100000000 || client > 0x00007FFFFFFFFFFFULL ||
            MiscLooksLikeStatusStringBlob(client))
        {
            uintptr_t c50 = IsValidPtr(net) ? Read<uintptr_t>(net + oak_offsets::network::ManagerNetworkClient) : 0;
            if (IsValidPtr(c50) && c50 > 0x100000000 && c50 <= 0x00007FFFFFFFFFFFULL &&
                !MiscLooksLikeStatusStringBlob(c50))
                client = c50;
            else
                client = 0;
        }
    }

    static int s_Once = 0;
    if ((s_Once++ % 25) == 0)
    {
        char b[192];
        wsprintfA(b, "roster: net=%p nm=%p client=%p steamthing=%d",
            (void*)net, (void*)nm, (void*)client, bestGot);
        Log(b);
        if (IsValidPtr(client) && client > 0x100000000 && client <= 0x00007FFFFFFFFFFFULL)
        {
            wsprintfA(b, "roster: c+10=%d c+1C=%d c+24=%d c+28=%d board18=%p",
                Read<int>(client + 0x10),
                Read<int>(client + oak_offsets::network::IdentityCount),
                Read<int>(client + oak_offsets::network::ScoreboardSize),
                Read<int>(client + 0x28),
                (void*)Read<uintptr_t>(client + oak_offsets::network::ScoreboardPtr));
            Log(b);
            // One-shot dump of client fields to locate scoreboard on this build
            static int s_Dump = 0;
            if (s_Dump++ < 2)
            {
                for (uintptr_t off = 0; off <= 0x60; off += 8)
                {
                    uintptr_t p = Read<uintptr_t>(client + off);
                    int asInt = Read<int>(client + off);
                    wsprintfA(b, "roster: dump +0x%02X ptr=%p int=%d",
                        (unsigned)off, (void*)p, asInt);
                    Log(b);
                }
            }
        }
    }

    // helloworld layout: ScoreBoard@0x10 contiguous stride 0x160, count@0x18 —
    // live build often has count@0x10=N instead; try both.
    if (bestGot < 1 && IsValidPtr(client) && client > 0x100000000 && client <= 0x00007FFFFFFFFFFFULL)
    {
        int counts[] = {
            Read<int>(client + 0x10),
            Read<int>(client + 0x18),
            Read<int>(client + 0x1C),
            MiscScoreboardBestCount(client)
        };
        const uintptr_t boardOffsHw[] = { 0x08, 0x10, 0x00, 0x18, 0x20, 0x28 };
        const uintptr_t stridesHw[] = { 0x160, 0x170, 0x158, 0x168 };
        for (int ci = 0; ci < 4; ci++)
        {
            int count = counts[ci];
            if (count < 1 || count > 128) continue;
            for (int bi = 0; bi < 6; bi++)
            {
                uintptr_t board = Read<uintptr_t>(client + boardOffsHw[bi]);
                if (!IsValidPtr(board) || board < 0x100000000) continue;
                for (int st = 0; st < 4; st++)
                {
                    g_RosterCount = 0;
                    MiscTryIngestContiguous(board, count, stridesHw[st]);
                    MiscTryIngestTable(board, count);
                    if (g_RosterCount > bestGot)
                    {
                        bestGot = g_RosterCount;
                        CopyMemory(bestBuf, g_Roster, sizeof(ScoreboardRosterEntry) * (SIZE_T)g_RosterCount);
                        static int s_Hw = 0;
                        if ((s_Hw++ % 3) == 0)
                        {
                            char b[160];
                            wsprintfA(b, "roster: hw-hit off=0x%X count=%d stride=0x%X got=%d",
                                (unsigned)boardOffsHw[bi], count, (unsigned)stridesHw[st], bestGot);
                            Log(b);
                        }
                    }
                }
            }
        }
    }

    if (bestGot < 1 && IsValidPtr(client) && client > 0x100000000 && client <= 0x00007FFFFFFFFFFFULL)
    {
        int count = MiscScoreboardBestCount(client);
        if (count < 1) count = 8;
        const uintptr_t boardOffs[] = {
            oak_offsets::network::ScoreboardPtr, 0x20, 0x28, 0x30, 0x10, 0x38, 0x40, 0x48, 0x08
        };
        for (int bi = 0; bi < 9; bi++)
        {
            g_RosterCount = 0;
            uintptr_t board = Read<uintptr_t>(client + boardOffs[bi]);
            MiscIngestBoardAllLayouts(board, count);
            int szPair = Read<int>(client + boardOffs[bi] + 8);
            if (szPair > 0 && szPair <= 128)
                MiscIngestBoardAllLayouts(board, szPair);
            const uintptr_t countOffs[] = { 0x1C, 0x24, 0x10, 0x28, 0x20 };
            for (int ci = 0; ci < 5; ci++)
            {
                int c = Read<int>(client + countOffs[ci]);
                if (c > 0 && c <= 128)
                    MiscIngestBoardAllLayouts(board, c);
            }
            if (g_RosterCount > bestGot)
            {
                bestGot = g_RosterCount;
                CopyMemory(bestBuf, g_Roster, sizeof(ScoreboardRosterEntry) * (SIZE_T)g_RosterCount);
            }
        }
    }

    // Scan roots for identity tables (pointer + contiguous) — fallback only
    if (bestGot < 1)
    {
        uintptr_t roots[] = {
            client, net, nm,
            IsValidPtr(net) ? Read<uintptr_t>(net + 0x50) : 0,
            IsValidPtr(nm) ? Read<uintptr_t>(nm + 0x50) : 0
        };
        for (int r = 0; r < 5; r++)
        {
            int hits = 0; uintptr_t table = 0; int size = 0;
            MiscScanRootForScoreboard(roots[r], &hits, &table, &size);
            if (hits >= 1 && table && size >= 1)
            {
                g_RosterCount = 0;
                MiscIngestBoardAllLayouts(table, size);
                if (g_RosterCount > bestGot)
                {
                    bestGot = g_RosterCount;
                    CopyMemory(bestBuf, g_Roster, sizeof(ScoreboardRosterEntry) * (SIZE_T)g_RosterCount);
                }
                static int s_HitLog = 0;
                if ((s_HitLog++ % 10) == 0)
                {
                    char b[128];
                    wsprintfA(b, "roster: scanHit root=%d hits=%d size=%d got=%d",
                        r, hits, size, g_RosterCount);
                    Log(b);
                }
            }
        }
    }

    g_RosterCount = bestGot;
    if (bestGot > 0)
        CopyMemory(g_Roster, bestBuf, sizeof(ScoreboardRosterEntry) * (SIZE_T)bestGot);

    // Drop entries whose name is our local persona but SteamID differs (lobby/API pollution)
    {
        int w = 0;
        for (int i = 0; i < g_RosterCount; i++)
        {
            if (MiscNameIsFalselyLocal(g_Roster[i].name, g_Roster[i].steamId))
            {
                g_Roster[i].name[0] = 0; // keep steam/net for later EngString fill
            }
            g_Roster[w++] = g_Roster[i];
        }
        g_RosterCount = w;
    }

    // Do NOT wipe Steam tag cache on roster collapse — scoreboard often reports 0
    // briefly on this build and clearing tags caused name→"Player" flicker.
    static int s_LastRosterCount = -1;
    s_LastRosterCount = g_RosterCount;

    // World entities: DayZPlayer::GetName often returns the Steam persona in MP even when
    // the scoreboard pointer table is stale. Merge into roster by networkId.
    {
        uintptr_t world = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::World);
        if (IsValidPtr(world) && world > 0x100000000)
        {
            auto ingestList = [&](uintptr_t listOff, int maxScan) {
                uintptr_t data = 0; int count = 0;
                if (!ResolveEntityList(world, listOff, maxScan, data, count, nullptr))
                    return;
                if (!IsValidPtr(data) || count < 1) return;
                int n = count < maxScan ? count : maxScan;
                for (int i = 0; i < n; i++)
                {
                    uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
                    if (!IsValidPtr(ent) || ent < 0x100000000) continue;
                    uintptr_t typ = Read<uintptr_t>(ent + oak_offsets::entity::Type);
                    if (!IsValidPtr(typ) || typ < 0x100000000) continue;
                    uintptr_t cfg = Read<uintptr_t>(typ + oak_offsets::entitytype::ConfigName);
                    char cfgName[40] = {};
                    if (!ReadEngineString(cfg, cfgName, 40) || StrCmpI(cfgName, "dayzplayer") != 0)
                        continue;
                    int netId = MiscReadNetworkId(ent);
                    if (netId == 0) continue;
                    char gn[64] = {};
                    if (!MiscTryDayZPlayerGetName(ent, gn, 64) || !MiscSteamNameLooksValid(gn))
                        continue;
                    if (MiscNameIsFalselyLocal(gn, 0) &&
                        !(IsValidPtr(g_ResolvedLocalPlayer) && ent == g_ResolvedLocalPlayer))
                        continue;
                    // Upsert by networkId
                    bool found = false;
                    for (int r = 0; r < g_RosterCount; r++)
                    {
                        if (g_Roster[r].networkId != netId) continue;
                        if (!MiscSteamNameLooksValid(g_Roster[r].name))
                            lstrcpynA(g_Roster[r].name, gn, 64);
                        found = true;
                        break;
                    }
                    if (!found && g_RosterCount < kMaxRoster)
                    {
                        ScoreboardRosterEntry& e = g_Roster[g_RosterCount++];
                        e.used = true;
                        e.networkId = netId;
                        e.steamId = 0;
                        lstrcpynA(e.name, gn, 64);
                    }
                    // Hot cache for ESP
                    MiscCacheSteamTag(ent, netId, 0, gn, nullptr, 0);
                }
            };
            ingestList(oak_offsets::world::NearEntList, 64);
            ingestList(oak_offsets::world::FarEntList, 128);
            // World ingest may grow roster — keep count sticky for next wipe check
            s_LastRosterCount = g_RosterCount;
        }
    }

    // Seed / refresh local Steam persona (only touch the matching steam/net slot)
    {
        char localName[64] = {};
        unsigned long long localSid = 0;
        if (MiscSteamLocalPersona(localName, 64, &localSid) && localName[0])
        {
            lstrcpynA(g_LocalSteamName, localName, 64);
            g_LocalSteamId = localSid;

            int localNet = 0;
            if (IsValidPtr(g_ResolvedLocalPlayer) && g_ResolvedLocalPlayer > 0x100000000)
            {
                localNet = Read<int>(g_ResolvedLocalPlayer + oak_offsets::entity::NetworkIdPlayer);
                if (localNet == 0)
                    localNet = Read<int>(g_ResolvedLocalPlayer + oak_offsets::entity::NetworkId);
            }

            bool found = false;
            for (int i = 0; i < g_RosterCount; i++)
            {
                if ((localSid && g_Roster[i].steamId == localSid) ||
                    (localNet && g_Roster[i].networkId == localNet))
                {
                    g_Roster[i].steamId = localSid ? localSid : g_Roster[i].steamId;
                    if (localNet) g_Roster[i].networkId = localNet;
                    lstrcpynA(g_Roster[i].name, localName, 64);
                    found = true;
                    break;
                }
            }
            if (!found && g_RosterCount < kMaxRoster && localSid)
            {
                ScoreboardRosterEntry& e = g_Roster[g_RosterCount++];
                e.used = true;
                e.networkId = localNet;
                e.steamId = localSid;
                lstrcpynA(e.name, localName, 64);
            }
        }
    }

    // Scoreboard path is dead on this build — promote harvested SteamIDs → roster/tags.
    MiscPromoteOrphanSteams();

    static int s_Log = 0;
    if (false && ((s_Log++ % 15) == 0 || g_RosterCount != prev))
    {
        char b[128];
        wsprintfA(b, "roster: players=%d (scoreboard dump)", g_RosterCount);
        Log(b);
        for (int i = 0; i < g_RosterCount && i < 12; i++)
        {
            wsprintfA(b, "roster[%d] net=%d steam=...%u name='%s'",
                i, g_Roster[i].networkId,
                (unsigned)(g_Roster[i].steamId & 0xFFFFFFFFu),
                g_Roster[i].name);
            Log(b);
        }
    }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("roster: exception (skipped)");
    }
}

static bool MiscRosterLookup(int netId, char* outName, int outMax, unsigned long long* steamOut)
{
    if (outName) outName[0] = 0;
    if (steamOut) *steamOut = 0;
    if (netId == 0) return false;
    // Never force a roster rebuild from ESP resolve — that was racing orphan binds
    // and briefly wiping names. Refresh runs on its own cadence from UpdateMisc.
    int namedIdx = -1;
    int anyIdx = -1;
    for (int i = 0; i < g_RosterCount; i++)
    {
        if (!g_Roster[i].used || g_Roster[i].networkId != netId) continue;
        if (anyIdx < 0) anyIdx = i;
        if (MiscSteamNameLooksValid(g_Roster[i].name) &&
            !MiscNameIsFalselyLocal(g_Roster[i].name, g_Roster[i].steamId))
        {
            namedIdx = i;
            break;
        }
    }
    int idx = namedIdx >= 0 ? namedIdx : anyIdx;
    if (idx < 0) return false;
    if (steamOut) *steamOut = g_Roster[idx].steamId;
    if (outName && MiscSteamNameLooksValid(g_Roster[idx].name) &&
        !MiscNameIsFalselyLocal(g_Roster[idx].name, g_Roster[idx].steamId))
    {
        lstrcpynA(outName, g_Roster[idx].name, outMax);
        return true;
    }
    if (g_Roster[idx].steamId && outName)
    {
        // Cache-only persona (no Friends/HTTP on Present) — harvest already filled HTTP cache.
        if (MiscSteamPersonaNameEx(g_Roster[idx].steamId, outName, outMax, false) &&
            MiscSteamNameLooksValid(outName))
            return true;
    }
    // SteamID alone is not a successful name lookup
    return false;
}

static int MiscReadNetworkId(uintptr_t entity)
{
    int a = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer);
    if (a != 0) return a;
    return Read<int>(entity + oak_offsets::entity::NetworkId);
}

static bool MiscReadScoreboardName(uintptr_t identity, char* out, int outMax, unsigned long long* steamOut)
{
    if (out) out[0] = 0;
    if (steamOut) *steamOut = 0;
    if (!IsValidPtr(identity) || identity < 0x100000000)
        return false;

    if (steamOut)
    {
        const uintptr_t steamOffs[] = {
            oak_offsets::scoreboard_identity::SteamId, // 0xA0
            0x98, 0xA8, 0x88, 0x90, 0xB8, 0xC0
        };
        for (int i = 0; i < 7; i++)
        {
            unsigned long long sid = Read<unsigned long long>(identity + steamOffs[i]);
            if (sid > 76561197960265728ULL && sid < 80000000000000000ULL)
            {
                *steamOut = sid;
                break;
            }
        }
    }

    auto tryStr = [&](uintptr_t off) -> bool {
        uintptr_t p = Read<uintptr_t>(identity + off);
        if (!IsValidPtr(p) || p < 0x100000000 || p > 0x00007FFFFFFFFFFFULL) return false;
        if (ReadEngineString(p, out, outMax)) return true;
        // steamthing.txt: raw chars at EngString + OFF_TEXT (0x10), no length gate
        __try
        {
            const char* src = (const char*)(p + 0x10);
            int n = outMax - 1;
            for (int i = 0; i < n; i++)
            {
                char c = src[i];
                if (c == 0) { out[i] = 0; break; }
                if ((unsigned char)c < 32 || (unsigned char)c == 127) { out[0] = 0; return false; }
                out[i] = c;
                out[i + 1] = 0;
            }
            return out[0] != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
        return false;
    };
    // Network::PlayerName 0xF8, SDK also 0xF0, PlayerIdentity::Name 0xB0, plus probes
    if (tryStr(oak_offsets::network::PlayerName)) return true;
    if (tryStr(0xF0)) return true;
    if (tryStr(oak_offsets::player_identity::Name)) return true;
    if (tryStr(0x68)) return true;
    if (tryStr(0x78)) return true;
    if (tryStr(0x80)) return true;
    if (tryStr(0xE8)) return true;
    // Some builds embed EngString at +0xF8 (len @ +0x100, chars @ +0x108)
    if (ReadEngineString(identity + oak_offsets::network::PlayerName, out, outMax)) return true;
    if (ReadEngineString(identity + 0xF0, out, outMax)) return true;
    return out && out[0];
}

static bool MiscLookupIdentityByNetworkId(int netId, char* outName, int outMax, unsigned long long* steamOut)
{
    // Prefer full roster dump (entire server list) then fall back to single walk
    if (MiscRosterLookup(netId, outName, outMax, steamOut))
        return outName && outName[0];

    if (outName) outName[0] = 0;
    if (steamOut) *steamOut = 0;
    if (!g_GameModule || netId == 0)
        return false;

    uintptr_t net = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::Network);
    uintptr_t nm = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::NetworkManager);
    uintptr_t roots[] = {
        net, nm,
        IsValidPtr(nm) ? Read<uintptr_t>(nm + 0x50) : 0,
        IsValidPtr(net) ? Read<uintptr_t>(net + 0x50) : 0
    };

    for (int r = 0; r < 4; r++)
    {
        uintptr_t root = roots[r];
        if (!IsValidPtr(root) || root < 0x100000000) continue;
        for (uintptr_t tableOff = 0x0; tableOff <= 0x200; tableOff += 8)
        {
            uintptr_t table = Read<uintptr_t>(root + tableOff);
            int size = Read<int>(root + tableOff + 8);
            if (size <= 0 || size > 128)
                size = Read<int>(root + oak_offsets::network::IdentityCount);
            if (size <= 0 || size > 128)
                size = Read<int>(root + oak_offsets::network::ScoreboardSize);
            if (!IsValidPtr(table) || table < 0x100000000) continue;
            if (size <= 0 || size > 128) continue;
            const uintptr_t strides[] = { 0, 0x170, 0x160, 0x158, 0x168, 0x180 };
            for (int st = 0; st < 6; st++)
            {
                for (int i = 0; i < size; i++)
                {
                    uintptr_t ident = (strides[st] == 0)
                        ? Read<uintptr_t>(table + (uintptr_t)i * 8)
                        : (table + (uintptr_t)i * strides[st]);
                    if (!IsValidPtr(ident) || ident < 0x100000000) continue;
                    int idA = Read<int>(ident + oak_offsets::scoreboard_identity::NetworkId);
                    int idB = Read<int>(ident + 0x24);
                    int idC = Read<int>(ident + 0x28);
                    int idD = Read<int>(ident + 0x34);
                    if (idA != netId && idB != netId && idC != netId && idD != netId) continue;
                    if (MiscReadScoreboardName(ident, outName, outMax, steamOut))
                    {
                        if (steamOut && *steamOut && outName &&
                            (outName[0] == 0 || StrContainsI(outName, "Survivor")))
                        {
                            char persona[64] = {};
                            if (MiscSteamPersonaName(*steamOut, persona, 64))
                                lstrcpynA(outName, persona, outMax);
                        }
                        return outName && outName[0];
                    }
                }
            }
        }
    }
    return false;
}

static bool MiscCacheSteamTag(uintptr_t entity, int netId, unsigned long long steamId,
    const char* name, char* outName, int outMax)
{
    DWORD now = GetTickCount();
    // Prefer exact netId / steamId / entity match; never share a sentinel like -2 across players
    int slot = -1;
    for (int i = 0; i < 64; i++)
    {
        if (g_SteamTags[i].used && netId && g_SteamTags[i].networkId == netId) { slot = i; break; }
        if (g_SteamTags[i].used && steamId && g_SteamTags[i].steamId == steamId) { slot = i; break; }
        if (g_SteamTags[i].used && entity && g_SteamTags[i].entity == entity) { slot = i; break; }
    }
    if (slot < 0)
    {
        for (int i = 0; i < 64; i++)
        {
            if (!g_SteamTags[i].used || (now - g_SteamTags[i].lastOk) > 180000)
            {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) slot = (int)((entity >> 4) & 63);
    g_SteamTags[slot].used = true;
    if (entity) g_SteamTags[slot].entity = entity;
    if (netId) g_SteamTags[slot].networkId = netId;
    if (steamId) g_SteamTags[slot].steamId = steamId;
    // Never wipe a good cached name with empty / Survivor junk (was the Player flicker).
    // Do NOT use MiscNameIsFalselyLocal here — that helper means "skip for remotes",
    // but returning true for the local persona prevented outName fills (local tag='?').
    if (name && name[0] && MiscSteamNameLooksValid(name) && !StrContainsI(name, "Survivor"))
    {
        lstrcpynA(g_SteamTags[slot].name, name, 64);
        g_SteamTags[slot].lastOk = now;
    }
    else if (!g_SteamTags[slot].name[0])
    {
        // keep empty; do not refresh lastOk so soft-expire still works
    }
    else
    {
        // Keep prior good name; refresh stickiness
        g_SteamTags[slot].lastOk = now;
    }
    if (outName && g_SteamTags[slot].name[0])
        lstrcpynA(outName, g_SteamTags[slot].name, outMax);
    return g_SteamTags[slot].name[0] != 0;
}

static bool MiscTryDayZPlayerGetName(uintptr_t entity, char* out, int outMax)
{
    if (!out || outMax < 2 || !g_GameModule || !IsValidPtr(entity))
        return false;
    out[0] = 0;
    // Refuse non-players — GetName on infected AVs the engine.
    {
        char cfg[64] = {};
        ReadEntityConfigName(entity, cfg, 64);
        if (!cfg[0] || (StrCmpI(cfg, "dayzplayer") != 0 && !StrContainsI(cfg, "dayzplayer")))
            return false;
    }
    // GetName native ABI on this build still AVs inside DayZ (+0x31EDxx) even when
    // the player* is passed in RDX. Steam/roster/orphan paths cover names — keep the
    // native cold until the Enforce out-param shape is re-derived cleanly.
    (void)entity;
    return false;
#if 0
    // DayZPlayer::GetName — real entry on 1.29.0.163709 is 0x4E5120 (old dump was +0x10).
    // ABI: prologue does `mov rdi, rdx` and clobbers ecx with alloc size — the player*
    // is the SECOND argument (RDX), not RCX. Calling with a 1-arg typedef put garbage
    // in RDX and AVd DayZ+0x31EDB5 on read of 0x10000000xxxx.
    typedef uintptr_t (*GetNameFn)(uintptr_t /*unused*/, uintptr_t entity);
    const uintptr_t fnAddr = (uintptr_t)g_GameModule + 0x4E5120ull;
    if (!OakNativeEntryOk(fnAddr, "DayZPlayer::GetName"))
        return false;
    auto fn = (GetNameFn)fnAddr;
    uintptr_t rstr = 0;
    __try { rstr = fn(0, entity); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (IsValidPtr(rstr) && rstr > 0x100000000)
    {
        if (ReadEngineString(rstr, out, outMax) && MiscSteamNameLooksValid(out))
            return true;
        // Some builds return RString* / wrapper
        uintptr_t inner = Read<uintptr_t>(rstr);
        if (IsValidPtr(inner) && ReadEngineString(inner, out, outMax) && MiscSteamNameLooksValid(out))
            return true;
    }
    return false;
#endif
}

static bool MiscEmitPlayerTag(uintptr_t entity, int netId, bool isLocal,
    const char* nm, unsigned long long sid,
    char* outName, int outMax, ID3D11ShaderResourceView** outSrv)
{
    if (!nm || !MiscSteamNameLooksValid(nm) || StrContainsI(nm, "Survivor"))
        return false;
    if (!isLocal && MiscNameIsFalselyLocal(nm, sid))
        return false;
    if (outName) lstrcpynA(outName, nm, outMax);
    MiscCacheSteamTag(entity, netId, sid, nm, nullptr, 0);
    if (outSrv && g_MiscSteamAvatars && sid)
    {
        // Prefer session cache; only attempt one upload path if missing.
        *outSrv = MiscSteamAvatarCached(sid);
        if (!*outSrv)
            *outSrv = MiscSteamAvatarSrv(sid);
    }
    return outName && outName[0];
}

static void MiscTryOwnerIdentityName(uintptr_t entity, int netId, bool isLocal,
    char* name, int nameMax, unsigned long long* steamId)
{
    if (!name || nameMax < 2) return;
    __try
    {
        const uintptr_t ownerOffs[] = {
            oak_offsets::entity_owner::Owner, // 0xA0
            0x70, 0x90, 0x98, 0xA8, 0xB0, 0x148, 0x150
        };
        for (int oi = 0; oi < 8 && !name[0]; oi++)
        {
            uintptr_t owner = Read<uintptr_t>(entity + ownerOffs[oi]);
            if (!IsValidPtr(owner) || owner < 0x100000000) continue;
            uintptr_t ovt = Read<uintptr_t>(owner);
            bool ownerOk = IsValidPtr(ovt) && ovt > 0x100000000;
            if (g_GameModule && ownerOk)
            {
                uintptr_t mod = (uintptr_t)g_GameModule;
                if (!(ovt >= mod && ovt < mod + 0x8000000ULL))
                    ownerOk = false;
                if (owner >= mod && owner < mod + 0x8000000ULL)
                    ownerOk = false;
            }
            if (!ownerOk) continue;

            unsigned long long sid = 0;
            char nbuf[64] = {};
            MiscReadScoreboardName(owner, nbuf, 64, &sid);
            int idNet = Read<int>(owner + oak_offsets::scoreboard_identity::NetworkId);
            if (netId && idNet && idNet != netId && !sid)
                continue;
            if (!isLocal && g_LocalSteamId && sid && sid == g_LocalSteamId)
                continue;
            if (sid && steamId) *steamId = sid;
            if (MiscSteamNameLooksValid(nbuf) && !MiscNameIsFalselyLocal(nbuf, sid))
                lstrcpynA(name, nbuf, nameMax);
            else if (sid && !(g_LocalSteamId && sid == g_LocalSteamId))
                MiscSteamPersonaName(sid, name, nameMax);
            if (!name[0] && sid && netId && idNet == netId &&
                !(g_LocalSteamId && sid == g_LocalSteamId))
                MiscSteamPersonaName(sid, name, nameMax);
            if (MiscNameIsFalselyLocal(name, steamId ? *steamId : 0) && !isLocal)
                name[0] = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; }
}

static bool MiscResolvePlayerTag(uintptr_t entity, char* outName, int outMax, ID3D11ShaderResourceView** outSrv)
{
    OAK_MARK("tag.enter");
    if (outName) outName[0] = 0;
    if (outSrv) *outSrv = nullptr;
    if (!g_MiscSteamAvatars || !IsValidPtr(entity))
        return false;

    OAK_MARK("tag.netid");
    int netId = MiscReadNetworkId(entity);
    DWORD now = GetTickCount();

    // Trust ESP-resolved local only. World::LocalPlayer is often a stub / wrong pointer
    // on this build and was marking remotes as local → Cache rejected persona → tag='?'.
    bool isLocal = (IsValidPtr(g_ResolvedLocalPlayer) && entity == g_ResolvedLocalPlayer);
    if (!isLocal && !IsValidPtr(g_ResolvedLocalPlayer) && g_GameModule)
    {
        uintptr_t world = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::World);
        if (IsValidPtr(world))
        {
            uintptr_t lp = Read<uintptr_t>(world + oak_offsets::world::LocalPlayer);
            if (IsValidPtr(lp) && entity == lp) isLocal = true;
        }
    }

    // Soft-sticky cache: serve good names for minutes (was 4s → Player flicker).
    int softSlot = -1;
    for (int i = 0; i < 64; i++)
    {
        if (!g_SteamTags[i].used || !g_SteamTags[i].name[0]) continue;
        bool hit = false;
        if (netId && g_SteamTags[i].networkId == netId) hit = true;
        else if (g_SteamTags[i].entity == entity) hit = true;
        if (!hit) continue;
        if (MiscNameIsFalselyLocal(g_SteamTags[i].name, g_SteamTags[i].steamId) && !isLocal)
            continue;
        softSlot = i;
        if ((now - g_SteamTags[i].lastOk) < 300000)
        {
            g_SteamTags[i].entity = entity;
            g_SteamTags[i].lastOk = now;
            return MiscEmitPlayerTag(entity, netId, isLocal, g_SteamTags[i].name,
                g_SteamTags[i].steamId, outName, outMax, outSrv);
        }
        break;
    }

    // Local persona — always win for self (do not depend on CacheSteamTag return).
    if (isLocal)
    {
        if (!g_LocalSteamName[0])
        {
            char localName[64] = {};
            unsigned long long localSid = 0;
            if (MiscSteamLocalPersona(localName, 64, &localSid) && localName[0])
            {
                lstrcpynA(g_LocalSteamName, localName, 64);
                if (localSid) g_LocalSteamId = localSid;
            }
        }
        if (g_LocalSteamName[0])
            return MiscEmitPlayerTag(entity, netId, isLocal, g_LocalSteamName,
                g_LocalSteamId, outName, outMax, outSrv);
    }

    // Remotes: orphan / HTTP persona BEFORE Owner probing (Owner paths AV / pollute on this build).
    if (!isLocal)
    {
        OAK_MARK("tag.orphan");
        char on[64] = {};
        unsigned long long os = 0;
        if (MiscTryResolveFromOrphan(entity, netId, on, 64, &os) &&
            MiscEmitPlayerTag(entity, netId, isLocal, on, os, outName, outMax, outSrv))
            return true;

        OAK_MARK("tag.rosterlookup");
        char rn[64] = {};
        unsigned long long rs = 0;
        if (netId && MiscRosterLookup(netId, rn, 64, &rs) &&
            MiscEmitPlayerTag(entity, netId, isLocal, rn, rs, outName, outMax, outSrv))
            return true;
    }

    char name[64] = {};
    unsigned long long steamId = 0;

    // Scoreboard / identity (safe; RosterLookup no longer rebuilds)
    if (!isLocal && netId != 0)
    {
        OAK_MARK("tag.identity");
        MiscLookupIdentityByNetworkId(netId, name, 64, &steamId);
    }
    if (MiscNameIsFalselyLocal(name, steamId) && !isLocal)
        name[0] = 0;
    if (MiscSteamNameLooksValid(name) &&
        MiscEmitPlayerTag(entity, netId, isLocal, name, steamId, outName, outMax, outSrv))
        return true;

    // Entity::Owner → PlayerIdentity (SEH in helper — no C++ objects in this frame)
    if (!name[0])
        MiscTryOwnerIdentityName(entity, netId, isLocal, name, 64, &steamId);
    if (MiscSteamNameLooksValid(name) &&
        MiscEmitPlayerTag(entity, netId, isLocal, name, steamId, outName, outMax, outSrv))
        return true;

    // Game GetName() — only if still empty
    if (!MiscSteamNameLooksValid(name) || StrContainsI(name, "Survivor"))
    {
        char gn[64] = {};
        if (MiscTryDayZPlayerGetName(entity, gn, 64) && MiscSteamNameLooksValid(gn) &&
            (isLocal || !MiscNameIsFalselyLocal(gn, steamId)))
            lstrcpynA(name, gn, 64);
    }
    if (MiscSteamNameLooksValid(name) &&
        MiscEmitPlayerTag(entity, netId, isLocal, name, steamId, outName, outMax, outSrv))
        return true;

    if (!name[0] && steamId)
        MiscSteamPersonaName(steamId, name, 64);
    if (MiscSteamNameLooksValid(name) &&
        MiscEmitPlayerTag(entity, netId, isLocal, name, steamId, outName, outMax, outSrv))
        return true;

    // Sticky fallback: keep last good tag instead of dropping to "Player"
    if (softSlot >= 0 && g_SteamTags[softSlot].name[0] &&
        !(MiscNameIsFalselyLocal(g_SteamTags[softSlot].name, g_SteamTags[softSlot].steamId) && !isLocal))
        return MiscEmitPlayerTag(entity, netId, isLocal, g_SteamTags[softSlot].name,
            g_SteamTags[softSlot].steamId, outName, outMax, outSrv);

    return false;
}

// QA: scoreboard roster size. Solo servers cannot prove remote Steam names.
static void MiscLiveQaDumpNames()
{
    // Force a fresh roster pass (bypass 8s throttle) for QA dumps.
    g_RosterRefreshTick = 0;
    MiscRefreshScoreboardRoster();
    int remotes = 0;
    for (int i = 0; i < g_RosterCount; i++)
    {
        if (!g_Roster[i].used) continue;
        if (g_LocalSteamId && g_Roster[i].steamId == g_LocalSteamId) continue;
        if (g_LocalSteamName[0] && g_Roster[i].name[0] &&
            StrCmpI(g_Roster[i].name, g_LocalSteamName) == 0 && !g_Roster[i].steamId)
            continue;
        remotes++;
    }
    char b[192];
    wsprintfA(b, "liveqa[names] roster=%d remotes=%d local='%s' client=%p need_mp=%d",
        g_RosterCount, remotes,
        g_LocalSteamName[0] ? g_LocalSteamName : "?",
        (void*)g_CachedNetworkClient,
        remotes < 1 ? 1 : 0);
    Log(b);
    for (int i = 0; i < g_RosterCount && i < 16; i++)
    {
        if (!g_Roster[i].used) continue;
        wsprintfA(b, "liveqa[names] slot=%d net=%d sid_lo=%u name='%s'",
            i, g_Roster[i].networkId,
            (unsigned)(g_Roster[i].steamId & 0xFFFFFFFFu),
            g_Roster[i].name[0] ? g_Roster[i].name : "?");
        Log(b);
    }
    // Per-entity GetName probe on Near dayzplayers (proves MP name source)
    if (g_GameModule)
    {
        uintptr_t world = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::World);
        uintptr_t data = 0; int count = 0;
        if (IsValidPtr(world) &&
            ResolveEntityList(world, oak_offsets::world::NearEntList, 64, data, count, nullptr) &&
            IsValidPtr(data) && count > 0)
        {
            int n = count < 8 ? count : 8;
            for (int i = 0; i < n; i++)
            {
                uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
                if (!IsValidPtr(ent)) continue;
                uintptr_t typ = Read<uintptr_t>(ent + oak_offsets::entity::Type);
                if (!IsValidPtr(typ)) continue;
                uintptr_t cfg = Read<uintptr_t>(typ + oak_offsets::entitytype::ConfigName);
                char cfgName[32] = {};
                if (!ReadEngineString(cfg, cfgName, 32) || StrCmpI(cfgName, "dayzplayer") != 0)
                    continue;
                char gn[64] = {};
                MiscTryDayZPlayerGetName(ent, gn, 64);
                char tag[64] = {};
                MiscResolvePlayerTag(ent, tag, 64, nullptr);
                wsprintfA(b, "liveqa[names] ent net=%d getname='%s' tag='%s' local=%d",
                    MiscReadNetworkId(ent),
                    gn[0] ? gn : "?",
                    tag[0] ? tag : "?",
                    (IsValidPtr(g_ResolvedLocalPlayer) && ent == g_ResolvedLocalPlayer) ? 1 : 0);
                Log(b);
            }
        }
    }
}

// Real user32 pointers — freecam / menu must bypass our game-input IAT suppress.
typedef SHORT(WINAPI* t_GetAsyncKeyState)(int);
typedef SHORT(WINAPI* t_GetKeyState)(int);
typedef BOOL(WINAPI* t_GetKeyboardState)(PBYTE);
static t_GetAsyncKeyState o_GetAsyncKeyState = nullptr;
static t_GetKeyState o_GetKeyState = nullptr;
static t_GetKeyboardState o_GetKeyboardState = nullptr;
static volatile LONG g_FcSuppressGameInput = 0;
static LONG g_FcInputHooksInstalled = 0;

static SHORT MiscRealAsyncKey(int vk)
{
    if (o_GetAsyncKeyState)
        return o_GetAsyncKeyState(vk);
    return GetAsyncKeyState(vk);
}

static bool MiscFreecamGameInputSuppressed()
{
    return InterlockedCompareExchange(&g_FcSuppressGameInput, 1, 1) == 1;
}

static bool KeyPressedEdge(int vk)
{
    if (vk == 0) return false;
    static bool s_WasDown[256] = {};
    int idx = vk & 255;
    SHORT st = MiscRealAsyncKey(vk);
    bool down = (st & 0x8000) != 0;
    // Bit0 = pressed since last GetAsyncKeyState — catches quick taps between Present frames.
    bool queued = (st & 0x0001) != 0;
    bool edge = (down && !s_WasDown[idx]) || (queued && !s_WasDown[idx]);
    s_WasDown[idx] = down;
    return edge;
}

static bool KeyHeld(int vk)
{
    if (vk == 0) return false;
    return (MiscRealAsyncKey(vk) & 0x8000) != 0;
}

static void MiscFreecamShutdown();

static void MiscPushUi()
{
    ImGuiMenu_PushMiscFlags(
        g_MiscMiddleClickDespawn, g_MiscLootMagnet, g_MiscContainerMagnet,
        g_MiscDaytimeLock, g_MiscDisableOverlays,
        false, g_MiscSteamAvatars, g_MiscDrawWaypoints,
        g_MiscFreecam,
        g_ShowESP, g_AimbotEnabled, g_MagicBullet, g_Fullbright);
}

static void MiscApplyPanic()
{
    g_PanicHidden = true;
    if (g_PanicScope == OakPanicEspOnly)
    {
        g_ShowESP = false;
        Log("misc: PANIC (ESP only)");
        MiscPushUi();
        return;
    }
    if (g_PanicScope == OakPanicCombatOnly)
    {
        g_AimbotEnabled = false;
        g_MagicBullet = false;
        g_FastBullets = false;
        g_NoDispersion = false;
        g_PerfectBallistics = false;
        g_BulletTracers = false;
        g_ImpactMarkers = false;
        g_ShotIndicators = false;
        g_HitMarkers = false;
        g_Crosshair = false;
        g_GrenadeTrajectory = false;
        RestoreAmmoBackup();
        MiscPushUi();
        ImGuiMenu_PushAmmoFlags(false, false, false);
        Log("misc: PANIC (combat only)");
        return;
    }
    g_ShowESP = false;
    g_AimbotEnabled = false;
    g_MagicBullet = false;
    g_Fullbright = false;
    g_FastBullets = false;
    g_NoDispersion = false;
    g_PerfectBallistics = false;
    g_MiscFreecam = false;
    MiscFreecamShutdown();
    g_WorldMisc.thirdPerson = false;
    g_BulletTracers = false;
    g_ImpactMarkers = false;
    g_ShotIndicators = false;
    g_HitMarkers = false;
    g_Crosshair = false;
    g_GrenadeTrajectory = false;
    g_MiscLootMagnet = false;
    g_MiscContainerMagnet = false;
    g_MiscMiddleClickDespawn = false;
    RestoreAmmoBackup();
    if (IsValidPtr(g_CachedWorldPtr) && g_CachedWorldPtr > 0x100000000)
        WriteEyeAccom(g_CachedWorldPtr, 1.0f);
    MiscPushUi();
    ImGuiMenu_PushAmmoFlags(false, false, false);
    Log("misc: PANIC (features + ammo + freecam unwound)");
}

// Fullbright only — daytime lock removed.
static void MiscForceWorld(uintptr_t worldPtr)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;
    if (!g_Fullbright && !g_MiscDisableOverlays)
        return;

    __try
    {
        ApplyLightingForce(worldPtr, g_Fullbright, false);
        static int s_DtLog = 0;
        if (false && (++s_DtLog % 180) == 1)
        {
            char b[160];
            wsprintfA(b, "verify[fullbright] on=%d eyeAccomx10=%d",
                g_Fullbright ? 1 : 0,
                (int)(Read<float>(worldPtr + oak_offsets::world::EyeAccom) * 10.f));
            Log(b);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Friends — aimbot / magic bullet skip. Defined here so Steam tag helpers are visible.
static bool CombatIsFriendEntity(uintptr_t entity)
{
    if (!IsValidPtr(entity) || g_FriendCount <= 0)
        return false;

    // Never run Steam/GetName resolve on infected/animals — DayZPlayer::GetName AVs
    // when handed a non-player (seen as DayZ+0x31EDB5 read of 0x10000000xxxx).
    bool isP = false, isZ = false;
    if (!CombatEntityIsPlayerOrZombie(entity, isP, isZ) || !isP)
        return false;

    char name[64] = {};
    unsigned long long sid = 0;

    __try
    {
        int netId = MiscReadNetworkId(entity);
        DWORD now = GetTickCount();
        for (int i = 0; i < 64; i++)
        {
            if (g_SteamTags[i].used && g_SteamTags[i].networkId == netId && netId != 0 &&
                (now - g_SteamTags[i].lastOk) < 10000)
            {
                if (g_SteamTags[i].name[0])
                    lstrcpynA(name, g_SteamTags[i].name, 64);
                sid = g_SteamTags[i].steamId;
                break;
            }
        }
        if (!sid)
        {
            for (int i = 0; i < 64; i++)
            {
                if (g_SteamTags[i].used && g_SteamTags[i].entity == entity && g_SteamTags[i].steamId)
                {
                    sid = g_SteamTags[i].steamId;
                    if (g_SteamTags[i].name[0]) lstrcpynA(name, g_SteamTags[i].name, 64);
                    break;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!name[0])
    {
        __try { MiscTryDayZPlayerGetName(entity, name, 64); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    for (int i = 0; i < g_FriendCount && i < 16; i++)
    {
        if (g_FriendSteamId[i] && sid && g_FriendSteamId[i] == sid)
            return true;
        if (g_FriendName[i][0] && name[0] && StrCmpI(g_FriendName[i], name) == 0)
            return true;
    }
    return false;
}

// Fence / gate / wall / watchtower — DayZ base building kits + placed parts.
static bool MiscIsBaseBuildingName(const char* n)
{
    if (!n || !n[0]) return false;
    const char* keys[] = {
        "fence", "gate", "wall", "watchtower", "basebuilding", "bbp_",
        "shelter", "hesco", "barricade", "territory", "flag_base",
        "metalplate", "woodencase", "camonet", "barbedwire", "fencekit",
        "gatekit", "wallkit", "watchtowerkit", "floor", "roof", "pillar"
    };
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (StrContainsI(n, keys[i])) return true;
    return false;
}

// Visual bury only — do NOT write IsDead / EntityDead@0x15D.
// Fake-killing networked ents left them in CDP/Near lists and corrupted the heap
// (RtlSizeHeap AV on 0xFFFFFFFF) a few seconds after a despawn burst.
static void MiscDespawnEntity(uintptr_t entity)
{
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return;

    __try
    {
        Vec3 pos;
        if (!GetEntityPosition(entity, pos))
            return;
        pos.y -= 500.f;
        uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
        if (!IsValidPtr(vs) || vs < 0x100000000)
            return;
        Write<float>(vs + 0x2C, pos.x);
        Write<float>(vs + 0x30, pos.y);
        Write<float>(vs + 0x34, pos.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void MiscMiddleClickDespawn(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!g_MiscMiddleClickDespawn)
        return;
    // Still allow while menu open so testing isn't blocked; skip only if ImGui wants the mouse
    if (ImGuiMenu_IsOpen() && ImGui::GetIO().WantCaptureMouse)
        return;

    int key = g_BindMiddleClickDespawn ? g_BindMiddleClickDespawn : VK_MBUTTON;
    if (!KeyPressedEdge(key))
        return;
    if (!IsValidPtr(worldPtr))
        return;

    // Rate-limit — rapid bursts were the crash repro
    static DWORD s_LastDespawn = 0;
    DWORD nowDespawn = GetTickCount();
    if (nowDespawn - s_LastDespawn < 350)
        return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    float best = 1e12f;
    uintptr_t bestEnt = 0;
    float maxDist = (float)(g_MiscDespawnRange > 1 ? g_MiscDespawnRange : 25);
    float maxScreen = (g_ScreenWidth < g_ScreenHeight ? g_ScreenWidth : g_ScreenHeight) * 0.45f;
    if (maxScreen < 180.f) maxScreen = 180.f;
    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;

    auto consider = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            return;
        Vec3 pos;
        if (!GetEntityPosition(ent, pos)) return;
        if (Distance3D(localPos, pos) > maxDist) return;

        // Never soft-despawn players — networked CDP ownership + fake death = heap corruption.
        // Infected / animals / base parts only.
        bool isP = false, isZ = false;
        if (CombatEntityIsPlayerOrZombie(ent, isP, isZ))
        {
            if (isP) return;
            if (!isZ) return;
        }
        else
        {
            char cfg[64] = {};
            char tn[64] = {};
            ReadEntityConfigName(ent, cfg, 64);
            ReadEntityTypeName(ent, tn, 64);
            if (!cfg[0] && !tn[0]) return;
            bool animal = StrCmpI(cfg, "dayzanimal") == 0;
            bool infected = StrCmpI(cfg, "dayzinfected") == 0;
            bool base = MiscIsBaseBuildingName(tn) || MiscIsBaseBuildingName(cfg);
            if (!animal && !infected && !base)
                return;
            if (StrCmpI(cfg, "dayzplayer") == 0)
                return;
        }
        Vec2 scr;
        if (!WorldToScreen(pos, scr) && !WorldToScreen({ pos.x, pos.y + 1.f, pos.z }, scr)
            && !WorldToScreen({ pos.x, pos.y + 1.6f, pos.z }, scr))
            return;
        float sdx = scr.x - cx, sdy = scr.y - cy;
        float sd = sqrtf(sdx * sdx + sdy * sdy);
        if (sd < best && sd < maxScreen)
        {
            best = sd;
            bestEnt = ent;
        }
    };

    const uintptr_t lists[] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = 0; int count = 0;
        if (!ResolveEntityList(worldPtr, lists[li], 2000, data, count, nullptr))
        {
            data = Read<uintptr_t>(worldPtr + lists[li]);
            count = Read<int>(worldPtr + lists[li] + 8);
            if (li == 1)
            {
                int farC = Read<int>(worldPtr + oak_offsets::world::FarTableSize);
                if (farC > 0 && farC < 2000) count = farC;
            }
            if (!IsValidPtr(data) || count <= 0 || count > 2000) continue;
        }
        int n = count > 400 ? 400 : count;
        for (int i = 0; i < n; i++)
            consider(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        if (IsValidPtr(data) && count > 0 && count < 2000)
        {
            int n = count > 400 ? 400 : count;
            for (int i = 0; i < n; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                consider(Read<uintptr_t>(entry + 8));
            }
        }
    }

    if (bestEnt)
    {
        s_LastDespawn = nowDespawn;
        MiscDespawnEntity(bestEnt);
        char buf[96];
        wsprintfA(buf, "misc: despawn ok scr=%d", (int)best);
        Log(buf);
    }
    else
        Log("misc: despawn — no target under crosshair");
}

static void MiscLootMagnet(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!g_MiscLootMagnet)
        return;
    if (g_BindLootMagnet != 0 && !KeyHeld(g_BindLootMagnet))
        return;
    if (!IsValidPtr(worldPtr) || !(g_LocalPlayerValid || g_MiscFreecam))
        return;

    // ~20 Hz — writing every Present is wasted work and churns memory
    static DWORD s_Last = 0;
    DWORD now = GetTickCount();
    if (now - s_Last < 100)
        return;
    s_Last = now;

    float range = (float)(g_MiscLootMagnetRange > 1 ? g_MiscLootMagnetRange : 8);
    uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
    int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
    if (!IsValidPtr(data) || count <= 0 || count > 200000)
        return;

    Vec3 dest = g_MiscFreecam ? g_CameraPos : g_LocalPlayerPos;
    dest.y += 0.35f;
    Vec3 origin = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    int pulled = 0;
    int n = count > 160 ? 160 : count;
    for (int i = 0; i < n; i++)
    {
        uintptr_t entry = data + (uintptr_t)i * 0x18;
        if (Read<WORD>(entry) != 1) continue;
        uintptr_t ent = Read<uintptr_t>(entry + 8);
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            continue;

        Vec3 pos;
        if (!GetEntityPosition(ent, pos)) continue;
        if (Distance3D(origin, pos) > range) continue;

        // Cheap player/zombie reject — avoid full CombatEntityIsPlayerOrZombie string walk
        char cfg[48] = {};
        if (ReadEntityConfigName(ent, cfg, 48))
        {
            if (StrCmpI(cfg, "dayzplayer") == 0 || StrCmpI(cfg, "dayzinfected") == 0)
                continue;
        }

        uintptr_t vs = Read<uintptr_t>(ent + offsets::entity::VisualState);
        if (!IsValidPtr(vs) || vs < 0x100000000) continue;
        if (!Write<float>(vs + 0x2C, dest.x)) continue;
        Write<float>(vs + 0x30, dest.y);
        Write<float>(vs + 0x34, dest.z);
        if (++pulled >= 20) break;
    }
    (void)pulled;
}

// Pull chests/barrels/crates/tents through walls (same VisualState trick as loot magnet).
static void MiscContainerMagnet(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!g_MiscContainerMagnet)
        return;
    if (g_BindContainerMagnet != 0 && !KeyHeld(g_BindContainerMagnet))
        return;
    if (!IsValidPtr(worldPtr) || !(g_LocalPlayerValid || g_MiscFreecam))
        return;

    static DWORD s_Last = 0;
    DWORD now = GetTickCount();
    if (now - s_Last < 120)
        return;
    s_Last = now;

    float range = (float)(g_MiscContainerMagnetRange > 1 ? g_MiscContainerMagnetRange : 60);
    Vec3 dest = g_MiscFreecam ? g_CameraPos : g_LocalPlayerPos;
    dest.y += 0.4f;
    Vec3 origin = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    int pulled = 0;

    auto tryPull = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            return;
        Vec3 pos;
        if (!GetEntityPosition(ent, pos)) return;
        if (Distance3D(origin, pos) > range) return;

        char tn[64] = {};
        char cfg[64] = {};
        ReadEntityTypeName(ent, tn, 64);
        ReadEntityConfigName(ent, cfg, 64);
        if (!IsContainerName(tn) && !IsContainerName(cfg))
            return;
        if (MiscIsBaseBuildingName(tn) || MiscIsBaseBuildingName(cfg))
            return;
        uintptr_t vs = Read<uintptr_t>(ent + offsets::entity::VisualState);
        if (!IsValidPtr(vs) || vs < 0x100000000) return;
        if (!Write<float>(vs + 0x2C, dest.x)) return;
        Write<float>(vs + 0x30, dest.y);
        Write<float>(vs + 0x34, dest.z);
        pulled++;
    };

    // Containers often live in ItemList and SlowEntList
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
        int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
        if (IsValidPtr(data) && count > 0 && count < 200000)
        {
            int n = count > 300 ? 300 : count;
            for (int i = 0; i < n && pulled < 20; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                tryPull(Read<uintptr_t>(entry + 8));
            }
        }
    }
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        if (IsValidPtr(data) && count > 0 && count < 2000)
        {
            int n = count > 400 ? 400 : count;
            for (int i = 0; i < n && pulled < 20; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                tryPull(Read<uintptr_t>(entry + 8));
            }
        }
    }
    // Skip Near/Far once Item+Slow already filled the pull budget.
    if (pulled >= 20)
        return;
    const uintptr_t lists[] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2 && pulled < 20; li++)
    {
        uintptr_t data = 0; int count = 0;
        if (!ResolveEntityList(worldPtr, lists[li], 2000, data, count, nullptr))
        {
            data = Read<uintptr_t>(worldPtr + lists[li]);
            count = Read<int>(worldPtr + lists[li] + 8);
            if (li == 1)
            {
                int farC = Read<int>(worldPtr + oak_offsets::world::FarTableSize);
                if (farC > 0 && farC < 2000) count = farC;
            }
            if (!IsValidPtr(data) || count <= 0 || count > 2000) continue;
        }
        int n = count > 300 ? 300 : count;
        for (int i = 0; i < n && pulled < 20; i++)
            tryPull(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }

    if (pulled > 0)
    {
        static int s_Log = 0;
        if ((s_Log++ % 60) == 0)
        {
            char b[72];
            wsprintfA(b, "misc: container magnet pulled %d", pulled);
            Log(b);
        }
    }
}

// Cannot CreateObject from usermode — teleport nearest existing fence/gate/wall to feet.
static void MiscPullNearestBasePart(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!IsValidPtr(worldPtr) || !g_LocalPlayerValid)
    {
        Log("misc: pull base — need valid local player in-world");
        return;
    }

    uintptr_t best = 0;
    float bestD = 1e12f;
    char bestName[64] = {};
    Vec3 localPos = g_LocalPlayerPos;

    auto consider = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            return;
        char tn[64] = {};
        char cfg[64] = {};
        ReadEntityTypeName(ent, tn, 64);
        ReadEntityConfigName(ent, cfg, 64);
        if (!MiscIsBaseBuildingName(tn) && !MiscIsBaseBuildingName(cfg))
            return;
        Vec3 pos;
        if (!GetEntityPosition(ent, pos)) return;
        float d = Distance3D(localPos, pos);
        if (d < bestD)
        {
            bestD = d;
            best = ent;
            if (tn[0]) lstrcpynA(bestName, tn, 64);
            else lstrcpynA(bestName, cfg, 64);
        }
    };

    const uintptr_t lists[] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = 0; int count = 0;
        if (!ResolveEntityList(worldPtr, lists[li], 4000, data, count, nullptr))
        {
            data = Read<uintptr_t>(worldPtr + lists[li]);
            count = Read<int>(worldPtr + lists[li] + 8);
            if (li == 1)
            {
                int farC = Read<int>(worldPtr + oak_offsets::world::FarTableSize);
                if (farC > 0 && farC < 4000) count = farC;
            }
            if (!IsValidPtr(data) || count <= 0 || count > 4000) continue;
        }
        int n = count > 300 ? 300 : count;
        for (int i = 0; i < n; i++)
            consider(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        if (IsValidPtr(data) && count > 0 && count < 4000)
        {
            int n = count > 300 ? 300 : count;
            for (int i = 0; i < n; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                consider(Read<uintptr_t>(entry + 8));
            }
        }
    }
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
        int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
        if (IsValidPtr(data) && count > 0 && count < 200000)
        {
            int n = count > 1500 ? 1500 : count;
            for (int i = 0; i < n; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if (Read<WORD>(entry) != 1) continue;
                consider(Read<uintptr_t>(entry + 8));
            }
        }
    }

    if (!best)
    {
        Log("misc: pull base — no fence/gate/wall found in world lists");
        return;
    }

    uintptr_t vs = Read<uintptr_t>(best + offsets::entity::VisualState);
    if (!IsValidPtr(vs) || vs < 0x100000000)
    {
        Log("misc: pull base — target has no VisualState");
        return;
    }
    Write<float>(vs + 0x2C, localPos.x + 2.0f);
    Write<float>(vs + 0x30, localPos.y);
    Write<float>(vs + 0x34, localPos.z + 0.5f);
    char buf[128];
    wsprintfA(buf, "misc: pulled '%s' from %dm to feet", bestName[0] ? bestName : "base", (int)bestD);
    Log(buf);
}

static void MiscDrawWaypoints()
{
    if (!g_MiscDrawWaypoints || g_WaypointCount <= 0 || g_PanicHidden)
        return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    Vec3 ref = g_MiscFreecam && g_CameraValid ? g_CameraPos
        : (g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos);
    for (int i = 0; i < g_WaypointCount; i++)
    {
        Vec3 w{ g_WaypointX[i], g_WaypointY[i], g_WaypointZ[i] };
        Vec2 s;
        if (!WorldToScreen(w, s)) continue;
        float dist = Distance3D(ref, w);
        char label[80];
        wsprintfA(label, "%s  %dm", g_WaypointName[i][0] ? g_WaypointName[i] : "WP", (int)dist);

        const float* wc = g_WaypointColor[i];
        float wr = wc[0], wg = wc[1], wb = wc[2], wa = wc[3] > 0.01f ? wc[3] : 0.95f;
        if (wa < 0.01f)
        {
            wr = 1.f; wg = 0.78f; wb = 0.28f; wa = 0.95f;
        }
        ImU32 fill = ToCol(wr, wg, wb, wa);
        ImU32 ring = IM_COL32(20, 16, 8, 220);
        ImU32 stem = ToCol(wr, wg, wb, wa * 0.55f);
        dl->AddLine(ImVec2(s.x, s.y), ImVec2(s.x, s.y + 18.f), stem, 2.0f);
        dl->AddCircleFilled(ImVec2(s.x, s.y), 5.5f, ring, 10);
        dl->AddCircleFilled(ImVec2(s.x, s.y), 3.8f, fill, 10);
        DrawEspBadge(s.x, s.y + 20.f, label, wr, wg, wb, wa);
    }
}

static Vec3 g_FreecamPos = {};
static Vec3 g_FreecamVel = {}; // COM CameraTool-style velocity for smooth fly
static Vec3 g_FreecamBodyFreeze = {};
static float g_FreecamBodyRot[9] = {}; // VisualState 3x3 at +0x8 — freeze facing
static bool g_FreecamBodyRotValid = false;
static bool g_FreecamArmed = false;
static bool g_FreecamBodyFreezeValid = false;
static float g_FreecamYaw = 0.f;   // radians, freecam-owned look
static float g_FreecamPitch = 0.f;
static volatile LONG g_FcMouseAccumDx = 0;
static volatile LONG g_FcMouseAccumDy = 0;
static LARGE_INTEGER g_FreecamQpcLast = {};
static LARGE_INTEGER g_FreecamQpcFreq = {};
static unsigned g_FreecamFrameStamp = 0xFFFFFFFF;

static void MiscFreecamOnRawMouse(int dx, int dy)
{
    if (dx) InterlockedAdd(&g_FcMouseAccumDx, dx);
    if (dy) InterlockedAdd(&g_FcMouseAccumDy, dy);
}

// Camera VT[2] = full matrix update (rewrites position from player).
// Camera VT[6] = rotation-only update. We nop BOTH so freecam owns pos+look;
// mouse is eaten for the pawn and applied only to freecam yaw/pitch.
static bool g_FreecamVtSwapped = false;
static uintptr_t g_FreecamVtTable = 0;
static uintptr_t g_FreecamVtOrig2 = 0;
static uintptr_t g_FreecamVtOrig6 = 0;
static void* g_FreecamCamNop = nullptr; // executable `ret` stub


// Shared NoGrass (Misc toggle and/or freecam clutter crash-guard)
static bool g_NoGrassHeld = false;
static unsigned int g_NoGrassOrig = 0;
static uintptr_t g_NoGrassWorld = 0;

static float MiscVecLen2(float x, float z)
{
    return Sqrt(x * x + z * z);
}

static bool MiscFreecamPtrInGameModule(uintptr_t p)
{
    if (!g_GameModule || !p)
        return false;
    uintptr_t base = (uintptr_t)g_GameModule;
    return p >= base && p < base + 0x8000000ULL;
}

static bool MiscFreecamWriteVtSlot(uintptr_t slotAddr, uintptr_t value)
{
    if (!slotAddr || slotAddr < 0x10000)
        return false;
    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)slotAddr, sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    bool ok = false;
    __try
    {
        *(uintptr_t*)slotAddr = value;
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    DWORD tmp = 0;
    VirtualProtect((LPVOID)slotAddr, sizeof(uintptr_t), oldProt, &tmp);
    return ok;
}

// Shared NoGrass — ONLY the explicit Misc toggle.
// Do NOT auto-tie to freecam: toggling World::NoGrass rebuilds grass streaming and
// commonly pins the client ~30 FPS for a while. Do NOT write 0xBF0 (unsafe).
static void MiscApplyNoGrass(uintptr_t worldPtr)
{
    const bool want = g_MiscNoGrass && !g_ShuttingDown && !g_PanicHidden;
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;

    if (!want)
    {
        if (g_NoGrassHeld)
        {
            uintptr_t w = (IsValidPtr(g_NoGrassWorld) && g_NoGrassWorld > 0x100000000) ? g_NoGrassWorld : worldPtr;
            Write<unsigned int>(w + offsets::world::NoGrass, g_NoGrassOrig);
            g_NoGrassHeld = false;
            g_NoGrassWorld = 0;
        }
        return;
    }

    if (!g_NoGrassHeld)
    {
        g_NoGrassOrig = Read<unsigned int>(worldPtr + offsets::world::NoGrass);
        g_NoGrassWorld = worldPtr;
        g_NoGrassHeld = true;
        Write<unsigned int>(worldPtr + offsets::world::NoGrass, 0); // once on engage — not every frame
    }
}

// Private per-camera vtable copy — NEVER patch DayZ's shared .rdata VT (that crashed).
// Owner must be explicit: freecam off used to tear down soft-3PP every frame
// because both features shared g_FreecamVtSwapped alone.
static uintptr_t* g_FreecamFakeVt = nullptr;
static uintptr_t g_FreecamCameraObj = 0;
static uintptr_t g_FreecamRealVt = 0;
static const size_t kFreecamVtSlots = 32;
enum CamVtOwner : int { kCamVtNone = 0, kCamVtFreecam = 1, kCamVtSoft3pp = 2 };
static int g_CamVtOwner = kCamVtNone;

static void MiscFreecamRestoreVt()
{
    if (g_FreecamCameraObj && g_FreecamRealVt)
    {
        __try
        {
            if (IsValidPtr(g_FreecamCameraObj) && g_FreecamCameraObj > 0x100000000)
            {
                uintptr_t cur = 0;
                __try { cur = *(uintptr_t*)g_FreecamCameraObj; }
                __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
                // Only restore if we still own the slot
                if (cur == (uintptr_t)g_FreecamFakeVt || cur == g_FreecamRealVt)
                {
                    DWORD oldProt = 0;
                    if (VirtualProtect((LPVOID)g_FreecamCameraObj, sizeof(uintptr_t), PAGE_READWRITE, &oldProt))
                    {
                        __try { *(uintptr_t*)g_FreecamCameraObj = g_FreecamRealVt; }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                        DWORD tmp = 0;
                        VirtualProtect((LPVOID)g_FreecamCameraObj, sizeof(uintptr_t), oldProt, &tmp);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_FreecamVtSwapped = false;
    g_CamVtOwner = kCamVtNone;
    g_FreecamVtTable = 0;
    g_FreecamCameraObj = 0;
    g_FreecamRealVt = 0;
    g_FreecamVtOrig2 = 0;
    g_FreecamVtOrig6 = 0;
}

// Private camera VT:
//   freecam (nopLook): nop VT[2] (pos) + VT[6] (look) — freecam owns both
//   soft-3PP (!nopLook): nop VT[2] only — engine look (VT[6]) stays live so mouse
//     still drives the basis; we own translation every Present. Writing pos without
//     nopping VT[2] only moved W2S/ESP (after Present) while the real view stayed 1PP.
static void MiscCamOverrideInstall(uintptr_t camera, bool nopLook, int forceOwner = -1)
{
    if (!IsValidPtr(camera) || camera < 0x100000000)
        return;

    const int wantOwner = (forceOwner >= 0)
        ? forceOwner
        : (nopLook ? kCamVtFreecam : kCamVtSoft3pp);

    // Freecam wins over everything.
    if (g_CamVtOwner == kCamVtFreecam && wantOwner != kCamVtFreecam)
        return;

    // Already installed for this camera with the right nop set?
    if (g_FreecamVtSwapped && g_FreecamCameraObj == camera && g_FreecamFakeVt && g_FreecamCamNop)
    {
        if (nopLook)
        {
            g_FreecamFakeVt[2] = (uintptr_t)g_FreecamCamNop;
            g_FreecamFakeVt[6] = (uintptr_t)g_FreecamCamNop;
        }
        else
        {
            // Soft-3PP: kill pos update only; restore look slot to real VT[6].
            g_FreecamFakeVt[2] = (uintptr_t)g_FreecamCamNop;
            if (g_FreecamVtOrig6)
                g_FreecamFakeVt[6] = g_FreecamVtOrig6;
        }
        uintptr_t cur = 0;
        __try { cur = *(uintptr_t*)camera; }
        __except (EXCEPTION_EXECUTE_HANDLER) { cur = 0; }
        if (cur != (uintptr_t)g_FreecamFakeVt)
        {
            DWORD oldProt = 0;
            if (VirtualProtect((LPVOID)camera, sizeof(uintptr_t), PAGE_READWRITE, &oldProt))
            {
                __try { *(uintptr_t*)camera = (uintptr_t)g_FreecamFakeVt; }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                DWORD tmp = 0;
                VirtualProtect((LPVOID)camera, sizeof(uintptr_t), oldProt, &tmp);
            }
        }
        g_CamVtOwner = wantOwner;
        return;
    }

    if (g_FreecamVtSwapped)
        MiscFreecamRestoreVt();

    uintptr_t vtable = 0;
    __try { vtable = *(uintptr_t*)camera; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!MiscFreecamPtrInGameModule(vtable))
        return;

    uintptr_t fn2 = 0, fn6 = 0;
    __try
    {
        fn2 = *(uintptr_t*)(vtable + 2 * sizeof(uintptr_t));
        fn6 = *(uintptr_t*)(vtable + 6 * sizeof(uintptr_t));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!MiscFreecamPtrInGameModule(fn2) || !MiscFreecamPtrInGameModule(fn6) || fn2 == fn6)
        return;

    if (!g_FreecamFakeVt)
    {
        g_FreecamFakeVt = (uintptr_t*)VirtualAlloc(nullptr, kFreecamVtSlots * sizeof(uintptr_t),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_FreecamFakeVt)
            return;
    }

    __try
    {
        for (size_t i = 0; i < kFreecamVtSlots; i++)
            g_FreecamFakeVt[i] = *(uintptr_t*)(vtable + i * sizeof(uintptr_t));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!g_FreecamCamNop)
    {
        g_FreecamCamNop = VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_FreecamCamNop)
            *(unsigned char*)g_FreecamCamNop = 0xC3; // ret
    }
    if (!g_FreecamCamNop)
        return;

    g_FreecamVtOrig2 = fn2;
    g_FreecamVtOrig6 = fn6;
    g_FreecamFakeVt[2] = (uintptr_t)g_FreecamCamNop;
    if (nopLook)
        g_FreecamFakeVt[6] = (uintptr_t)g_FreecamCamNop;
    // else keep copied VT[6] (engine look)

    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)camera, sizeof(uintptr_t), PAGE_READWRITE, &oldProt))
        return;
    bool ok = false;
    __try
    {
        *(uintptr_t*)camera = (uintptr_t)g_FreecamFakeVt;
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    DWORD tmp = 0;
    VirtualProtect((LPVOID)camera, sizeof(uintptr_t), oldProt, &tmp);
    if (!ok)
        return;

    g_FreecamCameraObj = camera;
    g_FreecamRealVt = vtable;
    g_FreecamVtTable = (uintptr_t)g_FreecamFakeVt;
    g_FreecamVtSwapped = true;
    g_CamVtOwner = wantOwner;
}

static void MiscFreecamDisconnectCamera(uintptr_t camera)
{
    MiscCamOverrideInstall(camera, true);
    if (g_FreecamVtSwapped && g_FreecamCameraObj == camera)
    {
        static bool s_Logged = false;
        if (!s_Logged)
        {
            s_Logged = true;
            Log("freecam: private camera VT (pos+look owned, body frozen)");
        }
    }
}

static void MiscFreecamWriteCamPos(uintptr_t camera, const Vec3& pos)
{
    if (!IsValidPtr(camera) || camera < 0x100000000)
        return;
    Write<float>(camera + offsets::camera::InvertedViewTranslation + 0, pos.x);
    Write<float>(camera + offsets::camera::InvertedViewTranslation + 4, pos.y);
    Write<float>(camera + offsets::camera::InvertedViewTranslation + 8, pos.z);
}

static bool g_3ppArmed = false;
static bool g_3ppSoftCamWasOn = false; // legacy soft-cam leftover cleanup
static bool g_3ppViewOn = true;
static char g_3ppQa[56] = "off";

// Vanilla 3PP unlock (NOT soft-cam):
// 1) Patch Is3rdPersonDisabled @ RVA 0x8ED160 → xor al,al; ret
//    NOTE: 0x8ED190 is IsCrosshairDisabled (cmp [rax+0x78]) — do NOT patch that.
//    Real gate checks dword [rax+0x74] (m_Disable3rdPerson).
// 2) Patch World-allow writer disable path @ RVA 0xA85292: xor cl,cl → mov cl,1
//    Layout: jne allow; xor cl,cl; jmp store; mov cl,1; … mov [rax+0x2984],cl
// 3) Hold World+0x2984=1 and Network+0x9C=1 each Present
// Stock V / CameraViewChanged then drives the real engine 3PP camera+body.
// NEVER write via ManagerNetworkClient (+0x50) — BAN_NET / CDP AV.
static constexpr size_t k3ppPatchN = 3;
static BYTE g_3ppOrig[16] = {};
static BYTE* g_3ppFn = nullptr;
static bool g_3ppPatched = false;

static constexpr UINT_PTR k3ppIs3rdPersonDisabledRva = 0x8ED160ull; // gate cmp [rax+0x74] (string native was wrong @ 0x8F5F40)
static constexpr UINT_PTR k3ppWorldAllowWriterRva = 0xA85292ull; // xor cl,cl (disable path)
static constexpr size_t k3ppWorldAllowStoreOff = 0x1A; // from writer: … 88 88 84 29
// Bulk settings copies that also store World+0x2984 from a register (must force imm 1).
static constexpr UINT_PTR k3ppWorldAllowCopyRvas[] = {
    0x53BA14ull,
    0x5F4A34ull,
};
static BYTE g_3ppAllowOrig[2] = {};
static BYTE* g_3ppAllowFn = nullptr;
static bool g_3ppAllowPatched = false;
static BYTE g_3ppCopyOrig[2][7] = {};
static BYTE* g_3ppCopyFn[2] = {};
static bool g_3ppCopyPatched[2] = {};
static constexpr uintptr_t kWorldThirdPersonAllow = 0x2984ull;

static BYTE* MiscFindIs3rdPersonDisabled()
{
    if (!g_GameModule)
        return nullptr;
    // Preferred: registered native Is3rdPersonDisabled (cmp dword [rax+0x74], 0).
    BYTE* cand = (BYTE*)g_GameModule + k3ppIs3rdPersonDisabledRva;
    __try
    {
        if (cand[0] == 0x48 && cand[1] == 0x83 && cand[2] == 0xEC && cand[3] == 0x28 &&
            cand[17] == 0x48 && cand[18] == 0x85 && cand[19] == 0xC0 &&
            cand[22] == 0x83 && cand[23] == 0x78 && cand[24] == 0x74 && cand[25] == 0x00 &&
            cand[26] == 0x74 && cand[27] == 0x07 && cand[28] == 0xB0 && cand[29] == 0x01)
            return cand;
        // Already patched?
        if (cand[0] == 0x32 && cand[1] == 0xC0 && cand[2] == 0xC3)
            return cand;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    HMODULE mod = (HMODULE)g_GameModule;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
        return nullptr;
    BYTE* base = (BYTE*)mi.lpBaseOfDll;
    SIZE_T size = mi.SizeOfImage;
    if (size < 0x1000 || size > 0x20000000)
        return nullptr;
    __try
    {
        for (SIZE_T i = 0; i + 42 < size; i++)
        {
            BYTE* p = base + i;
            if (p[0] != 0x48 || p[1] != 0x83 || p[2] != 0xEC || p[3] != 0x28)
                continue;
            if (p[17] != 0x48 || p[18] != 0x85 || p[19] != 0xC0)
                continue;
            // Must be +0x74 (3PP). +0x78 is IsCrosshairDisabled — skip.
            if (p[22] != 0x83 || p[23] != 0x78 || p[24] != 0x74 || p[25] != 0x00)
                continue;
            if (p[26] != 0x74 || p[27] != 0x07 || p[28] != 0xB0 || p[29] != 0x01)
                continue;
            return p;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static void Misc3ppRestoreAllowWriter()
{
    if (g_3ppAllowPatched && g_3ppAllowFn)
    {
        __try
        {
            DWORD old = 0;
            if (VirtualProtect(g_3ppAllowFn, 2, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(g_3ppAllowFn, g_3ppAllowOrig, 2);
                FlushInstructionCache(GetCurrentProcess(), g_3ppAllowFn, 2);
                VirtualProtect(g_3ppAllowFn, 2, old, &old);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_3ppAllowPatched = false;
    }
    for (int i = 0; i < 2; i++)
    {
        if (!g_3ppCopyPatched[i] || !g_3ppCopyFn[i])
            continue;
        __try
        {
            DWORD old = 0;
            if (VirtualProtect(g_3ppCopyFn[i], 7, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(g_3ppCopyFn[i], g_3ppCopyOrig[i], 7);
                FlushInstructionCache(GetCurrentProcess(), g_3ppCopyFn[i], 7);
                VirtualProtect(g_3ppCopyFn[i], 7, old, &old);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_3ppCopyPatched[i] = false;
    }
}

static bool Misc3ppInstallAllowWriter()
{
    if (!g_GameModule)
        return false;
    bool ok = g_3ppAllowPatched;
    if (!g_3ppAllowPatched)
    {
        BYTE* p = (BYTE*)g_GameModule + k3ppWorldAllowWriterRva;
        __try
        {
            // Disable path at RVA 0xA85292: 32 C9 (xor cl,cl) → must become B1 01.
            // Do NOT patch 0xA85DC6 (that is already the allow branch mov cl,1).
            // Store mov [rax+0x2984],cl is +0x1A from the xor.
            if ((p[0] == 0x32 && p[1] == 0xC9) || (p[0] == 0xB1 && p[1] == 0x01))
            {
                if (p[k3ppWorldAllowStoreOff] == 0x88 && p[k3ppWorldAllowStoreOff + 1] == 0x88 &&
                    p[k3ppWorldAllowStoreOff + 2] == 0x84 && p[k3ppWorldAllowStoreOff + 3] == 0x29)
                {
                    if (p[0] == 0xB1 && p[1] == 0x01)
                    {
                        g_3ppAllowFn = p;
                        g_3ppAllowPatched = true;
                        ok = true;
                    }
                    else
                    {
                        DWORD old = 0;
                        if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old))
                        {
                            memcpy(g_3ppAllowOrig, p, 2);
                            p[0] = 0xB1; // mov cl, 1 (force allow even when disable3rdPerson=1)
                            p[1] = 0x01;
                            FlushInstructionCache(GetCurrentProcess(), p, 2);
                            VirtualProtect(p, 2, old, &old);
                            g_3ppAllowFn = p;
                            g_3ppAllowPatched = true;
                            ok = true;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Force bulk setting copies to write imm 1 into World+0x2984.
    for (int i = 0; i < 2; i++)
    {
        if (g_3ppCopyPatched[i])
            continue;
        BYTE* p = (BYTE*)g_GameModule + k3ppWorldAllowCopyRvas[i];
        __try
        {
            // 40 88 A9/AB 84 29 00 00
            if (p[0] != 0x40 || p[1] != 0x88 || p[3] != 0x84 || p[4] != 0x29 || p[5] != 0 || p[6] != 0)
                continue;
            const BYTE modrm = p[2]; // A9=[rcx+disp32] r8b, AB=[rbx+disp32] r8b
            BYTE immModrm = 0;
            if (modrm == 0xA9) immModrm = 0x81; // [rcx+disp32]
            else if (modrm == 0xAB) immModrm = 0x83; // [rbx+disp32]
            else
                continue;
            DWORD old = 0;
            if (!VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old))
                continue;
            memcpy(g_3ppCopyOrig[i], p, 7);
            p[0] = 0xC6;
            p[1] = immModrm;
            p[2] = 0x84; p[3] = 0x29; p[4] = 0; p[5] = 0;
            p[6] = 0x01; // imm8 = 1 (allow)
            FlushInstructionCache(GetCurrentProcess(), p, 7);
            VirtualProtect(p, 7, old, &old);
            g_3ppCopyFn[i] = p;
            g_3ppCopyPatched[i] = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return ok && g_3ppCopyPatched[0] && g_3ppCopyPatched[1];
}

static void Misc3ppRestoreNative()
{
    if (g_3ppPatched && g_3ppFn)
    {
        __try
        {
            DWORD old = 0;
            if (VirtualProtect(g_3ppFn, k3ppPatchN, PAGE_EXECUTE_READWRITE, &old))
            {
                memcpy(g_3ppFn, g_3ppOrig, k3ppPatchN);
                FlushInstructionCache(GetCurrentProcess(), g_3ppFn, k3ppPatchN);
                VirtualProtect(g_3ppFn, k3ppPatchN, old, &old);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_3ppPatched = false;
    }
    Misc3ppRestoreAllowWriter();
}

static bool Misc3ppInstallNative()
{
    if (g_3ppPatched)
        return true;
    if (!g_3ppFn)
        g_3ppFn = MiscFindIs3rdPersonDisabled();
    if (!g_3ppFn)
        return false;
    __try
    {
        if (g_3ppFn[0] == 0x32 && g_3ppFn[1] == 0xC0 && g_3ppFn[2] == 0xC3)
        {
            g_3ppPatched = true;
            return true;
        }
        DWORD old = 0;
        if (!VirtualProtect(g_3ppFn, k3ppPatchN, PAGE_EXECUTE_READWRITE, &old))
            return false;
        memcpy(g_3ppOrig, g_3ppFn, k3ppPatchN);
        g_3ppFn[0] = 0x32;
        g_3ppFn[1] = 0xC0;
        g_3ppFn[2] = 0xC3;
        FlushInstructionCache(GetCurrentProcess(), g_3ppFn, k3ppPatchN);
        VirtualProtect(g_3ppFn, k3ppPatchN, old, &old);
        g_3ppPatched = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void Misc3ppForceWorldAllow(uintptr_t worldPtr)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;
    __try
    {
        // allow=1 — always rewrite (net/bulk paths race Present)
        Write<unsigned char>(worldPtr + kWorldThirdPersonAllow, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Vanilla third person — hijack engine allow path; no soft shoulder-cam.
static void MiscApplyThirdPerson(uintptr_t worldPtr, uintptr_t camera, uintptr_t localPlayer)
{
    OAK_MARK("3pp");
    if (!g_WorldMisc.thirdPerson || g_PanicHidden || g_ShuttingDown)
    {
        if (g_3ppSoftCamWasOn && g_CamVtOwner == kCamVtSoft3pp)
        {
            MiscFreecamRestoreVt();
            g_3ppSoftCamWasOn = false;
        }
        if (g_3ppArmed || g_3ppPatched || g_3ppAllowPatched)
        {
            Misc3ppRestoreNative();
            g_3ppArmed = false;
            g_3ppViewOn = true;
            lstrcpynA(g_3ppQa, "off", 56);
            Log("3pp: off (vanilla unlock restored)");
        }
        return;
    }
    if (!g_GameModule)
        return;

    // Kill leftover soft-cam VT from the broken distance/height experiment.
    if (g_CamVtOwner == kCamVtSoft3pp || g_3ppSoftCamWasOn)
    {
        MiscFreecamRestoreVt();
        g_3ppSoftCamWasOn = false;
    }

    const bool gate = Misc3ppInstallNative();
    const bool allowW = Misc3ppInstallAllowWriter();
    Misc3ppForceWorldAllow(worldPtr);

    __try
    {
        uintptr_t net = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::Network);
        if (IsValidPtr(net) && net > 0x100000000 && net < 0x00007FFFFFFFFFFFULL)
        {
            if (Read<unsigned char>(net + oak_offsets::network::ThirdPersonFlag) != 1)
                Write<unsigned char>(net + oak_offsets::network::ThirdPersonFlag, 1);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    unsigned char worldAllow = 0;
    __try
    {
        if (IsValidPtr(worldPtr))
            worldAllow = Read<unsigned char>(worldPtr + kWorldThirdPersonAllow);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (!g_3ppArmed)
    {
        g_3ppArmed = true;
        wsprintfA(g_3ppQa, "vanilla gate=%d wr=%d c0=%d c1=%d w=%u",
            gate ? 1 : 0, allowW ? 1 : 0,
            g_3ppCopyPatched[0] ? 1 : 0, g_3ppCopyPatched[1] ? 1 : 0,
            (unsigned)worldAllow);
        char b[220];
        wsprintfA(b, "3pp: VANILLA unlock gate=%d writer=%d copies=%d/%d w2984=%u — stock V",
            gate ? 1 : 0, g_3ppAllowPatched ? 1 : 0,
            g_3ppCopyPatched[0] ? 1 : 0, g_3ppCopyPatched[1] ? 1 : 0,
            (unsigned)worldAllow);
        Log(b);
    }
    else
    {
        wsprintfA(g_3ppQa, "vanilla w2984=%u gate=%d wr=%d",
            (unsigned)worldAllow, gate ? 1 : 0, allowW ? 1 : 0);
    }
}

static void MiscFreecamWriteCamLook(uintptr_t camera, float yaw, float pitch)
{
    if (!IsValidPtr(camera) || camera < 0x100000000)
        return;
    // Y-up basis from yaw (around Y) + pitch
    const float cy = cosf(yaw), sy = sinf(yaw);
    const float cp = cosf(pitch), sp = sinf(pitch);
    Vec3 forward = { sy * cp, sp, cy * cp };
    Vec3 worldUp = { 0.f, 1.f, 0.f };
    // right = normalize(worldUp × forward)
    Vec3 right = {
        worldUp.y * forward.z - worldUp.z * forward.y,
        worldUp.z * forward.x - worldUp.x * forward.z,
        worldUp.x * forward.y - worldUp.y * forward.x
    };
    float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
    if (rl < 1e-4f)
        right = { cy, 0.f, -sy };
    else
    {
        right.x /= rl; right.y /= rl; right.z /= rl;
    }
    // up = forward × right
    Vec3 up = {
        forward.y * right.z - forward.z * right.y,
        forward.z * right.x - forward.x * right.z,
        forward.x * right.y - forward.y * right.x
    };
    Write<float>(camera + offsets::camera::InvertedViewRight + 0, right.x);
    Write<float>(camera + offsets::camera::InvertedViewRight + 4, right.y);
    Write<float>(camera + offsets::camera::InvertedViewRight + 8, right.z);
    Write<float>(camera + offsets::camera::InvertedViewUp + 0, up.x);
    Write<float>(camera + offsets::camera::InvertedViewUp + 4, up.y);
    Write<float>(camera + offsets::camera::InvertedViewUp + 8, up.z);
    Write<float>(camera + offsets::camera::InvertedViewForward + 0, forward.x);
    Write<float>(camera + offsets::camera::InvertedViewForward + 4, forward.y);
    Write<float>(camera + offsets::camera::InvertedViewForward + 8, forward.z);
}

// Soft body root — VisualState translation + frozen facing (no turn).
// Do NOT write FutureVisualState from Present (sim-thread owns it → AV).
static void MiscFreecamPinBodyTranslation(uintptr_t localPlayer, const Vec3& pos)
{
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
        return;
    __try
    {
        uintptr_t vs = Read<uintptr_t>(localPlayer + offsets::entity::VisualState);
        if (!IsValidPtr(vs) || vs < 0x100000000) return;
        if (g_FreecamBodyRotValid)
        {
            for (int i = 0; i < 9; i++)
                Write<float>(vs + 0x8 + (uintptr_t)i * 4, g_FreecamBodyRot[i]);
        }
        Write<float>(vs + 0x2C, pos.x);
        Write<float>(vs + 0x30, pos.y);
        Write<float>(vs + 0x34, pos.z);
        Write<float>(vs + 0x8 + 9 * 4, pos.x);
        Write<float>(vs + 0x8 + 10 * 4, pos.y);
        Write<float>(vs + 0x8 + 11 * 4, pos.z);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// HARD body-input kill while freecam is on:
// 1) IAT-hook GetAsyncKeyState/GetKeyState/GetKeyboardState in the game → always 0
//    (freecam uses o_* / MiscRealAsyncKey so WASD/QE fly still works)
// 2) PeekMessage strips keyboard + mouse-button + raw-keyboard (see main.cpp)
// 3) Float-only IC axis clear + VisualState pin (backup if engine bypasses user32)
static bool MiscFreecamHookIat(HMODULE mod, const char* dll, const char* func, PVOID hook, PVOID* orig)
{
    if (!mod || !dll || !func || !hook || !orig) return false;
    __try
    {
        BYTE* base = (BYTE*)mod;
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
                if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 32);
                if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 32);
                if (c1 != c2) { match = false; break; }
            }
            if (!match || (*a && *a != '.') || *b) continue;

            IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (; origThunk->u1.AddressOfData; ++thunk, ++origThunk)
            {
                if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
                if (lstrcmpA(ibn->Name, func) != 0) continue;
                DWORD old = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(PVOID), PAGE_READWRITE, &old))
                    return false;
                if (!*orig)
                    *orig = (PVOID)thunk->u1.Function;
                thunk->u1.Function = (ULONGLONG)hook;
                VirtualProtect(&thunk->u1.Function, sizeof(PVOID), old, &old);
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static bool MiscFreecamShouldHideKey(int vKey)
{
    if (!MiscFreecamGameInputSuppressed())
        return false;
    // Escape + Oak menu must always work — never trap the player in freecam/AA.
    if (vKey == VK_ESCAPE)
        return false;
    if (ImGuiMenu_IsOpen())
        return false;
    return true;
}

static SHORT WINAPI hk_FcGetAsyncKeyState(int vKey)
{
    SHORT v = o_GetAsyncKeyState ? o_GetAsyncKeyState(vKey) : GetAsyncKeyState(vKey);
    if (!MiscFreecamShouldHideKey(vKey))
        return v;
    return 0; // game sees no keys / mouse buttons
}

static SHORT WINAPI hk_FcGetKeyState(int nVirtKey)
{
    SHORT v = o_GetKeyState ? o_GetKeyState(nVirtKey) : GetKeyState(nVirtKey);
    if (!MiscFreecamShouldHideKey(nVirtKey))
        return v;
    return 0;
}

static BOOL WINAPI hk_FcGetKeyboardState(PBYTE lpKeyState)
{
    BOOL ok = o_GetKeyboardState ? o_GetKeyboardState(lpKeyState) : GetKeyboardState(lpKeyState);
    if (!ok || !lpKeyState || !MiscFreecamGameInputSuppressed() || ImGuiMenu_IsOpen())
        return ok;
    // Zero gameplay keys but keep Escape visible to DayZ / ImGui.
    BYTE esc = lpKeyState[VK_ESCAPE];
    memset(lpKeyState, 0, 256);
    lpKeyState[VK_ESCAPE] = esc;
    return ok;
}

static void MiscFreecamInstallInputHooks()
{
    if (InterlockedCompareExchange(&g_FcInputHooksInstalled, 1, 0) != 0)
        return;

    HMODULE mods[384];
    DWORD needed = 0;
    int hookedAsync = 0, hookedKey = 0, hookedKb = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
    {
        int n = (int)(needed / sizeof(HMODULE));
        if (n > 384) n = 384;
        for (int i = 0; i < n; i++)
        {
            PVOID discard = nullptr;
            if (MiscFreecamHookIat(mods[i], "user32.dll", "GetAsyncKeyState",
                    (PVOID)hk_FcGetAsyncKeyState, o_GetAsyncKeyState ? &discard : (PVOID*)&o_GetAsyncKeyState))
                hookedAsync++;
            discard = nullptr;
            if (MiscFreecamHookIat(mods[i], "user32.dll", "GetKeyState",
                    (PVOID)hk_FcGetKeyState, o_GetKeyState ? &discard : (PVOID*)&o_GetKeyState))
                hookedKey++;
            discard = nullptr;
            if (MiscFreecamHookIat(mods[i], "user32.dll", "GetKeyboardState",
                    (PVOID)hk_FcGetKeyboardState, o_GetKeyboardState ? &discard : (PVOID*)&o_GetKeyboardState))
                hookedKb++;
        }
    }
    // Always resolve real ptrs even if IAT miss (our freecam path)
    if (!o_GetAsyncKeyState)
        o_GetAsyncKeyState = (t_GetAsyncKeyState)GetProcAddress(GetModuleHandleA("user32.dll"), "GetAsyncKeyState");
    if (!o_GetKeyState)
        o_GetKeyState = (t_GetKeyState)GetProcAddress(GetModuleHandleA("user32.dll"), "GetKeyState");
    if (!o_GetKeyboardState)
        o_GetKeyboardState = (t_GetKeyboardState)GetProcAddress(GetModuleHandleA("user32.dll"), "GetKeyboardState");

    char b[160];
    wsprintfA(b, "freecam: input IAT hooks async=%d key=%d kb=%d", hookedAsync, hookedKey, hookedKb);
    Log(b);
}

static void MiscFreecamSetGameInputSuppressed(bool on)
{
    if (on)
        MiscFreecamInstallInputHooks();
    InterlockedExchange(&g_FcSuppressGameInput, on ? 1 : 0);
    if (on)
    {
        // Force-release stuck ADS / fire — DayZ raw-input can keep buttons latched
        INPUT up[2] = {};
        up[0].type = INPUT_MOUSE;
        up[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        up[1].type = INPUT_MOUSE;
        up[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        SendInput(2, up, sizeof(INPUT));
    }
}

static void MiscClearPlayerMoveAxes(uintptr_t localPlayer)
{
    // Zero InputController move axes so engine WASD doesn't fight camera-relative drive.
    // Does NOT suppress GetAsyncKeyState (Escape/menu stay live).
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
        return;
    __try
    {
        uintptr_t skel = Read<uintptr_t>(localPlayer + oak_offsets::player::Skeleton);
        uintptr_t ic = Read<uintptr_t>(localPlayer + oak_offsets::player::InputController);
        if (!IsValidPtr(ic) || ic < 0x100000000)
            return;
        if (skel && ic == skel)
            return;
        if (g_GameModule)
        {
            uintptr_t mod = (uintptr_t)g_GameModule;
            if (ic >= mod && ic < mod + 0x8000000ULL)
                return;
            uintptr_t vt = Read<uintptr_t>(ic);
            if (!(IsValidPtr(vt) && vt >= mod && vt < mod + 0x8000000ULL))
                return;
        }
        for (uintptr_t off = 0x10; off <= 0xC0; off += 4)
        {
            float f = Read<float>(ic + off);
            if (f != f) continue;
            float a = fabsf(f);
            if (a > 0.01f && a <= 1.0001f)
                Write<float>(ic + off, 0.f);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void MiscFreecamDisableBodyInput(uintptr_t localPlayer)
{
    MiscFreecamSetGameInputSuppressed(true);
    MiscClearPlayerMoveAxes(localPlayer);
}

static void MiscFreecamShutdown()
{
    MiscFreecamSetGameInputSuppressed(false);
    MiscFreecamRestoreVt();
    g_FreecamArmed = false;
    g_FreecamBodyFreezeValid = false;
    g_FreecamBodyRotValid = false;
    g_FreecamFrameStamp = 0xFFFFFFFF;
    g_FreecamQpcLast.QuadPart = 0;
    g_FreecamVel = {};
    InterlockedExchange(&g_FcMouseAccumDx, 0);
    InterlockedExchange(&g_FcMouseAccumDy, 0);
}
static void MiscApplyFreecam(uintptr_t worldPtr, uintptr_t camera, uintptr_t localPlayer)
{
    if (!g_MiscFreecam || g_PanicHidden || g_ShuttingDown)
    {
        // Only tear down when freecam itself was armed. Soft-3PP reuses the same
        // VT-swap flags — restoring here every frame made soft-cam affect ESP only.
        if (g_FreecamArmed)
        {
            MiscFreecamShutdown();
            Log("freecam: off");
        }
        return;
    }
    if (!IsValidPtr(camera) || camera < 0x100000000)
        return;

    // Brief warm-up only (was 2.5s — felt like freecam "didn't work")
    // Warp desync must pin immediately — zombies keep aggro if we wait.
    static DWORD s_FcWarm = 0;
    if (!g_WarpDesyncActive)
    {
        if (!s_FcWarm) s_FcWarm = GetTickCount();
        if ((GetTickCount() - s_FcWarm) < 400)
            return;
    }

    // Prefer the resolved dayzplayer. World::LocalPlayer is often a non-typed stub —
    // pinning that leaves the real pawn free to walk on WASD.
    if (IsValidPtr(g_ResolvedLocalPlayer) && g_ResolvedLocalPlayer > 0x100000000)
        localPlayer = g_ResolvedLocalPlayer;
    else if (IsValidPtr(worldPtr) && worldPtr > 0x100000000)
    {
        uintptr_t lp = Read<uintptr_t>(worldPtr + offsets::world::LocalPlayer);
        // Only accept stub if it already looks like a typed entity (game-module vtable)
        if (IsValidPtr(lp) && lp > 0x100000000 && g_GameModule)
        {
            uintptr_t mod = (uintptr_t)g_GameModule;
            uintptr_t vt = Read<uintptr_t>(lp);
            if (IsValidPtr(vt) && vt >= mod && vt < mod + 0x8000000ULL)
                localPlayer = lp;
        }
    }

    // Camera object recycled (death/rejoin/streaming) — drop fake VT immediately
    if (g_FreecamVtSwapped && g_FreecamCameraObj && g_FreecamCameraObj != camera)
    {
        MiscFreecamRestoreVt();
        g_FreecamArmed = false;
        g_FreecamBodyFreezeValid = false;
        g_FreecamBodyRotValid = false;
        Log("freecam: camera changed — VT restored");
    }

    unsigned frame = ImGuiMenu_FrameCount();
    bool doInput = (frame != g_FreecamFrameStamp);
    if (doInput)
        g_FreecamFrameStamp = frame;
    else if (g_FreecamArmed)
        return; // ESP + misc both call us — one apply/frame only (was 2x cam/IC/VS work)

    if (!g_FreecamArmed)
    {
        g_FreecamPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
        if ((g_FreecamPos.x == 0.f && g_FreecamPos.y == 0.f && g_FreecamPos.z == 0.f) && g_LocalPlayerValid)
            g_FreecamPos = g_LocalPlayerPos;

        g_FreecamBodyFreezeValid = false;
        g_FreecamBodyRotValid = false;
        if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
        {
            Vec3 body{};
            if (GetEntityPosition(localPlayer, body))
            {
                g_FreecamBodyFreeze = body;
                g_FreecamBodyFreezeValid = true;
            }
            __try
            {
                uintptr_t vs = Read<uintptr_t>(localPlayer + offsets::entity::VisualState);
                if (IsValidPtr(vs) && vs > 0x100000000)
                {
                    for (int i = 0; i < 9; i++)
                        g_FreecamBodyRot[i] = Read<float>(vs + 0x8 + (uintptr_t)i * 4);
                    g_FreecamBodyRotValid = true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_FreecamBodyRotValid = false; }
        }
        if (!g_FreecamBodyFreezeValid && g_LocalPlayerValid)
        {
            g_FreecamBodyFreeze = g_LocalPlayerPos;
            g_FreecamBodyFreezeValid = true;
        }

        // Seed freecam look from current camera forward
        {
            Vec3 forward = Read<Vec3>(camera + offsets::camera::InvertedViewForward);
            float fl = sqrtf(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
            if (fl > 1e-4f)
            {
                forward.x /= fl; forward.y /= fl; forward.z /= fl;
                g_FreecamYaw = atan2f(forward.x, forward.z);
                g_FreecamPitch = asinf(forward.y < -1.f ? -1.f : (forward.y > 1.f ? 1.f : forward.y));
            }
            else
            {
                g_FreecamYaw = 0.f;
                g_FreecamPitch = 0.f;
            }
            InterlockedExchange(&g_FcMouseAccumDx, 0);
            InterlockedExchange(&g_FcMouseAccumDy, 0);
        }

        // Pull cam back/up so your character is in frame immediately
        // Warp lag-blink: stay at eye height — we need to walk away from the frozen body.
        if (!g_WarpDesyncActive)
        {
            Vec3 forward = Read<Vec3>(camera + offsets::camera::InvertedViewForward);
            float fl = MiscVecLen2(forward.x, forward.z);
            if (fl > 1e-4f)
            {
                g_FreecamPos.x -= (forward.x / fl) * 4.5f;
                g_FreecamPos.z -= (forward.z / fl) * 4.5f;
            }
            g_FreecamPos.y += 1.4f;
        }
        else if (g_WarpGhostValid)
        {
            g_FreecamBodyFreeze = g_WarpGhostPos;
            g_FreecamBodyFreezeValid = true;
            g_FreecamPos = g_WarpGhostPos;
            g_FreecamPos.y += 1.65f;
        }

        MiscFreecamDisconnectCamera(camera);
        g_FreecamArmed = true;
        g_FreecamVel = {};
        // Hard-disable combat while freecam — menu sync may re-enable next frame
        g_AimbotEnabled = false;
        g_MagicBullet = false;
        MiscFreecamDisableBodyInput(localPlayer);
        if (!g_FreecamQpcFreq.QuadPart)
            QueryPerformanceFrequency(&g_FreecamQpcFreq);
        QueryPerformanceCounter(&g_FreecamQpcLast);
        Log(g_FreecamVtSwapped
            ? "freecam: on — fly+look owned, body pos/facing frozen"
            : "freecam: on — VT failed; WASD fly, body pin");
    }
    else
    {
        MiscFreecamDisconnectCamera(camera);
        // Keep combat off + input suppressed every frame while freecam is armed
        g_AimbotEnabled = false;
        g_MagicBullet = false;
        MiscFreecamDisableBodyInput(localPlayer);
    }

    // QPC dt (COM CameraTool uses timeslice) — GetTickCount was chunky / felt laggy
    float dt = 0.016f;
    if (doInput)
    {
        if (!g_FreecamQpcFreq.QuadPart)
            QueryPerformanceFrequency(&g_FreecamQpcFreq);
        LARGE_INTEGER nowQ{};
        QueryPerformanceCounter(&nowQ);
        if (g_FreecamQpcLast.QuadPart && g_FreecamQpcFreq.QuadPart)
        {
            dt = (float)(nowQ.QuadPart - g_FreecamQpcLast.QuadPart) / (float)g_FreecamQpcFreq.QuadPart;
            if (dt < 0.f) dt = 0.f;
            if (dt > 0.05f) dt = 0.05f;
        }
        g_FreecamQpcLast = nowQ;

        if (!ImGuiMenu_IsOpen())
        {
            // Freecam-owned mouse look (deltas from PeekMessage/GetMessage WM_INPUT)
            LONG mdx = InterlockedExchange(&g_FcMouseAccumDx, 0);
            LONG mdy = InterlockedExchange(&g_FcMouseAccumDy, 0);
            // Fallback: DayZ often consumes raw input via GetMessage/WndProc — PeekMessage
            // never sees WM_INPUT, so cursor moves but yaw never changes. Use cursor delta.
            if (!(mdx | mdy))
            {
                HWND hwnd = GetForegroundWindow();
                if (hwnd && IsWindow(hwnd))
                {
                    POINT cur{};
                    if (GetCursorPos(&cur))
                    {
                        RECT rc{};
                        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 64)
                        {
                            POINT tl{ rc.left, rc.top };
                            POINT br{ rc.right, rc.bottom };
                            ClientToScreen(hwnd, &tl);
                            ClientToScreen(hwnd, &br);
                            const int cx = (tl.x + br.x) / 2;
                            const int cy = (tl.y + br.y) / 2;
                            static int s_HaveLast = 0;
                            static int s_LastX = 0, s_LastY = 0;
                            if (!s_HaveLast)
                            {
                                s_LastX = cur.x; s_LastY = cur.y; s_HaveLast = 1;
                            }
                            else
                            {
                                mdx = cur.x - s_LastX;
                                mdy = cur.y - s_LastY;
                                if (abs(mdx) > 200 || abs(mdy) > 200)
                                { mdx = 0; mdy = 0; }
                            }
                            SetCursorPos(cx, cy);
                            s_LastX = cx; s_LastY = cy;
                        }
                    }
                }
            }
            if (mdx | mdy)
            {
                const float sens = 0.0025f; // rad / mouse count
                g_FreecamYaw += (float)mdx * sens;
                g_FreecamPitch -= (float)mdy * sens;
                const float lim = 1.553343f; // ~89 deg
                if (g_FreecamPitch > lim) g_FreecamPitch = lim;
                if (g_FreecamPitch < -lim) g_FreecamPitch = -lim;
            }
            // Dual/live QA: prove look path moves yaw (raw WM_INPUT or cursor-delta fallback).
            {
                static DWORD s_LookLog = 0;
                static LONG s_PeakAbsDx = 0, s_PeakAbsDy = 0;
                static int s_LookFrames = 0;
                const LONG adx = mdx < 0 ? -mdx : mdx;
                const LONG ady = mdy < 0 ? -mdy : mdy;
                if (adx > s_PeakAbsDx) s_PeakAbsDx = adx;
                if (ady > s_PeakAbsDy) s_PeakAbsDy = ady;
                if (mdx | mdy) s_LookFrames++;
                DWORD nowL = GetTickCount();
                if (!s_LookLog) s_LookLog = nowL;
                if ((nowL - s_LookLog) > 2500)
                {
                    char lb[160];
                    wsprintfA(lb, "liveqa[freecam-look] frames=%d peakDx=%d peakDy=%d yawx100=%d pitchx100=%d",
                        s_LookFrames, (int)s_PeakAbsDx, (int)s_PeakAbsDy,
                        (int)(g_FreecamYaw * 100.f), (int)(g_FreecamPitch * 100.f));
                    Log(lb);
                    s_LookLog = nowL;
                    s_PeakAbsDx = 0; s_PeakAbsDy = 0; s_LookFrames = 0;
                }
            }

            // Basis from freecam yaw/pitch (not character-facing)
            const float cy = cosf(g_FreecamYaw), sy = sinf(g_FreecamYaw);
            const float cp = cosf(g_FreecamPitch), sp = sinf(g_FreecamPitch);
            Vec3 forward = { sy * cp, sp, cy * cp };
            Vec3 worldUp = { 0.f, 1.f, 0.f };
            Vec3 right = {
                worldUp.y * forward.z - worldUp.z * forward.y,
                worldUp.z * forward.x - worldUp.x * forward.z,
                worldUp.x * forward.y - worldUp.y * forward.x
            };
            float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
            if (rl < 1e-4f) right = { cy, 0.f, -sy };
            else { right.x /= rl; right.y /= rl; right.z /= rl; }
            Vec3 up = {
                forward.y * right.z - forward.z * right.y,
                forward.z * right.x - forward.x * right.z,
                forward.x * right.y - forward.y * right.x
            };

            // Slider is real meters/second. Old code used speed*14 + per-frame vel
            // (not *dt), so 2 m/s felt like ~60–200. Integrate vel * dt.
            float speed = g_MiscFreecamSpeed;
            if (g_WarpDesyncActive)
            {
                // Ground scout — sprint feel, not freecam fly speeds.
                speed = 6.5f;
                if (KeyHeld(VK_SHIFT)) speed = 11.f;
                if (KeyHeld(VK_CONTROL)) speed = 2.5f;
            }
            else
            {
                if (speed < 1.f) speed = 1.f;
                if (speed > 80.f) speed = 80.f;
                if (KeyHeld(VK_SHIFT)) speed *= 3.f;
                if (KeyHeld(VK_CONTROL)) speed *= 0.25f;
            }

            float fwd = 0.f, strafe = 0.f, alt = 0.f;
            if (KeyHeld('W') || KeyHeld(VK_UP)) fwd += 1.f;
            if (KeyHeld('S') || KeyHeld(VK_DOWN)) fwd -= 1.f;
            if (KeyHeld('D') || KeyHeld(VK_RIGHT)) strafe += 1.f;
            if (KeyHeld('A') || KeyHeld(VK_LEFT)) strafe -= 1.f;
            // Warp lag-blink: ground walk only — no fly.
            if (!g_WarpDesyncActive)
            {
                if (KeyHeld(VK_SPACE) || KeyHeld('E') || KeyHeld(VK_PRIOR)) alt += 1.f;
                if (KeyHeld('Q') || KeyHeld(VK_NEXT)) alt -= 1.f;
            }

            Vec3 wish;
            if (g_WarpDesyncActive)
            {
                // Flat WASD relative to look XZ (sprint feel, not freecam fly).
                float flatYaw = g_FreecamYaw;
                float sy = sinf(flatYaw), cy = cosf(flatYaw);
                Vec3 f2 = { sy, 0.f, cy };
                Vec3 r2 = { cy, 0.f, -sy };
                wish = {
                    f2.x * fwd + r2.x * strafe,
                    0.f,
                    f2.z * fwd + r2.z * strafe
                };
            }
            else
            {
                wish = {
                    forward.x * fwd + right.x * strafe + up.x * alt,
                    forward.y * fwd + right.y * strafe + up.y * alt,
                    forward.z * fwd + right.z * strafe + up.z * alt
                };
            }
            float wlen = sqrtf(wish.x * wish.x + wish.y * wish.y + wish.z * wish.z);
            Vec3 desired = {};
            if (wlen > 1e-4f)
            {
                desired.x = (wish.x / wlen) * speed;
                desired.y = (wish.y / wlen) * speed;
                desired.z = (wish.z / wlen) * speed;
            }

            // Snappy approach to desired velocity (still in m/s)
            float blend = 1.f - expf(-14.f * dt);
            if (blend > 1.f) blend = 1.f;
            g_FreecamVel.x += (desired.x - g_FreecamVel.x) * blend;
            g_FreecamVel.y += (desired.y - g_FreecamVel.y) * blend;
            g_FreecamVel.z += (desired.z - g_FreecamVel.z) * blend;

            g_FreecamPos.x += g_FreecamVel.x * dt;
            g_FreecamPos.y += g_FreecamVel.y * dt;
            g_FreecamPos.z += g_FreecamVel.z * dt;
            if (g_WarpDesyncActive && g_WarpGhostValid)
            {
                g_FreecamBodyFreeze = g_WarpGhostPos;
                g_FreecamBodyFreezeValid = true;
                g_FreecamPos.y = g_WarpGhostPos.y + 1.65f;
                g_FreecamVel.y = 0.f;
            }
        }
        else
        {
            // Menu open — drop pending mouse so look doesn't jump on close
            InterlockedExchange(&g_FcMouseAccumDx, 0);
            InterlockedExchange(&g_FcMouseAccumDy, 0);
        }
    }

    if (g_FreecamPos.x != g_FreecamPos.x || g_FreecamPos.y != g_FreecamPos.y || g_FreecamPos.z != g_FreecamPos.z)
        return;

    __try
    {
        // UC: write camera position + freecam-owned look every frame
        MiscFreecamWriteCamPos(camera, g_FreecamPos);
        MiscFreecamWriteCamLook(camera, g_FreecamYaw, g_FreecamPitch);
        if (IsValidPtr(worldPtr) && worldPtr > 0x100000000)
        {
            uintptr_t cam2 = Read<uintptr_t>(worldPtr + offsets::world::Camera);
            if (IsValidPtr(cam2) && cam2 > 0x100000000 && cam2 != camera)
            {
                MiscFreecamWriteCamPos(cam2, g_FreecamPos);
                MiscFreecamWriteCamLook(cam2, g_FreecamYaw, g_FreecamPitch);
            }
        }

        // Root body + wipe pawn input so WASD/F/fire cannot control the character
        if (g_FreecamBodyFreezeValid && IsValidPtr(localPlayer) && localPlayer > 0x100000000)
        {
            MiscFreecamDisableBodyInput(localPlayer);
            MiscFreecamPinBodyTranslation(localPlayer, g_FreecamBodyFreeze);
            g_LocalPlayerPos = g_FreecamBodyFreeze;
            g_LocalPlayerValid = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    g_CameraPos = g_FreecamPos;
    g_CameraValid = true;
}

// Post-frame: keep body rooted and input dead (Present is late in the frame)
static void MiscFreecamPostFramePin(uintptr_t worldPtr)
{
    if (!g_MiscFreecam || !g_FreecamArmed || !g_FreecamBodyFreezeValid || g_PanicHidden)
        return;
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;
    __try
    {
        uintptr_t lp = g_ResolvedLocalPlayer;
        if (!IsValidPtr(lp) || lp < 0x100000000)
            lp = Read<uintptr_t>(worldPtr + offsets::world::LocalPlayer);
        if (IsValidPtr(lp) && lp > 0x100000000)
            MiscFreecamPinBodyTranslation(lp, g_FreecamBodyFreeze);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void MiscDrawFreecamHud()
{
    if (!g_MiscFreecam || g_PanicHidden || !g_FreecamArmed)
        return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    ImU32 soft = IM_COL32(230, 240, 255, 90);
    ImU32 core = IM_COL32(245, 250, 255, 210);
    dl->AddLine(ImVec2(cx - 10.f, cy), ImVec2(cx - 3.f, cy), soft, 1.2f);
    dl->AddLine(ImVec2(cx + 3.f, cy), ImVec2(cx + 10.f, cy), soft, 1.2f);
    dl->AddLine(ImVec2(cx, cy - 10.f), ImVec2(cx, cy - 3.f), soft, 1.2f);
    dl->AddLine(ImVec2(cx, cy + 3.f), ImVec2(cx, cy + 10.f), soft, 1.2f);
    dl->AddCircle(ImVec2(cx, cy), 2.2f, core, 12, 1.2f);

    // Warp owns the HUD banner (OakBatch4DrawWarpOverlay) — don't scream FREECAM.
    if (g_WarpDesyncActive)
        return;

    char label[80];
    wsprintfA(label, "FREECAM  %d m/s%s%s",
        (int)(g_MiscFreecamSpeed + 0.5f),
        g_MiscFreecamMoveBody ? "  · body follow" : "  · BODY LOCKED",
        g_FreecamVtSwapped ? "" : "  · weak");
    DrawEspBadge(cx, 28.f, label, 0.55f, 0.82f, 1.0f, 0.95f);
}

static void MiscCopyCoordsToClipboard()
{
    float x = g_LocalPlayerValid ? g_LocalPlayerPos.x : g_CameraPos.x;
    float y = g_LocalPlayerValid ? g_LocalPlayerPos.y : g_CameraPos.y;
    float z = g_LocalPlayerValid ? g_LocalPlayerPos.z : g_CameraPos.z;
    char tmp[96];
    wsprintfA(tmp, "%d.%02d, %d.%02d, %d.%02d",
        (int)x, (int)(fabsf(x) * 100) % 100,
        (int)y, (int)(fabsf(y) * 100) % 100,
        (int)z, (int)(fabsf(z) * 100) % 100);
    if (OpenClipboard(nullptr))
    {
        EmptyClipboard();
        size_t len = (size_t)lstrlenA(tmp) + 1;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
        if (h)
        {
            memcpy(GlobalLock(h), tmp, len);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
        CloseClipboard();
        Log("misc: copied coords");
    }
}

static void MiscHandleKeybinds()
{
    // ESC exits freecam / cancels warp desync even with menu open.
    if (g_MiscFreecam && KeyPressedEdge(VK_ESCAPE))
    {
        const bool wasWarp = g_WarpDesyncActive;
        g_WarpDesyncActive = false;
        g_WarpCommitting = false;
        g_WarpGhostValid = false;
        g_MiscFreecam = false;
        MiscFreecamShutdown();
        MiscPushUi();
        if (wasWarp)
        {
            ImGuiMenu_SetWarpStatus("cancelled (body stayed)");
            Log("warp: desync cancelled (esc)");
        }
        else
            Log("freecam: off (esc)");
    }

    if (ImGuiMenu_IsOpen())
        return;

    bool changed = false;
    auto togOrHold = [&](int bind, bool& flag, unsigned holdBit) {
        if (bind == 0) return;
        if (g_BindHoldMask & holdBit)
        {
            const bool held = KeyHeld(bind);
            if (flag != held) { flag = held; changed = true; }
        }
        else if (KeyPressedEdge(bind))
        {
            flag = !flag;
            changed = true;
        }
    };

    togOrHold(g_BindEsp, g_ShowESP, 1u);
    togOrHold(g_BindAimbot, g_AimbotEnabled, 2u);
    togOrHold(g_BindMagicBullet, g_MagicBullet, 4u);
    togOrHold(g_BindFullbright, g_Fullbright, 8u);
    togOrHold(g_BindDisableOverlays, g_MiscDisableOverlays, 16u);
    togOrHold(g_BindContainerMagnet, g_MiscContainerMagnet, 64u);

    // Freecam: always allow ESC to exit; toggle bind; force shutdown immediately on off
    {
        const bool wasFc = g_MiscFreecam;
        if (g_MiscFreecam && KeyPressedEdge(VK_ESCAPE))
        {
            g_MiscFreecam = false;
            changed = true;
        }
        else if (!g_WarpDesyncActive)
            togOrHold(g_BindFreecam, g_MiscFreecam, 128u);
        if (wasFc && !g_MiscFreecam)
        {
            MiscFreecamShutdown();
            if (!g_WarpDesyncActive) // warp ESC path already logged
                Log("freecam: off (bind/esc)");
        }
    }

    for (int pi = 0; pi < 3; pi++)
    {
        if (g_ProfileHotkey[pi] != 0 && KeyPressedEdge(g_ProfileHotkey[pi]))
        {
            ImGuiMenu_LoadProfile(pi);
            changed = true;
        }
    }

    if (KeyPressedEdge(g_BindPanic) || ImGuiMenu_ConsumePanicLatch())
    {
        if (g_PanicHidden)
        {
            g_PanicHidden = false;
            Log("misc: panic cleared");
        }
        else
            MiscApplyPanic();
        changed = true;
    }
    if (KeyPressedEdge(g_BindAddWaypoint) && g_LocalPlayerValid)
    {
        ImGuiMenu_AddWaypointHere(g_LocalPlayerPos.x, g_LocalPlayerPos.y, g_LocalPlayerPos.z, nullptr);
        Log("misc: waypoint added");
    }
    if (KeyPressedEdge(g_BindCopyCoords) || ImGuiMenu_ConsumeCopyCoords())
        MiscCopyCoordsToClipboard();

    // Gameplay QA dump — F8 writes verify + last ESP counters to oak_imgui.log
    if (KeyPressedEdge(VK_F8))
        OakRuntimeVerify("f8-qa");

    if (changed)
        MiscPushUi();
}

static void UpdateMiscFeatures(uintptr_t worldPtr, uintptr_t localPlayer)
{
    { OAK_MARK("misc.keybinds"); MiscHandleKeybinds(); }
    { OAK_MARK("misc.steamharvest"); MiscEnsureSteamHarvest(); }
    // Steam persona / harvest only when name or avatar ESP is armed.
    if (g_MiscSteamAvatars)
    {
        if (g_FnRunCallbacks)
        {
            OAK_MARK("misc.steamcallbacks");
            __try { g_FnRunCallbacks(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        static DWORD s_SnapTick = 0;
        DWORD now = GetTickCount();
        if (!s_SnapTick || (now - s_SnapTick) > 1000)
        {
            s_SnapTick = now;
            { OAK_MARK("misc.collectremotes"); MiscCollectRemotePlayersForNames(); }
            { OAK_MARK("misc.promotesteams"); MiscPromoteOrphanSteams(); }
        }
        { OAK_MARK("misc.roster"); MiscRefreshScoreboardRoster(); }
    }

    // No grass — explicit Misc toggle only (never auto with freecam; that caused ~30 FPS)
    if (IsValidPtr(worldPtr) && worldPtr > 0x100000000)
    {
        OAK_MARK("misc.nograss");
        MiscApplyNoGrass(worldPtr);
    }

    // Pull fence/gate can fire from menu even while open
    if (KeyPressedEdge(g_BindPullBasePart) || ImGuiMenu_ConsumePullBasePart())
        MiscPullNearestBasePart(worldPtr, localPlayer);

    // Soft 3PP / freecam when ESP is off (RenderESP already applied + refreshed W2S when ESP on)
    if ((g_MiscFreecam || g_WorldMisc.thirdPerson) && !g_ShowESP && IsValidPtr(worldPtr) && worldPtr > 0x100000000)
    {
        uintptr_t cam = Read<uintptr_t>(worldPtr + offsets::world::Camera);
        { OAK_MARK("misc.freecam"); MiscApplyFreecam(worldPtr, cam, localPlayer); }
        { OAK_MARK("misc.3pp"); MiscApplyThirdPerson(worldPtr, cam, localPlayer); }
        if (IsValidPtr(cam) && cam > 0x100000000)
            RefreshW2SCache(cam);
    }

    static int s_Log = 0;
    if (false && (s_Log++ % 600) == 0)
    {
        char buf[192];
        wsprintfA(buf, "misc: day=%d lootMag=%d boxMag=%d despawn=%d wp=%d free=%d local=%d",
            g_MiscDaytimeLock ? 1 : 0, g_MiscLootMagnet ? 1 : 0,
            g_MiscContainerMagnet ? 1 : 0, g_MiscMiddleClickDespawn ? 1 : 0,
            g_WaypointCount, g_MiscFreecam ? 1 : 0, g_LocalPlayerValid ? 1 : 0);
        Log(buf);
    }

    if (g_PanicHidden)
        return;

    __try
    {
        { OAK_MARK("misc.forceworld"); MiscForceWorld(worldPtr); }
        { OAK_MARK("misc.despawn"); MiscMiddleClickDespawn(worldPtr, localPlayer); }
        { OAK_MARK("misc.lootmagnet"); MiscLootMagnet(worldPtr, localPlayer); }
        { OAK_MARK("misc.boxmagnet"); MiscContainerMagnet(worldPtr, localPlayer); }
        { OAK_MARK("misc.waypoints"); MiscDrawWaypoints(); }
        { OAK_MARK("misc.freecamhud"); MiscDrawFreecamHud(); }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int s_Ex = 0;
        if ((s_Ex++ % 120) == 0)
            Log("misc: exception caught (safe continue)");
    }
}
