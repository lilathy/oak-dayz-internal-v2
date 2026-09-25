// Shared inventory / cargo readers for container ESP + player threat gear.
// Layout (UC 1.28 + Oak live): InventoryItem::ItemInventory 0x658,
// ItemInventory::CargoGrid 0x148, CargoGrid::ItemList 0x38,
// DayZPlayerInventory::Hands 0x1B0, Clothing 0x150.

#ifndef OAK_INVENTORY_IMPL_INL
#define OAK_INVENTORY_IMPL_INL

enum { kOakInvMaxLines = 8 };

static bool OakInvIsJunkClassName(const char* nm)
{
    if (!nm || !nm[0]) return true;
    if (StrCmpI(nm, "InventoryItem") == 0) return true;
    if (StrCmpI(nm, "Inventory_Base") == 0) return true;
    if (StrCmpI(nm, "ItemBase") == 0) return true;
    if (StrCmpI(nm, "Clothing") == 0) return true;
    if (StrCmpI(nm, "Clothing_Base") == 0) return true;
    if (StrCmpI(nm, "Weapon") == 0) return true;
    if (StrCmpI(nm, "Weapon_Base") == 0) return true;
    if (StrCmpI(nm, "Rifle_Base") == 0) return true;
    if (StrCmpI(nm, "Pistol_Base") == 0) return true;
    if (StrCmpI(nm, "EntityAI") == 0) return true;
    if (StrCmpI(nm, "Entity") == 0) return true;
    if (StrCmpI(nm, "Object") == 0) return true;
    if (StrCmpI(nm, "Man") == 0) return true;
    if (StrCmpI(nm, "DayZPlayer") == 0) return true;
    if (StrCmpI(nm, "SurvivorBase") == 0) return true;
    if (StrCmpI(nm, "Container_Base") == 0) return true;
    if (StrCmpI(nm, "ItemContainer") == 0) return true;
    if (StrContainsI(nm, "InventoryItem")) return true;
    if (StrContainsI(nm, "ProxyMagazine") || StrContainsI(nm, "ProxyWeapon")) return true;
    if (lstrlenA(nm) < 3) return true;
    return false;
}

static bool OakInvLooksLikeItemName(const char* nm)
{
    if (!nm || !nm[0]) return false;
    if (OakInvIsJunkClassName(nm)) return false;
    int n = 0;
    for (const char* p = nm; *p && n < 64; p++, n++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) return false;
    }
    if (n < 3 || n > 48) return false;
    return true;
}

// Prefer ConfigName (concrete class), then TypeName. Reject engine base classes.
static bool OakInvReadItemName(uintptr_t item, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(item) || item < 0x100000000ULL) return false;

    char cfg[64] = {};
    char tn[64] = {};
    // Use the same proven readers as loot ESP.
    ReadEntityConfigName(item, cfg, 64);
    ReadEntityTypeName(item, tn, 64);

    if (OakInvLooksLikeItemName(cfg))
    {
        lstrcpynA(out, cfg, outMax);
        return true;
    }
    if (OakInvLooksLikeItemName(tn))
    {
        lstrcpynA(out, tn, outMax);
        return true;
    }
    return false;
}

static bool OakInvIsPlateCarrier(const char* nm)
{
    if (!nm || !nm[0]) return false;
    if (StrContainsI(nm, "PlateCarrier")) return true;
    if (StrContainsI(nm, "PressVest")) return true;
    if (StrContainsI(nm, "UKAssVest")) return true;
    if (StrContainsI(nm, "PoliceVest")) return true;
    if (StrContainsI(nm, "TacticalVest") && !StrContainsI(nm, "Pouch")) return true;
    if (StrContainsI(nm, "Vest") &&
        (StrContainsI(nm, "Plate") || StrContainsI(nm, "Ballistic") ||
         StrContainsI(nm, "Carrier") || StrContainsI(nm, "Press") ||
         StrContainsI(nm, "Assault") || StrContainsI(nm, "UKAss")))
        return true;
    return false;
}

static bool OakInvIsThreatHelmet(const char* nm)
{
    if (!nm || !nm[0]) return false;
    if (StrContainsI(nm, "BaseballCap") || StrContainsI(nm, "Beanie") ||
        StrContainsI(nm, "Ushanka") || StrContainsI(nm, "CowboyHat") ||
        StrContainsI(nm, "FlatCap") || StrContainsI(nm, "RadarCap") ||
        StrContainsI(nm, "Zmijovka") || StrContainsI(nm, "Boonie"))
        return false;
    if (StrContainsI(nm, "BallisticHelmet")) return true;
    if (StrContainsI(nm, "Mich2001") || StrContainsI(nm, "Mich")) return true;
    if (StrContainsI(nm, "GorkaHelmet")) return true;
    if (StrContainsI(nm, "TankerHelmet")) return true;
    if (StrContainsI(nm, "FirefightersHelmet")) return true;
    if (StrContainsI(nm, "ConstructionHelmet")) return true;
    if (StrContainsI(nm, "DirtBikeHelmet")) return true;
    if (StrContainsI(nm, "MotorcycleHelmet")) return true;
    if (StrContainsI(nm, "GreatHelm") || StrContainsI(nm, "NorseHelm")) return true;
    if (StrContainsI(nm, "Helmet")) return true;
    return false;
}

static bool OakInvIsBackpack(const char* nm)
{
    if (!nm || !nm[0]) return false;
    if (StrContainsI(nm, "Pouch")) return false;
    if (StrContainsI(nm, "Bag")) return true; // AssaultBag, AliceBag, TaloonBag, …
    if (StrContainsI(nm, "Coyote")) return true;
    if (StrContainsI(nm, "Tortilla")) return true;
    if (StrContainsI(nm, "Drybag") || StrContainsI(nm, "DryBag")) return true;
    if (StrContainsI(nm, "MountainBag")) return true;
    if (StrContainsI(nm, "HuntingBag")) return true;
    if (StrContainsI(nm, "ChildBag")) return true;
    if (StrContainsI(nm, "CourierBag")) return true;
    if (StrContainsI(nm, "ImprovisedBag")) return true;
    return false;
}

static bool OakInvPushUnique(char lines[][48], int maxLines, int* found, const char* nm)
{
    if (!lines || !found || !nm || !nm[0] || *found >= maxLines) return false;
    for (int d = 0; d < *found; d++)
        if (StrCmpI(lines[d], nm) == 0) return false;
    lstrcpynA(lines[*found], nm, 48);
    (*found)++;
    return true;
}

// CargoGrid → ItemList array (UC: CargoGrid::ItemList = 0x38).
static int OakInvCollectFromCargoObj(uintptr_t cargo, char lines[][48], int maxLines, int found)
{
    if (!lines || maxLines <= 0 || found >= maxLines) return found;
    if (!IsValidPtr(cargo) || cargo < 0x100000000ULL) return found;

    __try
    {
        // Primary UC path first, then nearby count/array layouts.
        const uintptr_t arrOffs[] = { 0x38, 0x30, 0x40, 0x48, 0x18, 0x20 };
        const uintptr_t cntOffs[] = { 0x44, 0x40, 0x3C, 0x48, 0x50, 0x28 };
        for (int ai = 0; ai < 6 && found < maxLines; ai++)
        {
            uintptr_t arr = Read<uintptr_t>(cargo + arrOffs[ai]);
            if (!IsValidPtr(arr) || arr < 0x100000000ULL) continue;

            int cnt = Read<int>(cargo + cntOffs[ai]);
            if (cnt <= 0 || cnt > 80)
                cnt = Read<int>(arr + 8); // Enfusion array {data,size}
            if (cnt <= 0 || cnt > 80)
                cnt = Read<int>(cargo + arrOffs[ai] + 8);

            uintptr_t data = arr;
            // If arr looks like { ptr, count } wrapper, unwrap.
            {
                uintptr_t maybe = Read<uintptr_t>(arr);
                char test[48] = {};
                if (IsValidPtr(maybe) && maybe > 0x100000000ULL &&
                    !OakInvReadItemName(arr, test, 48) &&
                    OakInvReadItemName(maybe, test, 48))
                {
                    data = arr; // flat EntityAI*
                }
                else if (IsValidPtr(maybe) && maybe > 0x100000000ULL)
                {
                    int ac = Read<int>(arr + 8);
                    if (ac > 0 && ac <= 80 && OakInvReadItemName(maybe, test, 48))
                    {
                        data = maybe;
                        cnt = ac;
                    }
                    else if (OakInvReadItemName(maybe, test, 48))
                    {
                        // Treat as flat array starting at arr
                        data = arr;
                    }
                }
            }
            if (cnt <= 0 || cnt > 80) continue;

            int hits = 0;
            int lim = cnt > 24 ? 24 : cnt;
            for (int i = 0; i < lim && found < maxLines; i++)
            {
                uintptr_t item = Read<uintptr_t>(data + (uintptr_t)i * 8);
                if (!IsValidPtr(item) || item < 0x100000000ULL) continue;
                char nm[48] = {};
                if (!OakInvReadItemName(item, nm, 48)) continue;
                if (OakInvPushUnique(lines, maxLines, &found, nm))
                    hits++;
            }
            if (hits > 0)
                return found;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// Try to read an EntityAI* array at `arr` with count `cnt`.
static int OakInvTryItemArray(uintptr_t arr, int cnt, char lines[][48], int maxLines, int found)
{
    if (!IsValidPtr(arr) || arr < 0x100000000ULL) return found;
    if (cnt <= 0 || cnt > 64) return found;
    __try
    {
        for (int i = 0; i < cnt && found < maxLines; i++)
        {
            uintptr_t item = Read<uintptr_t>(arr + (uintptr_t)i * 8);
            char nm[48] = {};
            if (!OakInvReadItemName(item, nm, 48)) continue;
            if (IsContainerName(nm)) continue;
            OakInvPushUnique(lines, maxLines, &found, nm);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// Wide CargoGrid walker — live layout (2026-08): ItemList @ +0x60, counts near +0x58.
// UC 0x38 is null on this build.
static int OakInvCollectFromCargoObjWide(uintptr_t cargo, char lines[][48], int maxLines, int found)
{
    if (!IsValidPtr(cargo) || cargo < 0x100000000ULL) return found;
    int before = found;
    found = OakInvCollectFromCargoObj(cargo, lines, maxLines, found);
    if (found > before) return found;

    __try
    {
        // Hot path from live Barrel_Red dump
        {
            uintptr_t arr60 = Read<uintptr_t>(cargo + 0x60);
            int c58 = Read<int>(cargo + 0x58);
            int c5C = Read<int>(cargo + 0x5C);
            if (IsValidPtr(arr60) && arr60 > 0x100000000ULL)
            {
                // Prefer a generous scan — early return used to miss later slots.
                int n2 = OakInvTryItemArray(arr60, 24, lines, maxLines, found);
                if (n2 > found) found = n2;
                if (c58 > 0 && c58 <= 48)
                    found = OakInvTryItemArray(arr60, c58, lines, maxLines, found);
                if (c5C > 0 && c5C <= 48)
                    found = OakInvTryItemArray(arr60, c5C, lines, maxLines, found);
                // arr60 may be { data, size }
                uintptr_t data = Read<uintptr_t>(arr60);
                int ac = Read<int>(arr60 + 8);
                if (IsValidPtr(data) && data > 0x100000000ULL && ac > 0 && ac <= 48)
                    found = OakInvTryItemArray(data, ac, lines, maxLines, found);
                // If ItemList@0x60 produced names, stop — further offset fishing adds junk.
                if (found > before) return found;
            }
        }

        for (uintptr_t off = 0x08; off <= 0x80 && found < maxLines; off += 8)
        {
            if (off == 0x28) continue; // owner back-ptr to container
            uintptr_t arr = Read<uintptr_t>(cargo + off);
            if (!IsValidPtr(arr) || arr < 0x100000000ULL) continue;
            // Skip game-module vtables
            if (g_GameModule)
            {
                uintptr_t mod = (uintptr_t)g_GameModule;
                if (arr >= mod && arr < mod + 0x5000000) continue;
            }

            for (int d = -8; d <= 16; d += 4)
            {
                if ((int)off + d < 0) continue;
                int cnt = Read<int>(cargo + off + (uintptr_t)d);
                if (cnt <= 0 || cnt > 48) continue;
                found = OakInvTryItemArray(arr, cnt, lines, maxLines, found);
                if (found >= maxLines) return found;
            }

            {
                uintptr_t data = Read<uintptr_t>(arr);
                int cnt = Read<int>(arr + 8);
                if (IsValidPtr(data) && data > 0x100000000ULL && cnt > 0 && cnt <= 48)
                {
                    found = OakInvTryItemArray(data, cnt, lines, maxLines, found);
                    if (found >= maxLines) return found;
                }
            }

            char nm0[48] = {};
            uintptr_t first = Read<uintptr_t>(arr);
            if (OakInvReadItemName(first, nm0, 48))
            {
                found = OakInvTryItemArray(arr, 12, lines, maxLines, found);
                if (found >= maxLines) return found;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

static int OakInvCollectFromInventory(uintptr_t inv, char lines[][48], int maxLines, int found)
{
    if (!IsValidPtr(inv) || inv < 0x100000000ULL) return found;
    __try
    {
        // ItemInventory::CargoGrid @ 0x148 (UC) — wide walker for drifted ItemList offs
        {
            uintptr_t cargo = Read<uintptr_t>(inv + 0x148);
            found = OakInvCollectFromCargoObjWide(cargo, lines, maxLines, found);
            if (found >= maxLines) return found;
        }

        // Some builds expose grid/list near Clothing-ish slots
        {
            uintptr_t grid = Read<uintptr_t>(inv + 0x150);
            int cnt = Read<int>(inv + 0x15C);
            if (IsValidPtr(grid) && grid > 0x100000000ULL && cnt > 0 && cnt <= 80)
            {
                for (int i = 0; i < cnt && found < maxLines; i++)
                {
                    uintptr_t item = Read<uintptr_t>(grid + (uintptr_t)i * 8);
                    if (!IsValidPtr(item)) continue;
                    char nm[48] = {};
                    if (!OakInvReadItemName(item, nm, 48)) continue;
                    OakInvPushUnique(lines, maxLines, &found, nm);
                }
                found = OakInvCollectFromCargoObjWide(grid, lines, maxLines, found);
                if (found >= maxLines) return found;
            }
        }

        for (uintptr_t off = 0x130; off <= 0x190 && found < maxLines; off += 8)
        {
            if (off == 0x148 || off == 0x150) continue;
            uintptr_t p = Read<uintptr_t>(inv + off);
            if (!IsValidPtr(p) || p < 0x100000000ULL) continue;
            found = OakInvCollectFromCargoObjWide(p, lines, maxLines, found);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// Brute harvest: any concrete item name under a root object (inv / cargo / entity).
static int OakInvHarvestNamesUnder(uintptr_t root, uintptr_t offLo, uintptr_t offHi,
    char lines[][48], int maxLines, int found)
{
    if (!IsValidPtr(root) || root < 0x100000000ULL) return found;
    if (offHi < offLo) return found;
    __try
    {
        for (uintptr_t off = offLo; off <= offHi && found < maxLines; off += 8)
        {
            uintptr_t p = Read<uintptr_t>(root + off);
            if (!IsValidPtr(p) || p < 0x100000000ULL) continue;
            char nm[48] = {};
            if (OakInvReadItemName(p, nm, 48))
            {
                // Skip the container itself-ish names
                if (IsContainerName(nm)) continue;
                OakInvPushUnique(lines, maxLines, &found, nm);
                continue;
            }
            // One level nested (CargoGrid → entries)
            for (uintptr_t nOff = 0x20; nOff <= 0x60 && found < maxLines; nOff += 8)
            {
                uintptr_t q = Read<uintptr_t>(p + nOff);
                if (!IsValidPtr(q) || q < 0x100000000ULL) continue;
                if (!OakInvReadItemName(q, nm, 48)) continue;
                if (IsContainerName(nm)) continue;
                OakInvPushUnique(lines, maxLines, &found, nm);
            }
            // Flat array at p[0..N)
            int cntGuess = Read<int>(p + 8);
            if (cntGuess > 0 && cntGuess <= 32)
            {
                uintptr_t data = Read<uintptr_t>(p);
                if (!IsValidPtr(data) || data < 0x100000000ULL)
                    data = p;
                for (int i = 0; i < cntGuess && found < maxLines; i++)
                {
                    uintptr_t item = Read<uintptr_t>(data + (uintptr_t)i * 8);
                    if (!OakInvReadItemName(item, nm, 48)) continue;
                    if (IsContainerName(nm)) continue;
                    OakInvPushUnique(lines, maxLines, &found, nm);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// World ptr for parent-scan fallback (set each ESP frame).
static uintptr_t g_OakInvWorld = 0;
static void OakInvSetWorld(uintptr_t worldPtr) { g_OakInvWorld = worldPtr; }

// Find items in ItemList whose HierarchyParent (or nearby ptr) == container.
static int OakInvCollectByParentScan(uintptr_t container, char lines[][48], int maxLines, int found)
{
    if (!IsValidPtr(container) || !IsValidPtr(g_OakInvWorld)) return found;
    __try
    {
        uintptr_t data = Read<uintptr_t>(g_OakInvWorld + oak_offsets::world::ItemList);
        int count = Read<int>(g_OakInvWorld + oak_offsets::world::ItemListSize);
        if (!IsValidPtr(data) || count <= 0) return found;
        if (count > 4000) count = 4000;

        // HierarchyParent is typically near inventory/network region on EntityAI.
        const uintptr_t parentOffs[] = {
            0x318, 0x320, 0x328, 0x330, 0x338, 0x340, 0x348, 0x350,
            0x358, 0x360, 0x368, 0x370, 0x378, 0x380, 0x388, 0x390,
            0x398, 0x3A0, 0x3A8, 0x3B0, 0x3B8, 0x3C0, 0x3C8, 0x3D0,
            0x4E0, 0x4E8, 0x4F0, 0x4F8, 0x500, 0x508, 0x510, 0x518,
            0x620, 0x628, 0x630, 0x638, 0x640, 0x648, 0x650, 0x658
        };

        int step = count > 1200 ? 2 : 1;
        for (int i = 0; i < count && found < maxLines; i += step)
        {
            uintptr_t item = Read<uintptr_t>(data + (uintptr_t)i * 8);
            if (!IsValidPtr(item) || item == container) continue;
            bool child = false;
            for (int pi = 0; pi < (int)(sizeof(parentOffs) / sizeof(parentOffs[0])); pi++)
            {
                if (Read<uintptr_t>(item + parentOffs[pi]) == container)
                {
                    child = true;
                    break;
                }
            }
            if (!child) continue;
            char nm[48] = {};
            if (!OakInvReadItemName(item, nm, 48)) continue;
            if (IsContainerName(nm)) continue;
            OakInvPushUnique(lines, maxLines, &found, nm);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

static int OakInvCollectEntityCargo(uintptr_t entity, char lines[][48], int maxLines)
{
    if (!lines || maxLines <= 0) return 0;
    if (!IsValidPtr(entity) || entity < 0x100000000ULL) return 0;
    int found = 0;
    __try
    {
        // Pick the single richest inventory pointer — merging 0x650+0x658 mixed
        // real cargo with junk (e.g. Mag_*) and made labels flicker.
        const uintptr_t invOffs[] = { 0x658, 0x650, 0x660, 0x648, 0x668, 0x670 };
        char best[64][48] = {};
        int bestN = 0;
        const int bestCap = (maxLines < 64) ? maxLines : 64;
        for (int i = 0; i < 6; i++)
        {
            uintptr_t inv = Read<uintptr_t>(entity + invOffs[i]);
            if (!IsValidPtr(inv) || inv < 0x100000000ULL) continue;
            char tmp[64][48] = {};
            int n = OakInvCollectFromInventory(inv, tmp, bestCap, 0);
            if (n > bestN)
            {
                bestN = n;
                for (int k = 0; k < n; k++)
                    lstrcpynA(best[k], tmp[k], 48);
            }
        }
        for (int k = 0; k < bestN && found < maxLines; k++)
            OakInvPushUnique(lines, maxLines, &found, best[k]);

        // Parent scan only as last resort — false-positives nearby ground loot.
        if (found == 0)
            found = OakInvCollectByParentScan(entity, lines, maxLines, found);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// One-shot layout dump for live cargo offset discovery.
static void OakInvDumpCargoLayout(uintptr_t entity, const char* tag)
{
    if (!IsValidPtr(entity)) return;
    static int s_Dumps = 0;
    if (s_Dumps >= 0) return; // set >= 2 to re-enable live layout dumps
    __try
    {
        char entNm[64] = {};
        ReadEntityTypeName(entity, entNm, 64);
        char hdr[160];
        wsprintfA(hdr, "inv: dump#%d '%s' tag=%s ent=%p",
            s_Dumps, entNm[0] ? entNm : "?", tag ? tag : "-", (void*)entity);
        Log(hdr);

        const uintptr_t invOffs[] = { 0x650, 0x658, 0x660, 0x648 };
        for (int i = 0; i < 4; i++)
        {
            uintptr_t inv = Read<uintptr_t>(entity + invOffs[i]);
            if (!IsValidPtr(inv) || inv < 0x100000000ULL) continue;
            char b[220];
            wsprintfA(b, "inv: dump ent+0x%X -> inv=%p", (unsigned)invOffs[i], (void*)inv);
            Log(b);

            uintptr_t cargo = Read<uintptr_t>(inv + 0x148);
            if (!IsValidPtr(cargo) || cargo < 0x100000000ULL)
            {
                Log("inv: dump cargo null @ inv+0x148");
                continue;
            }
            wsprintfA(b, "inv: dump cargo=%p", (void*)cargo);
            Log(b);

            // Hex-ish pointer map of CargoGrid
            for (uintptr_t off = 0x00; off <= 0x78; off += 8)
            {
                uintptr_t p = Read<uintptr_t>(cargo + off);
                int i0 = Read<int>(cargo + off);
                int i4 = Read<int>(cargo + off + 4);
                char nm[48] = {};
                char nm1[48] = {};
                if (IsValidPtr(p) && p > 0x100000000ULL)
                {
                    OakInvReadItemName(p, nm, 48);
                    uintptr_t e0 = Read<uintptr_t>(p);
                    OakInvReadItemName(e0, nm1, 48);
                }
                if (!IsValidPtr(p) && i0 == 0 && i4 == 0) continue;
                wsprintfA(b, "inv: dump cargo+0x%02X p=%p i0=%d i4=%d nm='%s' [0]='%s'",
                    (unsigned)off, (void*)p, i0, i4, nm[0] ? nm : "-", nm1[0] ? nm1 : "-");
                Log(b);
            }

            // Deep sample cargo+0x60 (live ItemList candidate)
            {
                uintptr_t arr60 = Read<uintptr_t>(cargo + 0x60);
                wsprintfA(b, "inv: dump arr60=%p", (void*)arr60);
                Log(b);
                if (IsValidPtr(arr60) && arr60 > 0x100000000ULL)
                {
                    for (int k = 0; k < 8; k++)
                    {
                        uintptr_t slot = Read<uintptr_t>(arr60 + (uintptr_t)k * 8);
                        char nm[48] = {};
                        OakInvReadItemName(slot, nm, 48);
                        char nm2[48] = {};
                        if (IsValidPtr(slot))
                            OakInvReadItemName(Read<uintptr_t>(slot), nm2, 48);
                        // Also try slot as cargo entry with item at +0x0/+0x8/+0x10
                        char nm3[48] = {}, nm4[48] = {};
                        if (IsValidPtr(slot))
                        {
                            OakInvReadItemName(Read<uintptr_t>(slot + 0x8), nm3, 48);
                            OakInvReadItemName(Read<uintptr_t>(slot + 0x10), nm4, 48);
                        }
                        wsprintfA(b, "inv: dump a60[%d]=%p nm='%s' *0='%s' *8='%s' *10='%s'",
                            k, (void*)slot, nm[0] ? nm : "-", nm2[0] ? nm2 : "-",
                            nm3[0] ? nm3 : "-", nm4[0] ? nm4 : "-");
                        Log(b);
                    }
                }
            }

            char foundLines[8][48] = {};
            int fn = OakInvCollectFromCargoObjWide(cargo, foundLines, 8, 0);
            wsprintfA(b, "inv: dump wideCargoN=%d first='%s'",
                fn, (fn > 0 && foundLines[0][0]) ? foundLines[0] : "-");
            Log(b);
        }
        s_Dumps++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("inv: dump exception"); }
}

struct OakInvGearLine
{
    char label[16];
    char name[48];
};

static void OakInvScanAttachmentsForGear(uintptr_t inv,
    char* plateNm, int plateMax, bool* hasPlate,
    char* helmNm, int helmMax, bool* hasHelm,
    char* bagNm, int bagMax, bool* hasBag)
{
    if (!IsValidPtr(inv)) return;

    auto consider = [&](uintptr_t item) {
        if (!IsValidPtr(item) || item < 0x100000000ULL) return;
        char nm[48] = {};
        if (!OakInvReadItemName(item, nm, 48)) return;
        if (hasPlate && !*hasPlate && plateNm && OakInvIsPlateCarrier(nm))
        {
            lstrcpynA(plateNm, nm, plateMax);
            *hasPlate = true;
        }
        else if (hasHelm && !*hasHelm && helmNm && OakInvIsThreatHelmet(nm))
        {
            lstrcpynA(helmNm, nm, helmMax);
            *hasHelm = true;
        }
        else if (hasBag && !*hasBag && bagNm && OakInvIsBackpack(nm))
        {
            lstrcpynA(bagNm, nm, bagMax);
            *hasBag = true;
        }
    };

    auto done = [&]() -> bool {
        return (!hasPlate || *hasPlate) && (!hasHelm || *hasHelm) && (!hasBag || *hasBag);
    };

    __try
    {
        // DayZPlayerInventory::Clothing @ 0x150 — may be entity*, array, or cargo.
        uintptr_t clothing = Read<uintptr_t>(inv + 0x150);
        if (IsValidPtr(clothing) && clothing > 0x100000000ULL)
        {
            consider(clothing);
            for (int i = 0; i < 16 && !done(); i++)
                consider(Read<uintptr_t>(clothing + (uintptr_t)i * 8));
            char tmpLines[6][48] = {};
            int n = OakInvCollectFromCargoObj(clothing, tmpLines, 6, 0);
            for (int i = 0; i < n; i++)
            {
                if (hasPlate && !*hasPlate && plateNm && OakInvIsPlateCarrier(tmpLines[i]))
                {
                    lstrcpynA(plateNm, tmpLines[i], plateMax);
                    *hasPlate = true;
                }
                if (hasHelm && !*hasHelm && helmNm && OakInvIsThreatHelmet(tmpLines[i]))
                {
                    lstrcpynA(helmNm, tmpLines[i], helmMax);
                    *hasHelm = true;
                }
                if (hasBag && !*hasBag && bagNm && OakInvIsBackpack(tmpLines[i]))
                {
                    lstrcpynA(bagNm, tmpLines[i], bagMax);
                    *hasBag = true;
                }
            }
        }

        // Attachment / clothing slot sweep (skip Hands 0x1B0)
        for (uintptr_t off = 0xE0; off <= 0x200 && !done(); off += 8)
        {
            if (off == 0x1B0) continue;
            consider(Read<uintptr_t>(inv + off));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Threat-focused gear: gun + plate + helmet + backpack.
static int OakInvCollectPlayerGear(uintptr_t player, OakInvGearLine* out, int maxOut)
{
    if (!out || maxOut <= 0) return 0;
    if (!IsValidPtr(player) || player < 0x100000000ULL) return 0;
    int n = 0;
    __try
    {
        char handsNm[48] = {};
        bool hasHands = false;

        // Reuse the battle-tested hands resolver (rejects empty TypeName).
        uintptr_t hands = GetLocalHandsWeapon(player);
        if (IsValidPtr(hands) && hands > 0x100000000ULL)
            hasHands = OakInvReadItemName(hands, handsNm, 48);

        if (!hasHands)
        {
            uintptr_t inv = Read<uintptr_t>(player + 0x650);
            if (IsValidPtr(inv))
            {
                const uintptr_t handOffs[] = { 0x1B0, 0xF8 };
                for (int hi = 0; hi < 2 && !hasHands; hi++)
                {
                    uintptr_t h = Read<uintptr_t>(inv + handOffs[hi]);
                    if (OakInvReadItemName(h, handsNm, 48))
                        hasHands = true;
                }
            }
        }

        if (n < maxOut)
        {
            lstrcpynA(out[n].label, "Gun", 16);
            lstrcpynA(out[n].name, hasHands ? handsNm : "-", 48);
            n++;
        }

        char plateNm[48] = {};
        char helmNm[48] = {};
        char bagNm[48] = {};
        bool hasPlate = false, hasHelm = false, hasBag = false;

        uintptr_t inv = Read<uintptr_t>(player + 0x650);
        if (!IsValidPtr(inv) || inv < 0x100000000ULL)
            inv = Read<uintptr_t>(player + 0x658);
        if (IsValidPtr(inv) && inv > 0x100000000ULL)
        {
            OakInvScanAttachmentsForGear(inv,
                plateNm, 48, &hasPlate,
                helmNm, 48, &hasHelm,
                bagNm, 48, &hasBag);
        }

        if (n < maxOut)
        {
            lstrcpynA(out[n].label, "Plate", 16);
            lstrcpynA(out[n].name, hasPlate ? plateNm : "-", 48);
            n++;
        }
        if (n < maxOut)
        {
            lstrcpynA(out[n].label, "Helm", 16);
            lstrcpynA(out[n].name, hasHelm ? helmNm : "-", 48);
            n++;
        }
        if (n < maxOut)
        {
            lstrcpynA(out[n].label, "Bag", 16);
            lstrcpynA(out[n].name, hasBag ? bagNm : "-", 48);
            n++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

#endif // OAK_INVENTORY_IMPL_INL
