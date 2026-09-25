// Phase B batch-4 ESP / loot — included from main.cpp after oak_features_impl.inl

static OakBatch4EspSettings g_Batch4Esp = {};

static bool Batch4LootExtrasOn()
{
    return g_Batch4Esp.lootExtrasEnabled;
}

void OakBatch4EspApplySettings(const OakBatch4EspSettings& s)
{
    g_Batch4Esp = s;
}

static int g_Batch4QualityProbes = 0;
static int g_Batch4QtyProbes = 0;
static int g_Batch4ContainerProbes = 0;
static int g_Batch4LookingChecks = 0;

// Crosshair-target player for inventory viewer (T4-14)
static uintptr_t g_Batch4CrosshairPlayer = 0;
static float g_Batch4CrosshairDistPx = 1e9f;

enum { kBatch4VisCache = 64 };
struct OakBatch4VisSlot {
    uintptr_t ent;
    DWORD tick;
    bool occluded;
    int sticky;       // hysteresis counter
    uint32_t boneOcc; // bit i => bone i occluded
    DWORD boneTick;
};
static OakBatch4VisSlot g_Batch4VisCache[kBatch4VisCache];

static OakBatch4VisSlot& OakBatch4VisSlotFor(uintptr_t entity)
{
    unsigned h = (unsigned)((entity >> 4) ^ (entity >> 12)) & (kBatch4VisCache - 1);
    return g_Batch4VisCache[h];
}

enum { kBatch4LookCache = 32 };
struct OakBatch4LookSlot {
    uintptr_t ent;
    DWORD tick;
    bool looking;
};
static OakBatch4LookSlot g_Batch4LookCache[kBatch4LookCache];

struct OakChernarusContamZone {
    float x, z, radiusM;
    const char* label;
};

// Legacy static table kept only as offline fallback when no Contaminated* entities
// are visible yet (loading). Live path prefers entity scan (any map).
static const OakChernarusContamZone k_ChernarusContam[] = {
    { 2165.f, 3370.f, 75.f, "Pavlovo" },
    { 2080.f, 3500.f, 100.f, "PavlovoN" },
    { 13917.f, 11178.f, 75.f, "Ship" },
};

enum { kOakContamCacheMax = 48 };
struct OakContamZoneLive {
    uintptr_t ent;
    float x, y, z;
    float radiusM;
    char label[40];
    DWORD lastSeen;
    bool locked; // once set, x/z/y/radius stay fixed (no camera/player jitter)
    bool isArea; // ContaminatedArea* — preferred over Trigger "Gas" spam
};
static OakContamZoneLive g_ContamLive[kOakContamCacheMax];
static int g_ContamLiveN = 0;
static DWORD g_ContamLastScan = 0;

static uintptr_t g_Batch4VisWorld = 0;
static uintptr_t g_Batch4VisLocal = 0;
static int g_Batch4VisLosChecks = 0;

static void OakBatch4ResetFrame()
{
    g_Batch4QualityProbes = 0;
    g_Batch4QtyProbes = 0;
    g_Batch4ContainerProbes = 0;
    g_Batch4LookingChecks = 0;
    g_Batch4VisLosChecks = 0;
    g_Batch4CrosshairPlayer = 0;
    g_Batch4CrosshairDistPx = 1e9f;
}

static void OakBatch4SetVisContext(uintptr_t worldPtr, uintptr_t localPlayer)
{
    g_Batch4VisWorld = worldPtr;
    g_Batch4VisLocal = localPlayer;
}

static float OakBatch4DistPointToSeg2(const Vec3& p, const Vec3& a, const Vec3& b, float* outT)
{
    float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    float apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
    float ab2 = abx * abx + aby * aby + abz * abz;
    float t = 0.f;
    if (ab2 > 1e-6f)
        t = (apx * abx + apy * aby + apz * abz) / ab2;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    if (outT) *outT = t;
    float cx = a.x + abx * t, cy = a.y + aby * t, cz = a.z + abz * t;
    float dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
    return dx * dx + dy * dy + dz * dz;
}

enum { kOakOccRadCache = 512 };
struct OakOccRadSlot { uintptr_t ent; float r; unsigned frame; };
static OakOccRadSlot g_OakOccRad[kOakOccRadCache];

// Returns occluder radius (m). Checks config AND type (cfg is often just "house").
static float OakBatch4OccluderRadiusM(uintptr_t ent, bool slowDefault)
{
    if (!IsValidPtr(ent) || ent < 0x100000000)
        return 0.f;
    unsigned h = (unsigned)((ent >> 4) ^ (ent >> 12)) & (kOakOccRadCache - 1);
    if (g_OakOccRad[h].ent == ent &&
        ((unsigned)g_FrameCount - g_OakOccRad[h].frame) < 45u)
        return g_OakOccRad[h].r;

    bool isP = false, isZ = false;
    if (CombatEntityIsPlayerOrZombie(ent, isP, isZ))
    {
        g_OakOccRad[h] = { ent, 0.f, (unsigned)g_FrameCount };
        return 0.f;
    }

    char cfg[64] = {};
    char tn[64] = {};
    ReadEntityConfigName(ent, cfg, 64);
    ReadEntityTypeName(ent, tn, 64);

    auto junk = [](const char* s) -> bool {
        if (!s || !s[0]) return false;
        return StrContainsI(s, "dayzanimal") || StrContainsI(s, "Animal_") ||
            StrContainsI(s, "Inventory") || StrContainsI(s, "Magazine") ||
            StrContainsI(s, "Ammo") || StrContainsI(s, "Bullet") ||
            StrContainsI(s, "Grenade") || StrContainsI(s, "Clothing") ||
            StrContainsI(s, "Weapon") || StrContainsI(s, "Survivo") ||
            StrContainsI(s, "Zmb") || StrContainsI(s, "Infected") ||
            StrContainsI(s, "Helicopter") || StrContainsI(s, "Proxy") ||
            StrContainsI(s, "Particle") || StrContainsI(s, "Sound") ||
            StrContainsI(s, "PlayerBase") || StrContainsI(s, "DayZPlayer") ||
            StrContainsI(s, "ItemOptics") || StrContainsI(s, "itemoptics") ||
            StrContainsI(s, "Optic") || StrContainsI(s, "Suppress") ||
            StrContainsI(s, "Attachment") || StrContainsI(s, "Bag") ||
            StrContainsI(s, "Bottle") || StrContainsI(s, "Bandage") ||
            StrContainsI(s, "Food") || StrContainsI(s, "Pistol") ||
            StrContainsI(s, "Rifle") || StrContainsI(s, "Seed");
    };
    // NOTE: do NOT junk CarScript — cars are usable soft occluders when Land_ is absent.
    float r = 0.f;
    if (!(junk(cfg) || junk(tn)))
    {
        auto hit = [](const char* s, const char* k) -> bool {
            return s && s[0] && StrContainsI(s, k);
        };
        auto big = [&](const char* s) -> bool {
            return hit(s, "Land_") || hit(s, "House") || hit(s, "Building") ||
                hit(s, "Ruin") || hit(s, "Castle") || hit(s, "Church") ||
                hit(s, "Factory") || hit(s, "Hangar") || hit(s, "Warehouse") ||
                hit(s, "StaticObj") || hit(s, "Shelter") || hit(s, "bldr_") ||
                hit(s, "Residential") || hit(s, "Industrial") || hit(s, "Mil_") ||
                hit(s, "Civ_");
        };
        if (big(tn) || big(cfg))
            r = 14.f;
        else if (hit(tn, "Sedan") || hit(cfg, "Sedan") || hit(tn, "Hatchback") || hit(cfg, "Hatchback") ||
            hit(tn, "Offroad") || hit(cfg, "Offroad") || hit(tn, "Truck") || hit(cfg, "Truck") ||
            hit(tn, "CivilianSedan") || hit(cfg, "CivilianSedan") || hit(tn, "Bus") || hit(cfg, "Bus") ||
            hit(tn, "V3S") || hit(cfg, "V3S"))
            r = 3.2f;
        else if (hit(tn, "Wall") || hit(cfg, "Wall") || hit(tn, "Fence") || hit(cfg, "Fence") ||
            hit(tn, "Gate") || hit(cfg, "Gate") || hit(tn, "Barricade") || hit(cfg, "Barricade"))
            r = 2.5f;
        else if (hit(tn, "Tree") || hit(cfg, "Tree") || hit(tn, "Bush") || hit(cfg, "Bush") ||
            hit(tn, "Rock") || hit(cfg, "Rock") || hit(tn, "Wreck") || hit(cfg, "Wreck"))
            r = 2.0f;
    }
    // No blanket slowDefault — that marked random loot as walls.
    (void)slowDefault;
    g_OakOccRad[h] = { ent, r, (unsigned)g_FrameCount };
    return r;
}

enum { kOakOccMax = 384 };
struct OakOccSphere { Vec3 pos; float r; };
static OakOccSphere g_OakOcc[kOakOccMax];
static int g_OakOccN = 0;
static unsigned g_OakOccFrameId = 0xFFFFFFFFu;
static int g_OakOccDbgNear = 0;
static int g_OakOccDbgScan = 0;
static int g_OakOccDbgRad = 0;
static int g_OakOccDbgSlow = 0;
static int g_OakOccDbgItem = 0;
static char g_OakOccDbgSample[48] = {};

static void OakBatch4GatherOccluders(uintptr_t worldPtr, uintptr_t localPlayer)
{
    if (!g_W2S.valid || !IsValidPtr(worldPtr)) { g_OakOccN = 0; return; }
    if (g_OakOccFrameId == (unsigned)g_FrameCount) return;
    g_OakOccFrameId = (unsigned)g_FrameCount;
    g_OakOccN = 0;
    g_OakOccDbgNear = 0;
    g_OakOccDbgScan = 0;
    g_OakOccDbgRad = 0;
    g_OakOccDbgSlow = 0;
    g_OakOccDbgItem = 0;
    g_OakOccDbgSample[0] = 0;

    Vec3 cam = g_W2S.translation;
    if (g_CameraValid)
        cam = g_CameraPos;
    else if (g_LocalPlayerValid)
        cam = g_LocalPlayerPos;

    auto push = [&](uintptr_t ent, bool slowDefault) {
        if (g_OakOccN >= kOakOccMax) return;
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer) return;
        g_OakOccDbgScan++;
        float r = 0.f;
        __try {
            r = OakBatch4OccluderRadiusM(ent, slowDefault);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (r <= 0.f) return;
        g_OakOccDbgRad++;
        Vec3 pos{};
        if (!GetEntityPosition(ent, pos)) return;
        if (!std::isfinite(pos.x) || !std::isfinite(pos.z)) return;
        float dx = pos.x - cam.x, dy = pos.y - cam.y, dz = pos.z - cam.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 200.f || d < 0.4f) return;
        g_OakOcc[g_OakOccN].pos = pos;
        g_OakOcc[g_OakOccN].r = r;
        g_OakOccN++;
        if (d < 60.f) g_OakOccDbgNear++;
        if (!g_OakOccDbgSample[0])
        {
            char tn[64] = {};
            __try { ReadEntityTypeName(ent, tn, 64); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (tn[0]) lstrcpynA(g_OakOccDbgSample, tn, 48);
        }
    };

    const uintptr_t listOffs[2] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2; li++) {
        uintptr_t data = 0; int count = 0;
        if (!ResolveEntityList(worldPtr, listOffs[li], 2000, data, count, nullptr)) continue;
        int maxI = count > 400 ? 400 : count;
        for (int i = 0; i < maxI; i++)
            push(Read<uintptr_t>(data + (uintptr_t)i * 8), true);
    }

    // Match ESP list walk: Slow flag!=0; ItemTable allows flag==0 with live ptr.
    auto scanSlow = [&](uintptr_t listOff, uintptr_t sizeOff, bool isItem, int hardCap) {
        uintptr_t data = Read<uintptr_t>(worldPtr + listOff);
        int count = Read<int>(worldPtr + sizeOff);
        if (count < 0 || count > 200000)
        {
            DWORD cd = Read<DWORD>(worldPtr + sizeOff);
            count = (cd < 200000) ? (int)cd : 0;
        }
        if (!isItem)
        {
            int valid = Read<int>(worldPtr + oak_offsets::world::SlowEntValidCount);
            if (valid > count && valid < 30000)
                count = valid;
        }
        if (!IsValidPtr(data) || data < 0x100000000 || count <= 0) return;
        if (isItem) g_OakOccDbgItem = count;
        else g_OakOccDbgSlow = count;
        int maxI = count > hardCap ? hardCap : count;
        // ItemTable: rolling window so we still catch nearby Land_* over a few frames.
        static int s_itemBase = 0;
        int base = 0;
        if (isItem && count > maxI)
        {
            base = s_itemBase % count;
            s_itemBase = (s_itemBase + maxI) % count;
        }
        for (int n = 0; n < maxI && g_OakOccN < kOakOccMax; n++)
        {
            int i = isItem ? ((base + n) % count) : n;
            uintptr_t entry = data + (uintptr_t)i * 0x18;
            WORD flag = Read<WORD>(entry);
            uintptr_t ent = Read<uintptr_t>(entry + 0x8);
            if (!IsValidPtr(ent) || ent < 0x100000000) continue;
            if (flag == 0 && !isItem) continue;
            push(ent, true);
        }
    };
    scanSlow(oak_offsets::world::SlowEntList, oak_offsets::world::SlowTableSize, false, 2000);
    scanSlow(oak_offsets::world::ItemList, oak_offsets::world::ItemListSize, true, 2000);
    // occ-census Slow/Item string walk removed — was debug-only and cost thousands of reads/4s.
}

static bool OakBatch4PointBlockedByGather(const Vec3& pt)
{
    if (!g_W2S.valid || g_OakOccN <= 0) return false;
    Vec3 cam = g_W2S.translation;
    if (g_CameraValid)
        cam = g_CameraPos;
    else if (g_LocalPlayerValid)
        cam = g_LocalPlayerPos;
    float segLen = Distance3D(cam, pt);
    if (segLen < 1.2f) return false;
    for (int i = 0; i < g_OakOccN; i++)
    {
        const OakOccSphere& o = g_OakOcc[i];
        float t = 0.f;
        float d2 = OakBatch4DistPointToSeg2(o.pos, cam, pt, &t);
        // Must sit clearly between camera and target (not at either end).
        if (t < 0.08f || t > 0.92f) continue;
        float along = t * segLen;
        if (along < 1.0f || along > segLen - 0.8f) continue;
        float r = o.r;
        if (r > 20.f) r = 20.f;
        float yOn = cam.y + (pt.y - cam.y) * t;
        if (fabsf(o.pos.y - yOn) > (r + 2.5f)) continue;
        // Strict capsule only — no wide cone (that false-occluded half the map).
        if (d2 <= r * r)
            return true;
    }
    return false;
}

static bool OakBatch4WorldToScreenZ(const Vec3& world, Vec2& screen, float& outViewZ)
{
    outViewZ = 0.f;
    if (!g_W2S.valid) return false;
    Vec3 temp;
    temp.x = world.x - g_W2S.translation.x;
    temp.y = world.y - g_W2S.translation.y;
    temp.z = world.z - g_W2S.translation.z;
    float x = temp.x * g_W2S.right.x + temp.y * g_W2S.right.y + temp.z * g_W2S.right.z;
    float y = temp.x * g_W2S.up.x + temp.y * g_W2S.up.y + temp.z * g_W2S.up.z;
    float z = temp.x * g_W2S.forward.x + temp.y * g_W2S.forward.y + temp.z * g_W2S.forward.z;
    outViewZ = z;
    if (z < 0.65f) return false;
    float invZ = 1.0f / z;
    float nx = (x / g_W2S.projX) * invZ;
    float ny = (y / g_W2S.projY) * invZ;
    screen.x = g_ScreenHalfW + (nx * g_ScreenHalfW);
    screen.y = g_ScreenHalfH - (ny * g_ScreenHalfH);
    return screen.x >= -80.f && screen.x <= g_ScreenWidth + 80.f &&
           screen.y >= -80.f && screen.y <= g_ScreenHeight + 80.f;
}

static int g_OakVisDbgOcc = 0;
static int g_OakVisDbgVis = 0;
static int g_OakVisDbgLog = 0;

static bool OakBatch4IsOccludedScreen(uintptr_t worldPtr, uintptr_t localPlayer, uintptr_t /*targetEnt*/, const Vec3& head)
{
    if (!IsValidPtr(worldPtr) || !g_W2S.valid) return false;
    float tz = 0.f; Vec2 tscr{};
    if (!OakBatch4WorldToScreenZ(head, tscr, tz) || tz < 1.f) return false;

    // Prop-sphere occluder heuristic (cars/wrecks in entity lists).
    OakBatch4GatherOccluders(worldPtr, localPlayer);
    if (OakBatch4PointBlockedByGather(head))
    {
        g_OakVisDbgOcc++;
        return true;
    }
    g_OakVisDbgVis++;
    return false;
}

// Kept for triggerbot — world-space prop cylinder test.
static bool OakBatch4LosClear(uintptr_t worldPtr, uintptr_t localPlayer, uintptr_t targetEnt, const Vec3& aimPos)
{
    // Prefer screen-space result when W2S is valid (same idea, better hit rate).
    if (g_W2S.valid && OakBatch4IsOccludedScreen(worldPtr, localPlayer, targetEnt, aimPos))
        return false;
    return true;
}

static bool OakBatch4EntityLooksSolidOccluder(uintptr_t ent)
{
    return OakBatch4OccluderRadiusM(ent, false) > 0.f;
}

static bool OakBatch4GetEntityForwardXZ(uintptr_t entity, float& fx, float& fz)
{
    fx = 0.f; fz = 1.f;
    if (!IsValidPtr(entity)) return false;
    __try
    {
        uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
        if (!IsValidPtr(vs))
            vs = Read<uintptr_t>(entity + offsets::entity::FutureVisualState);
        if (!IsValidPtr(vs)) return false;
        fx = Read<float>(vs + 0x8 + 2 * 4);
        fz = Read<float>(vs + 0x8 + 2 * 4 + 8);
        float len = sqrtf(fx * fx + fz * fz);
        if (len < 0.01f) return false;
        fx /= len; fz /= len;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool OakBatch4ReadEngineString(uintptr_t strObj, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(strObj) || strObj < 0x100000000) return false;
    __try
    {
        WORD len = Read<WORD>(strObj + 0x8);
        if (len == 0 || len > 255) return false;
        int n = (len < outMax - 1) ? len : (outMax - 1);
        uintptr_t data = strObj + 0x10;
        memcpy(out, (const void*)data, (size_t)n);
        out[n] = 0;
        return out[0] != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

static bool OakBatch4NameIsJunk(const char* nm)
{
    if (!nm || !nm[0]) return true;
    if (StrCmpI(nm, "InventoryItem") == 0 || StrCmpI(nm, "Clothing") == 0 ||
        StrCmpI(nm, "Weapon") == 0 || StrCmpI(nm, "ItemBase") == 0 ||
        StrCmpI(nm, "EntityAI") == 0 || StrCmpI(nm, "Weapon_Base") == 0 ||
        StrCmpI(nm, "Clothing_Base") == 0)
        return true;
    if (StrContainsI(nm, "InventoryItem")) return true;
    return false;
}

static bool OakBatch4ReadItemName(uintptr_t itemEnt, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(itemEnt)) return false;
    char cfg[64] = {};
    char tn[64] = {};
    ReadEntityConfigName(itemEnt, cfg, 64);
    ReadEntityTypeName(itemEnt, tn, 64);
    if (cfg[0] && !OakBatch4NameIsJunk(cfg))
    {
        lstrcpynA(out, cfg, outMax);
        return true;
    }
    if (tn[0] && !OakBatch4NameIsJunk(tn))
    {
        lstrcpynA(out, tn, outMax);
        return true;
    }
    return false;
}

// --- Item quality badge (T4-9) ---
// Reuse ProbeEntityHealthBlock from esp_addons_impl.inl (same as player vitals).
// That path rejects exact 0.0 junk that was labeling everything Ruined.

static int OakBatch4MapQualityTier(float health01)
{
    if (health01 != health01 || health01 < 0.f) return -1;
    if (health01 > 1.05f) health01 /= 100.f;
    if (health01 > 1.f) health01 = 1.f;
    // Must have a real reading — never map bare 0 → Ruined.
    if (health01 <= 0.001f) return -1;
    if (health01 >= 0.70f) return 0; // Pristine
    if (health01 >= 0.50f) return 1; // Worn
    if (health01 >= 0.30f) return 2; // Damaged
    if (health01 >= 0.10f) return 3; // Badly
    return 4; // Ruined only when 0.001 < hp < 0.10
}

static const char* OakBatch4QualityLabel(int tier)
{
    switch (tier)
    {
    case 0: return "Pristine";
    case 1: return "Worn";
    case 2: return "Damaged";
    case 3: return "Badly";
    case 4: return "Ruined";
    default: return nullptr;
    }
}

static void OakBatch4QualityColor(int tier, float* r, float* g, float* b)
{
    switch (tier)
    {
    case 0: *r = 0.35f; *g = 0.95f; *b = 0.45f; break;
    case 1: *r = 0.85f; *g = 0.90f; *b = 0.30f; break;
    case 2: *r = 0.95f; *g = 0.65f; *b = 0.20f; break;
    case 3: *r = 0.95f; *g = 0.35f; *b = 0.20f; break;
    default: *r = 0.55f; *g = 0.55f; *b = 0.55f; break;
    }
}

enum { kBatch4QualityCache = 96 };
struct OakBatch4QualitySlot {
    uintptr_t ent;
    int tier;
    float raw; // 0..1
    DWORD tick;
};
static OakBatch4QualitySlot g_Batch4QualityCache[kBatch4QualityCache];

// Live (2026-08-08): Item health *level* is BYTE at entity+0x194 (DayZ GameConstants):
//   0=Pristine, 1=Worn, 2=Damaged, 3=Badly Damaged, 4=Ruined.
// Verified vs user: WeaponCleaningKit=2 Damaged, Mag_AKM_30Rnd=0xFF (unset→Pristine).
// DO NOT use inv+0xBC — that float is unrelated junk (false Worn/Damaged badges).
enum { kBatch4ItemHealthLevelOff = 0x194 };

static int OakBatch4ReadItemHealthLevel(uintptr_t entity)
{
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return -1;
    unsigned char lvl = Read<unsigned char>(entity + kBatch4ItemHealthLevelOff);
    if (lvl <= 4)
        return (int)lvl;
    // Magazines / some entities leave 0xFF until damaged — treat as Pristine.
    if (lvl == 0xFF)
        return 0;
    return -1;
}

static bool OakBatch4ReadItemHealth01(uintptr_t entity, float* out01)
{
    // Kept for callers expecting 0..1; derive from discrete health level midpoints.
    if (!out01) return false;
    *out01 = -1.f;
    int lvl = OakBatch4ReadItemHealthLevel(entity);
    if (lvl < 0) return false;
    // Midpoints matching OakBatch4MapQualityTier thresholds.
    static const float kMid[] = { 0.85f, 0.60f, 0.40f, 0.20f, 0.05f };
    *out01 = kMid[lvl];
    return true;
}

static bool OakBatch4ProbeQuality(uintptr_t entity, int* outTier, float* outRaw)
{
    if (!outTier) return false;
    *outTier = -1;
    if (outRaw) *outRaw = -1.f;
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    unsigned h = (unsigned)((entity >> 4) ^ (entity >> 12)) & (kBatch4QualityCache - 1);
    OakBatch4QualitySlot& slot = g_Batch4QualityCache[h];
    DWORD now = GetTickCount();
    if (slot.ent == entity && slot.tier != -1 && (now - slot.tick) < 1000)
    {
        if (slot.tier >= 0)
        {
            *outTier = slot.tier;
            if (outRaw) *outRaw = slot.raw;
            return true;
        }
        return false;
    }

    if (g_Batch4QualityProbes >= g_Batch4Esp.quality.maxProbesPerFrame)
    {
        if (slot.ent == entity && slot.tier >= 0)
        {
            *outTier = slot.tier;
            if (outRaw) *outRaw = slot.raw;
            return true;
        }
        return false;
    }
    g_Batch4QualityProbes++;

    bool ok = false;
    int tier = -1;
    float health01 = -1.f;
    char tn[64] = {};
    __try
    {
        ReadEntityTypeName(entity, tn, 64);
        tier = OakBatch4ReadItemHealthLevel(entity);
        if (tier >= 0 && tier <= 4)
        {
            ok = true;
            *outTier = tier;
            static const float kMid[] = { 0.85f, 0.60f, 0.40f, 0.20f, 0.05f };
            health01 = kMid[tier];
            if (outRaw) *outRaw = health01;
        }

        static int s_QOk = 0;
        static int s_QMiss = 0;
        if (ok && s_QOk < 24)
        {
            char db[160];
            wsprintfA(db, "quality: ok tier=%d lvl=%d '%s'",
                tier, tier, tn[0] ? tn : "?");
            Log(db);
            s_QOk++;
        }
        else if (!ok && s_QMiss < 8)
        {
            char db[160];
            wsprintfA(db, "quality: miss '%s'", tn[0] ? tn : "?");
            Log(db);
            s_QMiss++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

    slot.ent = entity;
    slot.tick = now;
    if (ok && tier >= 0)
    {
        slot.tier = tier;
        slot.raw = health01;
        return true;
    }
    slot.tier = -2;
    return false;
}

// Live (2026-08-08): ItemBase quantity block lives behind entity+0x748.
// Layout at that object: float qty@+0xF8, float prev@+0xFC, int init@+0x100,
// int min@+0x104, int max@+0x108  (matches m_VarQuantity / Prev / Init / Min / Max).
enum { kBatch4QtyRootOff = 0x748 };
enum { kBatch4QtyFloatOff = 0xF8 };
enum { kBatch4QtyMaxOff = 0x108 };
enum { kBatch4QtyMinOff = 0x104 };
enum { kBatch4QtyCache = 96 };
struct OakBatch4QtySlot {
    uintptr_t ent;
    int qty;
    DWORD tick;
};
static OakBatch4QtySlot g_Batch4QtyCache[kBatch4QtyCache];
static uintptr_t g_Batch4QtyRootOff = kBatch4QtyRootOff; // allow live retarget if 0x748 goes stale

static bool OakBatch4QtyBlockLooksValid(uintptr_t block, float* outQty, int* outMax)
{
    if (!IsValidPtr(block) || block < 0x100000000)
        return false;
    if (g_GameModule)
    {
        uintptr_t mod = (uintptr_t)g_GameModule;
        if (block >= mod && block < mod + 0x8000000ULL)
            return false;
    }
    float qty = Read<float>(block + kBatch4QtyFloatOff);
    int mn = Read<int>(block + kBatch4QtyMinOff);
    int mx = Read<int>(block + kBatch4QtyMaxOff);
    if (!std::isfinite(qty))
        return false;
    // HasQuantity() false → max stays 0; clothing/junk often BAD ptr or garbage max.
    if (mx < 2 || mx > 500000)
        return false;
    if (mn < -1 || mn > mx)
        return false;
    if (qty < (float)mn - 0.5f || qty > (float)mx + 0.5f)
        return false;
    if (outQty) *outQty = qty;
    if (outMax) *outMax = mx;
    return true;
}

static bool OakBatch4ReadQuantityRaw(uintptr_t entity, float* outQty, int* outMax)
{
    if (!outQty) return false;
    *outQty = -1.f;
    if (outMax) *outMax = -1;
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    const uintptr_t rootOffs[] = {
        g_Batch4QtyRootOff, 0x748, 0x740, 0x750, 0x738, 0x758
    };
    for (int i = 0; i < (int)(sizeof(rootOffs) / sizeof(rootOffs[0])); i++)
    {
        uintptr_t off = rootOffs[i];
        if (!off) continue;
        uintptr_t block = Read<uintptr_t>(entity + off);
        float qty = -1.f;
        int mx = -1;
        if (!OakBatch4QtyBlockLooksValid(block, &qty, &mx))
            continue;
        if (off != g_Batch4QtyRootOff)
            g_Batch4QtyRootOff = off;
        *outQty = qty;
        if (outMax) *outMax = mx;
        return true;
    }
    return false;
}

static bool OakBatch4ProbeQuantity(uintptr_t entity, int* outQty)
{
    if (!outQty) return false;
    *outQty = -1;
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    unsigned h = (unsigned)((entity >> 4) ^ (entity >> 12)) & (kBatch4QtyCache - 1);
    OakBatch4QtySlot& slot = g_Batch4QtyCache[h];
    DWORD now = GetTickCount();
    // qty >= 0 hit; qty == -2 cached miss (HasQuantity=false / junk).
    if (slot.ent == entity && slot.qty != -1 && (now - slot.tick) < 1000)
    {
        if (slot.qty >= 0)
        {
            *outQty = slot.qty;
            return true;
        }
        return false;
    }

    if (g_Batch4QtyProbes >= g_Batch4Esp.quantity.maxProbesPerFrame)
    {
        if (slot.ent == entity && slot.qty >= 0)
        {
            *outQty = slot.qty;
            return true;
        }
        return false;
    }
    g_Batch4QtyProbes++;

    bool ok = false;
    int qty = -1;
    char tn[64] = {};
    __try
    {
        // Magazines / ammo boxes: prefer classic ammo ints when present.
        char cfg[64] = {};
        ReadEntityTypeName(entity, tn, 64);
        ReadEntityConfigName(entity, cfg, 64);
        if (StrContainsI(tn, "Mag_") || StrContainsI(tn, "Magazine") ||
            StrContainsI(cfg, "magazine"))
        {
            int ammo = -1, cap = -1;
            if (MagLooksValid(entity, 0, ammo, cap, true) && ammo >= 0)
            {
                qty = ammo;
                ok = true;
            }
        }

        if (!ok)
        {
            float raw = -1.f;
            int mx = -1;
            if (OakBatch4ReadQuantityRaw(entity, &raw, &mx))
            {
                qty = (int)(raw + 0.5f);
                // Show stacks/liquids/energy (max>=2). Skip barren HasQuantity=false.
                if (qty >= 1 && mx >= 2)
                    ok = true;
            }
        }

        static int s_QtyOkDiag = 0;
        static int s_QtyMissDiag = 0;
        if (ok && s_QtyOkDiag < 24)
        {
            char db[160];
            wsprintfA(db, "quantity: ok ent..%04X qty=%d '%s'",
                (unsigned)(entity & 0xFFFF), qty, tn[0] ? tn : "?");
            Log(db);
            s_QtyOkDiag++;
        }
        else if (!ok && s_QtyMissDiag < 8)
        {
            char db[160];
            wsprintfA(db, "quantity: miss ent..%04X '%s'",
                (unsigned)(entity & 0xFFFF), tn[0] ? tn : "?");
            Log(db);
            s_QtyMissDiag++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }

    slot.ent = entity;
    slot.tick = now;
    if (ok && qty >= 0)
    {
        slot.qty = qty;
        *outQty = qty;
        return true;
    }
    slot.qty = -2; // cached miss
    return false;
}

static bool OakBatch4VisColorsActive(bool isPlayer)
{
    if (isPlayer && !g_PlayerUseVisColors) return false;
    if (!isPlayer && !g_ZombieUseVisColors) return false;
    // Style toggle alone enables the feature (no separate batch4 master required).
    return true;
}

static bool OakBatch4IsEntityOccluded(uintptr_t entity, int listIdx, bool isPlayer)
{
    if (!entity) return false;
    (void)listIdx; // Near/Far list flips cause ESP flash — do not use for color.

    OakBatch4VisSlot& slot = OakBatch4VisSlotFor(entity);
    DWORD now = GetTickCount();
    if (slot.ent == entity && (now - slot.tick) < 400)
        return slot.occluded;

    bool raw = false;
    __try
    {
        if (IsValidPtr(g_Batch4VisWorld) && g_W2S.valid && g_Batch4VisLosChecks < 120)
        {
            g_Batch4VisLosChecks++;
            Vec3 head = GetBonePosition(entity, BONE_HEAD, isPlayer);
            Vec3 chest = GetBonePosition(entity, BONE_SPINE1, isPlayer);
            if (head.x == 0.f && head.y == 0.f && head.z == 0.f)
            {
                if (GetEntityPosition(entity, head))
                    head.y += isPlayer ? 1.6f : 1.4f;
            }
            if (chest.x == 0.f && chest.y == 0.f && chest.z == 0.f)
                chest = head;

            bool headOcc = false, chestOcc = false;
            if (!(head.x == 0.f && head.y == 0.f && head.z == 0.f))
                headOcc = OakBatch4IsOccludedScreen(g_Batch4VisWorld, g_Batch4VisLocal, entity, head);
            if (!(chest.x == 0.f && chest.y == 0.f && chest.z == 0.f))
                chestOcc = OakBatch4IsOccludedScreen(g_Batch4VisWorld, g_Batch4VisLocal, entity, chest);
            // Hidden only when both samples are blocked (peek = visible).
            raw = headOcc && chestOcc;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { raw = false; }

    if (slot.ent != entity)
    {
        slot.ent = entity;
        slot.sticky = 0;
        slot.occluded = raw;
        slot.boneOcc = 0;
        slot.boneTick = 0;
    }
    // Stronger hysteresis so colors don't strobe when LOS jitters.
    if (raw)
    {
        if (slot.sticky < 6) slot.sticky++;
    }
    else
    {
        if (slot.sticky > -6) slot.sticky--;
    }
    if (slot.sticky >= 3) slot.occluded = true;
    else if (slot.sticky <= -3) slot.occluded = false;
    // else keep previous slot.occluded

    if (slot.occluded) g_OakVisDbgOcc++;
    else g_OakVisDbgVis++;
    static DWORD s_VisLogTick = 0;
    if (!s_VisLogTick || (now - s_VisLogTick) > 10000)
    {
        s_VisLogTick = now;
        char b[160];
        wsprintfA(b, "vis-colors: occ=%d vis=%d occN=%d near60=%d raw=%d n=%s",
            g_OakVisDbgOcc, g_OakVisDbgVis, g_OakOccN, g_OakOccDbgNear,
            raw ? 1 : 0,
            g_OakOccDbgSample[0] ? g_OakOccDbgSample : "-");
        Log(b);
        g_OakVisDbgOcc = g_OakVisDbgVis = 0;
    }

    slot.tick = now;
    return slot.occluded;
}

// Per-bone occlusion for split skeleton / box. Bits written into entity vis slot.
static void OakBatch4UpdateBoneOccMask(uintptr_t entity, bool isPlayer,
    const Vec3* bonePos, const bool* boneOk, int boneCount)
{
    if (!entity || !bonePos || !boneOk || boneCount <= 0) return;
    if (!OakBatch4VisColorsActive(isPlayer)) return;
    if (!IsValidPtr(g_Batch4VisWorld) || !g_W2S.valid) return;

    OakBatch4VisSlot& slot = OakBatch4VisSlotFor(entity);
    DWORD now = GetTickCount();
    if (slot.ent == entity && slot.boneTick && (now - slot.boneTick) < 180)
        return;

    uint32_t mask = 0;
    // Prefer silhouette bones first (head→feet, arms) so splits look right under budget.
    static const int kPri[] = {
        BONE_HEAD, BONE_NECK, BONE_SPINE2, BONE_SPINE1, BONE_PELVIS,
        BONE_L_SHOULDER, BONE_R_SHOULDER, BONE_L_ELBOW, BONE_R_ELBOW,
        BONE_L_HAND, BONE_R_HAND, BONE_L_HIP, BONE_R_HIP,
        BONE_L_KNEE, BONE_R_KNEE, BONE_L_FOOT, BONE_R_FOOT
    };
    int budget = 14;
    __try
    {
        for (int pi = 0; pi < (int)(sizeof(kPri) / sizeof(kPri[0])); pi++)
        {
            int i = kPri[pi];
            if (i < 0 || i >= boneCount) continue;
            if (!boneOk[i]) continue;
            if (budget <= 0) break;
            if (bonePos[i].x == 0.f && bonePos[i].y == 0.f && bonePos[i].z == 0.f)
                continue;
            budget--;
            g_Batch4VisLosChecks++;
            if (g_Batch4VisLosChecks > 220) break;
            if (OakBatch4IsOccludedScreen(g_Batch4VisWorld, g_Batch4VisLocal, entity, bonePos[i]))
                mask |= (1u << i);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (slot.ent != entity)
    {
        slot.ent = entity;
        slot.sticky = 0;
        slot.occluded = false;
    }
    slot.boneOcc = mask;
    slot.boneTick = now;
}

static bool OakBatch4BoneOccluded(uintptr_t entity, int boneId)
{
    if (!entity || boneId < 0 || boneId >= 32) return false;
    OakBatch4VisSlot& slot = OakBatch4VisSlotFor(entity);
    if (slot.ent != entity) return false;
    return (slot.boneOcc & (1u << boneId)) != 0;
}

static const float* OakBatch4VisColorPair(bool isPlayer, bool occluded)
{
    if (isPlayer)
        return occluded ? g_PlayerColorOccluded : g_PlayerColorVisible;
    return occluded ? g_ZombieColorOccluded : g_ZombieColorVisible;
}

static bool OakBatch4ProbePointOcc(uintptr_t entity, const Vec3& worldPos)
{
    if (!IsValidPtr(g_Batch4VisWorld) || !g_W2S.valid)
        return false;
    if (g_Batch4VisLosChecks > 220)
        return false;
    g_Batch4VisLosChecks++;
    return OakBatch4IsOccludedScreen(g_Batch4VisWorld, g_Batch4VisLocal, entity, worldPos);
}

static const float* OakBatch4PickPlayerBoxColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal)
{
    if (!OakBatch4VisColorsActive(true) || !isPlayer)
        return normal;
    return OakBatch4IsEntityOccluded(entity, listIdx, true) ? g_PlayerColorOccluded : g_PlayerColorVisible;
}

static const float* OakBatch4PickZombieBoxColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal)
{
    if (!OakBatch4VisColorsActive(false) || isPlayer)
        return normal;
    return OakBatch4IsEntityOccluded(entity, listIdx, false) ? g_ZombieColorOccluded : g_ZombieColorVisible;
}

// Shared picker for skeleton / name / box.
static const float* OakBatch4PickEntityColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal)
{
    return isPlayer
        ? OakBatch4PickPlayerBoxColor(entity, listIdx, true, normal)
        : OakBatch4PickZombieBoxColor(entity, listIdx, false, normal);
}

static float OakBatch4LookingDotThreshold()
{
    int ang = g_Batch4Esp.lookingAtMe.angleDeg;
    if (ang < 5) ang = 5;
    if (ang > 90) ang = 90;
    return cosf((float)ang * 3.14159265f / 180.f);
}

static bool OakBatch4IsLookingAtMe(uintptr_t entity, int distM, bool isPlayer)
{
    if (!g_Batch4Esp.lookingAtMe.enabled || distM < 0 ||
        distM > g_Batch4Esp.lookingAtMe.maxDistanceM)
        return false;
    if (isPlayer && !g_Batch4Esp.lookingAtMe.players)
        return false;
    if (!isPlayer && !g_Batch4Esp.lookingAtMe.zombies)
        return false;

    unsigned h = (unsigned)((entity >> 4) ^ (entity >> 12)) & (kBatch4LookCache - 1);
    OakBatch4LookSlot& slot = g_Batch4LookCache[h];
    DWORD now = GetTickCount();
    if (slot.ent == entity && (now - slot.tick) < (DWORD)OAK_BATCH4_LOOKING_THROTTLE_MS)
        return slot.looking;

    // Budget covers players + zombies in the same frame.
    if (g_Batch4LookingChecks >= 48)
        return slot.ent == entity ? slot.looking : false;
    g_Batch4LookingChecks++;

    bool looking = false;
    __try
    {
        Vec3 fwd{};
        if (!OakGetEntityLookDir(entity, fwd))
            goto done;

        Vec3 epos{};
        if (!GetEntityPosition(entity, epos))
            goto done;

        // Aim at local camera/player eye height.
        Vec3 me = g_CameraValid ? g_CameraPos : g_LocalPlayerPos;
        if (!g_CameraValid && g_LocalPlayerValid)
            me.y = g_LocalPlayerPos.y + 1.6f;

        Vec3 toMe = { me.x - epos.x, me.y - epos.y, me.z - epos.z };
        float tLen = sqrtf(toMe.x * toMe.x + toMe.y * toMe.y + toMe.z * toMe.z);
        if (tLen < 0.5f) goto done;
        toMe.x /= tLen; toMe.y /= tLen; toMe.z /= tLen;

        float dot = fwd.x * toMe.x + fwd.y * toMe.y + fwd.z * toMe.z;
        looking = (dot >= OakBatch4LookingDotThreshold());
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { looking = false; }

done:
    slot.ent = entity;
    slot.tick = now;
    slot.looking = looking;
    return looking;
}

static void OakBatch4DrawLookingAtMe(uintptr_t entity, int dist, bool isPlayer)
{
    if (g_PanicHidden || !entity) return;
    if (!g_Batch4Esp.lookingAtMe.enabled) return;
    if (!OakBatch4IsLookingAtMe(entity, dist, isPlayer))
        return;

    Vec3 head = GetBonePosition(entity, BONE_HEAD, isPlayer);
    Vec2 hs;
    if (!WorldToScreen(head, hs))
        return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    const float* c = isPlayer ? g_Batch4Esp.lookingAtMe.color : g_Batch4Esp.lookingAtMe.colorZombie;
    ImU32 col = ToCol(c[0], c[1], c[2], c[3]);
    dl->AddCircle(ImVec2(hs.x, hs.y - 16.f), 10.f, col, 16, 2.2f);
    DrawEspBadge(hs.x, hs.y - 32.f, "LOOKING", c[0], c[1], c[2], c[3]);
}

static bool OakBatch4ReadHandsWeaponName(uintptr_t player, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(player)) return false;
    __try
    {
        uintptr_t inv = Read<uintptr_t>(player + 0x650);
        if (!IsValidPtr(inv)) return false;
        uintptr_t hands = Read<uintptr_t>(inv + 0x1B0);
        if (!IsValidPtr(hands)) return false;
        return OakBatch4ReadItemName(hands, out, outMax);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}

#include "oak_inventory_impl.inl"

enum { kBatch4ContCache = 128, kBatch4ContCollectMax = 64 };
struct OakBatch4ContCacheSlot {
    uintptr_t ent;
    DWORD probeTick;   // last successful content update
    DWORD drawTick;    // last on-screen draw
    float sx, sy;      // smoothed screen anchor
    bool hasPos;
    int n;             // displayed lines stored
    int total;         // total unique item types found
    int more;          // total - shown
    char lines[OAK_BATCH4_MAX_CONTAINER_LINES][48];
};
static OakBatch4ContCacheSlot g_Batch4ContCache[kBatch4ContCache];

static void OakBatch4SortContLines(char lines[][48], int n)
{
    for (int i = 0; i < n; i++)
    {
        for (int j = i + 1; j < n; j++)
        {
            if (StrCmpI(lines[j], lines[i]) < 0)
            {
                char tmp[48];
                lstrcpynA(tmp, lines[i], 48);
                lstrcpynA(lines[i], lines[j], 48);
                lstrcpynA(lines[j], tmp, 48);
            }
        }
    }
}

static OakBatch4ContCacheSlot* OakBatch4FindContCache(uintptr_t entity, bool create)
{
    int freeIdx = -1;
    int oldestIdx = 0;
    DWORD oldestTick = 0xFFFFFFFFu;
    for (int i = 0; i < kBatch4ContCache; i++)
    {
        if (g_Batch4ContCache[i].ent == entity)
            return &g_Batch4ContCache[i];
        if (freeIdx < 0 && g_Batch4ContCache[i].ent == 0)
            freeIdx = i;
        DWORD t = g_Batch4ContCache[i].drawTick
            ? g_Batch4ContCache[i].drawTick
            : g_Batch4ContCache[i].probeTick;
        if (t < oldestTick)
        {
            oldestTick = t;
            oldestIdx = i;
        }
    }
    if (!create) return nullptr;
    int idx = (freeIdx >= 0) ? freeIdx : oldestIdx;
    ZeroMemory(&g_Batch4ContCache[idx], sizeof(g_Batch4ContCache[idx]));
    g_Batch4ContCache[idx].ent = entity;
    return &g_Batch4ContCache[idx];
}

// Refresh cargo into cache on a slow cadence. Drawing never depends on this frame's probe.
static void OakBatch4RefreshContainerCache(uintptr_t entity)
{
    if (!IsValidPtr(entity)) return;
    if (IsValidPtr(g_Batch4VisWorld))
        OakInvSetWorld(g_Batch4VisWorld);

    OakBatch4ContCacheSlot* slot = OakBatch4FindContCache(entity, true);
    if (!slot) return;

    DWORD now = GetTickCount();
    const DWORD kProbeIntervalMs = 4000;
    // Keep serving prior contents; only re-read every few seconds.
    if (slot->n > 0 && (now - slot->probeTick) < kProbeIntervalMs)
        return;

    if (g_Batch4ContainerProbes >= g_Batch4Esp.containerContents.maxContainersPerFrame)
        return;
    g_Batch4ContainerProbes++;

    char all[kBatch4ContCollectMax][48] = {};
    int total = OakInvCollectEntityCargo(entity, all, kBatch4ContCollectMax);
    if (total <= 0)
    {
        __try
        {
            const uintptr_t rootOffs[] = { 0x658, 0x650, 0x660, 0x668 };
            for (int ri = 0; ri < 4 && total < kBatch4ContCollectMax; ri++)
            {
                uintptr_t cargo = Read<uintptr_t>(entity + rootOffs[ri]);
                if (!IsValidPtr(cargo) || cargo < 0x100000000) continue;
                total = OakInvCollectFromInventory(cargo, all, kBatch4ContCollectMax, total);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (total <= 0)
    {
        // Never wipe a good cache on a miss — only age it out after long absence.
        if (slot->n > 0 && (now - slot->probeTick) > 20000)
        {
            slot->n = 0;
            slot->total = 0;
            slot->more = 0;
        }
        return;
    }

    OakBatch4SortContLines(all, total);

    // Do not shrink a richer cache on a weaker partial read.
    if (slot->n > 0 && total < slot->total && (now - slot->probeTick) < 12000)
        return;

    int maxShow = g_Batch4Esp.containerContents.maxLines;
    if (maxShow < 1) maxShow = 1;
    if (maxShow > OAK_BATCH4_MAX_CONTAINER_LINES)
        maxShow = OAK_BATCH4_MAX_CONTAINER_LINES;

    int show = total < maxShow ? total : maxShow;
    slot->ent = entity;
    slot->probeTick = now;
    slot->total = total;
    slot->n = show;
    slot->more = (total > show) ? (total - show) : 0;
    for (int i = 0; i < show; i++)
        lstrcpynA(slot->lines[i], all[i], 48);

    static int s_ContFillLog = 0;
    if (s_ContFillLog < 8)
    {
        char tn[64] = {};
        ReadEntityTypeName(entity, tn, 64);
        char b[160];
        wsprintfA(b, "inv: cache fill '%s' show=%d total=%d more=%d first='%s'",
            tn[0] ? tn : "?", show, total, slot->more,
            (show > 0 && slot->lines[0][0]) ? slot->lines[0] : "-");
        Log(b);
        s_ContFillLog++;
    }
}

static void OakBatch4OnContainerEsp(uintptr_t entity, float screenX, float screenY)
{
    if (g_PanicHidden || !entity || !Batch4LootExtrasOn() || !g_Batch4Esp.containerContents.enabled) return;

    OakBatch4RefreshContainerCache(entity);
    OakBatch4ContCacheSlot* slot = OakBatch4FindContCache(entity, false);
    if (!slot || slot->n <= 0) return;

    DWORD now = GetTickCount();
    // Smooth anchor so badges don't jitter with W2S noise.
    if (!slot->hasPos)
    {
        slot->sx = screenX;
        slot->sy = screenY;
        slot->hasPos = true;
    }
    else
    {
        const float a = 0.28f;
        slot->sx = slot->sx + (screenX - slot->sx) * a;
        slot->sy = slot->sy + (screenY - slot->sy) * a;
    }
    slot->drawTick = now;

    float x = slot->sx;
    float y = slot->sy + 22.f;
    for (int i = 0; i < slot->n; i++)
    {
        if (!slot->lines[i][0]) continue;
        DrawEspBadge(x, y, slot->lines[i], 0.70f, 0.82f, 0.95f, 0.88f);
        y += ImGui::GetFontSize() + 3.f;
    }
    if (slot->more > 0)
    {
        char more[48];
        wsprintfA(more, "+%d more", slot->more);
        DrawEspBadge(x, y, more, 0.55f, 0.62f, 0.72f, 0.80f);
    }
}

static void OakBatch4AppendItemExtras(uintptr_t entity, char* infoText, int infoMax)
{
    if (!infoText || infoMax < 8) return;
    if (!Batch4LootExtrasOn() || !g_Batch4Esp.quantity.enabled || g_PanicHidden)
        return;
    if (!IsValidPtr(entity))
        return;

    int qty = -1;
    if (!OakBatch4ProbeQuantity(entity, &qty) || qty < 1)
        return;

    int len = 0;
    while (len < infoMax - 1 && infoText[len]) len++;
    if (len <= 0 || len > infoMax - 12)
        return;

    // Append " xN" (e.g. WaterBottle [12m] x513).
    char suf[16];
    wsprintfA(suf, " x%d", qty);
    for (int i = 0; suf[i] && len < infoMax - 1; i++)
        infoText[len++] = suf[i];
    infoText[len] = 0;
}

static void OakBatch4DrawItemQualityBadge(uintptr_t entity, float screenX, float nameY)
{
    if (!Batch4LootExtrasOn() || !g_Batch4Esp.quality.enabled || !g_Batch4Esp.quality.showBadge || g_PanicHidden)
        return;
    if (!IsValidPtr(entity))
        return;

    int tier = -1;
    float raw = -1.f;
    if (!OakBatch4ProbeQuality(entity, &tier, &raw) || tier < 0)
        return;

    const char* label = OakBatch4QualityLabel(tier);
    if (!label) return;

    static int s_QLog = 0;
    if (s_QLog < 8)
    {
        char b[96];
        wsprintfA(b, "quality: badge tier=%d hp=%d %s", tier, (int)(raw * 100.f + 0.5f), label);
        Log(b);
        s_QLog++;
    }

    float r, g, b;
    OakBatch4QualityColor(tier, &r, &g, &b);
    // Sit just above the item name badge.
    DrawEspBadge(screenX, nameY - 18.f, label, r, g, b, 0.95f);
}

static void OakBatch4OnPlayerEsp(uintptr_t entity, int dist)
{
    if (g_PanicHidden || !entity) return;

    OakBatch4DrawLookingAtMe(entity, dist, true);

    if (g_Batch4Esp.playerInvViewer.enabled && dist >= 0 &&
        dist <= g_Batch4Esp.playerInvViewer.maxDistanceM)
    {
        Vec3 head = GetBonePosition(entity, BONE_HEAD, true);
        Vec2 hs;
        if (WorldToScreen(head, hs))
        {
            float cx = g_ScreenWidth * 0.5f;
            float cy = g_ScreenHeight * 0.5f;
            float dx = hs.x - cx;
            float dy = hs.y - cy;
            float dpx = sqrtf(dx * dx + dy * dy);
            if (dpx < g_Batch4Esp.playerInvViewer.crosshairRadiusPx && dpx < g_Batch4CrosshairDistPx)
            {
                g_Batch4CrosshairDistPx = dpx;
                g_Batch4CrosshairPlayer = entity;
            }
        }
    }
}

static bool OakContamNameMatch(const char* s)
{
    if (!s || !s[0]) return false;
    return StrContainsI(s, "Contaminat") ||
        StrContainsI(s, "EffectArea");
}

static bool OakContamIsArea(const char* s)
{
    return s && s[0] && StrContainsI(s, "ContaminatedArea");
}

static bool OakContamIsTrigger(const char* s)
{
    return s && s[0] && StrContainsI(s, "ContaminatedTrigger");
}

// EffectArea::m_Radius synced float — prefer stable 50..200 values; ignore junk.
static float OakContamProbeRadiusM(uintptr_t ent, float fallback)
{
    if (!IsValidPtr(ent) || ent < 0x100000000)
        return fallback;
    float best = 0.f;
    int bestScore = -1;
    __try
    {
        for (uintptr_t off = 0x500; off < 0xA80; off += 4)
        {
            float v = Read<float>(ent + off);
            if (!(v == v) || v < 45.f || v > 220.f)
                continue;
            // Prefer exact-ish wiki radii.
            int score = 1;
            if (v >= 70.f && v <= 120.f) score = 3;
            else if (v >= 50.f && v <= 150.f) score = 2;
            if (score > bestScore)
            {
                bestScore = score;
                best = v;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return best > 1.f ? best : fallback;
}

static void OakContamCacheUpsert(uintptr_t ent, const Vec3& pos, float radiusM, const char* label, bool isArea)
{
    DWORD now = GetTickCount();
    int slot = -1;
    // Wide merge so Area + every nearby Trigger collapse to ONE zone icon.
    const float mergeR = 280.f;
    const float mergeR2 = mergeR * mergeR;
    for (int i = 0; i < g_ContamLiveN; i++)
    {
        if (g_ContamLive[i].ent == ent)
        {
            slot = i;
            break;
        }
        float dx = g_ContamLive[i].x - pos.x;
        float dz = g_ContamLive[i].z - pos.z;
        if ((dx * dx + dz * dz) < mergeR2)
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        if (g_ContamLiveN >= kOakContamCacheMax)
        {
            DWORD oldest = now;
            slot = 0;
            for (int i = 0; i < g_ContamLiveN; i++)
            {
                if (g_ContamLive[i].lastSeen < oldest)
                {
                    oldest = g_ContamLive[i].lastSeen;
                    slot = i;
                }
            }
            g_ContamLive[slot] = {};
        }
        else
        {
            slot = g_ContamLiveN++;
            g_ContamLive[slot] = {};
        }
    }
    OakContamZoneLive& z = g_ContamLive[slot];
    const bool wasArea = z.isArea;
    if (isArea || !z.ent)
        z.ent = ent;

    float r = radiusM;
    if (r < 45.f) r = 100.f;
    if (r > 250.f) r = 250.f;

    // Lock world geometry once — never rewrite from player/camera or trigger jitter.
    // Prefer Area position when upgrading a Trigger-bootstrapped slot.
    if (!z.locked)
    {
        z.x = pos.x;
        z.y = pos.y;
        z.z = pos.z;
        z.radiusM = r;
        z.locked = true;
    }
    else if (isArea && !wasArea)
    {
        // Upgrade: snap to Area center once, then stay locked.
        z.x = pos.x;
        z.y = pos.y;
        z.z = pos.z;
        if (r > z.radiusM) z.radiusM = r;
    }
    else if (isArea && r > z.radiusM + 5.f)
    {
        z.radiusM = r;
    }

    if (isArea)
        z.isArea = true;

    if (label && label[0])
    {
        if (isArea || !z.label[0] || StrCmpI(z.label, "Gas") == 0 || StrCmpI(z.label, "Zone") == 0)
            lstrcpynA(z.label, label, 40);
    }
    z.lastSeen = now;
}

static void OakContamScanWorld(uintptr_t worldPtr)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;
    DWORD now = GetTickCount();
    if (g_ContamLastScan && (now - g_ContamLastScan) < 1000)
        return;
    g_ContamLastScan = now;

    float fallback = g_Batch4Esp.contamination.defaultRadiusM;
    if (fallback < 45.f) fallback = 100.f;

    auto consider = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000)
            return;
        char cfg[64] = {};
        char tn[64] = {};
        ReadEntityConfigName(ent, cfg, 64);
        ReadEntityTypeName(ent, tn, 64);
        if (!OakContamNameMatch(cfg) && !OakContamNameMatch(tn))
            return;
        const char* raw = cfg[0] ? cfg : tn;
        // Skip pure Trigger if we only care about areas — still merge for radius,
        // but Areas drive the visible zone.
        bool isArea = OakContamIsArea(raw);
        bool isTrig = OakContamIsTrigger(raw);
        if (!isArea && !isTrig && !StrContainsI(raw, "EffectArea"))
            return;

        Vec3 pos{};
        if (!GetEntityPosition(ent, pos))
            return;
        // Reject nonsense positions
        if (!(pos.x == pos.x) || pos.x < 1.f || pos.x > 50000.f)
            return;

        float r = OakContamProbeRadiusM(ent, isTrig ? 75.f : fallback);
        char shortLab[40] = {};
        if (isArea)
        {
            const char* p = raw;
            if (StrContainsI(raw, "ContaminatedArea_"))
                p = raw + 17;
            lstrcpynA(shortLab, p, 40);
        }
        else if (isTrig)
            lstrcpynA(shortLab, "Gas", 40);
        else
            lstrcpynA(shortLab, "Zone", 40);

        OakContamCacheUpsert(ent, pos, r, shortLab, isArea || StrContainsI(raw, "EffectArea"));
    };

    __try
    {
        {
            uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
            int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
            if (IsValidPtr(data) && count > 0 && count < 8000)
            {
                int n = count > 1500 ? 1500 : count;
                for (int i = 0; i < n; i++)
                {
                    uintptr_t entry = data + (uintptr_t)i * 0x18;
                    if (Read<WORD>(entry) == 0) continue;
                    consider(Read<uintptr_t>(entry + 8));
                }
            }
        }
        const uintptr_t lists[] = {
            oak_offsets::world::NearEntList,
            oak_offsets::world::FarEntList
        };
        for (int li = 0; li < 2; li++)
        {
            uintptr_t data = Read<uintptr_t>(worldPtr + lists[li]);
            int count = Read<int>(worldPtr + lists[li] + 8);
            if (li == 1)
            {
                int farC = Read<int>(worldPtr + oak_offsets::world::FarTableSize);
                if (farC > 0 && farC < 8000) count = farC;
            }
            if (!IsValidPtr(data) || count <= 0 || count > 8000)
                continue;
            int n = count > 600 ? 600 : count;
            for (int i = 0; i < n; i++)
                consider(Read<uintptr_t>(data + (uintptr_t)i * 8));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Keep zones for a long time so walking inside doesn't "lose" them when
    // the entity drops out of Near/Far for a moment.
    for (int i = 0; i < g_ContamLiveN; )
    {
        if (now - g_ContamLive[i].lastSeen > 60000)
        {
            g_ContamLive[i] = g_ContamLive[g_ContamLiveN - 1];
            g_ContamLiveN--;
        }
        else
            i++;
    }

    // Drop Trigger leftovers that sit near a locked Area (one icon per gas).
    for (int i = 0; i < g_ContamLiveN; )
    {
        bool drop = false;
        if (!g_ContamLive[i].isArea)
        {
            for (int j = 0; j < g_ContamLiveN; j++)
            {
                if (i == j || !g_ContamLive[j].isArea) continue;
                float dx = g_ContamLive[i].x - g_ContamLive[j].x;
                float dz = g_ContamLive[i].z - g_ContamLive[j].z;
                if ((dx * dx + dz * dz) < (300.f * 300.f))
                {
                    drop = true;
                    break;
                }
            }
        }
        // Also collapse two non-Area "Gas" slots that share a center.
        if (!drop && !g_ContamLive[i].isArea)
        {
            for (int j = 0; j < i; j++)
            {
                float dx = g_ContamLive[i].x - g_ContamLive[j].x;
                float dz = g_ContamLive[i].z - g_ContamLive[j].z;
                if ((dx * dx + dz * dz) < (280.f * 280.f))
                {
                    drop = true;
                    break;
                }
            }
        }
        if (drop)
        {
            g_ContamLive[i] = g_ContamLive[g_ContamLiveN - 1];
            g_ContamLiveN--;
        }
        else
            i++;
    }
}

// Soft W2S for ring verts: keeps off-screen coords (no screen cull). Skipping
// those used to pack non-adjacent verts together and shred the circle.
static bool OakContamSoftW2S(float wx, float wy, float wz, float& sx, float& sy)
{
    if (!g_W2S.valid)
        return false;
    float tx = wx - g_W2S.translation.x;
    float ty = wy - g_W2S.translation.y;
    float tz = wz - g_W2S.translation.z;
    float x = tx * g_W2S.right.x + ty * g_W2S.right.y + tz * g_W2S.right.z;
    float y = tx * g_W2S.up.x + ty * g_W2S.up.y + tz * g_W2S.up.z;
    float z = tx * g_W2S.forward.x + ty * g_W2S.forward.y + tz * g_W2S.forward.z;
    if (z < 0.08f)
        return false;
    float invZ = 1.0f / z;
    sx = g_ScreenHalfW + ((x / g_W2S.projX) * invZ) * g_ScreenHalfW;
    sy = g_ScreenHalfH - ((y / g_W2S.projY) * invZ) * g_ScreenHalfH;
    return true;
}

// Clip camera-space segment a->b against z = zNear; returns clipped endpoints in front.
static int OakContamClipCamSeg(
    float ax, float ay, float az, float bx, float by, float bz, float zNear,
    float out[2][3])
{
    const bool aF = az >= zNear;
    const bool bF = bz >= zNear;
    if (aF && bF)
    {
        out[0][0] = ax; out[0][1] = ay; out[0][2] = az;
        out[1][0] = bx; out[1][1] = by; out[1][2] = bz;
        return 2;
    }
    if (!aF && !bF)
        return 0;
    float t = (zNear - az) / (bz - az);
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    float ix = ax + (bx - ax) * t;
    float iy = ay + (by - ay) * t;
    float iz = zNear;
    if (aF)
    {
        out[0][0] = ax; out[0][1] = ay; out[0][2] = az;
        out[1][0] = ix; out[1][1] = iy; out[1][2] = iz;
    }
    else
    {
        out[0][0] = ix; out[0][1] = iy; out[0][2] = iz;
        out[1][0] = bx; out[1][1] = by; out[1][2] = bz;
    }
    return 2;
}

static void OakContamCamToScreen(float x, float y, float z, float& sx, float& sy)
{
    float invZ = 1.0f / z;
    sx = g_ScreenHalfW + ((x / g_W2S.projX) * invZ) * g_ScreenHalfW;
    sy = g_ScreenHalfH - ((y / g_W2S.projY) * invZ) * g_ScreenHalfH;
}

// Draw a full world circle that stays continuous when partially off-screen.
// Behind-camera segments are near-plane clipped instead of dropping verts
// (dropping verts used to connect random points and look broken).
static void OakContamDrawWorldRing(ImDrawList* dl, float zx, float zz, float radiusM,
    float ringY, ImU32 lineCol, float thickness, ImU32 fillCol, bool doFill)
{
    if (!dl || !g_W2S.valid || radiusM < 1.f)
        return;

    const int segs = 72;
    const float zNear = 0.12f;

    float camX[73], camY[73], camZ[73];
    for (int s = 0; s <= segs; s++)
    {
        float ang = (float)s / (float)segs * 6.2831853f;
        float wx = zx + cosf(ang) * radiusM;
        float wz = zz + sinf(ang) * radiusM;
        float tx = wx - g_W2S.translation.x;
        float ty = ringY - g_W2S.translation.y;
        float tz = wz - g_W2S.translation.z;
        camX[s] = tx * g_W2S.right.x + ty * g_W2S.right.y + tz * g_W2S.right.z;
        camY[s] = tx * g_W2S.up.x + ty * g_W2S.up.y + tz * g_W2S.up.z;
        camZ[s] = tx * g_W2S.forward.x + ty * g_W2S.forward.y + tz * g_W2S.forward.z;
    }

    ImVec2 run[160];
    int rn = 0;

    auto flushRun = [&]() {
        if (rn >= 2)
            dl->AddPolyline(run, rn, lineCol, 0, thickness);
        rn = 0;
    };

    for (int s = 0; s < segs; s++)
    {
        float out[2][3];
        int n = OakContamClipCamSeg(
            camX[s], camY[s], camZ[s],
            camX[s + 1], camY[s + 1], camZ[s + 1],
            zNear, out);
        if (n == 0)
        {
            flushRun();
            continue;
        }

        float s0x, s0y, s1x, s1y;
        OakContamCamToScreen(out[0][0], out[0][1], out[0][2], s0x, s0y);
        OakContamCamToScreen(out[1][0], out[1][1], out[1][2], s1x, s1y);

        if (rn > 0)
        {
            float ddx = run[rn - 1].x - s0x;
            float ddy = run[rn - 1].y - s0y;
            if ((ddx * ddx + ddy * ddy) > 9.f)
                flushRun();
        }
        if (rn == 0 && rn < 160)
            run[rn++] = ImVec2(s0x, s0y);
        if (rn < 160)
            run[rn++] = ImVec2(s1x, s1y);
    }
    flushRun();

    // Closed fill only when the full ring is in front (no near-plane cuts).
    if (doFill)
    {
        ImVec2 full[73];
        int fn = 0;
        bool ok = true;
        for (int s = 0; s < segs; s++)
        {
            if (camZ[s] < zNear) { ok = false; break; }
            OakContamCamToScreen(camX[s], camY[s], camZ[s], full[fn].x, full[fn].y);
            fn++;
        }
        if (ok && fn >= 3)
            dl->AddConvexPolyFilled(full, fn, fillCol);
    }
}

static int OakContamProjectRing(float zx, float zz, float radiusM, float ringY,
    ImVec2* pts, int maxPts)
{
    // Legacy helper kept for spokes; soft W2S, no screen cull.
    if (!pts || maxPts < 8) return 0;
    const int segments = maxPts - 1;
    int valid = 0;
    __try
    {
        for (int s = 0; s <= segments; s++)
        {
            float ang = (float)s / (float)segments * 6.2831853f;
            float sx, sy;
            if (!OakContamSoftW2S(
                    zx + cosf(ang) * radiusM, ringY, zz + sinf(ang) * radiusM,
                    sx, sy))
                continue;
            pts[valid++] = ImVec2(sx, sy);
            if (valid >= maxPts) break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return valid; }
    return valid;
}

// Always-on icon projector: works in front AND behind the camera. Behind
// targets flip through the camera and clamp to the screen edge so the diamond
// never disappears when you turn away. Uses locked world Y only — never player Y.
static bool OakContamProjectIcon(float wx, float wy, float wz, float& sx, float& sy)
{
    if (!g_W2S.valid)
        return false;

    float tx = wx - g_W2S.translation.x;
    float ty = wy - g_W2S.translation.y;
    float tz = wz - g_W2S.translation.z;

    float x = tx * g_W2S.right.x + ty * g_W2S.right.y + tz * g_W2S.right.z;
    float y = tx * g_W2S.up.x + ty * g_W2S.up.y + tz * g_W2S.up.z;
    float z = tx * g_W2S.forward.x + ty * g_W2S.forward.y + tz * g_W2S.forward.z;

    const bool behind = z < 0.65f;
    if (behind)
    {
        x = -x;
        y = -y;
        z = -z;
        if (z < 0.65f) z = 0.65f;
    }

    float invZ = 1.0f / z;
    float nx = (x / g_W2S.projX) * invZ;
    float ny = (y / g_W2S.projY) * invZ;
    sx = g_ScreenHalfW + (nx * g_ScreenHalfW);
    sy = g_ScreenHalfH - (ny * g_ScreenHalfH);

    // Off-screen or behind → pin to screen edge along direction from center.
    const float pad = 28.f;
    const float minX = pad, maxX = g_ScreenWidth - pad;
    const float minY = pad, maxY = g_ScreenHeight - pad;
    const bool off =
        behind || sx < minX || sx > maxX || sy < minY || sy > maxY;
    if (off)
    {
        float dx = sx - g_ScreenHalfW;
        float dy = sy - g_ScreenHalfH;
        if (fabsf(dx) < 0.001f && fabsf(dy) < 0.001f)
            dy = -1.f;
        float ax = fabsf(dx) / (g_ScreenHalfW - pad);
        float ay = fabsf(dy) / (g_ScreenHalfH - pad);
        float t = (ax > ay) ? ax : ay;
        if (t < 0.001f) t = 1.f;
        sx = g_ScreenHalfW + dx / t;
        sy = g_ScreenHalfH + dy / t;
        if (sx < minX) sx = minX;
        if (sx > maxX) sx = maxX;
        if (sy < minY) sy = minY;
        if (sy > maxY) sy = maxY;
    }
    return true;
}

static void OakContamDrawScreenMarker(ImDrawList* dl, float zx, float zz,
    const char* label, const float* col, float markY)
{
    if (!dl) return;

    float csx = 0.f, csy = 0.f;
    if (!OakContamProjectIcon(zx, markY, zz, csx, csy))
        return;

    // One clean icon + short name — no "Gas 344m r=75" spam.
    const float s = 7.f;
    dl->AddQuadFilled(
        ImVec2(csx, csy - s), ImVec2(csx + s, csy),
        ImVec2(csx, csy + s), ImVec2(csx - s, csy),
        ToCol(col[0], col[1], col[2], 0.98f));
    dl->AddQuad(
        ImVec2(csx, csy - s), ImVec2(csx + s, csy),
        ImVec2(csx, csy + s), ImVec2(csx - s, csy),
        ToCol(0.05f, 0.08f, 0.05f, 0.85f), 1.2f);

    if (g_Batch4Esp.contamination.showLabels && label && label[0])
        DrawEspBadge(csx, csy - 18.f, label, col[0], col[1], col[2], 0.95f);
}

static void OakBatch4DrawContaminationCircles()
{
    if (!Batch4LootExtrasOn() || !g_Batch4Esp.contamination.enabled || g_PanicHidden) return;
    if (!g_LocalPlayerValid && !g_CameraValid) return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    // Rings sit underground by default; slider is fine-tune only.
    // Never track camera/player Y.
    Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    float yOff = g_Batch4Esp.contamination.yOffsetM;
    if (yOff < -40.f) yOff = -40.f;
    if (yOff > 20.f) yOff = 20.f;
    const float kUndergroundBaseM = 0.f; // was -22 (rings drew underground)

    const float* col = g_Batch4Esp.contamination.color;
    ImU32 lineCol = ToCol(col[0], col[1], col[2], 0.95f);
    ImU32 fillCol = ToCol(col[0], col[1], col[2], 0.16f);
    ImU32 groundCol = ToCol(col[0], col[1], col[2], 0.45f);

    const bool useLive = g_ContamLiveN > 0;
    const int liveN = useLive ? g_ContamLiveN : (int)(sizeof(k_ChernarusContam) / sizeof(k_ChernarusContam[0]));

    for (int zi = 0; zi < liveN; zi++)
    {
        float zx, zz, radiusM, zoneGroundY;
        const char* label;
        if (useLive)
        {
            const OakContamZoneLive& z = g_ContamLive[zi];
            zx = z.x; zz = z.z; radiusM = z.radiusM; zoneGroundY = z.y;
            label = z.label[0] ? z.label : "Gas";
        }
        else
        {
            const OakChernarusContamZone& z = k_ChernarusContam[zi];
            zx = z.x; zz = z.z; radiusM = z.radiusM;
            zoneGroundY = 0.f;
            label = z.label;
        }
        if (radiusM < 45.f) radiusM = 100.f;
        if (!(zoneGroundY == zoneGroundY))
            zoneGroundY = 0.f;

        float dx = zx - ref.x;
        float dz = zz - ref.z;
        float distXZ = sqrtf(dx * dx + dz * dz);
        if (distXZ > (float)g_Batch4Esp.contamination.maxDistanceM + radiusM)
            continue;

        const bool inside = distXZ <= radiusM;
        const float ringY = zoneGroundY + kUndergroundBaseM + yOff;

        OakContamDrawWorldRing(dl, zx, zz, radiusM, ringY, lineCol,
            inside ? 3.0f : 2.6f, fillCol, !inside);

        if (g_Batch4Esp.contamination.drawGroundRing)
        {
            const float groundY = zoneGroundY - 0.5f;
            OakContamDrawWorldRing(dl, zx, zz, radiusM, groundY, groundCol,
                1.4f, 0, false);

            const int spokes = 8;
            for (int s = 0; s < spokes; s++)
            {
                float ang = (float)s / (float)spokes * 6.2831853f;
                float px = zx + cosf(ang) * radiusM;
                float pz = zz + sinf(ang) * radiusM;
                float sax, say, sbx, sby;
                if (OakContamSoftW2S(px, groundY, pz, sax, say) &&
                    OakContamSoftW2S(px, ringY, pz, sbx, sby))
                    dl->AddLine(ImVec2(sax, say), ImVec2(sbx, sby), groundCol, 1.1f);
            }
        }

        // Icon uses same locked underground Y so it stays glued to the ring.
        OakContamDrawScreenMarker(dl, zx, zz, label, col, ringY);
        g_EspDrawCount++;
    }

    static DWORD s_Log = 0;
    DWORD now = GetTickCount();
    if (!s_Log || (now - s_Log) > 5000)
    {
        s_Log = now;
        char b[120];
        wsprintfA(b, "contam: liveN=%d mode=%s yOff=%d under=%d softRing",
            g_ContamLiveN, useLive ? "entity" : "fallback",
            (int)(yOff * 100.f), (int)(kUndergroundBaseM * 100.f));
        Log(b);
    }
}

static void OakBatch4DrawPlayerInvOverlay()
{
    if (!g_Batch4Esp.playerInvViewer.enabled || g_PanicHidden) return;

    // Prefer crosshair player; fall back to local so solo testing works.
    uintptr_t target = g_Batch4CrosshairPlayer;
    if (!IsValidPtr(target) && g_LocalPlayerValid && IsValidPtr(g_ResolvedLocalPlayer))
        target = g_ResolvedLocalPlayer;
    if (!IsValidPtr(target)) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f + 36.f;
    const float* c = g_Batch4Esp.playerInvViewer.color;

    OakInvGearLine gear[8] = {};
    int n = OakInvCollectPlayerGear(target, gear, 8);
    if (n <= 0) return;

    float y = cy;
    for (int i = 0; i < n; i++)
    {
        char line[96];
        wsprintfA(line, "%s: %s", gear[i].label, gear[i].name[0] ? gear[i].name : "-");
        DrawEspBadge(cx, y, line, c[0], c[1], c[2], c[3]);
        y += ImGui::GetFontSize() + 4.f;
    }

    static DWORD s_InvLog = 0;
    DWORD now = GetTickCount();
    if (!s_InvLog || (now - s_InvLog) > 5000)
    {
        s_InvLog = now;
        bool isLocal = (target == g_ResolvedLocalPlayer);
        char b[160];
        wsprintfA(b, "inv: viewer local=%d gun='%s' plate='%s' helm='%s' bag='%s'",
            isLocal ? 1 : 0,
            (n > 0 && gear[0].name[0]) ? gear[0].name : "-",
            (n > 1 && gear[1].name[0]) ? gear[1].name : "-",
            (n > 2 && gear[2].name[0]) ? gear[2].name : "-",
            (n > 3 && gear[3].name[0]) ? gear[3].name : "-");
        Log(b);
    }
}

static void OakBatch4QualityCensusOnce(uintptr_t worldPtr)
{
    static int s_Done = 0;
    if (s_Done || !IsValidPtr(worldPtr)) return;

    int okN = 0, missN = 0;
    __try
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
        int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
        if (!IsValidPtr(data) || count <= 0 || count > 20000)
            return;
        int n = count > 256 ? 256 : count;
        for (int i = 0; i < n && okN < 8; i++)
        {
            uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
            if (!IsValidPtr(ent)) continue;
            int tier = OakBatch4ReadItemHealthLevel(ent);
            if (tier < 0) { missN++; continue; }
            char tn[64] = {};
            ReadEntityTypeName(ent, tn, 64);
            unsigned char raw = Read<unsigned char>(ent + kBatch4ItemHealthLevelOff);
            char db[160];
            wsprintfA(db, "quality: census ok tier=%d raw=%u '%s'",
                tier, (unsigned)raw, tn[0] ? tn : "?");
            Log(db);
            okN++;
        }
        char sum[96];
        wsprintfA(sum, "quality: census done ok=%d miss=%d scanned=%d", okN, missN, n);
        Log(sum);
        s_Done = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void OakBatch4CargoCensusOnce(uintptr_t worldPtr)
{
    // Bump when cargo walker changes so we re-log live layout hits.
    static int s_DoneVer = 0;
    const int kCensusVer = 7;
    if (s_DoneVer == kCensusVer || !IsValidPtr(worldPtr)) return;

    // Pull enable from INI before ImGui sync if needed.
    if (!Batch4LootExtrasOn() || !g_Batch4Esp.containerContents.enabled)
    {
        Log("inv: census skip (loot extras / containerContents off)");
        s_DoneVer = kCensusVer;
        return;
    }

    Log("inv: census begin");
    int hit = 0, scanned = 0;
    __try
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::ItemList);
        int count = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
        if (!IsValidPtr(data) || count <= 0 || count > 20000)
        {
            // Item list not ready yet — retry later without Present-rate spam.
            static DWORD s_LastMiss = 0;
            DWORD now = GetTickCount();
            if (!s_LastMiss || (now - s_LastMiss) >= 2000)
            {
                s_LastMiss = now;
                Log("inv: census defer (item list not ready)");
            }
            return;
        }
        int n = count > 400 ? 400 : count;
        for (int i = 0; i < n && hit < 6; i++)
        {
            uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
            if (!IsValidPtr(ent)) continue;
            char tn[64] = {};
            ReadEntityTypeName(ent, tn, 64);
            if (!tn[0] || !IsContainerName(tn)) continue;
            scanned++;
            if (StrContainsI(tn, "Barrel_Red"))
                OakInvDumpCargoLayout(ent, "census");
            char lines[8][48] = {};
            // Bypass per-frame probe budget for census.
            int saved = g_Batch4ContainerProbes;
            g_Batch4ContainerProbes = 0;
            int cn = OakInvCollectEntityCargo(ent, lines, 8);
            g_Batch4ContainerProbes = saved;
            char b[160];
            wsprintfA(b, "inv: census container='%s' cargoN=%d first='%s'",
                tn, cn, (cn > 0 && lines[0][0]) ? lines[0] : "-");
            Log(b);
            if (cn > 0) hit++;
        }
        char sum[96];
        wsprintfA(sum, "inv: census done hit=%d scannedContainers=%d", hit, scanned);
        Log(sum);
        s_DoneVer = kCensusVer;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void OakBatch4DrawWorldOverlays(uintptr_t worldPtr)
{
    OakInvSetWorld(worldPtr);
    OakBatch4QualityCensusOnce(worldPtr);
    OakBatch4CargoCensusOnce(worldPtr);
    if (Batch4LootExtrasOn() && g_Batch4Esp.contamination.enabled)
        OakContamScanWorld(worldPtr);
    OakBatch4DrawContaminationCircles();
    OakBatch4DrawPlayerInvOverlay();
}
