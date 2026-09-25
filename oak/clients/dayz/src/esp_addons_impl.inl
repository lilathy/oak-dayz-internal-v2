// Included from main.cpp after ResolveEntityList / GetEntityPosition / WorldToScreen.

static DWORD g_HitMarkerUntil = 0;

enum { kMaxBulletTracks = 96, kMaxTrail = 96, kMaxImpacts = 48, kMaxShotInd = 16, kMaxNearbyHit = 64 };

struct BulletTrack {
    uintptr_t ptr;
    Vec3 trail[kMaxTrail];
    int trailCount;
    Vec3 firstPos;
    Vec3 lastPos;
    Vec3 vel;
    DWORD lastSeen;
    DWORD fadeUntil;   // when finished, keep drawing until this tick
    bool fromLocal;
    bool isGrenade;
    bool active;
    bool finished;     // projectile gone — full path stays and fades
};

struct ImpactMark {
    Vec3 pos;
    DWORD expire;
    bool used;
};

struct ShotInd {
    float dirX;
    float dirZ;
    DWORD expire;
    bool used;
};

static BulletTrack g_BulletTracks[kMaxBulletTracks];
static ImpactMark g_Impacts[kMaxImpacts];
static ShotInd g_ShotInds[kMaxShotInd];
static uintptr_t g_HitCheckEnts[kMaxNearbyHit];
static int g_HitCheckCount = 0;

static void HitCheck_Clear() { g_HitCheckCount = 0; }
static void HitCheck_Add(uintptr_t e)
{
    if (!IsValidPtr(e) || g_HitCheckCount >= kMaxNearbyHit) return;
    g_HitCheckEnts[g_HitCheckCount++] = e;
}

static bool ReadEngineString(uintptr_t strObj, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(strObj) || strObj < 0x100000000) return false;
    WORD len = Read<WORD>(strObj + 0x8);
    if (len == 0 || len > 255) return false;
    int n = (len < outMax - 1) ? len : (outMax - 1);
    uintptr_t data = strObj + 0x10;
    if (!ProbeMemRange(data, (size_t)n, false, nullptr))
        return false;
    __try {
        memcpy(out, (const void*)data, (size_t)n);
        out[n] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InvalidateMemPageCacheAround(data);
        out[0] = 0;
        return false;
    }
    return out[0] != 0;
}

static bool ReadEntityConfigName(uintptr_t entity, char* out, int outMax)
{
    uintptr_t type = Read<uintptr_t>(entity + offsets::entity::EntType);
    if (!IsValidPtr(type) || type < 0x100000000) return false;
    uintptr_t cfg = Read<uintptr_t>(type + offsets::entitytype::ConfigName);
    return ReadEngineString(cfg, out, outMax);
}

static bool ReadEntityTypeName(uintptr_t entity, char* out, int outMax)
{
    uintptr_t type = Read<uintptr_t>(entity + offsets::entity::EntType);
    if (!IsValidPtr(type) || type < 0x100000000) return false;
    uintptr_t tn = Read<uintptr_t>(type + offsets::entitytype::TypeName);
    return ReadEngineString(tn, out, outMax);
}

static bool EntityIsDead(uintptr_t entity)
{
    if (!IsValidPtr(entity)) return false;
    // Confirmed dead flag only (Entity::IsDead). Do NOT OR EntityDead@0x15D — it false-positives.
    unsigned char dead = Read<unsigned char>(entity + offsets::entity::IsDead);
    return dead != 0;
}

static bool IsContainerName(const char* n)
{
    if (!n || !n[0]) return false;
    const char* keys[] = {
        "barrel", "crate", "wooden_crate", "woodencrate", "seachest", "sea_chest",
        "tent", "mediumtent", "largetent", "carptent", "partytent", "undergroundstash",
        "stash", "ammobox", "protectorcase", "giftbox", "chest", "locker", "refrigerator",
        "wreck_", "wreckage"
    };
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (StrContainsI(n, keys[i])) return true;
    return false;
}

static bool IsTrapName(const char* n)
{
    if (!n || !n[0]) return false;
    const char* keys[] = {
        "landmine", "claymore", "tripwire", "beartrap", "fishtrap", "smallfishtrap",
        "rabbittrap", "improvisedexplosive", "ied", "bomb", "mine_base"
    };
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (StrContainsI(n, keys[i])) return true;
    // bare "mine" but not mineral/miner/admin
    if (StrContainsI(n, "mine") && !StrContainsI(n, "mineral") && !StrContainsI(n, "miner") && !StrContainsI(n, "admin"))
        return true;
    if (StrContainsI(n, "trap") && !StrContainsI(n, "transport"))
        return true;
    return false;
}

static bool IsGrenadeName(const char* n)
{
    if (!n || !n[0]) return false;
    const char* keys[] = {
        "grenade", "smokegrenade", "flashgrenade", "flashbang", "rdg2", "m18smoke",
        "rgn", "rgo", "grenade_chem", "grenade_rdg", "ammo_thrown",
        "rgd5", "m67", "flashbang", "grenade_base", "thrown"
    };
    for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        if (StrContainsI(n, keys[i])) return true;
    return false;
}

static bool EntityLooksLikeGrenade(uintptr_t ent)
{
    char cfg[64]; char tn[64];
    cfg[0] = 0; tn[0] = 0;
    ReadEntityConfigName(ent, cfg, 64);
    ReadEntityTypeName(ent, tn, 64);
    return IsGrenadeName(cfg) || IsGrenadeName(tn);
}

static void AppendTrailPoint(BulletTrack* t, const Vec3& pos)
{
    if (!t) return;
    // Skip near-duplicates so the path stays long without burning slots
    if (t->trailCount > 0)
    {
        Vec3& last = t->trail[t->trailCount - 1];
        float dx = pos.x - last.x, dy = pos.y - last.y, dz = pos.z - last.z;
        if (dx * dx + dy * dy + dz * dz < 0.04f) // <20cm
            return;
    }
    if (t->trailCount < kMaxTrail)
    {
        t->trail[t->trailCount++] = pos;
        return;
    }
    // Path full: drop every other early sample, keep recent density
    int w = 0;
    for (int r = 0; r < kMaxTrail; r++)
    {
        if (r < kMaxTrail / 2)
        {
            if ((r & 1) == 0)
                t->trail[w++] = t->trail[r];
        }
        else
            t->trail[w++] = t->trail[r];
    }
    t->trailCount = w;
    if (t->trailCount < kMaxTrail)
        t->trail[t->trailCount++] = pos;
    else
        t->trail[kMaxTrail - 1] = pos;
}

// Count cartridges via Magazine bullet list when present (UC: BulletList2 @ 0x5A8).
static int CountMagazineBullets(uintptr_t mag)
{
    if (!IsValidPtr(mag) || mag < 0x100000000) return -1;
    int best = -1;
    __try
    {
        const uintptr_t listOffs[] = { 0x5A8, 0x5B0, 0xE00, 0x5A0 };
        for (int li = 0; li < 4; li++)
        {
            uintptr_t data = Read<uintptr_t>(mag + listOffs[li]);
            int cnt = Read<int>(mag + listOffs[li] + 8);
            if (cnt <= 0 || cnt > 200)
                cnt = Read<int>(mag + listOffs[li] + 4);
            if (!IsValidPtr(data) || data < 0x100000000 || cnt <= 0 || cnt > 200)
                continue;
            int live = 0;
            int lim = (cnt > 100) ? 100 : cnt;
            for (int i = 0; i < lim; i++)
            {
                uintptr_t b = Read<uintptr_t>(data + (uintptr_t)i * 8);
                if (IsValidPtr(b) && b > 0x100000000) live++;
            }
            // Return populated cartridges only — never the slot capacity (Magnum trap)
            if (live > best) best = live;
            if (cnt >= 1 && cnt <= 100)
                return live; // includes 0 after emptying the cylinder
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return best;
}

static bool MagLooksValid(uintptr_t mag, uintptr_t weaponHands, int& ammoOut, int& capOut, bool fromMagSlot = false)
{
    ammoOut = -1;
    capOut = -1;
    // Allow mag == weaponHands for internal magazines (revolvers / some pistols)
    if (!IsValidPtr(mag) || mag < 0x100000000)
        return false;
    if (g_GameModule)
    {
        uintptr_t mod = (uintptr_t)g_GameModule;
        if (mag >= mod && mag < mod + 0x5000000)
            return false;
    }

    bool ok = false;
    __try
    {
        uintptr_t typ = Read<uintptr_t>(mag + oak_offsets::entity::Type);
        if (!IsValidPtr(typ) || typ < 0x100000000)
        {
            // MagazineRef slot sometimes lacks a readable Type early — still try UC ints
            if (!fromMagSlot)
                return false;
        }

        char tn[64] = {};
        char cfg[64] = {};
        if (IsValidPtr(typ) && typ > 0x100000000)
        {
            ReadEntityTypeName(mag, tn, 64);
            ReadEntityConfigName(mag, cfg, 64);
        }

        const bool isSelfWeapon = (mag == weaponHands);
        // Self-weapon = internal mag path only (revolvers). Detached mags must not be weapons.
        if (!isSelfWeapon &&
            (StrContainsI(tn, "Weapon_") || StrContainsI(cfg, "weapon") ||
             StrCmpI(cfg, "dayzplayer") == 0 || StrContainsI(tn, "Survivor") ||
             StrContainsI(tn, "Trap") || StrContainsI(tn, "Mine")))
            return false;
        if (isSelfWeapon &&
            !(StrContainsI(tn, "Magnum") || StrContainsI(tn, "Revolver") ||
              StrContainsI(tn, "P1") || StrContainsI(tn, "IJ70") ||
              StrContainsI(tn, "Deagle") || StrContainsI(tn, "Longhorn") ||
              StrContainsI(tn, "Flaregun") || StrContainsI(tn, "Derringer")))
            return false;

        // Detached mags must look like Mag_* — OR come from WeaponInventory::MagazineRef
        const bool nameOk =
            isSelfWeapon || fromMagSlot ||
            StrContainsI(tn, "Mag_") || StrContainsI(tn, "Magazine") ||
            StrContainsI(cfg, "magazine") || StrContainsI(tn, "_Mag") ||
            StrContainsI(tn, "Speedloader") || StrContainsI(tn, "Clip");

        // UC Magazine::AmmoCount=0x6B4; updater AmmoCount=0x6AC CapA=0x6B0 CapB=0x6B4.
        struct MagLayout { uintptr_t ammo; uintptr_t capA; uintptr_t capB; };
        const MagLayout layouts[] = {
            { 0x6B4, 0x6B0, 0x6AC }, // UC ammo @ 0x6B4
            { 0x6AC, 0x6B0, 0x6B4 }, // updater
            { 0x6A8, 0x6AC, 0x6B0 },
            { 0x654, 0x658, 0x65C },
            { 0x5AC, 0x5B0, 0x5B4 },
        };

        int ammo = -1, cap = -1;
        int bulletCount = -1; // lazy — CountMagazineBullets walks lists (expensive)

        int v6ac = Read<int>(mag + 0x6AC);
        int v6b0 = Read<int>(mag + 0x6B0);
        int v6b4 = Read<int>(mag + 0x6B4);
        auto sane = [](int v) { return v >= 0 && v <= 200; };

        // Named magazine / MagRef slot: pick count/cap from classic ints
        // Updater: AmmoCount=0x6AC Cap=0x6B0/0x6B4. UC lists 0x6B4 as AmmoCount —
        // on Magnum that field is often CAPACITY (always 6) while 0x6AC decreases.
        if (nameOk)
        {
            auto isCapLike = [](int v) {
                return v == 5 || v == 6 || v == 7 || v == 8 || v == 9 || v == 10 ||
                       v == 12 || v == 15 || v == 17 || v == 20 || v == 25 || v == 30 ||
                       v == 40 || v == 45 || v == 60 || v == 75 || v == 100;
            };
            // Prefer smaller of 6AC/6B4 as ammo when one looks like capacity
            if (sane(v6ac) && sane(v6b4) && v6ac <= v6b4 && isCapLike(v6b4))
            {
                ammo = v6ac;
                cap = v6b4;
            }
            else if (sane(v6ac) && sane(v6b0) && v6ac <= v6b0 && v6b0 >= 5)
            {
                ammo = v6ac;
                cap = v6b0;
            }
            else if (sane(v6b4) && sane(v6b0) && v6b4 <= v6b0 && v6b0 >= 5)
            {
                ammo = v6b4;
                cap = v6b0;
            }
            else if (sane(v6ac) && v6ac <= 100)
            {
                ammo = v6ac;
                if (sane(v6b0) && v6b0 >= ammo) cap = v6b0;
                else if (sane(v6b4) && v6b4 >= ammo) cap = v6b4;
            }
            if (ammo >= 0 && cap < 1) cap = ammo > 0 ? ammo : 1;
            if (ammo >= 0 && cap >= 0 && ammo > cap)
            {
                int t = ammo; ammo = cap; cap = t;
            }
        }

        // Live cartridge pointers beat stale capacity ints (Magnum always-6 bug)
        {
            bool wantLive = isSelfWeapon || StrContainsI(tn, "Magnum") || StrContainsI(tn, "Revolver") ||
                            fromMagSlot || ammo < 0 || (ammo > 0 && ammo == cap);
            if (wantLive)
            {
                bulletCount = CountMagazineBullets(mag);
                if (bulletCount >= 0 && bulletCount <= 200)
                {
                    if (cap < 1 || bulletCount <= cap)
                        ammo = bulletCount;
                    if (cap < ammo) cap = ammo;
                    if (cap < 1) cap = (bulletCount <= 1) ? 1 : bulletCount;
                }
            }
        }

        for (int li = 0; li < 5 && ammo < 0; li++)
        {
            int a = Read<int>(mag + layouts[li].ammo);
            int cA = Read<int>(mag + layouts[li].capA);
            int cB = Read<int>(mag + layouts[li].capB);
            int c = -1;
            if (cA >= 1 && cA <= 200) c = cA;
            else if (cB >= 1 && cB <= 200) c = cB;
            if (a < 0 || a > 200) continue;
            if (c < 1 && nameOk) c = a > 0 ? a : 1;
            if (c < 1) continue;
            if (a > c) continue;
            ammo = a;
            cap = c;
            break;
        }

        if (ammo < 0 || (!nameOk && cap < 1))
        {
            bulletCount = CountMagazineBullets(mag);
            if (bulletCount >= 0)
            {
                ammo = bulletCount;
                if (cap < ammo) cap = ammo;
                if (cap < 1) cap = (bulletCount <= 1) ? 1 : bulletCount;
            }
        }

        // MagRef objects on this build often have 0 at 6AC/6B0/6B4 — find (ammo,cap) ONCE and cache.
        if (fromMagSlot && (ammo < 0 || cap <= 1))
        {
            static uintptr_t s_WideAmmoOff = 0; // relative offset of ammo int (cap at +4)
            static bool s_WideTried = false;
            if (s_WideAmmoOff)
            {
                int a = Read<int>(mag + s_WideAmmoOff);
                int c = Read<int>(mag + s_WideAmmoOff + 4);
                if (a >= 0 && a <= 100 && c >= 5 && c <= 100 && a <= c)
                {
                    ammo = a; cap = c;
                }
            }
            else if (!s_WideTried)
            {
                s_WideTried = true; // one Present-thread scan ever — never per-frame
                for (uintptr_t off = 0x480; off <= 0x720; off += 4)
                {
                    int a = Read<int>(mag + off);
                    int c = Read<int>(mag + off + 4);
                    if (a < 0 || a > 100 || c < 5 || c > 100 || a > c) continue;
                    if (c == 8 || c == 10 || c == 15 || c == 17 || c == 20 || c == 30 ||
                        c == 6 || c == 7 || c == 12 || c == 25 || c == 40 || c == 60)
                    {
                        s_WideAmmoOff = off;
                        ammo = a; cap = c;
                        break;
                    }
                    if (!s_WideAmmoOff) { s_WideAmmoOff = off; ammo = a; cap = c; }
                }
            }
            else if (bulletCount >= 0)
            {
                ammo = bulletCount;
                if (cap < ammo) cap = (bulletCount <= 1) ? 8 : bulletCount;
            }
        }

        if (cap < 1 || ammo < 0)
            return false;
        // Magnum / revolvers: cylinder is always 6
        if (isSelfWeapon || StrContainsI(tn, "Magnum") || StrContainsI(tn, "Revolver"))
        {
            if (ammo > 6) ammo = 6;
            cap = 6;
        }
        if (!nameOk && !(cap == 5 || cap == 6 || cap == 7 || cap == 8 || cap == 9 ||
                         cap == 10 || cap == 12 || cap == 13 || cap == 15 || cap == 17 ||
                         cap == 18 || cap == 20 || cap == 25 || cap == 30 ||
                         cap == 40 || cap == 45 || cap == 60 || cap == 75 || cap == 100))
            return false;

        if (bulletCount > ammo && bulletCount <= cap)
            ammo = bulletCount;

        ammoOut = ammo;
        capOut = cap;
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

static bool GetHandWeaponInfo(uintptr_t entity, char* nameOut, int nameMax, int* ammoOut)
{
    if (nameOut && nameMax > 0) nameOut[0] = 0;
    if (ammoOut) *ammoOut = -1;
    if (!IsValidPtr(entity)) return false;

    // Fast result cache — nested Mag_* sweeps + Log dumps were killing FPS
    static uintptr_t s_FastHands = 0;
    static char s_FastName[64] = {};
    static int s_FastAmmo = -1;
    static DWORD s_FastTick = 0;
    static bool s_FastOk = false;

    bool ok = false;
    __try
    {
        uintptr_t hands = GetLocalHandsWeapon(entity);
        if (!IsValidPtr(hands) || hands < 0x100000000)
            return false;

        DWORD nowFast = GetTickCount();
        // No multi-frame ammo cache — must update every shot
        if (hands == s_FastHands && (nowFast - s_FastTick) < 16 && !ammoOut)
        {
            if (nameOut && nameMax > 0) lstrcpynA(nameOut, s_FastName, nameMax);
            return s_FastOk;
        }

        if (nameOut && nameMax > 0)
        {
            char raw[64] = {};
            if (!ReadEntityTypeName(hands, raw, 64))
                ReadEntityConfigName(hands, raw, 64);
            if (!raw[0])
            {
                uintptr_t typ = Read<uintptr_t>(hands + oak_offsets::entity::Type);
                if (IsValidPtr(typ))
                {
                    uintptr_t tn = Read<uintptr_t>(typ + oak_offsets::entitytype::TypeName);
                    ReadEngineString(tn, raw, 64);
                    if (!raw[0])
                    {
                        uintptr_t cfg = Read<uintptr_t>(typ + oak_offsets::entitytype::ConfigName);
                        ReadEngineString(cfg, raw, 64);
                    }
                }
            }
            if (StrContainsI(raw, "Trap") || StrContainsI(raw, "Mine") ||
                StrContainsI(raw, "Survivor") || StrCmpI(raw, "dayzplayer") == 0)
            {
                nameOut[0] = 0;
                s_FastHands = hands; s_FastName[0] = 0; s_FastAmmo = -1;
                s_FastTick = nowFast; s_FastOk = false;
                return false;
            }
            const char* p = raw;
            if (_strnicmp(raw, "Weapon_", 7) == 0) p = raw + 7;
            lstrcpynA(nameOut, p && p[0] ? p : raw, nameMax);
        }

        if (ammoOut)
        {
            int ammo = -1, cap = -1;
            uintptr_t bestMag = 0;
            auto considerMag = [&](uintptr_t mag, bool fromMagSlot = false) {
                int a = -1, c = -1;
                if (!MagLooksValid(mag, hands, a, c, fromMagSlot)) return;
                bool better = false;
                if (ammo < 0) better = true;
                else if (fromMagSlot && a < ammo && c == cap) better = true; // prefer current over capacity
                else if (c > cap && a <= c) better = true;
                else if (c == cap && a < ammo) better = true; // lower = more likely current after firing
                else if (cap <= 1 && c > 1) better = true;
                if (better) { ammo = a; cap = c; bestMag = mag; }
            };

            // Cheap path only: MagRef slots — no 0x80..0x1C0 / nested sweeps
            {
                uintptr_t wInv = Read<uintptr_t>(hands + 0x658);
                if (!IsValidPtr(wInv) || wInv < 0x100000000)
                    wInv = Read<uintptr_t>(hands + 0x650);
                if (IsValidPtr(wInv) && wInv > 0x100000000)
                {
                    const uintptr_t magSlots[] = { 0x150, 0x148, 0x158, 0x160, 0x140 };
                    for (int mi = 0; mi < 5; mi++)
                        considerMag(Read<uintptr_t>(wInv + magSlots[mi]), true);
                }
            }

            // Magnum / internal: always trust live cartridge count on the weapon object
            {
                char wtn[48] = {};
                ReadEntityTypeName(hands, wtn, 48);
                const bool revolverish =
                    StrContainsI(wtn, "Magnum") || StrContainsI(wtn, "Revolver") ||
                    StrContainsI(wtn, "Longhorn") || StrContainsI(wtn, "Derringer");
                if (revolverish || ammo < 0)
                {
                    int live = CountMagazineBullets(hands);
                    if (live >= 0 && live <= 30)
                    {
                        ammo = live;
                        if (revolverish) cap = 6;
                        else if (cap < ammo) cap = ammo;
                        bestMag = hands;
                    }
                    // Prefer updater AmmoCount 0x6AC over 0x6B4 (capacity trap)
                    int a6 = Read<int>(hands + 0x6AC);
                    int c6 = Read<int>(hands + 0x6B0);
                    int b6 = Read<int>(hands + 0x6B4);
                    if (a6 >= 0 && a6 <= 30 && (ammo < 0 || (revolverish && a6 <= 6)))
                    {
                        // If bullet list missing, 0x6AC is the updating count
                        if (ammo < 0 || (live < 0 && a6 != b6))
                        {
                            ammo = a6;
                            cap = (c6 >= a6 && c6 <= 100) ? c6 : ((b6 >= a6 && b6 <= 100) ? b6 : (revolverish ? 6 : a6));
                            bestMag = hands;
                        }
                    }
                }
            }

            // Hysteresis — short; never keep stale full cylinder after shots
            static uintptr_t s_CacheHands = 0;
            static int s_CacheAmmo = -1;
            static DWORD s_CacheTick = 0;
            DWORD now = GetTickCount();
            if (ammo >= 0)
            {
                s_CacheHands = hands;
                s_CacheAmmo = ammo;
                s_CacheTick = now;
                *ammoOut = ammo;
            }
            else if (s_CacheHands == hands && s_CacheAmmo >= 0 && (now - s_CacheTick) < 80)
            {
                *ammoOut = s_CacheAmmo;
            }

            static int s_WpnLog = 0;
            if (false && (++s_WpnLog % 900) == 1)
            {
                char wb[160];
                wsprintfA(wb, "verify[weapon] name='%s' ammo=%d cap=%d mag=%p",
                    nameOut && nameOut[0] ? nameOut : "?", *ammoOut, cap, (void*)bestMag);
                Log(wb);
            }
        }

        s_FastHands = hands;
        if (nameOut && nameOut[0]) lstrcpynA(s_FastName, nameOut, 64);
        else s_FastName[0] = 0;
        s_FastAmmo = ammoOut ? *ammoOut : -1;
        s_FastTick = nowFast;
        s_FastOk = (nameOut && nameOut[0]) || (ammoOut && *ammoOut >= 0);
        ok = s_FastOk;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// GlobalHealth / damage zones — preferred over PlayerStats heuristics (those read junk → hp=5).
static bool ReadNamedHealthFloat(uintptr_t typeArray, int typeCount, const char* want, float& out)
{
    out = -1.f;
    if (!IsValidPtr(typeArray) || typeArray < 0x100000000 || typeCount <= 0 || typeCount > 64)
        return false;
    for (int i = 0; i < typeCount; i++)
    {
        uintptr_t entry = typeArray + (uintptr_t)i * 0x10;
        uintptr_t namePtr = Read<uintptr_t>(entry);
        char name[64] = {};
        if (IsValidPtr(namePtr) && namePtr > 0x100000000)
        {
            // Prefer EngString reader (ProbeMem + SEH) — raw deref AV'd on bad zone names
            if (!ReadEngineString(namePtr, name, 64))
            {
                for (int c = 0; c < 63; c++)
                {
                    char ch = Read<char>(namePtr + c);
                    if (!ch) { name[c] = 0; break; }
                    if (ch < 32 || ch > 126) { name[0] = 0; break; }
                    name[c] = ch;
                }
            }
        }
        if (!name[0] || StrCmpI(name, want) != 0)
            continue;
        float v = Read<float>(entry + 0x8);
        if (v != v) continue;
        out = v;
        return true;
    }
    return false;
}

static bool ProbeEntityHealthBlock(uintptr_t eh, float* healthPct, float* bloodPct, float* shockPct)
{
    if (!IsValidPtr(eh) || eh < 0x100000000) return false;

    auto normPct = [](float v) -> float {
        if (v != v) return -1.f;
        if (v > 0.01f && v <= 1.05f) return v * 100.f;
        if (v > 1.05f && v <= 100.f) return v;
        return -1.f;
    };
    auto normBlood = [](float v) -> float {
        if (v != v) return -1.f;
        if (v > 100.f && v <= 7500.f) return v / 5000.f * 100.f;
        if (v > 0.01f && v <= 1.05f) return v * 100.f;
        if (v > 1.05f && v <= 100.f) return v;
        return -1.f;
    };

    // Live 1.29: DamageManager+0x28 = zone table, count @ +0x34 (NOT +0x30).
    // Each 0x10 slot starts with a zone-object pointer; health often lives on that object as 0..1.
    uintptr_t typeArr = Read<uintptr_t>(eh + 0x28);
    int typeCount = Read<int>(eh + 0x34);
    if (typeCount <= 0 || typeCount > 64)
        typeCount = Read<int>(eh + 0x30);
    if (typeCount <= 0 || typeCount > 64)
    {
        typeArr = Read<uintptr_t>(eh + 0x20);
        typeCount = Read<int>(eh + 0x28);
    }

    float v = -1.f;
    auto takeHealth = [&](const char* name) {
        if (!healthPct || *healthPct > 0.f) return;
        if (!ReadNamedHealthFloat(typeArr, typeCount, name, v)) return;
        float p = normPct(v);
        if (p > 0.f) *healthPct = p;
    };
    takeHealth("Health");
    takeHealth("GlobalHealth");
    if (bloodPct && *bloodPct < 0.f && ReadNamedHealthFloat(typeArr, typeCount, "Blood", v))
    {
        float p = normBlood(v);
        if (p >= 0.f) *bloodPct = p;
    }
    if (shockPct && *shockPct < 0.f && ReadNamedHealthFloat(typeArr, typeCount, "Shock", v))
    {
        float p = normPct(v);
        if (p >= 0.f) *shockPct = p;
    }

    // Zone-object walk: ONLY accept cur/max both in (0,1] (live GlobalHealth layout).
    // Do NOT max-scan random floats — that produced jumbo/nonsense bars on UAInterface.
    if (IsValidPtr(typeArr) && typeCount > 0 && typeCount <= 64)
    {
        float bestRatioHp = -1.f;
        float bestRatioBlood = -1.f;
        float bestRatioShock = -1.f;
        for (int i = 0; i < typeCount; i++)
        {
            uintptr_t slot = typeArr + (uintptr_t)i * 0x10;
            uintptr_t zone = Read<uintptr_t>(slot);
            if (!IsValidPtr(zone) || zone < 0x100000000) continue;
            if (g_GameModule)
            {
                uintptr_t mod = (uintptr_t)g_GameModule;
                if (zone >= mod && zone < mod + 0x8000000ULL) continue;
            }
            float z0 = Read<float>(zone + 0x0);
            float z4 = Read<float>(zone + 0x4);
            if (!(z0 == z0 && z4 == z4)) continue;
            if (z0 < 0.f || z0 > 1.05f || z4 < 0.05f || z4 > 1.05f) continue;
            float p = (z0 / z4) * 100.f;
            if (p > 100.f) p = 100.f;
            if (p > bestRatioHp) bestRatioHp = p;

            // Named zone type id sometimes at +8 (int). Prefer Health-ish names when present.
            char zname[32] = {};
            uintptr_t namePtr = Read<uintptr_t>(slot + 0x8);
            if (IsValidPtr(namePtr) && namePtr > 0x10000)
            {
                // Often a small type id, not a string — ignore if not printable
                char c0 = Read<char>(namePtr);
                if (c0 >= 'A' && c0 <= 'z')
                    ReadEngineString(namePtr, zname, 32);
            }
            if (zname[0])
            {
                if (StrContainsI(zname, "Blood") && p > bestRatioBlood) bestRatioBlood = p;
                if (StrContainsI(zname, "Shock") && p > bestRatioShock) bestRatioShock = p;
            }
        }
        if (healthPct && *healthPct <= 0.f && bestRatioHp > 0.f)
            *healthPct = bestRatioHp;
        if (bloodPct && *bloodPct < 0.f && bestRatioBlood > 0.f)
            *bloodPct = bestRatioBlood;
        if (shockPct && *shockPct < 0.f && bestRatioShock > 0.f)
            *shockPct = bestRatioShock;
    }

    // Direct floats on DamageManager — only 0..1 normalized (never 0..100 impostors from wrong objs)
    if (healthPct && *healthPct <= 0.f)
    {
        const uintptr_t fOffs[] = { 0x10, 0x14, 0x18, 0x0C, 0x08, 0x1C, 0x20 };
        for (int i = 0; i < 7; i++)
        {
            float raw = Read<float>(eh + fOffs[i]);
            if (!(raw == raw) || raw <= 0.01f || raw > 1.05f) continue;
            *healthPct = raw * 100.f;
            break;
        }
    }

    if (healthPct && *healthPct > 100.f) *healthPct = 100.f;
    if (bloodPct && *bloodPct > 100.f) *bloodPct = 100.f;
    if (shockPct && *shockPct > 100.f) *shockPct = 100.f;
    return (healthPct && *healthPct > 0.f) || (bloodPct && *bloodPct >= 0.f);
}

// PlayerStat labels are often const char* in DayZ_x64 .rdata, not EngString objects.
static bool ReadStatLabel(uintptr_t ptr, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(ptr) || ptr < 0x10000) return false;
    if (ReadEngineString(ptr, out, outMax) && out[0])
        return true;
    // Module / heap C-string
    __try
    {
        for (int c = 0; c < outMax - 1; c++)
        {
            char ch = Read<char>(ptr + (uintptr_t)c);
            if (!ch) { out[c] = 0; break; }
            if (ch < 32 || ch > 126) { out[0] = 0; return false; }
            out[c] = ch;
            out[c + 1] = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
    return out[0] != 0;
}

// Discover real PlayerStats.
// 0x6F0 on this build pointed at skeleton junk (head/neck1/lookat) — NOT stats.
// PlayerStat is an Enforce script object with Man m_Player back-pointer — hunt that.
static uintptr_t g_CachedStatsOff = 0;
static uintptr_t g_CachedStatValueOff = 0x2C;
static uintptr_t g_CachedEnergyRec = 0;
static uintptr_t g_CachedWaterRec = 0;
static uintptr_t g_CachedEnergyValOff = 0;
static uintptr_t g_CachedWaterValOff = 0;
static int g_DiscoverFailLogs = 0;

struct OakFileVit {
    int netLo;
    int hl;
    float hp01, hp, blood, shock, energy, water, stam, e01, w01;
    int bleed;
    bool used;
};
static OakFileVit g_FileVit[16];
static int g_FileVitN = 0;
static DWORD g_FileVitTick = 0;
struct OakFileZed { int netLo; float hp01, hp, x, y, z; bool hasPos; };
static OakFileZed g_FileZed[48];
static int g_FileZedN = 0;
static float g_QaZedHp = -1.f;
static int g_QaZedBars = 0;
static int g_QaZedSeen = 0;
static int g_QaZedNetOff = 0;

static float OakVitTokF(const char* line, const char* key, float defv)
{
    const char* p = strstr(line, key);
    if (!p) return defv;
    return (float)atof(p + lstrlenA(key));
}
static int OakVitTokI(const char* line, const char* key, int defv)
{
    const char* p = strstr(line, key);
    if (!p) return defv;
    return atoi(p + lstrlenA(key));
}
static float OakVitAsPct(float x, float maxHint)
{
    if (!(x == x) || x < 0.f) return -1.f;
    if (x <= 1.05f) return x * 100.f;
    float mx = maxHint;
    if (x > mx) mx = (x > 5000.f) ? 20000.f : 5000.f;
    if (mx < 1.f) mx = 100.f;
    float p = (x / mx) * 100.f;
    if (p > 100.f) p = 100.f;
    return p;
}
// Engine stores Shock/Energy/Water as remaining (100 HP = fine, 5000 energy = full).
// Overlay shows how much of the status you HAVE — 0% shock = none, 100% = KO.
static float OakStatusAmt(float remainingPct)
{
    if (remainingPct < 0.f) return -1.f;
    float a = 100.f - remainingPct;
    if (a < 0.f) a = 0.f;
    if (a > 100.f) a = 100.f;
    return a;
}
static void OakFileVitRefresh()
{
    const DWORD now = GetTickCount();
    if (g_FileVitTick && (now - g_FileVitTick) < 800u)
        return;
    g_FileVitTick = now;

    const char* paths[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZServer\\oak_profiles\\oak_vitals.txt",
        "C:\\PROGRA~2\\Steam\\STEAMA~1\\common\\DAYZSE~1\\OAK_PR~1\\oak_vitals.txt",
    };
    HANDLE hf = INVALID_HANDLE_VALUE;
    for (int pi = 0; pi < 2 && hf == INVALID_HANDLE_VALUE; pi++)
        hf = CreateFileA(paths[pi], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
        return; // keep last-good — server often has the file open for write

    char buf[16384];
    DWORD rd = 0;
    if (!ReadFile(hf, buf, sizeof(buf) - 1, &rd, nullptr) || rd < 8)
    {
        CloseHandle(hf);
        return;
    }
    CloseHandle(hf);
    buf[rd] = 0;

    OakFileVit acc = {};
    acc.hl = -1;
    acc.hp01 = acc.hp = acc.blood = acc.shock = acc.energy = acc.water = acc.stam = acc.e01 = acc.w01 = -1.f;
    OakFileVit tmp[16];
    OakFileZed ztmp[48];
    int n = 0;
    int zn = 0;
    bool have = false;
    for (char* line = buf; *line; )
    {
        char* nl = line;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        char save = *nl;
        *nl = 0;
        const bool isZed = (line[0] == 'z' && line[1] == 'e' && line[2] == 'd') || strstr(line, "zed ") != nullptr;
        if (isZed && zn < 48)
        {
            ztmp[zn].netLo = OakVitTokI(line, "net=", 0);
            ztmp[zn].hp01 = OakVitTokF(line, " hp01=", -1.f);
            ztmp[zn].hp = OakVitTokF(line, " hp=", -1.f);
            ztmp[zn].x = OakVitTokF(line, " x=", 0.f);
            ztmp[zn].y = OakVitTokF(line, " y=", 0.f);
            ztmp[zn].z = OakVitTokF(line, " z=", 0.f);
            ztmp[zn].hasPos = (strstr(line, " x=") != nullptr && strstr(line, " z=") != nullptr);
            if (ztmp[zn].hp01 >= 0.f || ztmp[zn].hp >= 0.f)
                zn++;
        }
        const bool isNet = strstr(line, "net=") != nullptr;
        const bool isPlr = strstr(line, "player=") != nullptr;
        if (!isZed && (isNet || isPlr || strstr(line, "blood=") || strstr(line, "energy=") || strstr(line, "e01=")))
        {
            if (isNet) acc.netLo = OakVitTokI(line, "net=", acc.netLo);
            acc.hl = OakVitTokI(line, " hl=", acc.hl);
            if (acc.hl < 0) acc.hl = OakVitTokI(line, " healthLevel=", acc.hl);
            acc.hp01 = OakVitTokF(line, " hp01=", acc.hp01);
            acc.hp = OakVitTokF(line, " hp=", acc.hp);
            acc.blood = OakVitTokF(line, " blood=", acc.blood);
            acc.shock = OakVitTokF(line, " shock=", acc.shock);
            acc.energy = OakVitTokF(line, " energy=", acc.energy);
            acc.water = OakVitTokF(line, " water=", acc.water);
            acc.stam = OakVitTokF(line, " stam=", acc.stam);
            acc.e01 = OakVitTokF(line, " e01=", acc.e01);
            acc.w01 = OakVitTokF(line, " w01=", acc.w01);
            acc.bleed = OakVitTokI(line, " bleed=", acc.bleed);
            acc.used = true;
            have = true;
            if (isNet && n < 16)
            {
                tmp[n++] = acc;
                acc = OakFileVit{};
                acc.hl = -1;
                acc.hp01 = acc.hp = acc.blood = acc.shock = acc.energy = acc.water = acc.stam = acc.e01 = acc.w01 = -1.f;
                have = false;
            }
        }
        *nl = save;
        line = nl;
        while (*line == '\n' || *line == '\r') line++;
    }
    if (have && n < 16)
        tmp[n++] = acc;
    // Only replace last-good if we actually parsed energy/shock/blood (not a truncated write).
    bool ok = false;
    for (int i = 0; i < n; i++)
    {
        if (tmp[i].energy >= 0.f || tmp[i].e01 >= 0.f || tmp[i].shock >= 0.f || tmp[i].water >= 0.f)
            ok = true;
    }
    if (ok)
    {
        g_FileVitN = n;
        for (int i = 0; i < n; i++)
            g_FileVit[i] = tmp[i];
    }
    if (zn > 0)
    {
        g_FileZedN = zn;
        for (int i = 0; i < zn; i++)
            g_FileZed[i] = ztmp[i];
    }
}
static void OakFileVitApply(uintptr_t entity, bool isLocal, bool isInfected,
    float* healthPct, float* bloodPct, float* shockPct,
    float* stamPct, float* hungerPct, float* thirstPct)
{
    if (isInfected || g_FileVitN <= 0)
        return;
    int entNet = 0;
    __try { entNet = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { entNet = 0; }
    if (!entNet)
    {
        __try { entNet = Read<int>(entity + oak_offsets::entity::NetworkId); }
        __except (EXCEPTION_EXECUTE_HANDLER) { entNet = 0; }
    }
    const OakFileVit* hit = nullptr;
    for (int i = 0; i < g_FileVitN; i++)
    {
        if (entNet && g_FileVit[i].netLo == entNet) { hit = &g_FileVit[i]; break; }
    }
    if (!hit && isLocal)
        hit = &g_FileVit[g_FileVitN - 1];
    if (!hit)
        return;

    // Authority overwrite for channels that have no client DamageSystem / PlayerStats.
    if (hungerPct && (hit->e01 >= 0.f || hit->energy >= 0.f))
    {
        float p = (hit->e01 >= 0.f) ? OakVitAsPct(hit->e01, 1.f) : OakVitAsPct(hit->energy, 5000.f);
        if (p >= 0.f) *hungerPct = p;
    }
    if (thirstPct && (hit->w01 >= 0.f || hit->water >= 0.f))
    {
        float p = (hit->w01 >= 0.f) ? OakVitAsPct(hit->w01, 1.f) : OakVitAsPct(hit->water, 5000.f);
        if (p >= 0.f) *thirstPct = p;
    }
    if (shockPct && hit->shock >= 0.f)
    {
        float p = OakVitAsPct(hit->shock, 100.f);
        if (p >= 0.f) *shockPct = p;
    }
    if (stamPct && *stamPct < 0.f && hit->stam >= 0.f)
    {
        float p = OakVitAsPct(hit->stam, 100.f);
        if (p >= 0.f) *stamPct = p;
    }
    if (bloodPct && *bloodPct < 0.f && hit->blood >= 0.f)
    {
        float p = OakVitAsPct(hit->blood, 5000.f);
        if (p >= 0.f) *bloodPct = p;
    }
    if (healthPct && *healthPct < 0.f)
    {
        if (hit->hp01 >= 0.f && hit->hp01 <= 1.05f)
            *healthPct = hit->hp01 * 100.f;
        else if (hit->hp > 1.05f && hit->hp <= 100.f)
            *healthPct = hit->hp;
    }
}

// Infected: HP only. File first (local server net / XZ), then client DS / GetHealth01.
static float OakZedFileHp(const OakFileZed& z)
{
    if (z.hp01 >= 0.f && z.hp01 <= 1.05f)
        return z.hp01 * 100.f;
    if (z.hp > 1.05f && z.hp <= 150.f)
        return z.hp;
    if (z.hp >= 0.f && z.hp <= 1.05f)
        return z.hp * 100.f;
    return -1.f;
}
static void OakZedHuntNetOff(uintptr_t entity)
{
    if (g_QaZedNetOff || g_FileZedN <= 0 || !IsValidPtr(entity))
        return;
    for (int o = 0x4C0; o <= 0x7C0; o += 4)
    {
        int v = 0;
        __try { v = Read<int>(entity + (uintptr_t)o); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!v)
            continue;
        for (int i = 0; i < g_FileZedN; i++)
        {
            if (g_FileZed[i].netLo && g_FileZed[i].netLo == v)
            {
                g_QaZedNetOff = o;
                return;
            }
        }
    }
}
static float ReadInfectedHpPct(uintptr_t entity)
{
    if (!IsValidPtr(entity))
        return -1.f;
    OakFileVitRefresh();
    int netA = 0, netB = 0, netC = 0;
    __try { netA = Read<int>(entity + oak_offsets::entity::NetworkId); }
    __except (EXCEPTION_EXECUTE_HANDLER) { netA = 0; }
    __try { netB = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { netB = 0; }
    if (!g_QaZedNetOff)
        OakZedHuntNetOff(entity);
    if (g_QaZedNetOff)
    {
        __try { netC = Read<int>(entity + (uintptr_t)g_QaZedNetOff); }
        __except (EXCEPTION_EXECUTE_HANDLER) { netC = 0; }
    }

    const OakFileZed* hit = nullptr;
    for (int i = 0; i < g_FileZedN; i++)
    {
        const int n = g_FileZed[i].netLo;
        if (!n)
            continue;
        if (n == netA || n == netB || n == netC)
        {
            hit = &g_FileZed[i];
            break;
        }
    }
    if (!hit)
    {
        Vec3 pos{};
        if (GetEntityPosition(entity, pos))
        {
            float best = 64.f * 64.f;
            for (int i = 0; i < g_FileZedN; i++)
            {
                if (!g_FileZed[i].hasPos)
                    continue;
                float dx = pos.x - g_FileZed[i].x;
                float dz = pos.z - g_FileZed[i].z;
                float d2 = dx * dx + dz * dz;
                if (d2 < best)
                {
                    best = d2;
                    hit = &g_FileZed[i];
                }
            }
            if (hit && best > 12.f * 12.f)
                hit = nullptr;
        }
    }
    if (hit)
    {
        float p = OakZedFileHp(*hit);
        if (p >= 0.f)
            return p;
    }

    uintptr_t ds = 0;
    __try { ds = Read<uintptr_t>(entity + oak_offsets::player::DamageManager); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ds = 0; }
    if (IsValidPtr(ds) && ds > 0x100000000)
    {
        float hp = -1.f, blood = -1.f, shock = -1.f;
        ProbeEntityHealthBlock(ds, &hp, &blood, &shock);
        if (hp >= 0.f && hp <= 1.05f)
            return hp * 100.f;
        if (hp > 1.05f && hp <= 150.f)
            return hp;
        if (g_GameModule)
        {
            typedef float(__fastcall* Fn_ObjGetHealth)(void* obj, const char* zone, const char* type);
            const uintptr_t hp01Addr = (uintptr_t)g_GameModule + 0x923F20;
            if (OakNativeEntryOk(hp01Addr, "Object::GetHealth01"))
            {
                auto fn01 = (Fn_ObjGetHealth)hp01Addr;
                float v = -1.f;
                __try { v = fn01((void*)entity, "", "Health"); }
                __except (EXCEPTION_EXECUTE_HANDLER) { v = -1.f; }
                if (v == v && v > 0.0001f && v <= 1.05f)
                    return v * 100.f;
            }
        }
    }
    return -1.f;
}

static bool IsStatLabelName(const char* sn)
{
    if (!sn || !sn[0]) return false;
    return StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0 ||
           StrCmpI(sn, "Stamina") == 0 || StrCmpI(sn, "HeatComfort") == 0 ||
           StrCmpI(sn, "Diet") == 0 || StrCmpI(sn, "Tremor") == 0 ||
           StrCmpI(sn, "Toxicity") == 0 || StrCmpI(sn, "Wet") == 0 ||
           StrCmpI(sn, "HeatBuffer") == 0 || StrCmpI(sn, "Specialty") == 0 ||
           StrCmpI(sn, "BloodType") == 0;
}

static bool IsHeapObj(uintptr_t p)
{
    if (!IsValidPtr(p) || p < 0x100000000ULL || p > 0x7FFFFFFFFFFFULL) return false;
    if (g_GameModule)
    {
        uintptr_t mod = (uintptr_t)g_GameModule;
        if (p >= mod && p < mod + 0x8000000ULL) return false;
    }
    return true;
}

// Pull label from a PlayerStat-like record (EngString, raw C-string, or script-string wrapper).
static bool ReadRecStatLabel(uintptr_t rec, char* sn, int snMax)
{
    if (!sn || snMax < 2) return false;
    sn[0] = 0;
    if (!IsHeapObj(rec)) return false;
    for (uintptr_t no = 0x0; no <= 0x60 && !sn[0]; no += 8)
    {
        uintptr_t np = Read<uintptr_t>(rec + no);
        if (!IsValidPtr(np)) continue;
        ReadStatLabel(np, sn, snMax);
        if (sn[0] && IsStatLabelName(sn)) return true;
        // Script string wrappers: +0x10 / +0x18 often hold the char*
        if (IsHeapObj(np))
        {
            for (uintptr_t io = 0x0; io <= 0x20 && !sn[0]; io += 8)
            {
                uintptr_t inner = Read<uintptr_t>(np + io);
                if (IsValidPtr(inner)) ReadStatLabel(inner, sn, snMax);
                if (sn[0] && IsStatLabelName(sn)) return true;
            }
        }
        sn[0] = 0;
    }
    // Embedded inline C-string in the record itself (safe via Read<>)
    for (uintptr_t off = 0x8; off <= 0x50; off += 4)
    {
        char c0 = Read<char>(rec + off);
        if (c0 != 'E' && c0 != 'e' && c0 != 'W' && c0 != 'w') continue;
        char buf[12] = {};
        for (int i = 0; i < 11; i++)
        {
            char ch = Read<char>(rec + off + (uintptr_t)i);
            if (ch < 32 || ch > 126) { buf[0] = 0; break; }
            buf[i] = ch;
            if (!ch) break;
        }
        if (StrCmpI(buf, "Energy") == 0) { lstrcpynA(sn, "Energy", snMax); return true; }
        if (StrCmpI(buf, "Water") == 0) { lstrcpynA(sn, "Water", snMax); return true; }
        if (StrCmpI(buf, "Stamina") == 0) { lstrcpynA(sn, "Stamina", snMax); return true; }
    }
    return false;
}

// PlayerStat<float>: prefer official RecordValue @ +0x2C, then (min,max,value) scan.
static bool ExtractStatValue(uintptr_t rec, const char* sn, float* valOut, uintptr_t* valOffOut)
{
    if (valOut) *valOut = -1.f;
    if (valOffOut) *valOffOut = 0;
    if (!IsHeapObj(rec)) return false;
    bool wantAbs = sn && (StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0 ||
                          StrCmpI(sn, "Stamina") == 0);

    // Official PlayerStats::RecordValue = 0x2C
    {
        float cur = Read<float>(rec + oak_offsets::player::RecordValue);
        float mx = Read<float>(rec + oak_offsets::player::RecordValue - 4); // often m_Max before m_Value
        float mn = Read<float>(rec + oak_offsets::player::RecordValue - 8);
        if (cur == cur && cur >= 0.f && cur <= 20000.f)
        {
            bool maxOk = (mx == 5000.f || mx == 20000.f || mx == 100.f ||
                          (mx > 10.f && mx <= 20000.f && mn <= cur && cur <= mx + 1.f));
            if (wantAbs || maxOk || (cur > 1.05f && cur < 5000.f) || cur == 0.f)
            {
                if (valOut) *valOut = cur;
                if (valOffOut) *valOffOut = oak_offsets::player::RecordValue;
                if (wantAbs) g_CachedStatValueOff = oak_offsets::player::RecordValue;
                return true;
            }
        }
    }

    float fallback = -1.f;
    uintptr_t fallbackOff = 0;

    // Pattern A: consecutive floats min,max,value — prefer cur < max (real gameplay values)
    for (uintptr_t vo = 0x08; vo <= 0x58; vo += 4)
    {
        float mn = Read<float>(rec + vo);
        float mx = Read<float>(rec + vo + 4);
        float cur = Read<float>(rec + vo + 8);
        if (mn != mn || mx != mx || cur != cur) continue;
        if (mx != 5000.f && mx != 20000.f && mx != 100.f &&
            !(wantAbs && mx > 100.f && mx <= 20000.f)) continue;
        if (mn < -50.f || mn > 100.f) continue;
        if (cur < mn - 1.f || cur > mx + 1.f) continue;
        if (cur < mx - 0.5f || cur == 0.f)
        {
            if (valOut) *valOut = cur;
            if (valOffOut) *valOffOut = vo + 8;
            if (wantAbs) g_CachedStatValueOff = vo + 8;
            return true;
        }
        if (fallbackOff == 0) { fallback = cur; fallbackOff = vo + 8; }
    }
    if (fallbackOff)
    {
        if (valOut) *valOut = fallback;
        if (valOffOut) *valOffOut = fallbackOff;
        if (wantAbs) g_CachedStatValueOff = fallbackOff;
        return true;
    }

    // Pattern B: lone abs value — never prefer exact 5000/20000 over real currents
    float best = -1.f;
    uintptr_t bestOff = 0;
    for (uintptr_t vo = 0x10; vo <= 0x60; vo += 4)
    {
        float t = Read<float>(rec + vo);
        if (t != t || t < 0.f || t > 20000.f) continue;
        if (wantAbs)
        {
            if (t == 0.f || (t > 1.05f && t != 5000.f && t != 20000.f))
            {
                best = t; bestOff = vo;
                if (t > 1.05f) break;
            }
        }
        else if (best < 0.f)
        {
            best = t; bestOff = vo;
        }
    }
    if (bestOff)
    {
        if (valOut) *valOut = best;
        if (valOffOut) *valOffOut = bestOff;
        if (wantAbs) g_CachedStatValueOff = bestOff;
        return true;
    }
    return false;
}

static bool StatRecLooksLikeEnergyWater(uintptr_t rec, char* snOut, int snMax, float* valOut)
{
    if (snOut && snMax > 0) snOut[0] = 0;
    if (valOut) *valOut = -1.f;
    char sn[48] = {};
    if (!ReadRecStatLabel(rec, sn, 48)) return false;
    if (!IsStatLabelName(sn)) return false;
    float v = -1.f;
    uintptr_t vo = 0;
    ExtractStatValue(rec, sn, &v, &vo);
    if (snOut) lstrcpynA(snOut, sn, snMax);
    if (valOut) *valOut = v;
    return true;
}

static bool WalkStatsRootForEnergy(uintptr_t root, int depth, char* foundName, float* foundVal)
{
    if (depth > 3 || !IsHeapObj(root)) return false;
    for (uintptr_t base = 0x0; base <= 0x60; base += 8)
    {
        uintptr_t arr = Read<uintptr_t>(root + base);
        if (!IsHeapObj(arr)) continue;
        int energyHits = 0;
        // Enforce array: data at +0x0/+0x8, or raw pointer table
        uintptr_t tables[3] = { arr, Read<uintptr_t>(arr + 0x0), Read<uintptr_t>(arr + 0x8) };
        for (int ti = 0; ti < 3; ti++)
        {
            uintptr_t tab = tables[ti];
            if (!IsHeapObj(tab)) continue;
            for (int i = 0; i < 24; i++)
            {
                uintptr_t rec = Read<uintptr_t>(tab + (uintptr_t)i * 8);
                char sn[48] = {};
                float v = -1.f;
                if (!StatRecLooksLikeEnergyWater(rec, sn, 48, &v)) continue;
                if (StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0)
                {
                    energyHits++;
                    if (foundName) lstrcpynA(foundName, sn, 48);
                    if (foundVal) *foundVal = v;
                    if (StrCmpI(sn, "Energy") == 0) { g_CachedEnergyRec = rec; ExtractStatValue(rec, sn, &v, &g_CachedEnergyValOff); }
                    if (StrCmpI(sn, "Water") == 0) { g_CachedWaterRec = rec; ExtractStatValue(rec, sn, &v, &g_CachedWaterValOff); }
                }
            }
        }
        if (energyHits >= 1) return true;
        if (depth < 3 && WalkStatsRootForEnergy(arr, depth + 1, foundName, foundVal))
            return true;
    }
    if (depth == 0)
    {
        for (uintptr_t po = 0x0; po <= 0x50; po += 8)
        {
            uintptr_t child = Read<uintptr_t>(root + po);
            if (child == root || !IsHeapObj(child)) continue;
            if (WalkStatsRootForEnergy(child, 1, foundName, foundVal))
                return true;
        }
    }
    return false;
}

static bool OakWalkWriteStat(uintptr_t root, int depth, const char* wantName, float value,
    uintptr_t* outRec, uintptr_t* outValOff)
{
    if (!wantName || !wantName[0] || depth > 3 || !IsHeapObj(root))
        return false;

    for (uintptr_t base = 0x0; base <= 0x60; base += 8)
    {
        uintptr_t arr = Read<uintptr_t>(root + base);
        if (!IsHeapObj(arr))
            continue;

        uintptr_t tables[3] = { arr, Read<uintptr_t>(arr + 0x0), Read<uintptr_t>(arr + 0x8) };
        for (int ti = 0; ti < 3; ti++)
        {
            uintptr_t tab = tables[ti];
            if (!IsHeapObj(tab))
                continue;
            for (int i = 0; i < 32; i++)
            {
                uintptr_t rec = Read<uintptr_t>(tab + (uintptr_t)i * 8);
                if (!IsHeapObj(rec))
                    continue;
                char sn[48] = {};
                if (!ReadRecStatLabel(rec, sn, 48) || StrCmpI(sn, wantName) != 0)
                    continue;

                uintptr_t valOff = 0;
                float cur = -1.f;
                if (!ExtractStatValue(rec, wantName, &cur, &valOff) || !valOff)
                    valOff = oak_offsets::player::RecordValue;
                if (Write<float>(rec + valOff, value))
                {
                    if (outRec) *outRec = rec;
                    if (outValOff) *outValOff = valOff;
                    return true;
                }
            }
        }
        if (depth < 3 && OakWalkWriteStat(arr, depth + 1, wantName, value, outRec, outValOff))
            return true;
    }

    if (depth == 0)
    {
        for (uintptr_t po = 0x0; po <= 0x50; po += 8)
        {
            uintptr_t child = Read<uintptr_t>(root + po);
            if (child == root || !IsHeapObj(child))
                continue;
            if (OakWalkWriteStat(child, 1, wantName, value, outRec, outValOff))
                return true;
        }
    }
    return false;
}

struct OakStatWriteCache {
    uintptr_t entity = 0;
    uintptr_t rec = 0;
    uintptr_t valOff = 0;
    char name[24] = {};
};
static OakStatWriteCache g_StatWriteCache = {};

static bool OakWalkReadStat(uintptr_t root, int depth, const char* wantName, float& outVal, uintptr_t* outRec, uintptr_t* outOff)
{
    outVal = -1.f;
    if (outRec) *outRec = 0;
    if (outOff) *outOff = 0;
    if (!wantName || !wantName[0] || depth > 3 || !IsHeapObj(root))
        return false;

    for (uintptr_t base = 0x0; base <= 0x60; base += 8)
    {
        uintptr_t arr = Read<uintptr_t>(root + base);
        if (!IsHeapObj(arr))
            continue;

        uintptr_t tables[3] = { arr, Read<uintptr_t>(arr + 0x0), Read<uintptr_t>(arr + 0x8) };
        for (int ti = 0; ti < 3; ti++)
        {
            uintptr_t tab = tables[ti];
            if (!IsHeapObj(tab))
                continue;
            for (int i = 0; i < 32; i++)
            {
                uintptr_t rec = Read<uintptr_t>(tab + (uintptr_t)i * 8);
                if (!IsHeapObj(rec))
                    continue;
                char sn[48] = {};
                if (!ReadRecStatLabel(rec, sn, 48) || StrCmpI(sn, wantName) != 0)
                    continue;

                uintptr_t valOff = 0;
                float cur = -1.f;
                if (!ExtractStatValue(rec, wantName, &cur, &valOff) || !valOff)
                    valOff = oak_offsets::player::RecordValue;
                outVal = Read<float>(rec + valOff);
                if (outRec) *outRec = rec;
                if (outOff) *outOff = valOff;
                return outVal == outVal;
            }
        }
        if (depth < 3 && OakWalkReadStat(arr, depth + 1, wantName, outVal, outRec, outOff))
            return true;
    }

    if (depth == 0)
    {
        for (uintptr_t po = 0x0; po <= 0x50; po += 8)
        {
            uintptr_t child = Read<uintptr_t>(root + po);
            if (child == root || !IsHeapObj(child))
                continue;
            if (OakWalkReadStat(child, 1, wantName, outVal, outRec, outOff))
                return true;
        }
    }
    return false;
}

static bool OakReadPlayerStat(uintptr_t entity, const char* name, float& outVal)
{
    outVal = -1.f;
    if (!IsValidPtr(entity) || entity < 0x100000000 || !name || !name[0])
        return false;

    if (g_StatWriteCache.entity == entity && g_StatWriteCache.rec && g_StatWriteCache.valOff &&
        StrCmpI(g_StatWriteCache.name, name) == 0 && IsHeapObj(g_StatWriteCache.rec))
    {
        outVal = Read<float>(g_StatWriteCache.rec + g_StatWriteCache.valOff);
        return outVal == outVal;
    }

    uintptr_t rec = 0, off = 0;
    auto tryRoot = [&](uintptr_t root) -> bool {
        return IsHeapObj(root) && OakWalkReadStat(root, 0, name, outVal, &rec, &off);
    };

    uintptr_t stats = Read<uintptr_t>(entity + oak_offsets::player::StatsContainer);
    if (tryRoot(stats))
        goto ok;
    const uintptr_t tryOff[] = {
        oak_offsets::player::StatsContainer, 0x6E8, 0x6E0, 0x700, 0x720, 0x740, 0x680, 0x660
    };
    for (int i = 0; i < 8; i++)
    {
        uintptr_t s = Read<uintptr_t>(entity + tryOff[i]);
        if (tryRoot(s))
            goto ok;
    }
    return false;

ok:
    if (rec && off)
    {
        g_StatWriteCache.entity = entity;
        g_StatWriteCache.rec = rec;
        g_StatWriteCache.valOff = off;
        lstrcpynA(g_StatWriteCache.name, name, 24);
    }
    return true;
}

static bool OakWritePlayerStat(uintptr_t entity, const char* name, float value)
{
    if (!IsValidPtr(entity) || entity < 0x100000000 || !name || !name[0])
        return false;

    if (g_StatWriteCache.entity == entity && g_StatWriteCache.rec && g_StatWriteCache.valOff &&
        StrCmpI(g_StatWriteCache.name, name) == 0 && IsHeapObj(g_StatWriteCache.rec))
    {
        return Write<float>(g_StatWriteCache.rec + g_StatWriteCache.valOff, value);
    }

    uintptr_t foundRec = 0;
    uintptr_t foundOff = 0;
    auto tryRoot = [&](uintptr_t root) -> bool {
        return IsHeapObj(root) && OakWalkWriteStat(root, 0, name, value, &foundRec, &foundOff);
    };

    uintptr_t stats = Read<uintptr_t>(entity + oak_offsets::player::StatsContainer);
    if (tryRoot(stats))
        goto cached_ok;

    const uintptr_t tryOff[] = {
        oak_offsets::player::StatsContainer, 0x6E8, 0x6E0, 0x700, 0x720, 0x740, 0x680, 0x660
    };
    for (int i = 0; i < 8; i++)
    {
        uintptr_t s = Read<uintptr_t>(entity + tryOff[i]);
        if (tryRoot(s))
            goto cached_ok;
    }
    return false;

cached_ok:
    g_StatWriteCache.entity = entity;
    g_StatWriteCache.rec = foundRec;
    g_StatWriteCache.valOff = foundOff;
    lstrcpynA(g_StatWriteCache.name, name, 24);
    return true;
}

static uintptr_t g_RdataEnergyStr = 0;
static uintptr_t g_RdataWaterStr = 0;
static int g_DiscoverExhausted = 0; // 1 = already scanned this session, stop hammering Present
static DWORD g_DiscoverNextTry = 0;

static uintptr_t FindModuleCString(const char* needle)
{
    if (!g_GameModule || !needle || !needle[0]) return 0;
    uintptr_t mod = (uintptr_t)g_GameModule;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int nlen = lstrlenA(needle);
    for (unsigned si = 0; si < nt->FileHeader.NumberOfSections; si++)
    {
        bool readable = (sec[si].Characteristics & IMAGE_SCN_MEM_READ) != 0;
        bool exec = (sec[si].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!readable || exec) continue;
        uintptr_t start = mod + sec[si].VirtualAddress;
        size_t size = sec[si].Misc.VirtualSize;
        if (size < (size_t)nlen + 1 || size > 0x4000000) continue;
        __try
        {
            for (size_t i = 0; i + (size_t)nlen < size; i++)
            {
                const char* p = (const char*)(start + i);
                if (p[0] != needle[0]) continue;
                bool ok = true;
                for (int c = 0; c < nlen; c++)
                    if (p[c] != needle[c]) { ok = false; break; }
                if (!ok || p[nlen] != 0) continue;
                return start + i;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return 0;
}

// Scan object for float-pair (cur, max=5000/20000) — used when labels are opaque.
static int CollectFloatPairsOnObj(uintptr_t obj, float* vals, uintptr_t* offs, int maxN)
{
    int n = 0;
    if (!IsHeapObj(obj) || !vals || maxN < 1) return 0;
    for (uintptr_t off = 0x0; off <= 0x120 && n < maxN; off += 4)
    {
        float cur = Read<float>(obj + off);
        float mx = Read<float>(obj + off + 4);
        if (mx != 5000.f && mx != 20000.f) continue;
        if (cur != cur || cur < 0.f || cur > mx + 1.f) continue;
        vals[n] = cur;
        if (offs) offs[n] = off;
        n++;
    }
    return n;
}

static bool IsBogusStatsPlayerOff(uintptr_t off)
{
    // Skeleton / InputController / HumanType — never treat as PlayerStats
    return off == 0x7E0 || off == 0x7E8 || off == 0x7F0
        || off == 0x180 || off == 0xA8 || off == 0x1C8;
}

// Reverse-hunt: PlayerStatBase::m_Player points back at Man. Keep cheap — Present-thread safe.
// NEVER accept a lone (0,5000) float-pair as Energy — that false-hit hung=0 and hid real Water@0x7E8.
static uintptr_t DiscoverByPlayerBackref(uintptr_t entity)
{
    if (!IsHeapObj(entity)) return 0;
    uintptr_t bestRoot = 0;
    uintptr_t bestRootOff = 0;
    int energyFound = 0;
    int labelHits = 0;

    __try
    {
        // Official StatsContainer=0x6F0. NEVER 0x7E0/0x7E8 (Skeleton / InputController).
        {
            const uintptr_t known[] = { 0x6F0, 0x700, 0x6E8, 0x6E0, 0x720, 0x740 };
            for (int ki = 0; ki < 6; ki++)
            {
                uintptr_t obj = Read<uintptr_t>(entity + known[ki]);
                if (!IsHeapObj(obj)) continue;
                char sn[48] = {}; float v = -1.f;
                if (WalkStatsRootForEnergy(obj, 0, sn, &v))
                {
                    bestRoot = obj; bestRootOff = known[ki]; energyFound++;
                    char b[96];
                    wsprintfA(b, "vitals: known+0x%X '%s'=%d", (unsigned)known[ki], sn[0]?sn:"?", (int)v);
                    Log(b);
                    g_CachedStatsOff = known[ki];
                    return obj;
                }
                if (StatRecLooksLikeEnergyWater(obj, sn, 48, &v) &&
                    (StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0))
                {
                    labelHits++;
                    if (StrCmpI(sn, "Energy") == 0)
                    {
                        g_CachedEnergyRec = obj;
                        ExtractStatValue(obj, sn, &v, &g_CachedEnergyValOff);
                    }
                    else
                    {
                        g_CachedWaterRec = obj;
                        ExtractStatValue(obj, sn, &v, &g_CachedWaterValOff);
                    }
                    bestRoot = obj; bestRootOff = known[ki];
                }
            }
            if (labelHits > 0 && bestRoot)
            {
                g_CachedStatsOff = bestRootOff;
                return bestRoot;
            }
        }

        for (uintptr_t off = 0x100; off <= 0xA00; off += 8)
        {
            if (IsBogusStatsPlayerOff(off)) continue;
            uintptr_t obj = Read<uintptr_t>(entity + off);
            if (!IsHeapObj(obj)) continue;

            bool back = false;
            for (uintptr_t bo = 0x0; bo <= 0x40; bo += 8)
            {
                if (Read<uintptr_t>(obj + bo) == entity) { back = true; break; }
            }

            char sn[48] = {};
            float v = -1.f;
            if (StatRecLooksLikeEnergyWater(obj, sn, 48, &v))
            {
                if (StrCmpI(sn, "Energy") == 0)
                {
                    energyFound++; labelHits++;
                    g_CachedEnergyRec = obj;
                    ExtractStatValue(obj, sn, &v, &g_CachedEnergyValOff);
                    bestRoot = obj; bestRootOff = off;
                }
                else if (StrCmpI(sn, "Water") == 0)
                {
                    labelHits++;
                    g_CachedWaterRec = obj;
                    ExtractStatValue(obj, sn, &v, &g_CachedWaterValOff);
                    if (!bestRoot) { bestRoot = obj; bestRootOff = off; }
                }
            }

            if (back)
            {
                for (uintptr_t co = 0x0; co <= 0x50; co += 8)
                {
                    uintptr_t child = Read<uintptr_t>(obj + co);
                    if (!IsHeapObj(child) || child == obj) continue;
                    if (WalkStatsRootForEnergy(child, 0, sn, &v) || WalkStatsRootForEnergy(obj, 0, sn, &v))
                    {
                        bestRoot = obj; bestRootOff = off; energyFound++; labelHits++;
                        break;
                    }
                    if (StatRecLooksLikeEnergyWater(child, sn, 48, &v))
                    {
                        if (StrCmpI(sn, "Energy") == 0)
                        {
                            g_CachedEnergyRec = child;
                            ExtractStatValue(child, sn, &v, &g_CachedEnergyValOff);
                            bestRoot = obj; bestRootOff = off; energyFound++; labelHits++;
                        }
                        else if (StrCmpI(sn, "Water") == 0)
                        {
                            g_CachedWaterRec = child;
                            ExtractStatValue(child, sn, &v, &g_CachedWaterValOff);
                            labelHits++;
                        }
                    }
                }

                // Float-pairs: require TWO pairs and at least one non-zero (real Energy/Water)
                float vals[4]; uintptr_t offs[4];
                int np = CollectFloatPairsOnObj(obj, vals, offs, 4);
                int nonzero = 0;
                for (int i = 0; i < np; i++) if (vals[i] > 0.f) nonzero++;
                if (np >= 2 && nonzero >= 1 && !g_CachedEnergyRec && !g_CachedWaterRec)
                {
                    g_CachedEnergyRec = obj;
                    g_CachedEnergyValOff = offs[0];
                    g_CachedWaterRec = obj;
                    g_CachedWaterValOff = offs[1];
                    bestRoot = obj; bestRootOff = off; energyFound++;
                    char b[128];
                    wsprintfA(b, "vitals: backref-floatpairs obj=%p n=%d e=%d w=%d off=0x%X",
                        (void*)obj, np, (int)vals[0], (int)vals[1], (unsigned)off);
                    Log(b);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("vitals: backref scan SEH");
        return 0;
    }

    if ((labelHits > 0 || energyFound || bestRoot) && !IsBogusStatsPlayerOff(bestRootOff))
    {
        g_CachedStatsOff = bestRootOff;
        char b[144];
        wsprintfA(b, "vitals: backref-hit root=%p playerOff=0x%X eRec=%p wRec=%p labels=%d",
            (void*)bestRoot, (unsigned)bestRootOff, (void*)g_CachedEnergyRec, (void*)g_CachedWaterRec, labelHits);
        Log(b);
        return bestRoot ? bestRoot : g_CachedEnergyRec;
    }
    return 0;
}

static uintptr_t DiscoverPlayerStats(uintptr_t entity)
{
    __try
    {
        // Invalidate previous wrong cache that pointed at Skeleton/InputController
        if (IsBogusStatsPlayerOff(g_CachedStatsOff))
        {
            g_CachedStatsOff = 0;
            g_CachedEnergyRec = 0;
            g_CachedWaterRec = 0;
            g_CachedEnergyValOff = 0;
            g_CachedWaterValOff = 0;
            Log("vitals: cleared bogus stats cache (was Skeleton/InputController)");
        }
        // Prefer official StatsContainer @ 0x6F0 before any expensive hunt
        {
            uintptr_t s6 = Read<uintptr_t>(entity + oak_offsets::player::StatsContainer);
            char sn[48] = {}; float v = -1.f;
            if (IsHeapObj(s6) && WalkStatsRootForEnergy(s6, 0, sn, &v))
            {
                g_CachedStatsOff = oak_offsets::player::StatsContainer;
                g_DiscoverExhausted = 0;
                return s6;
            }
        }
        if (g_CachedEnergyRec && IsHeapObj(g_CachedEnergyRec))
            return g_CachedEnergyRec;
        // STOP — wide 0x100..0xA00 Present-thread scans froze DayZ every ~5s.
        g_DiscoverExhausted = 1;
        g_DiscoverNextTry = 0xFFFFFFFEu;
        return 0;

        // Fast path: cached Energy/Water — reject lone zero-only Energy without Water label
        if (g_CachedEnergyRec && IsHeapObj(g_CachedEnergyRec))
        {
            bool waterOk = g_CachedWaterRec && IsHeapObj(g_CachedWaterRec);
            float t = -1.f;
            if (g_CachedEnergyValOff)
                t = Read<float>(g_CachedEnergyRec + g_CachedEnergyValOff);
            // Stale false-pair cache: Energy=0 and no Water → rediscover
            if (!waterOk && (t == 0.f || t < 0.f))
            {
                g_CachedEnergyRec = 0;
                g_CachedEnergyValOff = 0;
            }
            else if (t == t && t >= 0.f && t <= 20000.f)
                return g_CachedEnergyRec;
            else
            {
                char sn[48] = {};
                if (ReadRecStatLabel(g_CachedEnergyRec, sn, 48))
                    return g_CachedEnergyRec;
                g_CachedEnergyRec = 0;
            }
        }
        if (g_CachedStatsOff)
        {
            uintptr_t s = Read<uintptr_t>(entity + g_CachedStatsOff);
            char sn[48] = {};
            float v = -1.f;
            if (IsHeapObj(s) && WalkStatsRootForEnergy(s, 0, sn, &v))
                return s;
            // Do NOT clear g_CachedStatsOff here — remote ESP entities used to wipe it
            // every frame and re-trigger a full 0x100..0xA00 Present-thread scan → ~7 FPS.
            if (IsValidPtr(g_ResolvedLocalPlayer) && entity == g_ResolvedLocalPlayer)
                g_CachedStatsOff = 0;
            else
                return 0; // remote: skip expensive rediscovery
        }

        // Don't re-run expensive discovery every Present — retry every 5s max
        DWORD now = GetTickCount();
        if (g_DiscoverExhausted && now < g_DiscoverNextTry)
            return 0;

        // Remote ESP entities must never run full discovery (was the FPS melt)
        if (IsValidPtr(g_ResolvedLocalPlayer) && entity != g_ResolvedLocalPlayer)
            return 0;

        if (!g_RdataEnergyStr)
        {
            g_RdataEnergyStr = FindModuleCString("Energy");
            g_RdataWaterStr = FindModuleCString("Water");
            char b[96];
            wsprintfA(b, "vitals: rdata Energy=%p Water=%p",
                (void*)g_RdataEnergyStr, (void*)g_RdataWaterStr);
            Log(b);
        }

        // Pass 0: reverse m_Player + float pairs (bounded)
        {
            uintptr_t hit = DiscoverByPlayerBackref(entity);
            if (hit) { g_DiscoverExhausted = 0; return hit; }
        }

        // Pass A: rdata string ptr under player (2 deep, capped)
        if (g_RdataEnergyStr)
        {
            for (uintptr_t off = 0x100; off <= 0xA00; off += 8)
            {
                if (IsBogusStatsPlayerOff(off)) continue;
                uintptr_t a = Read<uintptr_t>(entity + off);
                if (!IsHeapObj(a)) continue;
                for (uintptr_t io = 0x0; io <= 0x80; io += 8)
                {
                    uintptr_t p = Read<uintptr_t>(a + io);
                    if (p == g_RdataEnergyStr || p == g_RdataWaterStr)
                    {
                        g_CachedStatsOff = off;
                        char sn[48] = {}; float v = -1.f;
                        StatRecLooksLikeEnergyWater(a, sn, 48, &v);
                        if (StrCmpI(sn, "Energy") == 0) g_CachedEnergyRec = a;
                        if (StrCmpI(sn, "Water") == 0) g_CachedWaterRec = a;
                        char b[144];
                        wsprintfA(b, "vitals: rdata-hit player+0x%X obj=%p '%s'=%d",
                            (unsigned)off, (void*)a, sn[0] ? sn : "?", (int)v);
                        Log(b);
                        g_DiscoverExhausted = 0;
                        return a;
                    }
                    if (!IsHeapObj(p)) continue;
                    for (uintptr_t jo = 0x0; jo <= 0x60; jo += 8)
                    {
                        uintptr_t q = Read<uintptr_t>(p + jo);
                        if (q != g_RdataEnergyStr && q != g_RdataWaterStr) continue;
                        g_CachedStatsOff = off;
                        char sn[48] = {}; float v = -1.f;
                        StatRecLooksLikeEnergyWater(p, sn, 48, &v);
                        if (StrCmpI(sn, "Energy") == 0) g_CachedEnergyRec = p;
                        if (StrCmpI(sn, "Water") == 0) g_CachedWaterRec = p;
                        char b[144];
                        wsprintfA(b, "vitals: rdata-hit2 +0x%X -> %p '%s'=%d",
                            (unsigned)off, (void*)p, sn[0] ? sn : "Energy", (int)v);
                        Log(b);
                        g_DiscoverExhausted = 0;
                        return p;
                    }
                }
            }
        }

        // Pass A2: find PlayerStat records whose m_ValueLabel points at rdata "Energy"/"Water"
        // (Enforce often stores const char* label, not EngString). Then parent = stats root.
        if (g_RdataEnergyStr || g_RdataWaterStr)
        {
            uintptr_t energyRec = 0, waterRec = 0;
            uintptr_t energyParentOff = 0;
            for (uintptr_t off = 0x100; off <= 0xA00; off += 8)
            {
                if (IsBogusStatsPlayerOff(off)) continue;
                uintptr_t container = Read<uintptr_t>(entity + off);
                if (!IsHeapObj(container)) continue;
                // Walk one level of arrays under container
                for (uintptr_t bo = 0x0; bo <= 0x60; bo += 8)
                {
                    uintptr_t arr = Read<uintptr_t>(container + bo);
                    if (!IsHeapObj(arr)) continue;
                    uintptr_t tabs[3] = { arr, Read<uintptr_t>(arr + 0x0), Read<uintptr_t>(arr + 0x8) };
                    for (int ti = 0; ti < 3; ti++)
                    {
                        uintptr_t tab = tabs[ti];
                        if (!IsHeapObj(tab)) continue;
                        for (int i = 0; i < 20; i++)
                        {
                            uintptr_t rec = Read<uintptr_t>(tab + (uintptr_t)i * 8);
                            if (!IsHeapObj(rec)) continue;
                            for (uintptr_t lo = 0x0; lo <= 0x50; lo += 8)
                            {
                                uintptr_t lp = Read<uintptr_t>(rec + lo);
                                if (lp == g_RdataEnergyStr)
                                {
                                    energyRec = rec;
                                    energyParentOff = off;
                                    float v = Read<float>(rec + oak_offsets::player::RecordValue);
                                    char b[128];
                                    wsprintfA(b, "vitals: EnergyRec player+0x%X[%d] val@2C=%d",
                                        (unsigned)off, i, (int)v);
                                    Log(b);
                                }
                                if (lp == g_RdataWaterStr)
                                {
                                    waterRec = rec;
                                    float v = Read<float>(rec + oak_offsets::player::RecordValue);
                                    char b[128];
                                    wsprintfA(b, "vitals: WaterRec player+0x%X[%d] val@2C=%d",
                                        (unsigned)off, i, (int)v);
                                    Log(b);
                                }
                            }
                        }
                    }
                }
                // Also: fixed EPlayerStats indices 3/4 — ONLY if record points at rdata Energy/Water
                for (uintptr_t bo = 0x0; bo <= 0x40 && (!energyRec || !waterRec); bo += 8)
                {
                    uintptr_t arr = Read<uintptr_t>(container + bo);
                    if (!IsHeapObj(arr)) continue;
                    uintptr_t data = Read<uintptr_t>(arr + 0x0);
                    if (!IsHeapObj(data)) data = arr;
                    uintptr_t eRec = Read<uintptr_t>(data + 3 * 8);
                    uintptr_t wRec = Read<uintptr_t>(data + 4 * 8);
                    if (!IsHeapObj(eRec) || !IsHeapObj(wRec)) continue;
                    bool eLab = false, wLab = false;
                    for (uintptr_t lo = 0x0; lo <= 0x50; lo += 8)
                    {
                        uintptr_t lp = Read<uintptr_t>(eRec + lo);
                        if (lp == g_RdataEnergyStr) eLab = true;
                        lp = Read<uintptr_t>(wRec + lo);
                        if (lp == g_RdataWaterStr) wLab = true;
                    }
                    if (!eLab || !wLab) continue;
                    float ev = Read<float>(eRec + oak_offsets::player::RecordValue);
                    float wv = Read<float>(wRec + oak_offsets::player::RecordValue);
                    if (ev != ev || wv != wv) continue;
                    if (ev < 0.f || ev > 5000.f || wv < 0.f || wv > 5000.f) continue;
                    energyRec = eRec;
                    waterRec = wRec;
                    energyParentOff = off;
                    char b[144];
                    wsprintfA(b, "vitals: idx3/4+label player+0x%X Energy=%d Water=%d",
                        (unsigned)off, (int)ev, (int)wv);
                    Log(b);
                }
            }
            if (energyRec && waterRec)
            {
                // Prefer RecordValue @ 0x2C; reject if ExtractStatValue would grab skeleton junk
                float ev = Read<float>(energyRec + oak_offsets::player::RecordValue);
                float wv = Read<float>(waterRec + oak_offsets::player::RecordValue);
                if (ev != ev || wv != wv || ev < 0.f || ev > 5000.f || wv < 0.f || wv > 5000.f)
                {
                    energyRec = 0; waterRec = 0;
                }
                else
                {
                    g_CachedEnergyRec = energyRec;
                    g_CachedWaterRec = waterRec;
                    g_CachedEnergyValOff = oak_offsets::player::RecordValue;
                    g_CachedWaterValOff = oak_offsets::player::RecordValue;
                    g_CachedStatsOff = energyParentOff ? energyParentOff : oak_offsets::player::StatsContainer;
                    g_DiscoverExhausted = 0;
                    char b[128];
                    wsprintfA(b, "vitals: locked E=%d W=%d off=0x%X", (int)ev, (int)wv, (unsigned)g_CachedStatsOff);
                    Log(b);
                    return energyRec;
                }
            }
        }

        // Pass B: classic array walk by readable labels (narrower range)
        for (uintptr_t off = 0x200; off <= 0x900; off += 8)
        {
            if (IsBogusStatsPlayerOff(off)) continue;
            uintptr_t s = Read<uintptr_t>(entity + off);
            if (!IsHeapObj(s)) continue;
            char sn[48] = {};
            float v = -1.f;
            if (!WalkStatsRootForEnergy(s, 0, sn, &v)) continue;
            g_CachedStatsOff = off;
            char b[128];
            wsprintfA(b, "vitals: PlayerStats @ player+0x%X '%s'=%d (cached)",
                (unsigned)off, sn[0] ? sn : "?", (int)v);
            Log(b);
            g_DiscoverExhausted = 0;
            return s;
        }

        // Pass C: nested float-pairs under player (depth 1 only — cheap)
        {
            float found[4]; int nFound = 0;
            uintptr_t foundObj[4]; uintptr_t foundOff[4];
            for (uintptr_t off = 0x200; off <= 0x900 && nFound < 4; off += 8)
            {
                if (IsBogusStatsPlayerOff(off)) continue;
                uintptr_t a = Read<uintptr_t>(entity + off);
                if (!IsHeapObj(a)) continue;
                float vals[4]; uintptr_t offs[4];
                int np = CollectFloatPairsOnObj(a, vals, offs, 4);
                for (int i = 0; i < np && nFound < 4; i++)
                {
                    found[nFound] = vals[i];
                    foundObj[nFound] = a;
                    foundOff[nFound] = offs[i];
                    nFound++;
                }
            }
            if (nFound >= 1)
            {
                g_CachedEnergyRec = foundObj[0];
                g_CachedEnergyValOff = foundOff[0];
                if (nFound >= 2)
                {
                    g_CachedWaterRec = foundObj[1];
                    g_CachedWaterValOff = foundOff[1];
                }
                char b[128];
                wsprintfA(b, "vitals: nest-floatpairs n=%d e=%d@%p+0x%X",
                    nFound, (int)found[0], (void*)foundObj[0], (unsigned)foundOff[0]);
                Log(b);
                g_DiscoverExhausted = 0;
                return foundObj[0];
            }
        }

        g_DiscoverExhausted = 1;
        g_DiscoverNextTry = now + 5000;
        if (g_DiscoverFailLogs < 3)
        {
            g_DiscoverFailLogs++;
            Log("vitals: Energy label NOT found under local player (wide scan)");
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("vitals: DiscoverPlayerStats SEH");
        g_DiscoverExhausted = 1;
        g_DiscoverNextTry = GetTickCount() + 5000;
        return 0;
    }
}

static void DumpVitalsProbeOnce(uintptr_t entity)
{
    (void)entity; // disabled — Discover every 5s froze Present
}

static bool ReadVitalsImpl(uintptr_t entity, float* healthPct, float* bloodPct, float* shockPct,
    float* stamPct, float* hungerPct, float* thirstPct);

static bool ReadVitals(uintptr_t entity, float* healthPct, float* bloodPct, float* shockPct,
    float* stamPct, float* hungerPct, float* thirstPct)
{
    if (healthPct) *healthPct = -1.f;
    if (bloodPct) *bloodPct = -1.f;
    if (shockPct) *shockPct = -1.f;
    if (stamPct) *stamPct = -1.f;
    if (hungerPct) *hungerPct = -1.f;
    if (thirstPct) *thirstPct = -1.f;
    if (!IsValidPtr(entity)) return false;
    bool ok = false;
    __try { ok = ReadVitalsImpl(entity, healthPct, bloodPct, shockPct, stamPct, hungerPct, thirstPct); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Raw Energy/Water floats for ground-truth QA (server Set → client read).
static void GetLocalEnergyWaterRaw(float* energyOut, float* waterOut)
{
    if (energyOut) *energyOut = -1.f;
    if (waterOut) *waterOut = -1.f;
    __try
    {
        if (energyOut && g_CachedEnergyRec && IsHeapObj(g_CachedEnergyRec))
        {
            float v = -1.f;
            ExtractStatValue(g_CachedEnergyRec, "Energy", &v, &g_CachedEnergyValOff);
            if (v < 0.f && g_CachedEnergyValOff)
                v = Read<float>(g_CachedEnergyRec + g_CachedEnergyValOff);
            *energyOut = v;
        }
        if (waterOut && g_CachedWaterRec && IsHeapObj(g_CachedWaterRec))
        {
            float v = -1.f;
            ExtractStatValue(g_CachedWaterRec, "Water", &v, &g_CachedWaterValOff);
            if (v < 0.f && g_CachedWaterValOff)
                v = Read<float>(g_CachedWaterRec + g_CachedWaterValOff);
            *waterOut = v;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool ReadVitalsImpl(uintptr_t entity, float* healthPct, float* bloodPct, float* shockPct,
    float* stamPct, float* hungerPct, float* thirstPct)
{
    DumpVitalsProbeOnce(entity);

    if (IsBogusStatsPlayerOff(g_CachedStatsOff))
    {
        g_CachedStatsOff = 0;
        g_CachedEnergyRec = 0;
        g_CachedWaterRec = 0;
        g_CachedEnergyValOff = 0;
        g_CachedWaterValOff = 0;
    }

    // Infected have HP/blood only — never invent hunger/thirst/stamina for them.
    bool isInfected = false;
    bool isLocal = (IsValidPtr(g_ResolvedLocalPlayer) && entity == g_ResolvedLocalPlayer);
    {
        char cfg[48] = {};
        ReadEntityConfigName(entity, cfg, 48);
        if (StrCmpI(cfg, "dayzinfected") == 0 || StrContainsI(cfg, "infected"))
            isInfected = true;
        else
        {
            char tn[48] = {};
            ReadEntityTypeName(entity, tn, 48);
            if (StrContainsI(tn, "Zmb") || StrContainsI(tn, "Infected"))
                isInfected = true;
        }
    }

    // Authority file FIRST — hung/thir/shock have no client DS/PlayerStats. Keep last-good
    // if the server currently has oak_vitals.txt open (empty/locked reads).
    if (!isInfected)
    {
        bool fileLocal = isLocal;
        if (!fileLocal)
        {
            char cfg2[48] = {};
            ReadEntityConfigName(entity, cfg2, 48);
            if (StrCmpI(cfg2, "dayzplayer") == 0)
                fileLocal = true;
        }
        OakFileVitRefresh();
        OakFileVitApply(entity, fileLocal, false,
            healthPct, bloodPct, shockPct, stamPct, hungerPct, thirstPct);
    }

    // Fast path for local: once Energy/Water records are cached, skip Discover + array walks
    if (isLocal && !isInfected && g_CachedEnergyRec && g_CachedWaterRec &&
        IsHeapObj(g_CachedEnergyRec) && IsHeapObj(g_CachedWaterRec))
    {
        auto asEnergyPctFast = [](float x) -> float {
            if (x != x || x < 0.f) return -1.f;
            if (x == 0.f) return 0.f;
            if (x > 0.f && x <= 1.05f) return -1.f;
            float mx = (x > 5000.f) ? 20000.f : 5000.f;
            float p = (x / mx) * 100.f;
            if (p > 100.f) p = 100.f;
            return p;
        };
        if (hungerPct && *hungerPct < 0.f)
        {
            float v = -1.f;
            if (g_CachedEnergyValOff)
                v = Read<float>(g_CachedEnergyRec + g_CachedEnergyValOff);
            if (v < 0.f) ExtractStatValue(g_CachedEnergyRec, "Energy", &v, &g_CachedEnergyValOff);
            float p = asEnergyPctFast(v);
            if (p >= 0.f) *hungerPct = p;
        }
        if (thirstPct && *thirstPct < 0.f)
        {
            float v = -1.f;
            ExtractStatValue(g_CachedWaterRec, "Water", &v, &g_CachedWaterValOff);
            if (v >= 0.f && v < 4999.f)
            {
                float p = asEnergyPctFast(v);
                if (p >= 0.f) *thirstPct = p;
            }
            else if (v == 0.f)
                *thirstPct = 0.f;
        }
        // Still try DamageManager for HP below — skip PlayerStats rediscovery
        // (flag checked after dmg block)
    }
    // File already filled Food/Water — do NOT heap-walk PlayerStats every HUD tick
    // (DiscoverPlayerStats was ~70ms/frame and got worse as the world streamed in).
    const bool fileHungThir =
        (hungerPct && *hungerPct >= 0.f) && (thirstPct && *thirstPct >= 0.f);
    const bool skipPlayerStats = !isInfected && (fileHungThir ||
        (isLocal && g_CachedEnergyRec && g_CachedWaterRec &&
         IsHeapObj(g_CachedEnergyRec) && IsHeapObj(g_CachedWaterRec) &&
         (hungerPct && *hungerPct >= 0.f) && (thirstPct && *thirstPct >= 0.f)));

    // Do NOT trust IsDead@0xE2 for vitals — false-positives on living players.

    uintptr_t mod = g_GameModule ? (uintptr_t)g_GameModule : 0;
    auto vtOk = [&](uintptr_t p) -> bool {
        if (!IsValidPtr(p) || p < 0x100000000) return false;
        if (mod && p >= mod && p < mod + 0x8000000ULL) return false;
        if (!mod) return true;
        uintptr_t vt = Read<uintptr_t>(p);
        return IsValidPtr(vt) && vt >= mod && vt < mod + 0x8000000ULL;
    };
    auto looksZoneHolder = [&](uintptr_t dmg) -> bool {
        // DamageManager often lacks a game-module vtable — only require a heap-looking ptr
        if (!IsValidPtr(dmg) || dmg < 0x100000000) return false;
        if (mod && dmg >= mod && dmg < mod + 0x8000000ULL) return false;
        uintptr_t a28 = Read<uintptr_t>(dmg + 0x28);
        int c34 = Read<int>(dmg + 0x34);
        int c30 = Read<int>(dmg + 0x30);
        if (IsValidPtr(a28) && ((c34 > 0 && c34 <= 64) || (c30 > 0 && c30 <= 64)))
            return true;
        float f10 = Read<float>(dmg + 0x10);
        return (f10 == f10 && ((f10 >= 0.f && f10 <= 1.05f) || (f10 >= 0.f && f10 <= 100.f)));
    };

    // EntityHealth / DamageSystem — live slot is player+0x188 (HasDamageSystem / GetHealth01).
    // NOT 0x6F8 (UAInterface), NOT stale updater 0x700 (null on players / module junk on infected).
    {
        static uintptr_t s_DmgOffCached = 0;
        // One-time migration: drop stale UAInterface / old 0x700 cache
        if (s_DmgOffCached == 0x6F8 || s_DmgOffCached == 0x6F0 || s_DmgOffCached == 0x700)
            s_DmgOffCached = 0;
        uintptr_t tryOffs[10];
        int nTry = 0;
        auto pushOff = [&](uintptr_t o) {
            if (o == 0x6F8 || o == 0x6F0) return; // UAInterface / Stats — not health
            for (int i = 0; i < nTry; i++) if (tryOffs[i] == o) return;
            if (nTry < 10) tryOffs[nTry++] = o;
        };
        if (s_DmgOffCached) pushOff(s_DmgOffCached);
        pushOff(oak_offsets::player::DamageManager); // 0x188
        pushOff(0x190);
        pushOff(0x180);
        // One-shot RTTI hunt only — wide NameObject scans every ReadVitals melt FPS.
        static bool s_DidDmgRttiHunt = false;
        if (!s_DmgOffCached && !s_DidDmgRttiHunt)
        {
            s_DidDmgRttiHunt = true;
            pushOff(0x700); // legacy updater — only used if looksZoneHolder passes
            pushOff(0x708);
            for (uintptr_t off = 0x100; off <= 0x220 && nTry < 10; off += 8)
            {
                if (off == 0x6F8 || off == 0x6F0) continue;
                uintptr_t cand = Read<uintptr_t>(entity + off);
                if (!IsValidPtr(cand) || cand < 0x100000000) continue;
                if (mod && cand >= mod && cand < mod + 0x8000000ULL) continue;
                char rtti[96] = {};
                if (!OakEngine_NameObject(cand, rtti, 96) || !rtti[0]) continue;
                if (StrContainsI(rtti, "UAInterface") || StrContainsI(rtti, "Inventory") ||
                    StrContainsI(rtti, "Anim") || StrContainsI(rtti, "Input") ||
                    StrContainsI(rtti, "PlayerType") || StrContainsI(rtti, "DayZPlayerType"))
                    continue;
                if (!(StrContainsI(rtti, "Damage") || StrContainsI(rtti, "EntityHealth") ||
                      StrContainsI(rtti, "HealthSystem") || StrContainsI(rtti, "DamageSystem")))
                    continue;
                if (!looksZoneHolder(cand)) continue;
                pushOff(off);
            }
            for (uintptr_t off = 0x5C0; off <= 0x820 && nTry < 10; off += 8)
            {
                if (off == 0x6F8 || off == 0x6F0) continue;
                uintptr_t cand = Read<uintptr_t>(entity + off);
                if (!IsValidPtr(cand) || cand < 0x100000000) continue;
                if (mod && cand >= mod && cand < mod + 0x8000000ULL) continue;
                char rtti[96] = {};
                if (!OakEngine_NameObject(cand, rtti, 96) || !rtti[0]) continue;
                if (StrContainsI(rtti, "UAInterface") || StrContainsI(rtti, "Inventory") ||
                    StrContainsI(rtti, "Anim") || StrContainsI(rtti, "Input"))
                    continue;
                if (!(StrContainsI(rtti, "Damage") || StrContainsI(rtti, "EntityHealth") ||
                      StrContainsI(rtti, "HealthSystem") || StrContainsI(rtti, "DamageSystem")))
                    continue;
                if (!looksZoneHolder(cand)) continue;
                pushOff(off);
            }
        }
        for (int di = 0; di < nTry; di++)
        {
            if (healthPct && *healthPct >= 0.f && bloodPct && *bloodPct >= 0.f) break;
            uintptr_t dmg = Read<uintptr_t>(entity + tryOffs[di]);
            if (!IsValidPtr(dmg) || dmg < 0x100000000) continue;
            if (mod && dmg >= mod && dmg < mod + 0x8000000ULL) continue;
            if (!looksZoneHolder(dmg)) continue;
            {
                char rtti[96] = {};
                if (OakEngine_NameObject(dmg, rtti, 96) && rtti[0] && StrContainsI(rtti, "UAInterface"))
                    continue;
            }
            float hpBefore = healthPct ? *healthPct : -1.f;
            ProbeEntityHealthBlock(dmg, healthPct, bloodPct, shockPct);
            if (healthPct && *healthPct >= 0.f && hpBefore < 0.f)
                s_DmgOffCached = tryOffs[di];
        }
    }

    // Enforce Object.GetHealth01 / GetHealth — only when DamageSystem exists at +0x188.
    // Calling with null DS just returns 0 and can spam engine diagnostics.
    if (((healthPct && *healthPct < 0.f) || (bloodPct && *bloodPct < 0.f) || (shockPct && *shockPct < 0.f)) &&
        IsValidPtr(entity))
    {
        uintptr_t dsPtr = 0;
        __try { dsPtr = Read<uintptr_t>(entity + oak_offsets::player::DamageManager); }
        __except (EXCEPTION_EXECUTE_HANDLER) { dsPtr = 0; }
        if (IsValidPtr(dsPtr) && dsPtr > 0x100000000 &&
            !(mod && dsPtr >= mod && dsPtr < mod + 0x8000000ULL))
        {
        typedef float(__fastcall* Fn_ObjGetHealth)(void* obj, const char* zone, const char* type);
        auto call01 = [&](const char* zone, const char* type, float* outAbsOr01) -> bool {
            if (!outAbsOr01 || !g_GameModule || !IsValidPtr(entity)) return false;
            const uintptr_t addr = (uintptr_t)g_GameModule + 0x923F20;
            if (!OakNativeEntryOk(addr, "Object::GetHealth01(zone)")) return false;
            auto fn = (Fn_ObjGetHealth)addr;
            float v = -1.f;
            __try { v = fn((void*)entity, zone ? zone : "", type ? type : ""); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (!(v == v) || v < 0.f) return false;
            // GetHealth01 → 0..1; treat tiny noise as empty (no DS)
            if (v <= 0.0001f) return false;
            *outAbsOr01 = v;
            return true;
        };
        auto callAbs = [&](const char* zone, const char* type, float* outAbs) -> bool {
            if (!outAbs || !g_GameModule || !IsValidPtr(entity)) return false;
            const uintptr_t addr = (uintptr_t)g_GameModule + 0x923FB0;
            if (!OakNativeEntryOk(addr, "Object::GetHealth(zone)")) return false;
            auto fn = (Fn_ObjGetHealth)addr;
            float v = -1.f;
            __try { v = fn((void*)entity, zone ? zone : "", type ? type : ""); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (!(v == v) || v < 0.f) return false;
            if (v <= 0.0001f) return false;
            *outAbs = v;
            return true;
        };
        auto toPct01 = [](float v) -> float {
            if (v > 0.01f && v <= 1.05f) return v * 100.f;
            if (v > 1.05f && v <= 100.f) return v;
            return -1.f;
        };
        auto toBloodPct = [](float v) -> float {
            if (v > 100.f && v <= 7500.f) return v / 5000.f * 100.f;
            if (v > 0.01f && v <= 1.05f) return v * 100.f;
            if (v > 1.05f && v <= 100.f) return v;
            return -1.f;
        };

        if (healthPct && *healthPct < 0.f)
        {
            float v = -1.f;
            if (call01("", "", &v) || call01("", "Health", &v) || call01("GlobalHealth", "Health", &v))
            {
                float p = toPct01(v);
                if (p > 0.f) *healthPct = p;
            }
            else if (callAbs("", "", &v) || callAbs("", "Health", &v))
            {
                float p = toPct01(v);
                if (p < 0.f && v > 1.f && v <= 100.f) p = v;
                if (p > 0.f) *healthPct = p;
            }
        }
        if (bloodPct && *bloodPct < 0.f)
        {
            float v = -1.f;
            if (call01("", "Blood", &v) || call01("GlobalHealth", "Blood", &v))
            {
                float p = toBloodPct(v);
                if (p >= 0.f) *bloodPct = p;
            }
            else if (callAbs("", "Blood", &v) || callAbs("GlobalHealth", "Blood", &v))
            {
                float p = toBloodPct(v);
                if (p >= 0.f) *bloodPct = p;
            }
        }
        if (shockPct && *shockPct < 0.f)
        {
            float v = -1.f;
            if (call01("", "Shock", &v) || callAbs("", "Shock", &v))
            {
                float p = toPct01(v);
                if (p < 0.f && v > 1.f && v <= 100.f) p = v;
                if (v > 100.f && v <= 7500.f) p = v / 5000.f * 100.f; // rare abs shock
                if (p >= 0.f) *shockPct = p;
            }
        }
        } // dsPtr valid
    }

    OakFileVitApply(entity, isLocal, isInfected,
        healthPct, bloodPct, shockPct, stamPct, hungerPct, thirstPct);

    // Old inline parser kept compiled-out — refresh/apply lives in OakFileVit*.
    if (false && !isInfected && isLocal &&
        ((healthPct && *healthPct < 0.f) || (bloodPct && *bloodPct < 0.f) ||
         (shockPct && *shockPct < 0.f) || (stamPct && *stamPct < 0.f) ||
         (hungerPct && *hungerPct < 0.f) || (thirstPct && *thirstPct < 0.f)))
    {
        struct FileVit {
            int netLo;
            int hl;
            float hp01;
            float hp;
            float blood;
            float shock;
            float energy;
            float water;
            float stam;
            int bleed;
            bool used;
        };
        static FileVit s_FileVit[16];
        static int s_FileVitN = 0;
        static DWORD s_FileVitTick = 0;
        const DWORD nowFv = GetTickCount();
        auto tokF = [](const char* line, const char* key, float defv) -> float {
            const char* p = strstr(line, key);
            if (!p) return defv;
            return (float)atof(p + lstrlenA(key));
        };
        auto tokI = [](const char* line, const char* key, int defv) -> int {
            const char* p = strstr(line, key);
            if (!p) return defv;
            return atoi(p + lstrlenA(key));
        };
        auto asStatPct = [](float x, float maxHint) -> float {
            if (!(x == x) || x < 0.f) return -1.f;
            if (x <= 1.05f) return x * 100.f;
            float mx = maxHint;
            if (x > mx) mx = (x > 5000.f) ? 20000.f : 5000.f;
            if (mx < 1.f) mx = 100.f;
            float p = (x / mx) * 100.f;
            if (p > 100.f) p = 100.f;
            return p;
        };
        if (!s_FileVitTick || (nowFv - s_FileVitTick) > 500u)
        {
            s_FileVitTick = nowFv;
            s_FileVitN = 0;
            const char* paths[] = {
                "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DayZServer\\oak_profiles\\oak_vitals.txt",
                "C:\\PROGRA~2\\Steam\\STEAMA~1\\common\\DAYZSE~1\\OAK_PR~1\\oak_vitals.txt",
            };
            HANDLE hf = INVALID_HANDLE_VALUE;
            for (int pi = 0; pi < 2 && hf == INVALID_HANDLE_VALUE; pi++)
                hf = CreateFileA(paths[pi], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE)
            {
                char buf[4096];
                DWORD rd = 0;
                if (ReadFile(hf, buf, sizeof(buf) - 1, &rd, nullptr) && rd > 0)
                {
                    buf[rd] = 0;
                    FileVit acc = {};
                    acc.hl = -1;
                    acc.hp01 = acc.hp = acc.blood = acc.shock = acc.energy = acc.water = acc.stam = -1.f;
                    bool have = false;
                    for (char* line = buf; *line; )
                    {
                        char* nl = line;
                        while (*nl && *nl != '\n' && *nl != '\r') nl++;
                        char save = *nl;
                        *nl = 0;
                        const bool isNet = strstr(line, "net=") != nullptr;
                        const bool isPlr = strstr(line, "player=") != nullptr;
                        if (isNet || isPlr || strstr(line, "blood=") || strstr(line, "energy="))
                        {
                            if (isNet) acc.netLo = tokI(line, "net=", acc.netLo);
                            acc.hl = tokI(line, " hl=", acc.hl);
                            if (acc.hl < 0) acc.hl = tokI(line, " healthLevel=", acc.hl);
                            acc.hp01 = tokF(line, " hp01=", acc.hp01);
                            acc.hp = tokF(line, " hp=", acc.hp);
                            acc.blood = tokF(line, " blood=", acc.blood);
                            acc.shock = tokF(line, " shock=", acc.shock);
                            acc.energy = tokF(line, " energy=", acc.energy);
                            acc.water = tokF(line, " water=", acc.water);
                            acc.stam = tokF(line, " stam=", acc.stam);
                            acc.bleed = tokI(line, " bleed=", acc.bleed);
                            acc.used = true;
                            have = true;
                            if (isNet && s_FileVitN < 16)
                            {
                                s_FileVit[s_FileVitN++] = acc;
                                acc = FileVit{};
                                acc.hl = -1;
                                acc.hp01 = acc.hp = acc.blood = acc.shock = acc.energy = acc.water = acc.stam = -1.f;
                                have = false;
                            }
                        }
                        *nl = save;
                        line = nl;
                        while (*line == '\n' || *line == '\r') line++;
                    }
                    if (have && s_FileVitN < 16)
                        s_FileVit[s_FileVitN++] = acc;
                }
                CloseHandle(hf);
            }
        }

        if (s_FileVitN > 0)
        {
            int entNet = 0;
            __try { entNet = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer); }
            __except (EXCEPTION_EXECUTE_HANDLER) { entNet = 0; }
            if (!entNet)
            {
                __try { entNet = Read<int>(entity + oak_offsets::entity::NetworkId); }
                __except (EXCEPTION_EXECUTE_HANDLER) { entNet = 0; }
            }

            const FileVit* hit = nullptr;
            for (int i = 0; i < s_FileVitN; i++)
            {
                if (entNet && s_FileVit[i].netLo == entNet) { hit = &s_FileVit[i]; break; }
            }
            if (!hit && isLocal)
                hit = &s_FileVit[s_FileVitN - 1];

            if (hit)
            {
                if (healthPct && *healthPct < 0.f)
                {
                    if (hit->hp01 >= 0.f && hit->hp01 <= 1.05f)
                        *healthPct = hit->hp01 * 100.f;
                    else if (hit->hp > 1.05f && hit->hp <= 100.f)
                        *healthPct = hit->hp;
                    else if (hit->hl >= 0 && hit->hl <= 4)
                    {
                        static const float kPct[5] = { 100.f, 50.f, 30.f, 20.f, 10.f };
                        *healthPct = kPct[hit->hl];
                    }
                }
                if (shockPct && *shockPct < 0.f && hit->shock >= 0.f)
                {
                    float p = asStatPct(hit->shock, 100.f);
                    if (p >= 0.f) *shockPct = p;
                }
                if (bloodPct && *bloodPct < 0.f && hit->blood >= 0.f)
                {
                    float p = asStatPct(hit->blood, 5000.f);
                    if (p >= 0.f) *bloodPct = p;
                }
                else if (bloodPct && *bloodPct < 0.f && hit->bleed != 0)
                    *bloodPct = 70.f;
                if (hungerPct && *hungerPct < 0.f && hit->energy >= 0.f)
                {
                    float p = asStatPct(hit->energy, 5000.f);
                    if (p >= 0.f) *hungerPct = p;
                }
                if (thirstPct && *thirstPct < 0.f && hit->water >= 0.f)
                {
                    float p = asStatPct(hit->water, 5000.f);
                    if (p >= 0.f) *thirstPct = p;
                }
                if (stamPct && *stamPct < 0.f && hit->stam >= 0.f)
                {
                    float p = asStatPct(hit->stam, 100.f);
                    if (p >= 0.f) *stamPct = p;
                }
            }
        }
    }

    // --- Coarse netsync vitals (remote players + local when DS is null) ---
    // Exact HP/blood are owner-only. Everyone gets m_HealthLevel (0..4), m_ShockSimplified,
    // m_CurrentShock, m_BleedingBits via PlayerBase RegisterNetSyncVariable*.
    // EnScript.GetClassVar @ 0x2D7E40 — NEVER call every Present tick (AV + FPS melt).
    // Prefer cached raw offsets once discovered; otherwise throttle GetClassVar per entity.
    if (!isInfected &&
        ((healthPct && *healthPct < 0.f) || (bloodPct && *bloodPct < 0.f) || (shockPct && *shockPct < 0.f)) &&
        g_GameModule && IsValidPtr(entity))
    {
        static int s_HlOff = -1;      // -1 unknown, 0 give-up on raw, >0 byte offset
        static int s_ShockFOff = -1;  // m_CurrentShock float
        static int s_ShockSOff = -1;  // m_ShockSimplified int
        static int s_BleedOff = -1;

        struct NetCache {
            uintptr_t ent;
            DWORD tick;
            int hl;
            int shockS;
            int bleed;
            float curShock;
            bool valid;
        };
        static NetCache s_Net[48];
        static int s_NetN = 0;
        const DWORD now = GetTickCount();

        auto levelToHpPct = [](int hl) -> float {
            // InjuryHandlerThresholds representative bar % (PRISTINE..RUINED).
            static const float kPct[5] = { 100.f, 50.f, 30.f, 20.f, 10.f };
            if (hl < 0 || hl > 4) return -1.f;
            return kPct[hl];
        };

        auto readRawInt = [&](int off, int* outV) -> bool {
            if (off <= 0 || !outV) return false;
            int v = 0;
            __try { v = Read<int>(entity + (uintptr_t)off); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            *outV = v;
            return true;
        };
        auto readRawFloat = [&](int off, float* outV) -> bool {
            if (off <= 0 || !outV) return false;
            float v = -1.f;
            __try { v = Read<float>(entity + (uintptr_t)off); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (!(v == v)) return false;
            *outV = v;
            return true;
        };

        // Fast path: cached offsets (cheap int/float reads only).
        bool gotHp = false, gotShock = false, gotBlood = false;
        if (s_HlOff > 0 && healthPct && *healthPct < 0.f)
        {
            int hl = -1;
            if (readRawInt(s_HlOff, &hl) && hl >= 0 && hl <= 4)
            {
                float p = levelToHpPct(hl);
                if (p >= 0.f) { *healthPct = p; gotHp = true; }
            }
        }
        if (shockPct && *shockPct < 0.f)
        {
            float cur = -1.f;
            if (s_ShockFOff > 0 && readRawFloat(s_ShockFOff, &cur) && cur >= 0.f)
            {
                float p = cur;
                if (p <= 1.05f) p *= 100.f;
                if (p > 100.f) p = 100.f;
                *shockPct = p;
                gotShock = true;
            }
            else if (s_ShockSOff > 0)
            {
                int ss = -1;
                if (readRawInt(s_ShockSOff, &ss) && ss >= 0 && ss <= 63)
                {
                    *shockPct = (ss / 63.f) * 100.f;
                    gotShock = true;
                }
            }
        }
        if (bloodPct && *bloodPct < 0.f && s_BleedOff > 0)
        {
            int bits = 0;
            if (readRawInt(s_BleedOff, &bits) && bits != 0)
            {
                *bloodPct = 70.f; // bleeding, volume unknown
                gotBlood = true;
            }
        }

        const bool needMore =
            (healthPct && *healthPct < 0.f && !gotHp) ||
            (shockPct && *shockPct < 0.f && !gotShock) ||
            (bloodPct && *bloodPct < 0.f && !gotBlood);

        // GetClassVar shape is still being proven via liveqa[netsync] A/B/C/D/E.
        // Until a non-AV form is confirmed, do NOT call it from the ESP Present path.
        static const bool kOakAllowGetClassVarHot = false;
        if (needMore && kOakAllowGetClassVarHot)
        {
            // Per-entity throttle: at most one GetClassVar burst / 450ms.
            NetCache* slot = nullptr;
            for (int i = 0; i < s_NetN; i++)
            {
                if (s_Net[i].ent == entity) { slot = &s_Net[i]; break; }
            }
            bool useCached = slot && slot->valid && (now - slot->tick) < 450u;
            if (!useCached)
            {
                typedef int(__fastcall* Fn_GetClassVar)(void* inst, void* varname, int index, void* result);
                const uintptr_t getVarAddr = (uintptr_t)g_GameModule + 0x2D7E40;
                auto getVar = OakNativeEntryOk(getVarAddr, "Script::GetClassVar")
                    ? (Fn_GetClassVar)getVarAddr : (Fn_GetClassVar)nullptr;
                auto callInt = [&](const char* name, int* outV) -> bool {
                    if (!name || !outV || !getVar) return false;
                    // Enforce string: native does mov rcx,[rdx] — pass pointer-to-cstr.
                    const char* namePtr = name;
                    int tmp = 0;
                    __try { getVar((void*)entity, (void*)&namePtr, 0, &tmp); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
                    *outV = tmp;
                    return true;
                };
                auto callFloat = [&](const char* name, float* outV) -> bool {
                    if (!name || !outV || !getVar) return false;
                    const char* namePtr = name;
                    float tmp = -1.f;
                    __try { getVar((void*)entity, (void*)&namePtr, 0, &tmp); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
                    if (!(tmp == tmp)) return false;
                    *outV = tmp;
                    return true;
                };

                int hl = -1, shockS = -1, bleed = 0;
                float curShock = -1.f;
                bool any = false;
                if (healthPct && *healthPct < 0.f)
                    any |= callInt("m_HealthLevel", &hl);
                if (shockPct && *shockPct < 0.f)
                {
                    any |= callFloat("m_CurrentShock", &curShock);
                    if (curShock < 0.f)
                        any |= callInt("m_ShockSimplified", &shockS);
                }
                if (bloodPct && *bloodPct < 0.f)
                    any |= callInt("m_BleedingBits", &bleed);

                if (!slot)
                {
                    if (s_NetN < 48) slot = &s_Net[s_NetN++];
                    else slot = &s_Net[0]; // overwrite oldest-ish
                    slot->ent = entity;
                }
                slot->tick = now;
                slot->hl = hl;
                slot->shockS = shockS;
                slot->bleed = bleed;
                slot->curShock = curShock;
                slot->valid = any;

                // One-shot raw offset discovery from a known value (avoids future GetClassVar).
                if (s_HlOff < 0 && hl >= 0 && hl <= 4)
                {
                    int hits[8]; int nh = 0;
                    for (uintptr_t off = 0x200; off < 0x1800 && nh < 8; off += 4)
                    {
                        int v = 0;
                        __try { v = Read<int>(entity + off); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (v == hl) hits[nh++] = (int)off;
                    }
                    if (nh == 1)
                        s_HlOff = hits[0];
                    else if (nh == 0)
                        s_HlOff = 0; // give up scanning; keep throttled GetClassVar
                    // nh>1: wait for a different hl sample next time
                }
                else if (s_HlOff < 0 && hl >= 0 && hl <= 4)
                {
                    // refine multi-candidates when value changes
                }
                if (s_ShockFOff < 0 && curShock >= 0.f && curShock <= 100.f)
                {
                    int hits[8]; int nh = 0;
                    for (uintptr_t off = 0x200; off < 0x1800 && nh < 8; off += 4)
                    {
                        float v = -1.f;
                        __try { v = Read<float>(entity + off); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (v == v && fabsf(v - curShock) < 0.01f) hits[nh++] = (int)off;
                    }
                    if (nh == 1) s_ShockFOff = hits[0];
                    else if (nh == 0) s_ShockFOff = 0;
                }
                if (s_ShockSOff < 0 && shockS >= 0 && shockS <= 63)
                {
                    int hits[8]; int nh = 0;
                    for (uintptr_t off = 0x200; off < 0x1800 && nh < 8; off += 4)
                    {
                        int v = 0;
                        __try { v = Read<int>(entity + off); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (v == shockS) hits[nh++] = (int)off;
                    }
                    if (nh == 1) s_ShockSOff = hits[0];
                    else if (nh == 0) s_ShockSOff = 0;
                }
                if (s_BleedOff < 0 && bleed != 0)
                {
                    int hits[8]; int nh = 0;
                    for (uintptr_t off = 0x200; off < 0x1800 && nh < 8; off += 4)
                    {
                        int v = 0;
                        __try { v = Read<int>(entity + off); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                        if (v == bleed) hits[nh++] = (int)off;
                    }
                    if (nh == 1) s_BleedOff = hits[0];
                }
            }

            if (slot && slot->valid)
            {
                if (healthPct && *healthPct < 0.f && slot->hl >= 0 && slot->hl <= 4)
                    *healthPct = levelToHpPct(slot->hl);
                if (shockPct && *shockPct < 0.f)
                {
                    if (slot->curShock >= 0.f)
                    {
                        float p = slot->curShock;
                        if (p <= 1.05f) p *= 100.f;
                        if (p > 100.f) p = 100.f;
                        *shockPct = p;
                    }
                    else if (slot->shockS >= 0 && slot->shockS <= 63)
                        *shockPct = (slot->shockS / 63.f) * 100.f;
                }
                if (bloodPct && *bloodPct < 0.f && slot->bleed != 0)
                    *bloodPct = 70.f;
            }
        }
    }

    // PlayerStats — local only. Remote ESP used to clear/rediscover every frame → 7 FPS.
    if (!isInfected && isLocal && !skipPlayerStats)
    {
        // SL_ENERGY_MAX / SL_WATER_MAX = 5000 (legacy Energy max was 20000)
        auto asEnergyPct = [](float x) -> float {
            if (x != x || x < 0.f) return -1.f;
            if (x == 0.f) return 0.f;
            if (x > 0.f && x <= 1.05f) return -1.f; // normalized impostor
            float mx = (x > 5000.f) ? 20000.f : 5000.f;
            float p = (x / mx) * 100.f;
            if (p > 100.f) p = 100.f;
            return p;
        };

        auto applyNamedStat = [&](const char* sn, float v) {
            if (!sn || !sn[0] || v < 0.f) return;
            if ((StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Hunger") == 0) &&
                hungerPct && *hungerPct < 0.f)
            {
                float p = asEnergyPct(v);
                if (p >= 0.f) *hungerPct = p;
            }
                else if ((StrCmpI(sn, "Water") == 0 || StrCmpI(sn, "Thirst") == 0) &&
                    thirstPct && *thirstPct < 0.f)
                {
                    // Never paint 100% from exact SL_WATER_MAX — that was the 63→100 flicker
                    if (v >= 4999.f && v <= 5000.f) return;
                    float p = asEnergyPct(v);
                    if (p >= 0.f) *thirstPct = p;
                }
            else if (StrCmpI(sn, "Stamina") == 0 && stamPct && *stamPct < 0.f)
            {
                float s = v;
                if (s <= 1.05f) s *= 100.f;
                if (s >= 0.f && s <= 100.f) *stamPct = s;
            }
            else if (StrCmpI(sn, "Blood") == 0 && bloodPct && *bloodPct < 0.f)
            {
                float p = asEnergyPct(v);
                if (p >= 0.f) *bloodPct = p;
            }
            else if ((StrCmpI(sn, "Health") == 0 || StrCmpI(sn, "GlobalHealth") == 0) &&
                healthPct && *healthPct <= 0.f)
            {
                float h = v;
                if (h <= 1.05f) h *= 100.f;
                if (h > 0.f && h <= 100.f) *healthPct = h;
            }
            else if (StrCmpI(sn, "Shock") == 0 && shockPct && *shockPct < 0.f)
            {
                float s = v;
                if (s <= 1.05f) s *= 100.f;
                if (s >= 0.f && s <= 100.f) *shockPct = s;
            }
        };

        // Direct cached Energy/Water records (from reverse m_Player / float-pair hunt)
        if (g_CachedEnergyRec && IsHeapObj(g_CachedEnergyRec) && hungerPct && *hungerPct < 0.f)
        {
            float v = -1.f;
            ExtractStatValue(g_CachedEnergyRec, "Energy", &v, &g_CachedEnergyValOff);
            if (v < 0.f && g_CachedEnergyValOff)
                v = Read<float>(g_CachedEnergyRec + g_CachedEnergyValOff);
            if (v >= 0.f) applyNamedStat("Energy", v);
        }
        if (g_CachedWaterRec && IsHeapObj(g_CachedWaterRec) && thirstPct && *thirstPct < 0.f)
        {
            float v = -1.f;
            // Always re-extract — cached valOff previously pointed at max=5000 → fake 100% thirst
            ExtractStatValue(g_CachedWaterRec, "Water", &v, &g_CachedWaterValOff);
            if (v < 0.f && g_CachedWaterValOff)
            {
                float raw = Read<float>(g_CachedWaterRec + g_CachedWaterValOff);
                if (raw != 5000.f && raw != 20000.f)
                    v = raw;
            }
            if (v >= 0.f && v != 5000.f && v != 20000.f)
                applyNamedStat("Water", v);
            else if (v == 0.f)
                applyNamedStat("Water", 0.f);
        }

        uintptr_t stats = DiscoverPlayerStats(entity);
        if (!IsValidPtr(stats) || stats < 0x100000000)
        {
            const uintptr_t tryStats[] = {
                oak_offsets::player::StatsContainer, 0x6E8, 0x6E0, 0x700, 0x720, 0x740, 0x680, 0x660
            };
            for (int ti = 0; ti < 8 && !IsValidPtr(stats); ti++)
            {
                uintptr_t s = Read<uintptr_t>(entity + tryStats[ti]);
                char sn[48] = {}; float v = -1.f;
                if (IsValidPtr(s) && WalkStatsRootForEnergy(s, 0, sn, &v))
                {
                    stats = s;
                    g_CachedStatsOff = tryStats[ti];
                }
            }
        }

        // Re-apply after discover (may have just filled caches)
        if (g_CachedEnergyRec && IsHeapObj(g_CachedEnergyRec) && hungerPct && *hungerPct < 0.f)
        {
            float v = -1.f;
            ExtractStatValue(g_CachedEnergyRec, "Energy", &v, &g_CachedEnergyValOff);
            if (v >= 0.f) applyNamedStat("Energy", v);
        }
        if (g_CachedWaterRec && IsHeapObj(g_CachedWaterRec) && thirstPct && *thirstPct < 0.f)
        {
            float v = -1.f;
            ExtractStatValue(g_CachedWaterRec, "Water", &v, &g_CachedWaterValOff);
            if (v >= 0.f && (v < 4999.f || v == 0.f))
                applyNamedStat("Water", v);
        }

        if (IsValidPtr(stats) && stats > 0x100000000)
        {
            auto readRecValue = [&](uintptr_t rec) -> float {
                float vTrip = -1.f; uintptr_t vo = 0;
                if (ExtractStatValue(rec, "Water", &vTrip, &vo) && vTrip >= 0.f && vTrip < 5000.f)
                    return vTrip;
                if (ExtractStatValue(rec, "Energy", &vTrip, &vo) && vTrip >= 0.f && vTrip < 5000.f)
                    return vTrip;
                uintptr_t prefer[] = {
                    g_CachedStatValueOff ? g_CachedStatValueOff : 0x2C,
                    0x2C, 0x28, 0x30, 0x24, 0x20, 0x34, 0x18, 0x1C, 0x38, 0x14, 0x10
                };
                float bestAbs = -1.f;
                bool haveZero = false;
                for (int vi = 0; vi < 12; vi++)
                {
                    if (!prefer[vi]) continue;
                    float t = Read<float>(rec + prefer[vi]);
                    if (t != t || t < 0.f || t > 20000.f) continue;
                    if (t == 0.f) { haveZero = true; continue; }
                    if (t > 1.05f && t != 5000.f && t != 20000.f && t != 100.f)
                        return t;
                    // Skip storing exact max as bestAbs — that painted thirst at 100%
                }
                if (haveZero) return 0.f;
                return -1.f;
            };

            static int s_StatDump = 0;
            auto walkStatArray = [&](uintptr_t root) {
                if (!IsValidPtr(root) || root < 0x100000000) return;
                const uintptr_t bases[] = { 0x20, 0x18, 0x10, 0x8, 0x28, 0x30, 0x0, 0x38, 0x40, 0x48 };
                for (int bi = 0; bi < 10; bi++)
                {
                    uintptr_t arr = Read<uintptr_t>(root + bases[bi]);
                    if (!IsHeapObj(arr)) continue;
                    uintptr_t tables[3] = { arr, Read<uintptr_t>(arr + 0x0), Read<uintptr_t>(arr + 0x8) };
                    for (int ti = 0; ti < 3; ti++)
                    {
                        uintptr_t tab = tables[ti];
                        if (!IsHeapObj(tab)) continue;
                        for (int i = 0; i < 24; i++)
                        {
                            uintptr_t rec = Read<uintptr_t>(tab + (uintptr_t)i * 8);
                            if (!IsHeapObj(rec)) continue;
                            char sn[48] = {};
                            if (!ReadRecStatLabel(rec, sn, 48)) continue;
                            float v = -1.f;
                            // Energy/Water: ExtractStatValue first (avoids max=5000 impostor from readRecValue)
                            if (StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0)
                            {
                                ExtractStatValue(rec, sn, &v, nullptr);
                                if (v < 0.f) v = readRecValue(rec);
                                if (v == 5000.f || v == 20000.f)
                                {
                                    // Reject pure max unless no other float looks like a current
                                    float alt = -1.f;
                                    ExtractStatValue(rec, sn, &alt, nullptr);
                                    if (alt >= 0.f && alt < v) v = alt;
                                }
                            }
                            else
                            {
                                v = readRecValue(rec);
                                if (v < 0.f)
                                    ExtractStatValue(rec, sn, &v, nullptr);
                            }
                            if (v < 0.f && (StrCmpI(sn, "Energy") == 0 || StrCmpI(sn, "Water") == 0))
                                v = 0.f;
                            if (s_StatDump < 16)
                            {
                                char db[96];
                                wsprintfA(db, "vitals: slot[%d] '%s'=%d", i, sn, (int)v);
                                Log(db);
                                s_StatDump++;
                            }
                            applyNamedStat(sn, v);
                            if (StrCmpI(sn, "Energy") == 0)
                            {
                                g_CachedEnergyRec = rec;
                                ExtractStatValue(rec, sn, &v, &g_CachedEnergyValOff);
                            }
                            if (StrCmpI(sn, "Water") == 0)
                            {
                                g_CachedWaterRec = rec;
                                ExtractStatValue(rec, sn, &v, &g_CachedWaterValOff);
                            }
                        }
                    }
                }
            };

            // Also try fixed EPlayerStats indices on arrays (ENERGY=3 WATER=4) if labels fail
            auto walkByIndex = [&](uintptr_t root) {
                if (!IsHeapObj(root)) return;
                if (hungerPct && *hungerPct >= 0.f && thirstPct && *thirstPct >= 0.f) return;
                const uintptr_t bases[] = { 0x20, 0x18, 0x10, 0x8, 0x0, 0x28 };
                for (int bi = 0; bi < 6; bi++)
                {
                    uintptr_t arr = Read<uintptr_t>(root + bases[bi]);
                    if (!IsHeapObj(arr)) continue;
                    uintptr_t tab = arr;
                    uintptr_t t1 = Read<uintptr_t>(arr + 0x0);
                    uintptr_t t2 = Read<uintptr_t>(arr + 0x8);
                    uintptr_t cands[3] = { tab, t1, t2 };
                    for (int ci = 0; ci < 3; ci++)
                    {
                        uintptr_t t = cands[ci];
                        if (!IsHeapObj(t)) continue;
                        // ENERGY=3, WATER=4 in EPlayerStats_v115
                        uintptr_t eRec = Read<uintptr_t>(t + 3 * 8);
                        uintptr_t wRec = Read<uintptr_t>(t + 4 * 8);
                        float ev = -1.f, wv = -1.f;
                        if (IsHeapObj(eRec))
                        {
                            ExtractStatValue(eRec, "Energy", &ev, &g_CachedEnergyValOff);
                            if (ev < 0.f) ev = readRecValue(eRec);
                            if (ev >= 0.f && hungerPct && *hungerPct < 0.f)
                            {
                                applyNamedStat("Energy", ev);
                                g_CachedEnergyRec = eRec;
                                if (s_StatDump < 20)
                                {
                                    char db[80]; wsprintfA(db, "vitals: idx3 Energy=%d", (int)ev); Log(db); s_StatDump++;
                                }
                            }
                        }
                        if (IsHeapObj(wRec))
                        {
                            ExtractStatValue(wRec, "Water", &wv, &g_CachedWaterValOff);
                            if (wv < 0.f) wv = readRecValue(wRec);
                            if (wv >= 0.f && thirstPct && *thirstPct < 0.f)
                            {
                                applyNamedStat("Water", wv);
                                g_CachedWaterRec = wRec;
                            }
                        }
                    }
                }
            };

            walkStatArray(stats);
            walkByIndex(stats);
            {
                char sn[48] = {};
                float vv = -1.f;
                if (StatRecLooksLikeEnergyWater(stats, sn, 48, &vv))
                    applyNamedStat(sn, vv >= 0.f ? vv : 0.f);
                for (uintptr_t so = 0x0; so <= 0x80; so += 8)
                {
                    uintptr_t sib = Read<uintptr_t>(stats + so);
                    if (StatRecLooksLikeEnergyWater(sib, sn, 48, &vv))
                        applyNamedStat(sn, vv >= 0.f ? vv : 0.f);
                    walkStatArray(sib);
                    walkByIndex(sib);
                }
            }
            if ((hungerPct && *hungerPct < 0.f) || (thirstPct && *thirstPct < 0.f))
            {
                for (uintptr_t po = 0x0; po <= 0x40; po += 8)
                {
                    uintptr_t pco = Read<uintptr_t>(stats + po);
                    if (!IsHeapObj(pco) || pco == stats) continue;
                    walkStatArray(pco);
                    walkByIndex(pco);
                    char sn2[48] = {}; float vv2 = -1.f;
                    if (StatRecLooksLikeEnergyWater(pco, sn2, 48, &vv2))
                        applyNamedStat(sn2, vv2 >= 0.f ? vv2 : 0.f);
                    if (hungerPct && *hungerPct >= 0.f && thirstPct && *thirstPct >= 0.f)
                        break;
                }
            }
        }

        // Stamina fallback: Entity::Stamina @ 0x6A4 — ignore exact 0 (uninitialized / wrong field)
        if (stamPct && *stamPct < 0.f)
        {
            float st = Read<float>(entity + oak_offsets::entity::Stamina);
            if (st == st && st > 0.f && st <= 100.f)
            {
                if (st <= 1.05f) st *= 100.f;
                if (st > 0.f) *stamPct = st;
            }
        }

        // Last resort float-pair scan DISABLED — Present hitch source
        if (false && ((hungerPct && *hungerPct < 0.f) || (thirstPct && *thirstPct < 0.f)))
        {
            int found = 0;
            float vals[4] = { -1.f, -1.f, -1.f, -1.f };
            for (uintptr_t off = 0x280; off <= 0x900 && found < 4; off += 4)
            {
                float cur = Read<float>(entity + off);
                float mx = Read<float>(entity + off + 4);
                if (mx != 5000.f && mx != 20000.f) continue;
                if (cur != cur || cur < 0.f || cur > mx) continue;
                // Skip if next looks like transform junk
                vals[found++] = cur;
                char b[96];
                wsprintfA(b, "vitals: float-pair @+0x%X cur=%d max=%d",
                    (unsigned)off, (int)cur, (int)mx);
                Log(b);
            }
            // Heuristic: first pair Energy, second Water (PCO register order)
            if (found >= 1 && hungerPct && *hungerPct < 0.f)
            {
                float p = (vals[0] / 5000.f) * 100.f;
                if (p > 100.f) p = 100.f;
                *hungerPct = p;
            }
            if (found >= 2 && thirstPct && *thirstPct < 0.f)
            {
                float p = (vals[1] / 5000.f) * 100.f;
                if (p > 100.f) p = 100.f;
                *thirstPct = p;
            }
        }
    }

    if (hungerPct && *hungerPct > 100.f) *hungerPct = 100.f;
    if (thirstPct && *thirstPct > 100.f) *thirstPct = 100.f;
    if (isInfected)
    {
        if (hungerPct) *hungerPct = -1.f;
        if (thirstPct) *thirstPct = -1.f;
        if (stamPct) *stamPct = -1.f;
    }

    // No +0x318 "all 1.0" fallback — that painted every bar at 100% while Energy was empty.

    // Treat exact 0 / sub-1% HP as "unknown" so we don't draw empty bars from junk probes
    if (healthPct && *healthPct >= 0.f && *healthPct < 1.f) *healthPct = -1.f;

    bool any = (healthPct && *healthPct > 0.f) || (stamPct && *stamPct >= 0.f) ||
               (hungerPct && *hungerPct >= 0.f) || (thirstPct && *thirstPct >= 0.f) ||
               (bloodPct && *bloodPct >= 0.f);
    return any;
}

static void DrawEspBars(float boxX, float boxY, float boxH,
    float health, float blood, float shock, float stam, float hunger, float thirst)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    if (boxH < 28.f) boxH = 28.f; // keep bars readable when skeleton box is tiny
    float barW = 5.f;
    float gap = 2.5f;
    float x = boxX - gap - barW;
    auto drawOne = [&](bool enabled, float pct, ImU32 fillHi, ImU32 fillLo) {
        if (!enabled || pct < 0.f) return;
        if (pct > 100.f) pct = 100.f;
        float h = boxH * (pct / 100.f);
        if (h < 2.f && pct > 0.f) h = 2.f;
        float y0 = boxY + boxH - h;
        dl->AddRectFilled(ImVec2(x - 1.f, boxY - 1.f), ImVec2(x + barW + 1.f, boxY + boxH + 1.f), IM_COL32(0, 0, 0, 220), 2.f);
        dl->AddRectFilled(ImVec2(x, boxY), ImVec2(x + barW, boxY + boxH), IM_COL32(18, 20, 24, 230), 2.f);
        if (pct > 0.f)
            dl->AddRectFilledMultiColor(ImVec2(x, y0), ImVec2(x + barW, boxY + boxH), fillHi, fillHi, fillLo, fillLo);
        x -= (barW + gap);
    };
    ImU32 hpHi = health >= 50.f ? IM_COL32(40, 255, 120, 255) : IM_COL32(255, 210, 40, 255);
    if (health >= 0.f && health < 25.f) hpHi = IM_COL32(255, 60, 50, 255);
    drawOne(g_BarHealth, health, hpHi, IM_COL32(20, 120, 60, 255));
    drawOne(g_BarBlood, blood, IM_COL32(255, 55, 70, 255), IM_COL32(120, 15, 25, 255));
    drawOne(g_BarShock, OakStatusAmt(shock), IM_COL32(80, 190, 255, 255), IM_COL32(20, 70, 140, 255));
    drawOne(g_BarStamina, stam, IM_COL32(255, 230, 80, 255), IM_COL32(140, 110, 20, 255));
    drawOne(g_BarHunger, hunger, IM_COL32(255, 160, 60, 255), IM_COL32(140, 70, 10, 255));
    drawOne(g_BarThirst, thirst, IM_COL32(70, 200, 255, 255), IM_COL32(20, 90, 140, 255));
    g_EspDrawCount++;
}

// Screen-space vitals for local player (always readable, not tied to tiny skeleton box)
static void DrawLocalVitalsHud(uintptr_t localPlayer)
{
    if (!IsValidPtr(localPlayer)) return;
    if (!g_LocalVitalsHud && !g_LocalWeaponAmmo) return;
    // Short warm-up only — long delay made vitals look "missing"
    static DWORD s_HudWarm = 0;
    if (!s_HudWarm) s_HudWarm = GetTickCount();
    if ((GetTickCount() - s_HudWarm) < 500)
        return;

    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float x = 18.f;
    float y = (float)g_ScreenHeight * 0.32f;
    float barW = 120.f;
    float barH = 10.f;
    float gap = 16.f;

    // Throttle heavy ReadVitals (150ms); ammo more often so fire updates feel live
    static DWORD s_VitTick = 0, s_AmmoTick = 0;
    static float s_Hp = -1.f, s_Blood = -1.f, s_Shock = -1.f, s_Stam = -1.f, s_Hung = -1.f, s_Thir = -1.f;
    static char s_Wpn[64] = {};
    static int s_Ammo = -1;
    static bool s_WpnOk = false;
    DWORD now = GetTickCount();
    if (g_LocalVitalsHud && (now - s_VitTick) > 150)
    {
        s_VitTick = now;
        __try {
            ReadVitals(localPlayer, &s_Hp, &s_Blood, &s_Shock, &s_Stam, &s_Hung, &s_Thir);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_LocalWeaponAmmo && (now - s_AmmoTick) > 50)
    {
        s_AmmoTick = now;
        s_Wpn[0] = 0; s_Ammo = -1; s_WpnOk = false;
        __try { s_WpnOk = GetHandWeaponInfo(localPlayer, s_Wpn, 64, &s_Ammo); }
        __except (EXCEPTION_EXECUTE_HANDLER) { s_WpnOk = false; }
    }

    if (g_LocalVitalsHud)
    {
        auto drawRow = [&](bool enabled, const char* label, float pct, ImU32 col) {
            if (!enabled) return;
            char t[64];
            if (pct < 0.f)
                wsprintfA(t, "%s --", label);
            else
            {
                if (pct > 100.f) pct = 100.f;
                wsprintfA(t, "%s %d%%", label, (int)(pct + 0.5f));
            }
            dl->AddText(ImVec2(x, y - 14.f), IM_COL32(255, 255, 255, 230), t);
            dl->AddRectFilled(ImVec2(x - 1.f, y - 1.f), ImVec2(x + barW + 1.f, y + barH + 1.f), IM_COL32(0, 0, 0, 200));
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + barW, y + barH), IM_COL32(20, 22, 26, 220));
            if (pct >= 0.f)
            {
                float fill = barW * (pct / 100.f);
                if (fill < 3.f && pct > 0.f) fill = 3.f;
                if (fill > 0.f)
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + fill, y + barH), col);
            }
            y += gap;
        };

        __try
        {
            drawRow(g_BarHealth, "HP", s_Hp, IM_COL32(50, 220, 100, 255));
            drawRow(g_BarBlood, "Blood", s_Blood, IM_COL32(255, 60, 70, 255));
            drawRow(g_BarShock, "Shock", OakStatusAmt(s_Shock), IM_COL32(80, 180, 255, 255));
            drawRow(g_BarStamina, "Stam", s_Stam, IM_COL32(255, 220, 70, 255));
            drawRow(g_BarHunger, "Food", s_Hung, IM_COL32(255, 160, 50, 255));
            drawRow(g_BarThirst, "Water", s_Thir, IM_COL32(60, 200, 255, 255));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    if (g_LocalWeaponAmmo && s_WpnOk)
    {
        char line[96];
        if (s_Ammo >= 0)
            wsprintfA(line, "%s  [%d]", s_Wpn, s_Ammo);
        else
            wsprintfA(line, "%s", s_Wpn[0] ? s_Wpn : "Weapon");
        dl->AddText(ImVec2(x, y), IM_COL32(255, 220, 80, 240), line);
    }
}

static void DrawWorldMarker(uintptr_t entity, const char* label, int distance, const float* col, float boxSize)
{
    Vec3 pos;
    if (!GetEntityPosition(entity, pos)) return;
    Vec2 scr;
    if (!WorldToScreen(pos, scr)) return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float s = boxSize * 0.85f;
    if (s < 7.f) s = 7.f;
    ImU32 fill = ToCol(col[0], col[1], col[2], 0.55f);
    ImU32 edge = ToCol(col[0], col[1], col[2], 1.f);
    ImU32 glow = ToCol(col[0], col[1], col[2], 0.25f);
    ImVec2 c(scr.x, scr.y);
    // soft glow
    dl->AddCircleFilled(c, s + 4.f, glow, 20);
    // diamond
    ImVec2 diamond[4] = {
        ImVec2(c.x, c.y - s),
        ImVec2(c.x + s, c.y),
        ImVec2(c.x, c.y + s),
        ImVec2(c.x - s, c.y),
    };
    dl->AddConvexPolyFilled(diamond, 4, fill);
    dl->AddPolyline(diamond, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 3.0f);
    dl->AddPolyline(diamond, 4, edge, ImDrawFlags_Closed, 1.8f);
    dl->AddCircleFilled(c, 2.2f, edge, 10);

    char line[96];
    if (label && label[0] && distance >= 0)
        wsprintfA(line, "%s  %dm", label, distance);
    else if (label && label[0])
        wsprintfA(line, "%s", label);
    else if (distance >= 0)
        wsprintfA(line, "%dm", distance);
    else
        return;
    DrawEspBadge(scr.x, scr.y + s + 6.f, line, col[0], col[1], col[2], col[3]);
    g_EspDrawCount++;
}

static uintptr_t GetWorldPtrSafe()
{
    if (!g_GameModule) return 0;
    uintptr_t w = Read<uintptr_t>((uintptr_t)g_GameModule + offsets::modbase::World);
    if (!IsValidPtr(w) || w < 0x100000000) return 0;
    uintptr_t cam = Read<uintptr_t>(w + offsets::world::Camera);
    if (!IsValidPtr(cam) || cam < 0x100000000) return 0;
    return w;
}

static bool ResolveBulletArray(uintptr_t worldPtr, uintptr_t& outData, int& outCount, bool& pointerArray)
{
    outData = 0;
    outCount = 0;
    pointerArray = true;

    auto validBullet = [&](uintptr_t bullet) -> bool {
        if (!IsValidPtr(bullet) || bullet < 0x100000000) return false;
        Vec3 p;
        return GetEntityPosition(bullet, p);
    };

    // Layout A: World+BulletList = pointer table, World+BulletCount = count
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::BulletList);
        int count = Read<int>(worldPtr + oak_offsets::world::BulletCount);
        if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 256)
        {
            int good = 0;
            int sample = count < 8 ? count : 8;
            for (int i = 0; i < sample; i++)
            {
                uintptr_t b = Read<uintptr_t>(data + (uintptr_t)i * 8);
                if (validBullet(b)) good++;
            }
            if (good > 0)
            {
                outData = data;
                outCount = count;
                pointerArray = true;
                return true;
            }
        }
    }

    // Layout B: list object { data*, count }
    {
        uintptr_t list = Read<uintptr_t>(worldPtr + oak_offsets::world::BulletList);
        if (IsValidPtr(list) && list > 0x100000000)
        {
            uintptr_t data = Read<uintptr_t>(list);
            int count = Read<int>(list + 8);
            if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 256)
            {
                int good = 0;
                int sample = count < 8 ? count : 8;
                for (int i = 0; i < sample; i++)
                {
                    uintptr_t b = Read<uintptr_t>(data + (uintptr_t)i * 8);
                    if (validBullet(b)) good++;
                }
                if (good > 0)
                {
                    outData = data;
                    outCount = count;
                    pointerArray = true;
                    return true;
                }

                // Contiguous 0x100 records
                good = 0;
                for (int i = 0; i < sample; i++)
                {
                    uintptr_t b = data + (uintptr_t)i * 0x100;
                    if (validBullet(b)) good++;
                }
                if (good > 0)
                {
                    outData = data;
                    outCount = count;
                    pointerArray = false;
                    return true;
                }
            }
        }
    }
    return false;
}

static BulletTrack* FindTrack(uintptr_t ptr)
{
    // Live tracks only — finished trails keep fading under their own slot.
    // Matching finished slots reused the trail and glued multi-shot tracers together.
    for (int i = 0; i < kMaxBulletTracks; i++)
        if (g_BulletTracks[i].active && !g_BulletTracks[i].finished && g_BulletTracks[i].ptr == ptr)
            return &g_BulletTracks[i];
    return nullptr;
}

static BulletTrack* AllocTrack(uintptr_t ptr)
{
    for (int i = 0; i < kMaxBulletTracks; i++)
    {
        if (!g_BulletTracks[i].active)
        {
            BulletTrack& t = g_BulletTracks[i];
            ZeroMem(&t, sizeof(t));
            t.ptr = ptr;
            t.active = true;
            return &t;
        }
    }
    // Recycle oldest finished fade first; if none, oldest live by lastSeen
    int bestFin = -1;
    DWORD bestFade = 0xFFFFFFFF;
    int bestLive = -1;
    DWORD bestSeen = 0xFFFFFFFF;
    for (int i = 0; i < kMaxBulletTracks; i++)
    {
        BulletTrack& t = g_BulletTracks[i];
        if (!t.active) continue;
        if (t.finished)
        {
            if (t.fadeUntil < bestFade) { bestFade = t.fadeUntil; bestFin = i; }
        }
        else if (t.lastSeen < bestSeen)
        {
            bestSeen = t.lastSeen;
            bestLive = i;
        }
    }
    int best = (bestFin >= 0) ? bestFin : bestLive;
    if (best < 0) return nullptr;
    BulletTrack& t = g_BulletTracks[best];
    ZeroMem(&t, sizeof(t));
    t.ptr = ptr;
    t.active = true;
    return &t;
}

static void AddImpact(const Vec3& pos)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxImpacts; i++)
    {
        if (!g_Impacts[i].used)
        {
            g_Impacts[i].used = true;
            g_Impacts[i].pos = pos;
            g_Impacts[i].expire = now + (DWORD)g_ImpactLifetimeMs;
            return;
        }
    }
    // overwrite oldest
    int best = 0;
    for (int i = 1; i < kMaxImpacts; i++)
        if (g_Impacts[i].expire < g_Impacts[best].expire) best = i;
    g_Impacts[best].used = true;
    g_Impacts[best].pos = pos;
    g_Impacts[best].expire = now + (DWORD)g_ImpactLifetimeMs;
}

static void AddShotIndicator(float dirX, float dirZ)
{
    float len = sqrtf(dirX * dirX + dirZ * dirZ);
    if (len < 0.01f) return;
    dirX /= len; dirZ /= len;
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxShotInd; i++)
    {
        if (!g_ShotInds[i].used)
        {
            g_ShotInds[i].used = true;
            g_ShotInds[i].dirX = dirX;
            g_ShotInds[i].dirZ = dirZ;
            g_ShotInds[i].expire = now + 900;
            return;
        }
    }
    // overwrite oldest
    int best = 0;
    for (int i = 1; i < kMaxShotInd; i++)
        if (g_ShotInds[i].expire < g_ShotInds[best].expire) best = i;
    g_ShotInds[best].used = true;
    g_ShotInds[best].dirX = dirX;
    g_ShotInds[best].dirZ = dirZ;
    g_ShotInds[best].expire = now + 900;
}

static bool NearAnyHitEntity(const Vec3& pos, float radius)
{
    for (int i = 0; i < g_HitCheckCount; i++)
    {
        Vec3 ep;
        if (!GetEntityPosition(g_HitCheckEnts[i], ep)) continue;
        // Use torso height bias — origin is at feet
        ep.y += 1.0f;
        if (Distance3D(pos, ep) <= radius) return true;
    }
    return false;
}

static bool SegmentHitsEntity(const Vec3& a, const Vec3& b, float radius)
{
    for (int i = 0; i < g_HitCheckCount; i++)
    {
        Vec3 ep;
        if (!GetEntityPosition(g_HitCheckEnts[i], ep)) continue;
        ep.y += 1.0f;
        // Closest point on segment to entity torso
        Vec3 ab{ b.x - a.x, b.y - a.y, b.z - a.z };
        float abLen2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
        float t = 0.f;
        if (abLen2 > 0.0001f)
        {
            t = ((ep.x - a.x) * ab.x + (ep.y - a.y) * ab.y + (ep.z - a.z) * ab.z) / abLen2;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
        }
        Vec3 closest{ a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
        if (Distance3D(closest, ep) <= radius) return true;
    }
    return false;
}

static void DrawGrenadeArc(const Vec3& start, const Vec3& vel, float alpha = 1.f)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || alpha <= 0.02f) return;
    if (alpha > 1.f) alpha = 1.f;

    float spd0 = sqrtf(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
    if (spd0 < 0.8f) return;

    ImU32 col = ToCol(g_ColorGrenade[0], g_ColorGrenade[1], g_ColorGrenade[2], g_ColorGrenade[3] * alpha);
    ImU32 glow = ToCol(g_ColorGrenade[0], g_ColorGrenade[1], g_ColorGrenade[2], 0.30f * alpha);
    Vec3 p = start;
    Vec3 v = vel;
    const float dt = 0.04f;
    const float grav = 9.81f;
    // Approximate flat ground at throw height — stop when falling back to ground plane
    // so the preview does not tunnel through the world (no terrain ray available).
    const float groundY = start.y - 0.15f;
    Vec2 prev{};
    bool hasPrev = false;
    Vec2 landScr{};
    bool hasLand = false;
    for (int i = 0; i < 100; i++)
    {
        Vec3 prevP = p;
        v.y -= grav * dt;
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;

        bool hitGround = (i > 2 && v.y < 0.f && p.y <= groundY && prevP.y > groundY);
        if (hitGround)
        {
            float dy = prevP.y - p.y;
            float t = (dy > 0.001f) ? ((prevP.y - groundY) / dy) : 1.f;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            p.x = prevP.x + (p.x - prevP.x) * t;
            p.y = groundY;
            p.z = prevP.z + (p.z - prevP.z) * t;
        }

        Vec2 scr;
        if (WorldToScreen(p, scr))
        {
            if (hasPrev)
            {
                dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(scr.x, scr.y), glow, 4.5f);
                dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(scr.x, scr.y), col, 2.2f);
            }
            prev = scr;
            hasPrev = true;
            if (hitGround)
            {
                landScr = scr;
                hasLand = true;
            }
        }
        else
            hasPrev = false;

        if (hitGround) break;
        if (p.y < groundY - 2.f) break;
    }
    if (hasLand)
    {
        dl->AddCircle(ImVec2(landScr.x, landScr.y), 7.f, col, 16, 2.0f);
        dl->AddCircleFilled(ImVec2(landScr.x, landScr.y), 3.0f, col, 10);
        static int s_Nade = 0;
        if ((++s_Nade % 30) == 1)
            Log("verify[grenade] landing mark (ground stop)");
    }
    else if (hasPrev)
        dl->AddCircleFilled(ImVec2(prev.x, prev.y), 3.5f, col, 10);
    g_EspDrawCount++;
}

static void DrawTrailPath(const BulletTrack& t, float alpha)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || t.trailCount < 2 || alpha <= 0.02f) return;
    if (alpha > 1.f) alpha = 1.f;

    ImU32 col, glow;
    float thickCore, thickGlow;
    if (t.isGrenade)
    {
        col = ToCol(g_ColorGrenade[0], g_ColorGrenade[1], g_ColorGrenade[2], g_ColorGrenade[3] * alpha);
        glow = ToCol(g_ColorGrenade[0], g_ColorGrenade[1], g_ColorGrenade[2], 0.35f * alpha);
        thickCore = 2.6f; thickGlow = 5.0f;
    }
    else if (t.fromLocal)
    {
        col = ToCol(g_ColorTracer[0], g_ColorTracer[1], g_ColorTracer[2], g_ColorTracer[3] * alpha);
        glow = ToCol(g_ColorTracer[0], g_ColorTracer[1], g_ColorTracer[2], 0.32f * alpha);
        thickCore = 2.2f; thickGlow = 4.2f;
    }
    else
    {
        col = ToCol(1.f, 0.35f, 0.18f, 0.90f * alpha);
        glow = ToCol(1.f, 0.22f, 0.10f, 0.32f * alpha);
        thickCore = 2.0f; thickGlow = 4.0f;
    }

    Vec2 prevScr{};
    bool hasPrev = false;
    for (int p = 0; p < t.trailCount; p++)
    {
        Vec2 scr;
        if (!WorldToScreen(t.trail[p], scr)) { hasPrev = false; continue; }
        if (hasPrev)
        {
            dl->AddLine(ImVec2(prevScr.x, prevScr.y), ImVec2(scr.x, scr.y), glow, thickGlow);
            dl->AddLine(ImVec2(prevScr.x, prevScr.y), ImVec2(scr.x, scr.y), col, thickCore);
        }
        prevScr = scr;
        hasPrev = true;
    }
    if (hasPrev)
    {
        // Start marker + tip
        Vec2 startScr;
        if (WorldToScreen(t.firstPos, startScr))
            dl->AddCircleFilled(ImVec2(startScr.x, startScr.y), 2.8f, col, 8);
        dl->AddCircleFilled(ImVec2(prevScr.x, prevScr.y), 3.0f, col, 10);
    }
    g_EspDrawCount++;
}

static void UpdateAndDrawCombatVisuals(uintptr_t worldPtr, uintptr_t localPlayer)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    DWORD now = GetTickCount();

    // Crosshair styles: 0 cross, 1 gap-cross, 2 dot, 3 circle, 4 T
    if (g_Crosshair && dl)
    {
        float cx = g_ScreenWidth * 0.5f;
        float cy = g_ScreenHeight * 0.5f;
        const float* cc = g_ColorCrosshair;
        if (g_CrosshairHighlight.enabled)
        {
            cc = g_CrosshairOnEnemy ? g_CrosshairHighlight.colorHighlight
                : g_CrosshairHighlight.colorDefault;
        }
        ImU32 col = ToCol(cc[0], cc[1], cc[2], cc[3]);
        ImU32 outline = IM_COL32(0, 0, 0, 220);
        float arm = (float)(g_CrosshairSize > 0 ? g_CrosshairSize : 8);
        float gap = (float)(g_CrosshairGap >= 0 ? g_CrosshairGap : 4);
        float t = (float)(g_CrosshairThickness > 0 ? g_CrosshairThickness : 2);
        int style = g_CrosshairStyle;
        if (style < 0) style = 0;
        if (style > 4) style = 1;
        auto seg = [&](float x1, float y1, float x2, float y2) {
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), outline, t + 2.f);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, t);
        };
        if (style == 2)
        {
            dl->AddCircleFilled(ImVec2(cx, cy), arm * 0.35f + 1.5f, outline, 12);
            dl->AddCircleFilled(ImVec2(cx, cy), arm * 0.28f + 1.0f, col, 12);
        }
        else if (style == 3)
        {
            dl->AddCircle(ImVec2(cx, cy), arm, outline, 24, t + 2.f);
            dl->AddCircle(ImVec2(cx, cy), arm, col, 24, t);
            dl->AddCircleFilled(ImVec2(cx, cy), 1.4f, col, 8);
        }
        else if (style == 4)
        {
            seg(cx - gap - arm, cy, cx + gap + arm, cy);
            seg(cx, cy, cx, cy + gap + arm);
        }
        else if (style == 0)
        {
            seg(cx - arm, cy, cx + arm, cy);
            seg(cx, cy - arm, cx, cy + arm);
        }
        else // gap-cross
        {
            seg(cx - gap - arm, cy, cx - gap, cy);
            seg(cx + gap, cy, cx + gap + arm, cy);
            seg(cx, cy - gap - arm, cx, cy - gap);
            seg(cx, cy + gap, cx, cy + gap + arm);
            dl->AddCircleFilled(ImVec2(cx, cy), 1.6f, col, 8);
        }
        g_EspDrawCount++;
    }

    // Hit marker
    if (g_HitMarkers && dl && now < g_HitMarkerUntil)
    {
        float cx = g_ScreenWidth * 0.5f;
        float cy = g_ScreenHeight * 0.5f;
        float a = (float)(g_HitMarkerUntil - now) / 280.f;
        if (a > 1.f) a = 1.f;
        ImU32 col = ToCol(g_ColorHitMarker[0], g_ColorHitMarker[1], g_ColorHitMarker[2], g_ColorHitMarker[3] * a);
        ImU32 outline = IM_COL32(0, 0, 0, (int)(200 * a));
        float s = 12.f;
        auto xline = [&](float x1, float y1, float x2, float y2) {
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), outline, 4.f);
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, 2.2f);
        };
        xline(cx - s, cy - s, cx - 4, cy - 4);
        xline(cx + s, cy - s, cx + 4, cy - 4);
        xline(cx - s, cy + s, cx - 4, cy + 4);
        xline(cx + s, cy + s, cx + 4, cy + 4);
    }

    // Shot direction arrows (screen edge) — direction is world XZ toward the shot origin
    if (g_ShotIndicators && dl)
    {
        float cx = g_ScreenWidth * 0.5f;
        float cy = g_ScreenHeight * 0.5f;
        float radius = (g_ScreenHeight < g_ScreenWidth ? g_ScreenHeight : g_ScreenWidth) * 0.38f;
        ImU32 col = ToCol(g_ColorShotInd[0], g_ColorShotInd[1], g_ColorShotInd[2], g_ColorShotInd[3]);

        uintptr_t cam = IsValidPtr(worldPtr) ? Read<uintptr_t>(worldPtr + offsets::world::Camera) : 0;
        Vec3 right{ 1,0,0 }, fwd{ 0,0,1 };
        if (IsValidPtr(cam))
        {
            right = Read<Vec3>(cam + offsets::camera::InvertedViewRight);
            fwd = Read<Vec3>(cam + offsets::camera::InvertedViewForward);
        }
        // Flatten camera basis onto XZ so pitch does not collapse arrows
        float rx = right.x, rz = right.z;
        float fx = fwd.x, fz = fwd.z;
        float rLen = sqrtf(rx * rx + rz * rz);
        float fLen = sqrtf(fx * fx + fz * fz);
        if (rLen > 0.01f) { rx /= rLen; rz /= rLen; } else { rx = 1.f; rz = 0.f; }
        if (fLen > 0.01f) { fx /= fLen; fz /= fLen; } else { fx = 0.f; fz = 1.f; }

        for (int i = 0; i < kMaxShotInd; i++)
        {
            if (!g_ShotInds[i].used) continue;
            if (now > g_ShotInds[i].expire) { g_ShotInds[i].used = false; continue; }
            float dx = g_ShotInds[i].dirX;
            float dz = g_ShotInds[i].dirZ;
            // Screen: +X = camera right, +Y = down ≈ -camera forward on XZ
            float sx = dx * rx + dz * rz;
            float sy = -(dx * fx + dz * fz);
            float len = sqrtf(sx * sx + sy * sy);
            if (len < 0.01f) continue;
            sx /= len; sy /= len;
            float px = cx + sx * radius;
            float py = cy + sy * radius;
            float ax = -sy, ay = sx;
            float life = (float)(g_ShotInds[i].expire - now) / 900.f;
            if (life < 0.f) life = 0.f;
            if (life > 1.f) life = 1.f;
            ImU32 c = ToCol(g_ColorShotInd[0], g_ColorShotInd[1], g_ColorShotInd[2], g_ColorShotInd[3] * life);
            dl->AddTriangleFilled(
                ImVec2(px + sx * 12.f, py + sy * 12.f),
                ImVec2(px - sx * 7.f + ax * 8.f, py - sy * 7.f + ay * 8.f),
                ImVec2(px - sx * 7.f - ax * 8.f, py - sy * 7.f - ay * 8.f),
                c);
        }
    }

    // Impacts
    if (g_ImpactMarkers && dl)
    {
        for (int i = 0; i < kMaxImpacts; i++)
        {
            if (!g_Impacts[i].used) continue;
            if (now > g_Impacts[i].expire) { g_Impacts[i].used = false; continue; }
            Vec2 scr;
            if (!WorldToScreen(g_Impacts[i].pos, scr)) continue;
            float life = (float)(g_Impacts[i].expire - now) / (float)(g_ImpactLifetimeMs > 0 ? g_ImpactLifetimeMs : 1);
            if (life < 0.f) life = 0.f;
            if (life > 1.f) life = 1.f;
            ImU32 col = ToCol(g_ColorImpact[0], g_ColorImpact[1], g_ColorImpact[2], g_ColorImpact[3] * life);
            ImU32 glow = ToCol(g_ColorImpact[0], g_ColorImpact[1], g_ColorImpact[2], 0.35f * life);
            float s = 6.f + (1.f - life) * 4.f;
            dl->AddCircle(ImVec2(scr.x, scr.y), s + 3.f, glow, 18, 3.f);
            dl->AddCircle(ImVec2(scr.x, scr.y), s, col, 18, 2.f);
            dl->AddLine(ImVec2(scr.x - s, scr.y - s), ImVec2(scr.x + s, scr.y + s), col, 2.f);
            dl->AddLine(ImVec2(scr.x + s, scr.y - s), ImVec2(scr.x - s, scr.y + s), col, 2.f);
        }
    }

    // Skip bullet/grenade world walks when all projectile visuals are off.
    if (!g_BulletTracers && !g_GrenadeTrajectory && !g_HitMarkers &&
        !g_ShotIndicators && !g_ImpactMarkers)
    {
        for (int i = 0; i < kMaxBulletTracks; i++)
            g_BulletTracks[i].active = false;
        return;
    }

    if (!IsValidPtr(worldPtr))
        return;

    uintptr_t bulletData = 0;
    int bulletCount = 0;
    bool ptrArray = true;
    bool haveBullets = ResolveBulletArray(worldPtr, bulletData, bulletCount, ptrArray);

    bool seen[kMaxBulletTracks];
    ZeroMem(seen, sizeof(seen));

    Vec3 localRef = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    bool haveLocal = g_LocalPlayerValid || g_CameraValid;

    auto touchProjectile = [&](uintptr_t ent, bool forceGrenade) {
        if (!IsValidPtr(ent) || ent < 0x100000000) return;
        if (ent == localPlayer) return;

        uintptr_t bvs = Read<uintptr_t>(ent + offsets::entity::VisualState);
        if (!IsValidPtr(bvs) || bvs < 0x100000000)
            bvs = Read<uintptr_t>(ent + offsets::entity::FutureVisualState);
        if (!IsValidPtr(bvs) || bvs < 0x100000000) return;

        Vec3 pos;
        if (!GetEntityPosition(ent, pos)) return;
        if (pos.x != pos.x || pos.y != pos.y || pos.z != pos.z) return;

        bool isGrenade = forceGrenade || EntityLooksLikeGrenade(ent);

        BulletTrack* t = FindTrack(ent);
        bool isNew = false;
        if (!t)
        {
            // Ignore grenades still in/near hands/inventory (not thrown yet)
            if (isGrenade && haveLocal && Distance3D(pos, localRef) < 2.3f)
                return;

            t = AllocTrack(ent);
            if (!t) return;
            isNew = true;
            // If a fading finished track still holds this ptr, multi-shot fix is active
            for (int fi = 0; fi < kMaxBulletTracks; fi++)
            {
                if (g_BulletTracks[fi].active && g_BulletTracks[fi].finished && g_BulletTracks[fi].ptr == ent)
                {
                    static int s_Split = 0;
                    if ((++s_Split % 5) == 1)
                        Log("verify[tracer] split new trail from fading ptr (multi-shot OK)");
                    break;
                }
            }
            t->firstPos = pos;
            t->lastPos = pos;
            t->fromLocal = haveLocal && Distance3D(pos, localRef) < 12.0f;
            // Fast bullets leave 12m in one tick — modest widen when magic bullet + firing
            if (!t->fromLocal && haveLocal && g_MagicBullet &&
                Distance3D(pos, localRef) < 40.0f &&
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                t->fromLocal = true;
            t->isGrenade = isGrenade;
            t->trailCount = 0;
            t->vel = { 0, 0, 0 };
            t->finished = false;
            t->fadeUntil = 0;
            AppendTrailPoint(t, pos);
        }

        int idx = (int)(t - g_BulletTracks);
        if (idx >= 0 && idx < kMaxBulletTracks) seen[idx] = true;

        Vec3 prev = t->lastPos;
        float dt = 0.016f;
        Vec3 newVel;
        newVel.x = (pos.x - prev.x) / dt;
        newVel.y = (pos.y - prev.y) / dt;
        newVel.z = (pos.z - prev.z) / dt;
        float spd = sqrtf(newVel.x * newVel.x + newVel.y * newVel.y + newVel.z * newVel.z);
        if (spd > 1.f)
        {
            // Light EMA so grenade prediction isn't jittery
            if (t->vel.x == 0.f && t->vel.y == 0.f && t->vel.z == 0.f)
                t->vel = newVel;
            else
            {
                t->vel.x = t->vel.x * 0.35f + newVel.x * 0.65f;
                t->vel.y = t->vel.y * 0.35f + newVel.y * 0.65f;
                t->vel.z = t->vel.z * 0.35f + newVel.z * 0.65f;
            }
        }
        t->lastPos = pos;
        t->lastSeen = now;
        if (isGrenade) t->isGrenade = true;

        AppendTrailPoint(t, pos);

        if (isNew && !t->fromLocal && !t->isGrenade && haveLocal && g_ShotIndicators)
        {
            float d0 = Distance3D(t->firstPos, localRef);
            if (d0 > 8.f && d0 < 600.f)
                AddShotIndicator(t->firstPos.x - localRef.x, t->firstPos.z - localRef.z);
        }

        // Stronger evidence only: segment must pass near a known player/infected torso
        if (g_HitMarkers && t->fromLocal && !t->isGrenade)
        {
            if (SegmentHitsEntity(prev, pos, 1.15f))
            {
                g_HitMarkerUntil = now + 280;
                static int s_Hm = 0;
                if ((++s_Hm % 3) == 1)
                    Log("verify[hitmarker] strong segment hit");
            }
        }
    };

    if (haveBullets)
    {
        for (int i = 0; i < bulletCount && i < 128; i++)
        {
            uintptr_t bullet = 0;
            if (ptrArray)
                bullet = Read<uintptr_t>(bulletData + (uintptr_t)i * 8);
            else
                bullet = bulletData + (uintptr_t)i * 0x100;
            touchProjectile(bullet, false);
        }
    }

    // Scan near / slow / item for thrown grenades (often not in BulletList)
    if (g_GrenadeTrajectory || g_BulletTracers)
    {
        // Near — pointer table
        {
            uintptr_t data = 0; int count = 0;
            if (ResolveEntityList(worldPtr, oak_offsets::world::NearEntList, 2000, data, count, nullptr))
            {
                int maxN = count < 120 ? count : 120;
                for (int i = 0; i < maxN; i++)
                {
                    uintptr_t ent = Read<uintptr_t>(data + (uintptr_t)i * 8);
                    if (!IsValidPtr(ent) || ent < 0x100000000) continue;
                    if (!EntityLooksLikeGrenade(ent)) continue;
                    touchProjectile(ent, true);
                }
            }
        }
        // Slow + Item — 0x18 slots
        uintptr_t slowLists[] = { oak_offsets::world::SlowEntList, oak_offsets::world::ItemList };
        uintptr_t slowCounts[] = { oak_offsets::world::SlowTableSize, oak_offsets::world::ItemListSize };
        for (int li = 0; li < 2; li++)
        {
            uintptr_t data = Read<uintptr_t>(worldPtr + slowLists[li]);
            int count = Read<int>(worldPtr + slowCounts[li]);
            if (!IsValidPtr(data) || data < 0x100000000 || count <= 0 || count > 2000) continue;
            int maxN = count < 200 ? count : 200;
            for (int i = 0; i < maxN; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                if ((Read<WORD>(entry) & 0xFFFF) != 1) continue;
                uintptr_t ent = Read<uintptr_t>(entry + 0x8);
                if (!IsValidPtr(ent) || ent < 0x100000000) continue;
                if (!EntityLooksLikeGrenade(ent)) continue;
                touchProjectile(ent, true);
            }
        }
    }

    // Finish unseen tracks → keep FULL path and fade
    int fadeMs = g_TracerLifetimeMs > 0 ? g_TracerLifetimeMs : 3000;
    for (int i = 0; i < kMaxBulletTracks; i++)
    {
        BulletTrack& t = g_BulletTracks[i];
        if (!t.active) continue;
        if (t.finished) continue;
        if (seen[i]) continue;
        if ((now - t.lastSeen) <= 70) continue;

        if (g_ImpactMarkers && !t.isGrenade)
            AddImpact(t.lastPos);
        // End-of-flight hit marker: require segment near torso (tighter than loose proximity)
        if (g_HitMarkers && t.fromLocal && !t.isGrenade &&
            SegmentHitsEntity(t.firstPos, t.lastPos, 1.35f))
            g_HitMarkerUntil = now + 320;

        // Need a real path (at least start→end)
        if (t.trailCount < 2)
        {
            // Synthesize a short segment from first to last if we only got one sample
            if (t.trailCount == 1 && Distance3D(t.firstPos, t.lastPos) > 0.5f)
                AppendTrailPoint(&t, t.lastPos);
        }
        t.finished = true;
        t.fadeUntil = now + (DWORD)fadeMs;
    }

    // Draw live + fading full paths; predict grenade arcs while in flight
    if (dl)
    {
        for (int i = 0; i < kMaxBulletTracks; i++)
        {
            BulletTrack& t = g_BulletTracks[i];
            if (!t.active) continue;

            if (t.finished)
            {
                if (now >= t.fadeUntil)
                {
                    t.active = false;
                    continue;
                }
                float life = (float)(t.fadeUntil - now) / (float)fadeMs;
                if (life < 0.f) life = 0.f;
                if (life > 1.f) life = 1.f;
                // Ease-out fade so the full line stays readable longer
                float alpha = life * life * (3.f - 2.f * life); // smoothstep
                if (t.isGrenade)
                {
                    if (g_GrenadeTrajectory)
                        DrawTrailPath(t, alpha);
                }
                else if (g_BulletTracers)
                {
                    DrawTrailPath(t, alpha);
                }
                continue;
            }

            // Live projectile
            if (t.isGrenade)
            {
                if (g_GrenadeTrajectory)
                {
                    DrawTrailPath(t, 1.f);
                    float spd = sqrtf(t.vel.x * t.vel.x + t.vel.y * t.vel.y + t.vel.z * t.vel.z);
                    if (spd > 2.0f)
                        DrawGrenadeArc(t.lastPos, t.vel, 0.85f);
                }
            }
            else if (g_BulletTracers)
            {
                DrawTrailPath(t, 1.f);
            }
        }
    }
}

static void DrawEntityExtras(uintptr_t entity, bool isPlayer, float boxX, float boxY, float boxW, float boxH, float headX, float footY)
{
    // Local vitals live on the left HUD — skip a second ReadVitals/Discover on the skeleton.
    if (IsValidPtr(g_ResolvedLocalPlayer) && entity == g_ResolvedLocalPlayer)
    {
        (void)boxW; (void)footY;
        return;
    }
    const bool isLocalEnt = false;
    if (!g_HealthBars && !(isPlayer && g_WeaponEsp))
    {
        (void)boxX; (void)boxY; (void)boxW; (void)boxH; (void)headX; (void)footY;
        return;
    }
    static DWORD s_ExWarm = 0;
    if (!s_ExWarm) s_ExWarm = GetTickCount();
    if ((GetTickCount() - s_ExWarm) < 500)
    {
        (void)boxW; (void)footY;
        return;
    }

    // Per-entity ring cache — single s_LastEnt forced full refresh when drawing N remotes
    enum { kExCache = 32 };
    struct ExSlot {
        uintptr_t ent;
        DWORD tick;
        float hp, blood, shock, stam, hung, thir;
        char wpn[48];
        int ammo;
        bool haveVit, haveWpn;
    };
    static ExSlot s_Ex[kExCache] = {};
    DWORD now = GetTickCount();
    unsigned h = (unsigned)((entity >> 4) ^ (entity >> 12)) & (kExCache - 1);
    ExSlot& slot = s_Ex[h];
    bool refresh = (slot.ent != entity) || (now - slot.tick) > 250;
    if (refresh)
    {
        slot.ent = entity;
        slot.tick = now;
        slot.haveVit = false;
        slot.haveWpn = false;
        slot.hp = slot.blood = slot.shock = slot.stam = slot.hung = slot.thir = -1.f;
        slot.wpn[0] = 0; slot.ammo = -1;
    }

    __try
    {
        if (g_HealthBars)
        {
            if (refresh)
            {
                if (!isPlayer)
                {
                    g_QaZedSeen++;
                    slot.hp = ReadInfectedHpPct(entity);
                    slot.blood = slot.shock = slot.stam = slot.hung = slot.thir = -1.f;
                    slot.haveVit = slot.hp >= 1.f;
                    if (slot.haveVit)
                    {
                        g_QaZedHp = slot.hp;
                        g_QaZedBars++;
                    }
                }
                else
                {
                    slot.haveVit = ReadVitals(entity, &slot.hp, &slot.blood, &slot.shock, &slot.stam, nullptr, nullptr);
                    OakFileVitRefresh();
                    OakFileVitApply(entity, isLocalEnt, false,
                        nullptr, nullptr, &slot.shock, nullptr, &slot.hung, &slot.thir);
                }
            }
            if (!isPlayer)
            {
                if (slot.haveVit && slot.hp >= 1.f)
                    DrawEspBars(boxX, boxY, boxH, slot.hp, -1.f, -1.f, -1.f, -1.f, -1.f);
            }
            else if (slot.haveVit && (slot.hp >= 0.f || slot.stam >= 0.f || slot.blood >= 0.f ||
                slot.hung >= 0.f || slot.thir >= 0.f || slot.shock >= 0.f))
                DrawEspBars(boxX, boxY, boxH, slot.hp, slot.blood, slot.shock, slot.stam, slot.hung, slot.thir);
        }

        if (isPlayer && g_WeaponEsp)
        {
            if (refresh)
            {
                char tmp[64] = {};
                slot.haveWpn = GetHandWeaponInfo(entity, tmp, 64, &slot.ammo);
                lstrcpynA(slot.wpn, tmp, 48);
            }
            if (slot.haveWpn)
            {
                char line[96];
                if (slot.ammo >= 0)
                    wsprintfA(line, "%s  [%d]", slot.wpn, slot.ammo);
                else
                    wsprintfA(line, "%s", slot.wpn);
                DrawEspBadge(headX, boxY - ImGui::GetFontSize() - 10.f, line, 1.00f, 0.82f, 0.20f, 1.f);
            }
            else
            {
                DrawEspBadge(headX, boxY - ImGui::GetFontSize() - 10.f, "Unarmed", 0.55f, 0.85f, 1.00f, 1.f);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    (void)boxW;
    (void)footY;
}

// Defined in misc_impl.inl (needs Steam tag / roster helpers)
static bool CombatIsFriendEntity(uintptr_t entity);

// ---------------------------------------------------------------------------
// Aimbot (mouse-accurate) + Magic bullet (sky-shot bone teleport)
// ---------------------------------------------------------------------------

struct CombatTarget {
    uintptr_t entity;
    Vec3 bonePos;
    float screenDist;
    float worldDist;
    bool isPlayer;
    bool onScreen;
    bool valid;
};

enum { kMaxLocalBulletMarks = 48 };
struct LocalBulletMark {
    uintptr_t ptr;
    DWORD lastSeen;
    bool used;
};

static CombatTarget g_CombatLock = {};
static CombatTarget g_MagicLock = {};
static CombatTarget g_GrenadeLock = {};
// Snapshot for Present-thread draw (UpdateCombatAim runs before ImGui NewFrame).
static CombatTarget g_CombatDrawAim = {};
static CombatTarget g_CombatDrawMb = {};
static CombatTarget g_CombatDrawGrenade = {};
static bool g_CombatDrawAimHeld = false;
static bool g_CombatDrawHoldingGrenade = false;
static LocalBulletMark g_LocalBulletMarks[kMaxLocalBulletMarks];

static float VecLen3(const Vec3& v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static Vec3 VecNorm3(Vec3 v)
{
    float L = VecLen3(v);
    if (L < 1e-5f) return { 0.f, 0.f, 1.f };
    v.x /= L; v.y /= L; v.z /= L;
    return v;
}

static Vec3 VecCross3(const Vec3& a, const Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static bool CombatEntityIsPlayerOrZombie(uintptr_t entity, bool& outPlayer, bool& outZombie)
{
    outPlayer = false;
    outZombie = false;
    char cfg[64]; char tn[64];
    cfg[0] = 0; tn[0] = 0;
    ReadEntityConfigName(entity, cfg, 64);
    ReadEntityTypeName(entity, tn, 64);

    // Infected markers first — after rejoin/respawn ConfigName can briefly lie, but
    // TypeName "Zmb*" / dayzinfected is reliable. Never dual-flag as player.
    const bool infectedCfg =
        StrCmpI(cfg, "dayzinfected") == 0 || StrContainsI(cfg, "dayzinfected");
    const bool infectedTn =
        StrContainsI(tn, "Zmb") || StrContainsI(tn, "Infected") ||
        StrContainsI(tn, "zombie") || StrContainsI(tn, "dayzinfected");
    if (infectedCfg || infectedTn)
    {
        outZombie = true;
        return true;
    }

    if (StrCmpI(cfg, "dayzplayer") == 0 ||
        StrContainsI(tn, "Survivor") || StrContainsI(tn, "dayzplayer"))
    {
        outPlayer = true;
        return true;
    }
    return false;
}

// Magic-bullet gate: survivors only.
// NOTE: Do NOT treat "any NetworkId != 0" as a player — base parts (watchtower/fence/gate),
// vehicles, and other networked props also have IDs and were getting locked after rejoin.
static bool CombatIsMagicPlayerTarget(uintptr_t entity)
{
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    char cfg[64]; char tn[64];
    cfg[0] = 0; tn[0] = 0;
    ReadEntityConfigName(entity, cfg, 64);
    ReadEntityTypeName(entity, tn, 64);

    // Hard rejects
    if (StrCmpI(cfg, "dayzinfected") == 0 || StrContainsI(cfg, "dayzinfected"))
        return false;
    if (StrCmpI(cfg, "dayzanimal") == 0 || StrContainsI(cfg, "dayzanimal"))
        return false;
    if (StrContainsI(tn, "Zmb") || StrContainsI(tn, "Infected") ||
        StrContainsI(tn, "zombie") || StrContainsI(tn, "dayzinfected"))
        return false;
    // Base building / props that fooled the old NetworkId fallback
    {
        const char* ban[] = {
            "watchtower", "fence", "gate", "wall", "shelter", "tent", "barrel",
            "crate", "chest", "flag_", "territory", "hesco", "barbedwire",
            "camonet", "woodencrate", "seachest", "fireplace", "powergenerator",
            "car_", "vehicle", "wreck", "boat", "helicopter", "land_ "
        };
        for (int i = 0; i < (int)(sizeof(ban) / sizeof(ban[0])); i++)
        {
            if (StrContainsI(tn, ban[i]) || StrContainsI(cfg, ban[i]))
                return false;
        }
    }

    // Authoritative player type
    if (StrCmpI(cfg, "dayzplayer") == 0)
        return true;
    if (StrContainsI(tn, "Survivor") || StrContainsI(tn, "dayzplayer"))
        return true;

    // Config/type can lag after respawn — require DayZPlayer-shaped pointers, not bare NetId.
    // Watchtowers/fences have NetworkId but NOT player Inventory/Skeleton at these offsets.
    uintptr_t inv = Read<uintptr_t>(entity + oak_offsets::player::Inventory);
    uintptr_t skel = Read<uintptr_t>(entity + oak_offsets::player::Skeleton);
    if (!IsValidPtr(inv) || inv < 0x100000000)
        return false;
    if (!IsValidPtr(skel) || skel < 0x100000000)
        return false;
    int netId = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer);
    if (netId == 0)
        netId = Read<int>(entity + oak_offsets::entity::NetworkId);
    return netId != 0;
}

static bool CombatGetAimBone(uintptr_t entity, bool isPlayer, Vec3& out)
{
    Vec3 origin;
    if (!GetEntityPosition(entity, origin))
        return false;

    int boneIdx = (g_AimbotBone == 1) ? BONE_SPINE1 : BONE_HEAD;
    Vec3 bone = GetBonePosition(entity, boneIdx, isPlayer);

    // Validate skeleton result against entity origin
    if (bone.x == bone.x && bone.y == bone.y && bone.z == bone.z)
    {
        float dx = bone.x - origin.x;
        float dy = bone.y - origin.y;
        float dz = bone.z - origin.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        // Plausible humanoid bone: within ~0.2–2.8m of feet origin, and head above pelvis-ish
        if (d2 > 0.04f && d2 < 8.0f && bone.y >= origin.y - 0.2f)
        {
            out = bone;
            return true;
        }
    }

    // Fallback: entity origin + fixed head/chest height (very reliable)
    out = origin;
    out.y += (g_AimbotBone == 1) ? 1.15f : 1.62f;
    return true;
}

static void CombatMarkLocalBullet(uintptr_t bullet)
{
    if (!IsValidPtr(bullet) || bullet < 0x100000000)
        return;
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxLocalBulletMarks; i++)
    {
        if (g_LocalBulletMarks[i].used && g_LocalBulletMarks[i].ptr == bullet)
        {
            g_LocalBulletMarks[i].lastSeen = now;
            return;
        }
    }
    for (int i = 0; i < kMaxLocalBulletMarks; i++)
    {
        if (!g_LocalBulletMarks[i].used)
        {
            g_LocalBulletMarks[i].ptr = bullet;
            g_LocalBulletMarks[i].lastSeen = now;
            g_LocalBulletMarks[i].used = true;
            return;
        }
    }
}

static bool CombatIsMarkedLocalBullet(uintptr_t bullet)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxLocalBulletMarks; i++)
    {
        if (!g_LocalBulletMarks[i].used) continue;
        // Short TTL — long marks invite UAF writes after despawn/pointer reuse
        if (now - g_LocalBulletMarks[i].lastSeen > 350)
        {
            g_LocalBulletMarks[i].used = false;
            continue;
        }
        if (g_LocalBulletMarks[i].ptr == bullet)
            return true; // do NOT refresh TTL here — only live-list Mark may extend life
    }
    return false;
}

// Drop marks whose pointers are no longer in this frame's bullet list (prevents recycled-ptr snaps).
static void CombatPurgeMarksNotInList(const uintptr_t* live, int liveCount)
{
    for (int i = 0; i < kMaxLocalBulletMarks; i++)
    {
        if (!g_LocalBulletMarks[i].used) continue;
        bool found = false;
        for (int j = 0; j < liveCount; j++)
        {
            if (live[j] == g_LocalBulletMarks[i].ptr) { found = true; break; }
        }
        if (!found)
            g_LocalBulletMarks[i].used = false;
    }
}

// One-shot markers: allow a short GUIDE window (multi-teleport) so trees/walls
// can't eat the round before it reaches the bone. Hard-stop after budget to avoid
// UAF on despawning projectiles.
static uintptr_t g_SnappedBullets[64];
static DWORD g_SnappedAt[64];
static int g_SnapGuides[64];

static int CombatFindSnapSlot(uintptr_t bullet, bool create)
{
    DWORD now = GetTickCount();
    int freeSlot = -1;
    for (int i = 0; i < 64; i++)
    {
        if (g_SnappedBullets[i] == 0 || (now - g_SnappedAt[i] > 2000))
        {
            if (freeSlot < 0) freeSlot = i;
            if (g_SnappedBullets[i] != 0)
            {
                g_SnappedBullets[i] = 0;
                g_SnapGuides[i] = 0;
            }
            continue;
        }
        if (g_SnappedBullets[i] == bullet)
            return i;
    }
    if (!create || freeSlot < 0) return -1;
    g_SnappedBullets[freeSlot] = bullet;
    g_SnappedAt[freeSlot] = now;
    g_SnapGuides[freeSlot] = 0;
    return freeSlot;
}

static bool CombatCanGuideBullet(uintptr_t bullet)
{
    int slot = CombatFindSnapSlot(bullet, true);
    if (slot < 0) return false;
    DWORD now = GetTickCount();
    // Slightly longer seat window when silent aim is on (still VS-only).
    const bool silentOn = g_SilentAim.enabled && !g_MagicBullet;
    const DWORD windowMs = silentOn ? 350u : 200u;
    const int maxGuides = silentOn ? 20 : 12;
    // g_SnappedAt = first guide start (do not refresh)
    if (now - g_SnappedAt[slot] > windowMs)
        return false;
    if (g_SnapGuides[slot] >= maxGuides)
        return false;
    return true;
}

static void CombatRememberGuide(uintptr_t bullet)
{
    int slot = CombatFindSnapSlot(bullet, true);
    if (slot < 0) return;
    g_SnapGuides[slot]++;
}

// Write bullet world position into VisualState only.
// FutureVisualState is owned by the sim thread — dual-writing it while bullets despawn
// was a likely source of heap AVs (ntdll free of -1) after heavy magic-bullet use.
static void CombatWriteEntityWorldPos(uintptr_t entity, const Vec3& pos)
{
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return;
    if (pos.x != pos.x || pos.y != pos.y || pos.z != pos.z)
        return;
    if (fabsf(pos.x) > 50000.f || fabsf(pos.y) > 50000.f || fabsf(pos.z) > 50000.f)
        return;

    const int labMod = OakLab_IsWriteAllowed(OAK_LAB_MB) ? OAK_LAB_MB
        : (OakLab_IsWriteAllowed(OAK_LAB_SILENT) ? OAK_LAB_SILENT
        : (OakLab_IsWriteAllowed(OAK_LAB_GRENADE) ? OAK_LAB_GRENADE : OAK_LAB_NONE));
    if (labMod == OAK_LAB_NONE)
        return;

    // Never teleport players/infected — recycled bullet slots sometimes reuse those ptrs
    {
        bool isP = false, isZ = false;
        if (CombatEntityIsPlayerOrZombie(entity, isP, isZ))
            return;
    }

    uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
    if (!IsValidPtr(vs) || vs < 0x100000000)
        return;
    // VisualState translation only — never FutureVisualState (sim-owned).
    OakLab_PushModule(labMod);
    OakLab_WriteVec3(labMod, vs + 0x2C, pos.x, pos.y, pos.z, 0);
    OakLab_PopModule();
}

// Score one entity into best FOV / nearest-world slots (no SEH — Read/Write already guarded).
static void CombatConsiderTarget(
    uintptr_t entity, uintptr_t localPlayer,
    const Vec3& localPos, float maxDist, float fovPx, float cx, float cy,
    CombatTarget& bestFov, float& bestFovScore,
    CombatTarget& bestWorld, float& bestWorldDist,
    int& considered,
    bool playersOnly)
{
    if (!IsValidPtr(entity) || entity < 0x100000000 || entity == localPlayer)
        return;
    if (EntityIsDead(entity))
        return;
    if (CombatIsFriendEntity(entity))
        return;

    bool isP = false, isZ = false;
    if (playersOnly)
    {
        // Do not require CombatEntityIsPlayerOrZombie first — ConfigName often lags
        // after respawn/rejoin while NetworkId is already valid.
        if (!CombatIsMagicPlayerTarget(entity))
            return;
        isP = true;
        isZ = false;
    }
    else
    {
        if (!CombatEntityIsPlayerOrZombie(entity, isP, isZ))
            return;
        if (!((isP && g_AimbotPlayers) || (isZ && g_AimbotZombies)))
            return;
    }

    Vec3 bone;
    if (!CombatGetAimBone(entity, isP, bone))
        return;

    float wdist = Distance3D(localPos, bone);
    if (wdist > maxDist || wdist < 0.35f)
        return;
    if (considered >= g_MaxAimTargets)
        return;

    considered++;
    CombatTarget cand = {};
    cand.entity = entity;
    cand.bonePos = bone;
    cand.worldDist = wdist;
    cand.isPlayer = isP;
    cand.valid = true;

    Vec2 scr;
    if (WorldToScreen(bone, scr))
    {
        float sdx = scr.x - cx, sdy = scr.y - cy;
        float sd = sqrtf(sdx * sdx + sdy * sdy);
        cand.screenDist = sd;
        cand.onScreen = true;
        if (sd <= fovPx && sd < bestFovScore)
        {
            bestFovScore = sd;
            bestFov = cand;
        }
    }

    if (wdist < bestWorldDist)
    {
        bestWorldDist = wdist;
        bestWorld = cand;
    }
}

// allowOffscreen: unused for MB (sky-shot sticky removed) — kept for aimbot nearest fallback
// playersOnly: magic bullet — ignore infected entirely; uses g_MagicBulletMaxDistance
static bool CombatFindBestTarget(uintptr_t worldPtr, uintptr_t localPlayer, CombatTarget& out, bool allowOffscreen, bool playersOnly)
{
    out = {};
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer))
        return false;
    if (!g_LocalPlayerValid && !g_CameraValid)
        return false;

    float fovPx = (float)(playersOnly
        ? (g_MagicBulletFov > 10 ? g_MagicBulletFov : 10)
        : (g_AimbotFov > 10 ? g_AimbotFov : 10));
    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    int rangeCap = playersOnly ? g_MagicBulletMaxDistance : g_AimbotMaxDistance;
    float maxDist = (float)(rangeCap > 10 ? rangeCap : 10);

    CombatTarget bestFov = {};
    CombatTarget bestWorld = {};
    float bestFovScore = 1e12f;
    float bestWorldDist = 1e12f;
    int considered = 0;

    // Near + Far: pointer tables (same resolution path as ESP)
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
        int maxI = count > 250 ? 250 : count;
        for (int i = 0; i < maxI; i++)
        {
            CombatConsiderTarget(
                Read<uintptr_t>(data + (uintptr_t)i * 8), localPlayer,
                localPos, maxDist, fovPx, cx, cy,
                bestFov, bestFovScore, bestWorld, bestWorldDist, considered, playersOnly);
        }
    }

    // Slow table: infected-heavy. Skip entirely for magic-bullet playersOnly —
    // scanning it after respawn/rejoin was a common way to lock Zmb* misreads.
    if (!playersOnly)
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + oak_offsets::world::SlowEntList);
        int count = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
        if (IsValidPtr(data) && data > 0x100000000 && count > 0 && count < 2000)
        {
            int maxI = count > 250 ? 250 : count;
            for (int i = 0; i < maxI; i++)
            {
                uintptr_t entry = data + (uintptr_t)i * 0x18;
                WORD flag = Read<WORD>(entry);
                if (flag != 1) continue;
                CombatConsiderTarget(
                    Read<uintptr_t>(entry + 0x8), localPlayer,
                    localPos, maxDist, fovPx, cx, cy,
                    bestFov, bestFovScore, bestWorld, bestWorldDist, considered, playersOnly);
            }
        }
    }

    static int s_TgtLog = 0;
    if (false && (g_AimbotEnabled || g_MagicBullet) && (s_TgtLog++ % 300) == 0)
    {
        char buf[192];
        wsprintfA(buf, "combat: considered=%d fovHit=%d worldHit=%d max=%d",
            considered, bestFov.valid ? 1 : 0, bestWorld.valid ? 1 : 0, (int)maxDist);
        Log(buf);
    }

    if (g_AimPriority == OakAimPriDistance)
    {
        if (bestWorld.valid)
        {
            out = bestWorld;
            return true;
        }
        if (bestFov.valid)
        {
            out = bestFov;
            return true;
        }
        return false;
    }

    if (bestFov.valid)
    {
        out = bestFov;
        return true;
    }
    if (allowOffscreen && bestWorld.valid)
    {
        out = bestWorld;
        return true;
    }
    return false;
}

static bool CombatTargetStillValid(uintptr_t localPlayer, CombatTarget& t, float fovPx, bool requireFov, bool playersOnly)
{
    if (!t.valid || !IsValidPtr(t.entity) || t.entity == localPlayer)
        return false;
    if (EntityIsDead(t.entity))
        return false;
    if (CombatIsFriendEntity(t.entity))
        return false;
    bool isP = false, isZ = false;
    if (playersOnly)
    {
        if (!CombatIsMagicPlayerTarget(t.entity))
            return false;
        isP = true;
        isZ = false;
    }
    else
    {
        if (!CombatEntityIsPlayerOrZombie(t.entity, isP, isZ))
            return false;
        if (!((isP && g_AimbotPlayers) || (isZ && g_AimbotZombies)))
            return false;
    }

    Vec3 bone;
    if (!CombatGetAimBone(t.entity, isP, bone))
        return false;

    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    int rangeCap = playersOnly ? g_MagicBulletMaxDistance : g_AimbotMaxDistance;
    float dist = Distance3D(localPos, bone);
    if (dist > (float)(rangeCap > 10 ? rangeCap : 10) || dist < 0.35f)
        return false;

    t.bonePos = bone;
    t.worldDist = dist;
    t.isPlayer = isP;

    Vec2 scr;
    if (WorldToScreen(bone, scr))
    {
        float cx = g_ScreenWidth * 0.5f;
        float cy = g_ScreenHeight * 0.5f;
        float sdx = scr.x - cx, sdy = scr.y - cy;
        float sd = sqrtf(sdx * sdx + sdy * sdy);
        t.screenDist = sd;
        t.onScreen = true;
        if (requireFov && sd > fovPx * 1.5f)
            return false;
        return true;
    }

    t.onScreen = false;
    return !requireFov;
}

// Mouse-move aimbot. Critical: never step past the remaining screen error, or
// movement + look makes it overshoot → correct other way → fight itself forever.
static float s_AimAccX = 0.f;
static float s_AimAccY = 0.f;
static float s_AimLastDx = 0.f;
static float s_AimLastDy = 0.f;
static bool s_AimHadSample = false;
static uintptr_t s_AimReactEntity = 0;
static DWORD s_AimReactArmTick = 0;

static bool CombatAimLosOk(uintptr_t worldPtr, uintptr_t localPlayer, const CombatTarget& t)
{
    if (!g_AimRequireLos || !t.valid)
        return true;
    return OakBatch4LosClear(worldPtr, localPlayer, t.entity, t.bonePos);
}

static bool CombatAimReactionReady(const CombatTarget& t)
{
    if (!t.valid || g_AimReactionMs <= 0)
        return true;
    DWORD now = GetTickCount();
    if (t.entity != s_AimReactEntity)
    {
        s_AimReactEntity = t.entity;
        s_AimReactArmTick = now;
    }
    return (now - s_AimReactArmTick) >= (DWORD)g_AimReactionMs;
}

static void CombatResetMouseAimState()
{
    s_AimAccX = 0.f;
    s_AimAccY = 0.f;
    s_AimLastDx = 0.f;
    s_AimLastDy = 0.f;
    s_AimHadSample = false;
    s_AimReactEntity = 0;
    s_AimReactArmTick = 0;
}

static void CombatApplyMouseAim(const Vec3& targetBone)
{
    if (ImGuiMenu_IsOpen())
        return;

    Vec2 scr;
    if (!WorldToScreen(targetBone, scr))
        return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    float dx = scr.x - cx;
    float dy = scr.y - cy;
    float dist = sqrtf(dx * dx + dy * dy);

    const bool ads = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const float dead = ads ? 2.75f : 1.35f;
    if (dist < dead)
    {
        // Settle — dump residual so we don't keep nudging across the bone
        s_AimAccX = 0.f;
        s_AimAccY = 0.f;
        s_AimLastDx = dx;
        s_AimLastDy = dy;
        s_AimHadSample = true;
        return;
    }

    float smooth = (float)(g_AimbotSmooth > 0 ? g_AimbotSmooth : 1);
    // Higher smooth = smaller fraction of remaining error (asymptotic, not fling)
    float frac = 1.f / (smooth + 2.5f);
    if (ads)
        frac *= 0.42f;
    // Near the bone: ease harder so we don't punch through
    if (dist < 40.f) frac *= 0.65f;
    if (dist < 18.f) frac *= 0.55f;
    if (frac > 0.45f) frac = 0.45f;
    if (frac < 0.04f) frac = 0.04f;

    // Human-like variance — never slower than the base fraction (tracking speed preserved).
    if (g_AimSmoothVarPct > 0)
    {
        const float baseFrac = frac;
        const float amp = ((float)g_AimSmoothVarPct / 100.f) * 0.22f;
        const float phase = (float)(GetTickCount() % 997) / 997.f;
        frac = baseFrac * (1.f + (phase * 2.f - 1.f) * amp);
        if (frac < baseFrac) frac = baseFrac;
        if (frac > baseFrac * 1.28f) frac = baseFrac * 1.28f;
    }

    // Sign flip vs last frame = we already overshot — brake hard
    if (s_AimHadSample)
    {
        if (s_AimLastDx * dx < 0.f)
        {
            frac *= 0.2f;
            s_AimAccX = 0.f;
        }
        if (s_AimLastDy * dy < 0.f)
        {
            frac *= 0.2f;
            s_AimAccY = 0.f;
        }
    }

    float mx = dx * frac;
    float my = dy * frac;

    // HARD anti-overshoot: never move more pixels than the remaining error
    if (fabsf(mx) > fabsf(dx)) mx = dx;
    if (fabsf(my) > fabsf(dy)) my = dy;

    // Soft speed cap (still ≤ remaining error)
    float maxStep = ads ? 14.f : 22.f;
    float mlen = sqrtf(mx * mx + my * my);
    if (mlen > maxStep && mlen > 1e-4f)
    {
        mx *= maxStep / mlen;
        my *= maxStep / mlen;
        if (fabsf(mx) > fabsf(dx)) mx = dx;
        if (fabsf(my) > fabsf(dy)) my = dy;
    }

    // Sub-pixel accumulator — avoids round-up that pushes past the head
    s_AimAccX += mx;
    s_AimAccY += my;
    LONG ix = (LONG)s_AimAccX;
    LONG iy = (LONG)s_AimAccY;
    s_AimAccX -= (float)ix;
    s_AimAccY -= (float)iy;

    s_AimLastDx = dx;
    s_AimLastDy = dy;
    s_AimHadSample = true;

    if (ix == 0 && iy == 0)
        return;
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)ix, (DWORD)iy, 0, 0);
}

// Retired: camera-matrix aim fought mouse/ADS and is not Lab-owned.
static void CombatApplyCameraAim(uintptr_t worldPtr, const Vec3& target)
{
    (void)worldPtr;
    (void)target;
}

// Magic-bullet InitSpeed / acceleration — high muzzle velocity so head snaps one-tap.
// Overrides Fast Bullets while MB is steering (UpdateCombatAim runs after UpdateFastBullets).
static void CombatBoostAmmoSpeed(uintptr_t localPlayer, float worldDist)
{
    uintptr_t ammoType = 0;
    if (!ResolveHeldAmmoType(localPlayer, ammoType))
        return;

    if (!AmmoTypeLooksValid(ammoType))
        return;
    if (!CaptureAmmoBackup(ammoType))
        return;

    // Aggressive acceleration: short TOF + high InitSpeed for lethality through walls
    float speed = worldDist / 0.035f;
    if (speed < 1200.f) speed = 1200.f;
    if (speed > 6000.f) speed = 6000.f;

    __try
    {
        const int labMod = OakLab_IsWriteAllowed(OAK_LAB_MB) ? OAK_LAB_MB
            : (OakLab_IsWriteAllowed(OAK_LAB_SILENT) ? OAK_LAB_SILENT
            : (OakLab_IsWriteAllowed(OAK_LAB_AMMO) ? OAK_LAB_AMMO : OAK_LAB_NONE));
        if (labMod == OAK_LAB_NONE)
            return;
        OakLab_PushModule(labMod);
        OakLab_WriteFloat(labMod, ammoType + 0x38C, speed, 0); // InitSpeed
        OakLab_WriteFloat(labMod, ammoType + 0x364, speed, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x398, speed, 0); // TypicalSpeed
        OakLab_WriteFloat(labMod, ammoType + 0x394, speed, 0); // MaxLeadSpeed
        OakLab_WriteFloat(labMod, ammoType + 0x3BC, 0.0f, 0);  // gravity off while guiding
        OakLab_WriteFloat(labMod, ammoType + 0x3B4, 0.0f, 0);  // air friction off
        OakLab_WriteFloat(labMod, ammoType + 0x3A4, 0.0f, 0);  // dispersion
        OakLab_WriteFloat(labMod, ammoType + 0x3CC, 0.0f, 0);
        OakLab_PopModule();
        g_AmmoBackup.dirty = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { OakLab_PopModule(); }
}

// Head/chest bone for MB/silent — pass isPlayer so zombie skeletons resolve.
static bool CombatGetMagicBone(uintptr_t entity, Vec3& out, bool isPlayer = true)
{
    Vec3 origin;
    if (!GetEntityPosition(entity, origin))
        return false;

    auto tryBone = [&](int boneIdx, float minY) -> bool {
        Vec3 bone = GetBonePosition(entity, boneIdx, isPlayer);
        if (bone.x != bone.x || bone.y != bone.y || bone.z != bone.z)
            return false;
        float dx = bone.x - origin.x;
        float dy = bone.y - origin.y;
        float dz = bone.z - origin.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > 0.01f && d2 < 10.0f && bone.y >= origin.y + minY)
        {
            out = bone;
            return true;
        }
        return false;
    };

    // Prefer configured aim bone when silent/aim asked for chest.
    if (g_AimbotBone == 1)
    {
        if (tryBone(BONE_SPINE1, 0.55f))
            return true;
        if (tryBone(BONE_SPINE2, 0.65f))
            return true;
    }

    if (tryBone(BONE_HEAD, isPlayer ? 1.15f : 1.0f))
        return true;
    if (tryBone(BONE_NECK, isPlayer ? 1.0f : 0.85f))
        return true;
    if (tryBone(BONE_SPINE1, 0.55f))
        return true;

    out = origin;
    out.y += isPlayer ? 1.65f : 1.55f;
    return true;
}

// Short lead from recent bone motion (matches slower MB TOF).
static Vec3 CombatLeadMagicBone(uintptr_t entity, Vec3 bone, float worldDist)
{
    static uintptr_t s_Ent = 0;
    static Vec3 s_Prev = {};
    static DWORD s_Tick = 0;
    DWORD now = GetTickCount();
    Vec3 led = bone;
    if (entity == s_Ent && s_Tick != 0 && (now - s_Tick) < 250 && (now - s_Tick) > 8)
    {
        float dt = (float)(now - s_Tick) * 0.001f;
        if (dt > 0.001f)
        {
            Vec3 vel = {
                (bone.x - s_Prev.x) / dt,
                (bone.y - s_Prev.y) / dt,
                (bone.z - s_Prev.z) / dt
            };
            float spd = VecLen3(vel);
            // Ignore teleport/noise
            if (spd > 0.15f && spd < 14.f)
            {
                float tof = worldDist / 2800.f; // match boosted InitSpeed
                if (tof < 0.01f) tof = 0.01f;
                if (tof > 0.18f) tof = 0.18f;
                led.x = bone.x + vel.x * tof;
                led.y = bone.y + vel.y * tof;
                led.z = bone.z + vel.z * tof;
            }
        }
    }
    s_Ent = entity;
    s_Prev = bone;
    s_Tick = now;
    return led;
}

// Core magic bullet — WALLBANG: multi-frame teleport onto bone so trees/walls
// never get a collision tick on the flight path. No safe PhysX-off in usermode.
static bool CombatSilentGuideActive()
{
    if (!g_SilentAim.enabled || g_MagicBullet)
        return false;
    if (!OakLab_IsWriteAllowed(OAK_LAB_SILENT))
        return false;
    int k = g_BindSilentAim > 0 ? g_BindSilentAim : g_SilentAim.key;
    if (k > 0 && !(GetAsyncKeyState(k) & 0x8000))
        return false;
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

static void CombatSnapLocalBulletsToBone(uintptr_t worldPtr, uintptr_t localPlayer, const Vec3& bone)
{
    // Rewrite: Lab-gated VS bone seat only. No wall-bypass mid-air. No FVS.
    if (!OakLab_IsWriteAllowed(OAK_LAB_MB) && !OakLab_IsWriteAllowed(OAK_LAB_SILENT))
        return;

    uintptr_t bulletData = 0;
    int bulletCount = 0;
    bool ptrArray = true;
    if (!ResolveBulletArray(worldPtr, bulletData, bulletCount, ptrArray))
        return;

    Vec3 localRef = g_CameraValid ? g_CameraPos : (g_LocalPlayerValid ? g_LocalPlayerPos : Vec3{});
    bool haveLocal = g_LocalPlayerValid || g_CameraValid;
    if (!haveLocal)
        return;

    bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!firing)
        return;

    const bool silentGuide = CombatSilentGuideActive();

    static uintptr_t s_PrevBullets[96];
    static int s_PrevCount = 0;
    uintptr_t curBullets[96];
    int curCount = 0;
    int snapped = 0;
    int marked = 0;

    for (int i = 0; i < bulletCount && i < 96; i++)
    {
        uintptr_t bullet = 0;
        if (ptrArray)
            bullet = Read<uintptr_t>(bulletData + (uintptr_t)i * 8);
        else
            bullet = bulletData + (uintptr_t)i * 0x100;
        if (!IsValidPtr(bullet) || bullet < 0x100000000 || bullet == localPlayer)
            continue;
        if (EntityLooksLikeGrenade(bullet))
            continue;

        Vec3 pos;
        if (!GetEntityPosition(bullet, pos))
            continue;
        if (pos.x != pos.x || pos.y != pos.y || pos.z != pos.z)
            continue;

        {
            bool isP = false, isZ = false;
            if (CombatEntityIsPlayerOrZombie(bullet, isP, isZ))
                continue;
        }

        curBullets[curCount++] = bullet;

        float fromCam = Distance3D(pos, localRef);
        bool wasSeen = false;
        for (int p = 0; p < s_PrevCount; p++)
        {
            if (s_PrevBullets[p] == bullet) { wasSeen = true; break; }
        }

        if (silentGuide || fromCam < 40.f || (!wasSeen && fromCam < 160.f))
        {
            CombatMarkLocalBullet(bullet);
            marked++;
        }
    }

    CombatPurgeMarksNotInList(curBullets, curCount);

    // Wallbang: seat just inside bone. Mid-air wall-bypass path removed in rewrite.
    Vec3 toBone = { bone.x - localRef.x, bone.y - localRef.y, bone.z - localRef.z };
    float rem = VecLen3(toBone);
    Vec3 snap = bone;
    if (rem > 0.05f)
    {
        Vec3 dir = VecNorm3(toBone);
        const float inset = 0.04f;
        snap.x = bone.x - dir.x * inset;
        snap.y = bone.y - dir.y * inset;
        snap.z = bone.z - dir.z * inset;
    }

    for (int i = 0; i < curCount; i++)
    {
        uintptr_t bullet = curBullets[i];
        if (!CombatIsMarkedLocalBullet(bullet))
            continue;
        if (!CombatCanGuideBullet(bullet))
            continue;

        // VS translation only — never FutureVisualState.
        CombatWriteEntityWorldPos(bullet, snap);
        CombatRememberGuide(bullet);
        snapped++;
    }

    for (int i = 0; i < curCount; i++)
        s_PrevBullets[i] = curBullets[i];
    s_PrevCount = curCount;

    if (snapped > 0)
    {
        char buf[200];
        int pen = g_WallBypass.enabled ? (int)(g_WallBypass.maxPenetrationM + 0.5f) : -1;
        wsprintfA(buf, "mb: bullets=%d marked=%d guided=%d wallbang=%d penM=%d",
            curCount, marked, snapped, g_WallBypass.enabled ? 0 : 1, pen);
        Log(buf);
        float dx = snap.x - bone.x, dy = snap.y - bone.y, dz = snap.z - bone.z;
        float err = sqrtf(dx * dx + dy * dy + dz * dz);
        float fromMuz = Distance3D(snap, localRef);
        int pass = g_WallBypass.enabled
            ? (fromMuz <= g_WallBypass.maxPenetrationM + 0.35f ? 1 : 0)
            : (err <= 0.15f ? 1 : 0);
        OakLab_ContractMbSnap(snapped, (int)(err * 100.f + 0.5f), 0 /* FVS dual-write removed */);
        wsprintfA(buf, "verify[mb-snap] guided=%d boneErrCm=%d travelCm=%d wallBypass=%d PASS=%d",
            snapped, (int)(err * 100.f + 0.5f), (int)(fromMuz * 100.f + 0.5f),
            g_WallBypass.enabled ? 1 : 0, pass);
        Log(buf);
    }
}

// Bullet Chain: seat the same local bullet through every target bone in one frame.
static void CombatSnapLocalBulletsThroughTargets(
    uintptr_t worldPtr, uintptr_t localPlayer, const CombatTarget* targets, int count)
{
    if (!targets || count <= 0)
        return;
    if (count == 1)
    {
        CombatSnapLocalBulletsToBone(worldPtr, localPlayer, targets[0].bonePos);
        return;
    }
    if (!OakLab_IsWriteAllowed(OAK_LAB_MB) && !OakLab_IsWriteAllowed(OAK_LAB_SILENT))
        return;

    uintptr_t bulletData = 0;
    int bulletCount = 0;
    bool ptrArray = true;
    if (!ResolveBulletArray(worldPtr, bulletData, bulletCount, ptrArray))
        return;

    Vec3 localRef = g_CameraValid ? g_CameraPos : (g_LocalPlayerValid ? g_LocalPlayerPos : Vec3{});
    if (!g_LocalPlayerValid && !g_CameraValid)
        return;
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
        return;

    static uintptr_t s_ChainPrev[96];
    static int s_ChainPrevCount = 0;
    uintptr_t curBullets[96];
    int curCount = 0;

    for (int i = 0; i < bulletCount && i < 96; i++)
    {
        uintptr_t bullet = 0;
        if (ptrArray)
            bullet = Read<uintptr_t>(bulletData + (uintptr_t)i * 8);
        else
            bullet = bulletData + (uintptr_t)i * 0x100;
        if (!IsValidPtr(bullet) || bullet < 0x100000000 || bullet == localPlayer)
            continue;
        if (EntityLooksLikeGrenade(bullet))
            continue;

        Vec3 pos;
        if (!GetEntityPosition(bullet, pos))
            continue;
        if (pos.x != pos.x || pos.y != pos.y || pos.z != pos.z)
            continue;

        {
            bool isP = false, isZ = false;
            if (CombatEntityIsPlayerOrZombie(bullet, isP, isZ))
                continue;
        }

        curBullets[curCount++] = bullet;
        float fromCam = Distance3D(pos, localRef);
        bool wasSeen = false;
        for (int p = 0; p < s_ChainPrevCount; p++)
        {
            if (s_ChainPrev[p] == bullet) { wasSeen = true; break; }
        }
        if (fromCam < 40.f || (!wasSeen && fromCam < 160.f))
            CombatMarkLocalBullet(bullet);
    }

    CombatPurgeMarksNotInList(curBullets, curCount);

    for (int i = 0; i < curCount; i++)
    {
        uintptr_t bullet = curBullets[i];
        if (!CombatIsMarkedLocalBullet(bullet) || !CombatCanGuideBullet(bullet))
            continue;

        for (int t = 0; t < count; t++)
        {
            const Vec3& bone = targets[t].bonePos;
            Vec3 toBone = { bone.x - localRef.x, bone.y - localRef.y, bone.z - localRef.z };
            float rem = VecLen3(toBone);
            Vec3 snap = bone;
            if (rem > 0.05f)
            {
                Vec3 dir = VecNorm3(toBone);
                snap.x = bone.x - dir.x * 0.04f;
                snap.y = bone.y - dir.y * 0.04f;
                snap.z = bone.z - dir.z * 0.04f;
            }
            CombatWriteEntityWorldPos(bullet, snap);
            CombatRememberGuide(bullet);
        }
    }

    for (int i = 0; i < curCount; i++)
        s_ChainPrev[i] = curBullets[i];
    s_ChainPrevCount = curCount;
}

static const int kMbChainMax = 24;

static int CombatCollectMbTargets(uintptr_t worldPtr, uintptr_t localPlayer, CombatTarget* out, int maxOut)
{
    if (!out || maxOut <= 0)
        return 0;
    if (!IsValidPtr(worldPtr) || !IsValidPtr(localPlayer))
        return 0;
    if (!g_LocalPlayerValid && !g_CameraValid)
        return 0;

    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
    float maxDist = (float)(g_MagicBulletMaxDistance > 10 ? g_MagicBulletMaxDistance : 10);
    int n = 0;

    auto consider = [&](uintptr_t entity) {
        if (n >= maxOut)
            return;
        if (!IsValidPtr(entity) || entity < 0x100000000 || entity == localPlayer)
            return;
        if (EntityIsDead(entity) || CombatIsFriendEntity(entity))
            return;
        if (!CombatIsMagicPlayerTarget(entity))
            return;

        Vec3 bone = {};
        if (!CombatGetMagicBone(entity, bone))
            return;
        float wdist = Distance3D(localPos, bone);
        if (wdist > maxDist || wdist != wdist)
            return;

        CombatTarget cand = {};
        cand.valid = true;
        cand.entity = entity;
        cand.bonePos = CombatLeadMagicBone(entity, bone, wdist);
        cand.worldDist = Distance3D(localPos, cand.bonePos);
        cand.isPlayer = true;

        int insert = n;
        while (insert > 0 && out[insert - 1].worldDist > cand.worldDist)
        {
            out[insert] = out[insert - 1];
            --insert;
        }
        out[insert] = cand;
        ++n;
    };

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
        int maxI = count > 250 ? 250 : count;
        for (int i = 0; i < maxI; i++)
            consider(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }
    return n;
}

static bool g_MbAutoFireHeld = false;

static void MagicBulletReleaseAutoFire()
{
    if (!g_MbAutoFireHeld)
        return;
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    INPUT inp = {};
    inp.type = INPUT_MOUSE;
    inp.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &inp, sizeof(INPUT));
    g_MbAutoFireHeld = false;
}

static void MagicBulletUpdateAutoFire(bool haveTarget)
{
    if (!g_MagicBullet || !g_MagicBulletAutoFire || !haveTarget
        || ImGuiMenu_IsOpen() || g_PanicHidden || g_MiscFreecam)
    {
        MagicBulletReleaseAutoFire();
        return;
    }

    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && !g_MbAutoFireHeld)
        return;

    if (!g_MbAutoFireHeld)
    {
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        INPUT inp = {};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &inp, sizeof(INPUT));
        g_MbAutoFireHeld = true;
    }
}

static void CombatDrawFovAndLock(const CombatTarget& aimLock, const CombatTarget& magicLock, bool aimKeyHeld)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    if ((g_AimbotDrawFov && g_AimbotEnabled) || (g_MagicBulletDrawFov && g_MagicBullet))
    {
        float cx = g_ScreenWidth * 0.5f;
        float cy = g_ScreenHeight * 0.5f;
        float r = g_MagicBullet && g_MagicBulletDrawFov
            ? (float)(g_MagicBulletFov > 10 ? g_MagicBulletFov : 10)
            : (float)(g_AimbotFov > 10 ? g_AimbotFov : 10);
        bool targetInFov = aimLock.valid || magicLock.valid;
        ImU32 col;
        if (g_FovHighlight.enabled)
        {
            const float* fc = targetInFov ? g_FovHighlight.colorActive : g_FovHighlight.colorIdle;
            col = ToCol(fc[0], fc[1], fc[2], fc[3]);
        }
        else
            col = aimKeyHeld ? IM_COL32(120, 220, 255, 170) : IM_COL32(180, 190, 200, 80);
        if (g_MagicBullet && g_MagicBulletDrawFov)
            col = magicLock.valid ? IM_COL32(255, 90, 90, 200) : IM_COL32(255, 140, 140, 90);
        dl->AddCircle(ImVec2(cx, cy), r, col, 64, 1.5f);
    }

    auto drawLock = [&](const CombatTarget& lock, ImU32 col, bool showLine) {
        if (!lock.valid) return;
        Vec2 scr;
        if (!WorldToScreen(lock.bonePos, scr)) return;
        dl->AddCircle(ImVec2(scr.x, scr.y), 7.f, col, 20, 2.2f);
        dl->AddCircleFilled(ImVec2(scr.x, scr.y), 2.2f, col, 8);
        if (showLine)
        {
            float cx = g_ScreenWidth * 0.5f;
            float cy = g_ScreenHeight * 0.5f;
            dl->AddLine(ImVec2(cx, cy), ImVec2(scr.x, scr.y), col, 1.4f);
        }
    };

    if (aimKeyHeld && g_AimbotEnabled)
        drawLock(aimLock, IM_COL32(80, 220, 255, 230), true);
    if (g_MagicBullet && magicLock.valid)
        drawLock(magicLock, IM_COL32(255, 70, 70, 220), false);
}

static bool LocalHoldingGrenade(uintptr_t localPlayer)
{
    uintptr_t hands = GetLocalHandsWeapon(localPlayer);
    if (!IsValidPtr(hands) || hands < 0x100000000)
        return false;
    if (EntityLooksLikeGrenade(hands))
        return true;
    char cfg[64] = {}, tn[64] = {};
    ReadEntityConfigName(hands, cfg, 64);
    ReadEntityTypeName(hands, tn, 64);
    // Narrow extras — do NOT match "flash" alone (Flashlight) or bare "smoke".
    if (StrContainsI(cfg, "rgd5") || StrContainsI(tn, "rgd5"))
        return true;
    if (StrContainsI(cfg, "rgo") || StrContainsI(tn, "rgo"))
        return true;
    if (StrContainsI(cfg, "m67") || StrContainsI(tn, "m67"))
        return true;
    if (StrContainsI(cfg, "flashgrenade") || StrContainsI(tn, "flashgrenade") ||
        StrContainsI(cfg, "flashbang") || StrContainsI(tn, "flashbang"))
        return true;
    if (StrContainsI(cfg, "smokegrenade") || StrContainsI(tn, "smokegrenade"))
        return true;
    return false;
}

// DayZ keeps the same Grenade_Base entity through the throw — track hands ptr
// so we don't depend on BulletList/near scans finding it after release.
static uintptr_t g_GrenadeTrackedEnt = 0;
static DWORD g_GrenadeTrackedUntil = 0;

static void GrenadeTpUpdateTracked(uintptr_t localPlayer, bool holding)
{
    if (holding)
    {
        uintptr_t hands = GetLocalHandsWeapon(localPlayer);
        if (IsValidPtr(hands) && hands > 0x100000000 && EntityLooksLikeGrenade(hands))
        {
            g_GrenadeTrackedEnt = hands;
            g_GrenadeTrackedUntil = GetTickCount() + 8000; // fuse window
        }
        return;
    }
    if (!g_GrenadeTrackedEnt)
        return;
    if ((int)(GetTickCount() - g_GrenadeTrackedUntil) > 0)
    {
        g_GrenadeTrackedEnt = 0;
        return;
    }
    if (!IsValidPtr(g_GrenadeTrackedEnt) || g_GrenadeTrackedEnt < 0x100000000 ||
        !EntityLooksLikeGrenade(g_GrenadeTrackedEnt))
    {
        g_GrenadeTrackedEnt = 0;
    }
}

static bool CombatReadEntityFeet(uintptr_t entity, Vec3& out)
{
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;
    uintptr_t vs = Read<uintptr_t>(entity + offsets::entity::VisualState);
    if (!IsValidPtr(vs) || vs < 0x100000000)
        return false;
    out = Read<Vec3>(vs + 0x2C);
    if (out.x != out.x || out.y != out.y || out.z != out.z)
        return false;
    if (fabsf(out.x) > 50000.f || fabsf(out.y) > 50000.f || fabsf(out.z) > 50000.f)
        return false;
    // Entity root often sits in terrain / mesh — lift to shin height so the
    // grenade can fuse/detonate above ground and register damage.
    out.y += 0.55f;
    return true;
}

// Client-only grenade snap (VS + FVS). Damage/explode is server-authoritative on
// vanilla DayZ — client memory cannot relocate server Explode() position.
static void CombatWriteGrenadeWorldPos(uintptr_t entity, const Vec3& pos)
{
    CombatWriteEntityWorldPos(entity, pos);
    if (!OakLab_IsWriteAllowed(OAK_LAB_GRENADE))
        return;
    uintptr_t fvs = Read<uintptr_t>(entity + offsets::entity::FutureVisualState);
    if (!IsValidPtr(fvs) || fvs < 0x100000000)
        return;
    OakLab_PushModule(OAK_LAB_GRENADE);
    OakLab_WriteVec3(OAK_LAB_GRENADE, fvs + 0x2C, pos.x, pos.y, pos.z, 0);
    OakLab_PopModule();
}

// Continuous VS snap of thrown grenades to lock feet. Prefers the tracked
// post-throw entity (same ptr that was in hands). Also scans BulletList +
// near/far/slow/item. Returns how many grenades were written this frame.
static int CombatSnapGrenadesToFeet(uintptr_t worldPtr, uintptr_t localPlayer, const Vec3& feetPos)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return 0;
    if (!OakLab_IsWriteAllowed(OAK_LAB_GRENADE))
        return 0;

    uintptr_t seen[64];
    int seenN = 0;
    int wrote = 0;
    auto snapOne = [&](uintptr_t ent) {
        if (!IsValidPtr(ent) || ent < 0x100000000 || ent == localPlayer)
            return;
        if (!EntityLooksLikeGrenade(ent))
            return;
        for (int i = 0; i < seenN; i++)
        {
            if (seen[i] == ent)
                return;
        }
        if (seenN < 64)
            seen[seenN++] = ent;
        CombatWriteGrenadeWorldPos(ent, feetPos);
        wrote++;
    };

    // Primary: the grenade we were holding when the throw started.
    if (g_GrenadeTrackedEnt)
        snapOne(g_GrenadeTrackedEnt);

    uintptr_t bulletData = 0;
    int bulletCount = 0;
    bool ptrArray = true;
    if (ResolveBulletArray(worldPtr, bulletData, bulletCount, ptrArray))
    {
        int n = bulletCount < 64 ? bulletCount : 64;
        for (int i = 0; i < n; i++)
        {
            uintptr_t bullet = ptrArray
                ? Read<uintptr_t>(bulletData + (uintptr_t)i * 8)
                : bulletData + (uintptr_t)i * 0x100;
            snapOne(bullet);
        }
    }

    const uintptr_t nearFar[] = { oak_offsets::world::NearEntList, oak_offsets::world::FarEntList };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = 0;
        int count = 0;
        if (!ResolveEntityList(worldPtr, nearFar[li], 2000, data, count, nullptr))
            continue;
        int maxN = count < 200 ? count : 200;
        for (int i = 0; i < maxN; i++)
            snapOne(Read<uintptr_t>(data + (uintptr_t)i * 8));
    }

    uintptr_t slowLists[] = { oak_offsets::world::SlowEntList, oak_offsets::world::ItemList };
    uintptr_t slowCounts[] = { oak_offsets::world::SlowTableSize, oak_offsets::world::ItemListSize };
    for (int li = 0; li < 2; li++)
    {
        uintptr_t data = Read<uintptr_t>(worldPtr + slowLists[li]);
        int count = Read<int>(worldPtr + slowCounts[li]);
        if (!IsValidPtr(data) || data < 0x100000000 || count <= 0 || count > 2000)
            continue;
        int maxN = count < 200 ? count : 200;
        for (int i = 0; i < maxN; i++)
        {
            uintptr_t entry = data + (uintptr_t)i * 0x18;
            if ((Read<WORD>(entry) & 0xFFFF) != 1)
                continue;
            snapOne(Read<uintptr_t>(entry + 0x8));
        }
    }

    return wrote;
}

static void CombatDrawGrenadeTeleport(const CombatTarget& lock, bool holding)
{
    if (!g_GrenadeTeleport.enabled)
        return;
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    float cx = g_ScreenWidth * 0.5f;
    float cy = g_ScreenHeight * 0.5f;
    float r = (float)(g_GrenadeTeleport.fovPx > 10 ? g_GrenadeTeleport.fovPx : 10);

    // FOV while module is on (so enable is visible immediately). Blue ring when locked.
    if (g_GrenadeTeleport.drawFov)
    {
        ImU32 col = lock.valid ? IM_COL32(60, 140, 255, 220)
            : (holding ? IM_COL32(60, 140, 255, 160) : IM_COL32(100, 140, 200, 100));
        dl->AddCircle(ImVec2(cx, cy), r, col, 64, 1.8f);
    }

    if (lock.valid)
    {
        Vec2 scr;
        if (WorldToScreen(lock.bonePos, scr))
        {
            ImU32 col = IM_COL32(60, 140, 255, 230);
            dl->AddCircle(ImVec2(scr.x, scr.y), 7.f, col, 20, 2.2f);
            dl->AddCircleFilled(ImVec2(scr.x, scr.y), 2.2f, col, 8);
        }
    }
}

static void UpdateCombatAim(uintptr_t worldPtr, uintptr_t localPlayer)
{
    __try
    {
        static uintptr_t s_LastLocal = 0;
        static uintptr_t s_LastWorld = 0;
        if (localPlayer != s_LastLocal || worldPtr != s_LastWorld)
        {
            // Respawn / character swap / server rejoin — drop sticky locks
            s_LastLocal = localPlayer;
            s_LastWorld = worldPtr;
            g_CombatLock = {};
            g_MagicLock = {};
            g_GrenadeLock = {};
            g_GrenadeTrackedEnt = 0;
            for (int i = 0; i < kMaxLocalBulletMarks; i++)
                g_LocalBulletMarks[i].used = false;
            for (int i = 0; i < 64; i++)
            {
                g_SnappedBullets[i] = 0;
                g_SnapGuides[i] = 0;
            }
        }

        const bool grenadeTpOn = g_GrenadeTeleport.enabled;
        if (!g_AimbotEnabled && !g_MagicBullet && !grenadeTpOn)
        {
            g_CombatLock = {};
            g_MagicLock = {};
            g_GrenadeLock = {};
            g_CombatDrawAim = {};
            g_CombatDrawMb = {};
            g_CombatDrawGrenade = {};
            g_CombatDrawAimHeld = false;
            g_CombatDrawHoldingGrenade = false;
            // Magic bullet may have temporarily rewritten AmmoType — put it back
            if (!g_FastBullets && !g_NoDispersion && !g_PerfectBallistics && !g_SilentAim.enabled)
                RestoreAmmoBackup();
            return;
        }
        if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
            return;
        if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
            return;
        // Do NOT gate on EntityIsDead(localPlayer) — IsDead@0xE2 false-positives on living
        // locals and was aborting aimbot + magic bullet every frame while ammo mods still ran.

        float fovPx = (float)(g_MagicBullet
            ? (g_MagicBulletFov > 10 ? g_MagicBulletFov : 10)
            : (g_AimbotFov > 10 ? g_AimbotFov : 10));
        int aimVk = g_BindAimAssist != 0 ? g_BindAimAssist
            : (g_AimbotB4.aimAssistKey != 0 ? g_AimbotB4.aimAssistKey : g_AimbotKey);
        bool aimKeyHeld = aimVk > 0 && (GetAsyncKeyState(aimVk) & 0x8000) != 0;

        CombatTarget aimTarget = {};
        if (g_AimbotEnabled && aimKeyHeld)
        {
            if (CombatTargetStillValid(localPlayer, g_CombatLock, fovPx, true, false) &&
                CombatAimLosOk(worldPtr, localPlayer, g_CombatLock))
                aimTarget = g_CombatLock;
            else if (CombatFindBestTarget(worldPtr, localPlayer, aimTarget, false, false))
            {
                if (CombatAimLosOk(worldPtr, localPlayer, aimTarget))
                    g_CombatLock = aimTarget;
                else
                    g_CombatLock = {};
            }
            else
                g_CombatLock = {};

            if (aimTarget.valid && !g_MiscFreecam)
            {
                if (CombatAimLosOk(worldPtr, localPlayer, aimTarget) &&
                    CombatAimReactionReady(aimTarget))
                {
                    // Mouse only — camera-matrix aim fights mouse (especially under ADS optics) and shakes
                    CombatApplyMouseAim(aimTarget.bonePos);
                }
            }
        }
        else if (!aimKeyHeld)
        {
            g_CombatLock = {};
            CombatResetMouseAimState();
        }

        CombatTarget mbTarget = {};
        if (g_MagicBullet && OakLab_IsWriteAllowed(OAK_LAB_MB))
        {
            bool firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            auto polishBone = [&](CombatTarget& t) {
                if (!t.valid) return;
                Vec3 bone;
                if (CombatGetMagicBone(t.entity, bone))
                {
                    t.bonePos = CombatLeadMagicBone(t.entity, bone, t.worldDist);
                    Vec3 localPos = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
                    t.worldDist = Distance3D(localPos, t.bonePos);
                }
            };

            CombatTarget chain[kMbChainMax];
            int chainCount = 0;
            if (g_MagicBulletChain || g_MagicBulletAutoFire)
                chainCount = CombatCollectMbTargets(worldPtr, localPlayer, chain, kMbChainMax);

            for (int ci = 0; ci < chainCount; ci++)
                polishBone(chain[ci]);

            // While firing: keep lock even off-FOV, else fall back to nearest player.
            if (firing && CombatTargetStillValid(localPlayer, g_MagicLock, fovPx, false, true))
            {
                mbTarget = g_MagicLock;
                polishBone(mbTarget);
                g_MagicLock = mbTarget;
            }
            else if (chainCount > 0)
            {
                mbTarget = chain[0];
                g_MagicLock = mbTarget;
            }
            else if (CombatFindBestTarget(worldPtr, localPlayer, mbTarget, firing /*offscreen OK while shooting*/, true))
            {
                polishBone(mbTarget);
                g_MagicLock = mbTarget;
            }
            else if (firing && g_MagicLock.valid)
            {
                mbTarget = g_MagicLock;
                polishBone(mbTarget);
            }
            else
            {
                g_MagicLock = {};
            }

            const bool anyTarget = mbTarget.valid || chainCount > 0;
            MagicBulletUpdateAutoFire(anyTarget);
            firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            if (g_MagicBulletChain && chainCount > 0 && firing)
            {
                CombatBoostAmmoSpeed(localPlayer, chain[0].worldDist);
                CombatSnapLocalBulletsThroughTargets(worldPtr, localPlayer, chain, chainCount);
            }
            else if (mbTarget.valid)
            {
                CombatBoostAmmoSpeed(localPlayer, mbTarget.worldDist);
                CombatSnapLocalBulletsToBone(worldPtr, localPlayer, mbTarget.bonePos);
            }
            else if (firing)
            {
                static int s_NoLock = 0;
                if ((s_NoLock++ % 300) == 0)
                    Log("mb: FIRING WITH NO LOCK - put target in FOV once to acquire");
            }

            static uintptr_t s_LastMbEnt = 0;
            static int s_MbRetargets = 0;
            static int s_MbFrames = 0;
            s_MbFrames++;
            if (mbTarget.valid && mbTarget.entity != s_LastMbEnt)
            {
                if (s_LastMbEnt) s_MbRetargets++;
                s_LastMbEnt = mbTarget.entity;
            }
            if (false && (s_MbFrames % 300) == 0)
            {
                char b[160];
                wsprintfA(b, "verify[mb] retargets=%d lock=%d dist=%d fovScore=%d rangeCap=%d sticky=%d",
                    s_MbRetargets, mbTarget.valid ? 1 : 0,
                    mbTarget.valid ? (int)mbTarget.worldDist : -1,
                    mbTarget.valid ? (int)mbTarget.screenDist : -1,
                    g_MagicBulletMaxDistance,
                    firing ? 1 : 0);
                Log(b);
                s_MbRetargets = 0;
            }
        }
        else
        {
            g_MagicLock = {};
            MagicBulletReleaseAutoFire();
        }

        // Grenade Teleporter: sticky lock while holding; snap thrown to feet.
        bool holdingGrenade = false;
        if (grenadeTpOn)
        {
            static int s_GrenadeIdleFrames = 0;
            holdingGrenade = LocalHoldingGrenade(localPlayer);
            GrenadeTpUpdateTracked(localPlayer, holdingGrenade);

            const int savedFov = g_AimbotFov;
            const int savedMbDist = g_MagicBulletMaxDistance;
            const int savedAimDist = g_AimbotMaxDistance;
            const bool savedAimP = g_AimbotPlayers;
            const bool savedAimZ = g_AimbotZombies;
            const int savedAimPri = g_AimPriority;

            const bool wantP = g_GrenadeTeleport.players;
            const bool wantZ = g_GrenadeTeleport.zombies;
            const bool playersOnly = wantP && !wantZ;
            const int rangeM = g_GrenadeTeleport.maxDistanceM > 10
                ? g_GrenadeTeleport.maxDistanceM : 10;

            g_AimbotFov = g_GrenadeTeleport.fovPx > 10 ? g_GrenadeTeleport.fovPx : 10;
            g_MagicBulletMaxDistance = rangeM;
            g_AimbotMaxDistance = rangeM;
            g_AimbotPlayers = wantP;
            g_AimbotZombies = wantZ;
            // Always crosshair-closest (same scorer MB uses), ignore Aimbot distance priority.
            g_AimPriority = OakAimPriCrosshair;
            float tpFov = (float)g_AimbotFov;

            if (!wantP && !wantZ)
            {
                g_GrenadeLock = {};
            }
            else if (holdingGrenade)
            {
                s_GrenadeIdleFrames = 0;
                // Re-pick every frame while aiming the throw — follow cursor like MB.
                CombatTarget tpTarget = {};
                if (CombatFindBestTarget(worldPtr, localPlayer, tpTarget, false, playersOnly))
                    g_GrenadeLock = tpTarget;
                else
                    g_GrenadeLock = {};
            }
            else if (g_GrenadeLock.valid)
            {
                // Keep last lock after throw so in-flight grenades still snap to feet.
                if (!CombatTargetStillValid(localPlayer, g_GrenadeLock, tpFov, false, playersOnly))
                    g_GrenadeLock = {};
            }

            g_AimbotFov = savedFov;
            g_MagicBulletMaxDistance = savedMbDist;
            g_AimbotMaxDistance = savedAimDist;
            g_AimbotPlayers = savedAimP;
            g_AimbotZombies = savedAimZ;
            g_AimPriority = savedAimPri;

            // Snap only after release — don't fight the held item in inventory.
            const bool labOk = OakLab_IsWriteAllowed(OAK_LAB_GRENADE) != 0;
            int snapped = 0;
            if (!holdingGrenade && g_GrenadeLock.valid && labOk)
            {
                Vec3 feet = {};
                if (CombatReadEntityFeet(g_GrenadeLock.entity, feet))
                {
                    snapped = CombatSnapGrenadesToFeet(worldPtr, localPlayer, feet);
                    if (snapped > 0)
                        s_GrenadeIdleFrames = 0;
                    else if (++s_GrenadeIdleFrames > 120)
                    {
                        g_GrenadeLock = {};
                        g_GrenadeTrackedEnt = 0;
                    }
                }
            }
            else if (!holdingGrenade && !g_GrenadeTrackedEnt)
            {
                s_GrenadeIdleFrames = 0;
            }

            static int s_GrenadeTpLog = 0;
            if ((s_GrenadeTpLog++ % 90) == 0)
            {
                char b[192];
                wsprintfA(b, "grenadeTp: hold=%d lock=%d track=%d lab=%d snap=%d fov=%d",
                    holdingGrenade ? 1 : 0, g_GrenadeLock.valid ? 1 : 0,
                    g_GrenadeTrackedEnt ? 1 : 0, labOk ? 1 : 0, snapped,
                    g_GrenadeTeleport.fovPx);
                Log(b);
            }
        }
        else
        {
            g_GrenadeLock = {};
            g_GrenadeTrackedEnt = 0;
        }

        // Snapshot for Present-thread draw (this function runs before ImGui NewFrame).
        g_CombatDrawAim = aimTarget.valid ? aimTarget : g_CombatLock;
        g_CombatDrawMb = mbTarget.valid ? mbTarget : g_MagicLock;
        g_CombatDrawGrenade = g_GrenadeLock;
        g_CombatDrawAimHeld = aimKeyHeld;
        g_CombatDrawHoldingGrenade = holdingGrenade;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Keep locks — wiping every AV frame made combat look permanently dead.
        static int s_ExLog = 0;
        if ((s_ExLog++ % 120) == 0)
            Log("combat: UpdateCombatAim exception (kept locks)");
    }
}

// Call from Present AFTER ImGuiMenu_BeginFrame — draws before NewFrame are wiped.
static void CombatPresentDrawOverlays()
{
    __try
    {
        if (g_ScreenWidth < 64 || g_ScreenHeight < 64)
            return;
        CombatDrawFovAndLock(g_CombatDrawAim, g_CombatDrawMb, g_CombatDrawAimHeld);
        CombatDrawGrenadeTeleport(g_CombatDrawGrenade, g_CombatDrawHoldingGrenade);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static int s_DrawEx = 0;
        if ((s_DrawEx++ % 120) == 0)
            Log("combat: PresentDrawOverlays exception");
    }
}
