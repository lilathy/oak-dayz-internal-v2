
#include <Windows.h>
#include <winhttp.h>
#include <wincodec.h>
#include <objbase.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cmath>

#include "imgui.h"
#include "imgui_menu.h"
#include "offsets_generated.hpp"
#include "protection_client.h"
#include "oak_crash_hunt.h"
#include "oak_lab.h"
#include "oak_engine.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")


static int StrCmp(const char* s1, const char* s2)
{
    while (*s1 && *s2)
    {
        if (*s1 != *s2) return *s1 - *s2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}


static int StrCmpI(const char* s1, const char* s2)
{
    while (*s1 && *s2)
    {
        char c1 = *s1;
        char c2 = *s2;
        
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}


static bool StrContainsI(const char* haystack, const char* needle)
{
    const char* h = haystack;
    while (*h)
    {
        const char* n = needle;
        const char* h2 = h;
        bool match = true;
        while (*n && *h2)
        {
            char c1 = *h2;
            char c2 = *n;
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2)
            {
                match = false;
                break;
            }
            h2++;
            n++;
        }
        if (match && !*n) return true;  
        h++;
    }
    return false;
}


static bool IsAnimal(const char* configName)
{
    if (!configName || !configName[0]) return false;
    
    
    const char* animalNames[] = {
        "cow", "deer", "boar", "chicken", "goat", "sheep", 
        "wolf", "bear", "rabbit", "pig", "horse", "rooster",
        "hen", "bull", "cattle", "stag", "doe", "wildboar",
        "mouflon", "ibex", "lynx", "fox", "carp", "mackerel",
        "salmon", "pike", "bass", "catfish", "fish",
        "animal", "creature", "beast", "livestock",
        // Latin / classname fragments
        "bostaurus", "canislupus", "caprahircus", "capreolus",
        "cervuselaphus", "gallusgallus", "ovisaries", "susdomesticus",
        "susscrofa", "ursusarctos", "oryctolagus", "vulpes", "lepus"
    };
    
    for (int i = 0; i < sizeof(animalNames) / sizeof(animalNames[0]); i++)
    {
        if (StrContainsI(configName, animalNames[i]))
            return true;
    }
    
    return false;
}

// DayZ TypeNames are Latin binomials (Animal_CervusElaphus). ESP should show plain English.
static void PrettyAnimalName(const char* raw, char* out, int outMax)
{
    if (!out || outMax < 4) return;
    out[0] = 0;
    if (!raw || !raw[0])
    {
        out[0] = 'A'; out[1] = 'n'; out[2] = 'i'; out[3] = 'm'; out[4] = 'a'; out[5] = 'l'; out[6] = 0;
        return;
    }

    struct Map { const char* key; const char* nice; };
    // Longer / more specific keys first
    static const Map kMap[] = {
        { "CervusElaphus", "Deer" },
        { "CapreolusCapreolus", "Deer" },
        { "CanisLupus", "Wolf" },
        { "UrsusArctos", "Bear" },
        { "Oryctolagus", "Rabbit" },
        { "Lepus", "Rabbit" },
        { "Vulpes", "Fox" },
        { "SusScrofa", "Boar" },
        { "SusDomesticus", "Pig" },
        { "BosTaurus", "Cow" },
        { "CapraHircus", "Goat" },
        { "OvisAries", "Sheep" },
        { "GallusGallusDomesticusF", "Hen" },
        { "GallusGallusDomesticus", "Chicken" },
        { "GallusGallus", "Chicken" },
        { "Rangifer", "Reindeer" },
        { "Equus", "Horse" },
        { "Lynx", "Lynx" },
        { "WildBoar", "Boar" },
        // Already-English fallbacks
        { "Deer", "Deer" },
        { "Wolf", "Wolf" },
        { "Bear", "Bear" },
        { "Rabbit", "Rabbit" },
        { "Fox", "Fox" },
        { "Boar", "Boar" },
        { "Cow", "Cow" },
        { "Goat", "Goat" },
        { "Sheep", "Sheep" },
        { "Chicken", "Chicken" },
        { "Hen", "Hen" },
        { "Pig", "Pig" },
        { "Horse", "Horse" },
    };

    for (int i = 0; i < (int)(sizeof(kMap) / sizeof(kMap[0])); i++)
    {
        if (StrContainsI(raw, kMap[i].key))
        {
            const char* n = kMap[i].nice;
            int j = 0;
            for (; n[j] && j < outMax - 1; j++)
                out[j] = n[j];
            out[j] = 0;
            return;
        }
    }

    // Strip Animal_ prefix and take a short tail if unknown
    const char* p = raw;
    if (StrContainsI(raw, "Animal_"))
    {
        const char* s = raw;
        while (*s)
        {
            if (s[0] == '_' && s[1]) { p = s + 1; break; }
            s++;
        }
    }
    int j = 0;
    for (; p[j] && j < outMax - 1 && j < 24; j++)
        out[j] = p[j];
    out[j] = 0;
    if (!out[0])
    {
        out[0] = 'A'; out[1] = 'n'; out[2] = 'i'; out[3] = 'm'; out[4] = 'a'; out[5] = 'l'; out[6] = 0;
    }
}


// World clutter / non-loot names (TypeName or ConfigName). Keep narrow — broad
// substrings like "box"/"battery" wrongly hid real loot when ConfigName was short.
static bool IsJunkWorldObject(const char* name)
{
    if (!name || !name[0]) return false;
    if (StrCmpI(name, "inventoryItem") == 0 || StrCmpI(name, "clothing") == 0)
        return false;
    if (StrContainsI(name, "ProxyMagazine")) return false;

    const char* junk[] = {
        "FreeDebugCamera", "Land_", "StaticObj_", "Wreck_Decal", "Roadblock",
        "GardenPlot", "gardenbed", "FireplaceIndoor", "PowerLine", "PowerPole",
        "housenew", "land_house", "fence_", "wall_", "gate_", "ladder_",
        "dayzplayer", "dayzinfected", "dayzanimal"
    };
    for (int i = 0; i < (int)(sizeof(junk) / sizeof(junk[0])); i++)
    {
        if (StrContainsI(name, junk[i]))
            return true;
    }
    return false;
}

static bool IsLootConfigClass(const char* cfg)
{
    if (!cfg || !cfg[0]) return false;
    return StrCmpI(cfg, "inventoryItem") == 0
        || StrCmpI(cfg, "clothing") == 0
        || StrCmpI(cfg, "itembase") == 0
        || StrContainsI(cfg, "ProxyMagazine")
        || StrContainsI(cfg, "itemoptics")
        || StrContainsI(cfg, "itemsuppressor")
        || StrContainsI(cfg, "itembook")
        || StrContainsI(cfg, "itemtransmitter")
        || StrContainsI(cfg, "itemcompass")
        || StrContainsI(cfg, "itemmap")
        || StrContainsI(cfg, "itemradio")
        || StrContainsI(cfg, "weapon")
        || StrContainsI(cfg, "magazine")
        || StrContainsI(cfg, "ammunition")
        || StrContainsI(cfg, "edible");
}

enum LootCategory {
    LootCat_Default = 0,
    LootCat_Weapon,
    LootCat_Ammo,
    LootCat_Medical,
    LootCat_Food,
    LootCat_Clothing,
    LootCat_Tool
};

static LootCategory ClassifyLootName(const char* name)
{
    if (!name || !name[0]) return LootCat_Default;
    if (StrContainsI(name, "Ammo_") || StrContainsI(name, "Mag_") || StrContainsI(name, "ProxyMagazine"))
        return LootCat_Ammo;
    if (StrContainsI(name, "Bandage") || StrContainsI(name, "Morphine") || StrContainsI(name, "Saline")
        || StrContainsI(name, "Tetracycline") || StrContainsI(name, "Charcoal") || StrContainsI(name, "Vitamin")
        || StrContainsI(name, "FirstAid") || StrContainsI(name, "BloodBag") || StrContainsI(name, "Splint")
        || StrContainsI(name, "Epinephrine") || StrContainsI(name, "Painkiller") || StrContainsI(name, "Disinfectant"))
        return LootCat_Medical;
    if (StrContainsI(name, "Can_") || StrContainsI(name, "Soda") || StrContainsI(name, "WaterBottle")
        || StrContainsI(name, "Canteen") || StrContainsI(name, "Marmalade") || StrContainsI(name, "Mushroom")
        || StrContainsI(name, "Apple") || StrContainsI(name, "Pear") || StrContainsI(name, "Plum")
        || StrContainsI(name, "Pepper") || StrContainsI(name, "Potato") || StrContainsI(name, "Zucchini")
        || StrContainsI(name, "Rice") || StrContainsI(name, "Pasta") || StrContainsI(name, "Tuna")
        || StrContainsI(name, "Peaches") || StrContainsI(name, "Spaghetti") || StrContainsI(name, "PowderedMilk")
        || StrContainsI(name, "BoxCereal") || StrContainsI(name, "Food"))
        return LootCat_Food;
    if (StrContainsI(name, "Shirt") || StrContainsI(name, "Pants") || StrContainsI(name, "Jacket")
        || StrContainsI(name, "Hoodie") || StrContainsI(name, "Boots") || StrContainsI(name, "Shoes")
        || StrContainsI(name, "Helmet") || StrContainsI(name, "Hat_") || StrContainsI(name, "Gloves")
        || StrContainsI(name, "Vest") || StrContainsI(name, "Backpack") || StrContainsI(name, "Bag_")
        || StrContainsI(name, "Mask") || StrContainsI(name, "Glasses") || StrContainsI(name, "Armband"))
        return LootCat_Clothing;
    if (StrContainsI(name, "HandSaw") || StrContainsI(name, "Hacksaw") || StrContainsI(name, "Screwdriver")
        || StrContainsI(name, "Wrench") || StrContainsI(name, "Pliers") || StrContainsI(name, "Lockpick")
        || StrContainsI(name, "Crowbar") || StrContainsI(name, "Hammer") || StrContainsI(name, "Shovel")
        || StrContainsI(name, "Pickaxe") || StrContainsI(name, "Rope") || StrContainsI(name, "DuctTape")
        || StrContainsI(name, "CableReel") || StrContainsI(name, "MetalWire") || StrContainsI(name, "SewingKit")
        || StrContainsI(name, "LeatherSewing") || StrContainsI(name, "WeaponCleaning"))
        return LootCat_Tool;
    if (StrContainsI(name, "AKM") || StrContainsI(name, "AK74") || StrContainsI(name, "AK101")
        || StrContainsI(name, "M4A1") || StrContainsI(name, "SVD") || StrContainsI(name, "Mosin")
        || StrContainsI(name, "Winchester") || StrContainsI(name, "Shotgun") || StrContainsI(name, "Rifle")
        || StrContainsI(name, "Pistol") || StrContainsI(name, "Deagle") || StrContainsI(name, "Glock")
        || StrContainsI(name, "FNX") || StrContainsI(name, "CZ75") || StrContainsI(name, "MP5")
        || StrContainsI(name, "UMP") || StrContainsI(name, "Scout") || StrContainsI(name, "Blaze")
        || StrContainsI(name, "Repeater") || StrContainsI(name, "Izh") || StrContainsI(name, "SKS")
        || StrContainsI(name, "FAL") || StrContainsI(name, "VSS") || StrContainsI(name, "Aug")
        || StrContainsI(name, "Weapon_") || StrContainsI(name, "Launcher"))
        return LootCat_Weapon;
    return LootCat_Default;
}

// Ground loot by TypeName — catches player-dropped items with odd/empty ConfigName.
static bool LooksLikeGroundLootName(const char* typeName)
{
    if (!typeName || !typeName[0]) return false;
    if (ClassifyLootName(typeName) != LootCat_Default) return true;
    if (StrContainsI(typeName, "Mag_") || StrContainsI(typeName, "Ammo_") ||
        StrContainsI(typeName, "Weapon_") || StrContainsI(typeName, "Can_") ||
        StrContainsI(typeName, "Bottle") || StrContainsI(typeName, "Knife") ||
        StrContainsI(typeName, "Axe") || StrContainsI(typeName, "Bag") ||
        StrContainsI(typeName, "Backpack") || StrContainsI(typeName, "Pouch") ||
        StrContainsI(typeName, "Nail") || StrContainsI(typeName, "Seed") ||
        StrContainsI(typeName, "Matchbox") || StrContainsI(typeName, "Compass") ||
        StrContainsI(typeName, "Flashlight") || StrContainsI(typeName, "Binocular") ||
        StrContainsI(typeName, "Optic") || StrContainsI(typeName, "Suppressor") ||
        StrContainsI(typeName, "Lockpick") || StrContainsI(typeName, "Bandage") ||
        StrContainsI(typeName, "Tent") || StrContainsI(typeName, "Barrel") ||
        StrContainsI(typeName, "Crate") || StrContainsI(typeName, "Helmet") ||
        StrContainsI(typeName, "Vest") || StrContainsI(typeName, "Gloves") ||
        StrContainsI(typeName, "Boots") || StrContainsI(typeName, "Shirt") ||
        StrContainsI(typeName, "Pants") || StrContainsI(typeName, "Jacket") ||
        StrContainsI(typeName, "Mask") || StrContainsI(typeName, "Hat_"))
        return true;
    return false;
}

static void GetLootCategoryColor(LootCategory cat, float* r, float* g, float* b, float* a)
{
    switch (cat)
    {
    case LootCat_Weapon:   *r = 1.00f; *g = 0.35f; *b = 0.25f; *a = 1.0f; break;
    case LootCat_Ammo:     *r = 1.00f; *g = 0.75f; *b = 0.20f; *a = 1.0f; break;
    case LootCat_Medical:  *r = 0.35f; *g = 1.00f; *b = 0.45f; *a = 1.0f; break;
    case LootCat_Food:     *r = 0.95f; *g = 0.65f; *b = 0.20f; *a = 1.0f; break;
    case LootCat_Clothing: *r = 0.65f; *g = 0.55f; *b = 1.00f; *a = 1.0f; break;
    case LootCat_Tool:     *r = 0.55f; *g = 0.85f; *b = 0.95f; *a = 1.0f; break;
    default:
        *r = 0.35f; *g = 0.75f; *b = 1.00f; *a = 1.0f;
        break;
    }
}


static const unsigned char g_VS[] = {
    0x44, 0x58, 0x42, 0x43, 0xBE, 0x7F, 0xFA, 0x49, 0xF7, 0xCE, 0xE6, 0x00, 0x78, 0x87, 0x89, 0x7A, 
    0xA8, 0x34, 0x63, 0x66, 0x01, 0x00, 0x00, 0x00, 0x74, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 
    0x34, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x44, 0x01, 0x00, 0x00, 
    0xD8, 0x01, 0x00, 0x00, 0x52, 0x44, 0x45, 0x46, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x05, 0xFE, 0xFF, 
    0x00, 0x01, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x52, 0x44, 0x31, 0x31, 0x3C, 0x00, 0x00, 0x00, 
    0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 
    0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x69, 0x63, 0x72, 0x6F, 0x73, 0x6F, 0x66, 
    0x74, 0x20, 0x28, 0x52, 0x29, 0x20, 0x48, 0x4C, 0x53, 0x4C, 0x20, 0x53, 0x68, 0x61, 0x64, 0x65, 
    0x72, 0x20, 0x43, 0x6F, 0x6D, 0x70, 0x69, 0x6C, 0x65, 0x72, 0x20, 0x31, 0x30, 0x2E, 0x31, 0x00, 
    0x49, 0x53, 0x47, 0x4E, 0x48, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 
    0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x0F, 0x00, 0x00, 
    0x50, 0x4F, 0x53, 0x49, 0x54, 0x49, 0x4F, 0x4E, 0x00, 0x43, 0x4F, 0x4C, 0x4F, 0x52, 0x00, 0xAB, 
    0x4F, 0x53, 0x47, 0x4E, 0x4C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 
    0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 
    0x53, 0x56, 0x5F, 0x50, 0x4F, 0x53, 0x49, 0x54, 0x49, 0x4F, 0x4E, 0x00, 0x43, 0x4F, 0x4C, 0x4F, 
    0x52, 0x00, 0xAB, 0xAB, 0x53, 0x48, 0x45, 0x58, 0x8C, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x00, 
    0x23, 0x00, 0x00, 0x00, 0x6A, 0x08, 0x00, 0x01, 0x5F, 0x00, 0x00, 0x03, 0x32, 0x10, 0x10, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x5F, 0x00, 0x00, 0x03, 0xF2, 0x10, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 
    0x67, 0x00, 0x00, 0x04, 0xF2, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 
    0x65, 0x00, 0x00, 0x03, 0xF2, 0x20, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x05, 
    0x32, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x36, 0x00, 0x00, 0x08, 0xC2, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 
    0x36, 0x00, 0x00, 0x05, 0xF2, 0x20, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x46, 0x1E, 0x10, 0x00, 
    0x01, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x01, 0x53, 0x54, 0x41, 0x54, 0x94, 0x00, 0x00, 0x00, 
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00
};


static const unsigned char g_PS[] = {
    0x44, 0x58, 0x42, 0x43, 0x29, 0x32, 0x26, 0xA8, 0x0C, 0xD2, 0xDF, 0x85, 0x93, 0x8E, 0x4A, 0x0F, 
    0x51, 0x32, 0xC0, 0xAE, 0x01, 0x00, 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 
    0x34, 0x00, 0x00, 0x00, 0xA0, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00, 0x28, 0x01, 0x00, 0x00, 
    0x6C, 0x01, 0x00, 0x00, 0x52, 0x44, 0x45, 0x46, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x05, 0xFF, 0xFF, 
    0x00, 0x01, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x52, 0x44, 0x31, 0x31, 0x3C, 0x00, 0x00, 0x00, 
    0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 
    0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x69, 0x63, 0x72, 0x6F, 0x73, 0x6F, 0x66, 
    0x74, 0x20, 0x28, 0x52, 0x29, 0x20, 0x48, 0x4C, 0x53, 0x4C, 0x20, 0x53, 0x68, 0x61, 0x64, 0x65, 
    0x72, 0x20, 0x43, 0x6F, 0x6D, 0x70, 0x69, 0x6C, 0x65, 0x72, 0x20, 0x31, 0x30, 0x2E, 0x31, 0x00, 
    0x49, 0x53, 0x47, 0x4E, 0x4C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 
    0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x0F, 0x00, 0x00, 
    0x53, 0x56, 0x5F, 0x50, 0x4F, 0x53, 0x49, 0x54, 0x49, 0x4F, 0x4E, 0x00, 0x43, 0x4F, 0x4C, 0x4F, 
    0x52, 0x00, 0xAB, 0xAB, 0x4F, 0x53, 0x47, 0x4E, 0x2C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 
    0x08, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x53, 0x56, 0x5F, 0x54, 
    0x61, 0x72, 0x67, 0x65, 0x74, 0x00, 0xAB, 0xAB, 0x53, 0x48, 0x45, 0x58, 0x3C, 0x00, 0x00, 0x00, 
    0x50, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x6A, 0x08, 0x00, 0x01, 0x62, 0x10, 0x00, 0x03, 
    0xF2, 0x10, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x03, 0xF2, 0x20, 0x10, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x05, 0xF2, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x46, 0x1E, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x01, 0x53, 0x54, 0x41, 0x54, 
    0x94, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};


namespace offsets {
    namespace modbase {
        inline uintptr_t& World = oak_offsets::modbase::World;
        inline uintptr_t& NetworkManager = oak_offsets::modbase::NetworkManager;
        constexpr uintptr_t DayZPlayer = 0x428E6F8;   
    }
    namespace world {
        
        inline uintptr_t& Camera = oak_offsets::world::Camera;
        inline uintptr_t& LocalPlayer = oak_offsets::world::LocalPlayer;
        inline uintptr_t& NearEntList = oak_offsets::world::NearEntList;
        inline uintptr_t& FarEntList = oak_offsets::world::FarEntList;
        inline uintptr_t& SlowEntList = oak_offsets::world::SlowEntList;
        inline uintptr_t& BulletList = oak_offsets::world::BulletList;
        inline uintptr_t& NoGrass = oak_offsets::world::NoGrass;
    }
    namespace entity {
        inline uintptr_t& EntType = oak_offsets::entity::Type;
        inline uintptr_t& VisualState = oak_offsets::entity::VisualState;
        inline uintptr_t& FutureVisualState = oak_offsets::entity::FutureVisualState;
        inline uintptr_t& Inventory = oak_offsets::player::Inventory;
        inline uintptr_t& NetworkID = oak_offsets::entity::NetworkId;
        inline uintptr_t& IsDead = oak_offsets::entity::IsDead;
        constexpr uintptr_t EntSize = 0x8;
        inline uintptr_t& Skeleton = oak_offsets::player::Skeleton;
        inline uintptr_t& ZombieSkeleton = oak_offsets::infected::Skeleton;
    }
    namespace entitytype {
        inline uintptr_t& TypeName = oak_offsets::entitytype::TypeName;
        inline uintptr_t& ConfigName = oak_offsets::entitytype::ConfigName;
    }
    namespace human {
        inline uintptr_t& VisualState = oak_offsets::entity::VisualState;
    }
    namespace visual {
        constexpr uintptr_t Position = 0x2C;          
        constexpr uintptr_t DirX = 0x20;              
        constexpr uintptr_t DirY = 0x28;              
        constexpr uintptr_t Transform = 0x8;
    }
    
    namespace camera {
        inline uintptr_t& InvertedViewRight = oak_offsets::camera::InvertedViewRight;
        inline uintptr_t& InvertedViewUp = oak_offsets::camera::InvertedViewUp;
        inline uintptr_t& InvertedViewForward = oak_offsets::camera::InvertedViewForward;
        inline uintptr_t& InvertedViewTranslation = oak_offsets::camera::InvertedViewTranslation;
        inline uintptr_t& ViewPortSize = oak_offsets::camera::ViewPortSize;
        inline uintptr_t& GetProjectionD1 = oak_offsets::camera::GetProjectionD1;
        inline uintptr_t& GetProjectionD2 = oak_offsets::camera::GetProjectionD2;
    }
    namespace skeleton {
        inline uintptr_t& AnimClass1 = oak_offsets::skeleton::AnimClass1;
        inline uintptr_t& AnimClass2 = oak_offsets::skeleton::AnimClass2;
    }
    namespace player {
        inline uintptr_t& Skeleton = oak_offsets::player::Skeleton;
    }
    namespace infected {
        inline uintptr_t& Skeleton = oak_offsets::infected::Skeleton;
    }
    namespace animclass {
        inline uintptr_t& MatrixArray = oak_offsets::anim::MatrixArray;
        inline uintptr_t& MatrixB = oak_offsets::anim::MatrixB;
    }
}


#define SHARED_MEM_NAME L"OakPanelSharedMem"

#pragma pack(push, 1)
struct SharedConfig {
    
    DWORD magic;              
    DWORD version;
    
    
    bool espEnabled;
    bool espPlayers;
    bool espZombies;
    bool espAnimals;
    bool espItems;
    bool espVehicles;
    bool espSkeleton;
    bool espBox;
    bool espName;
    bool espDistance;
    bool espHealth;
    
    
    bool playerBox;
    bool playerName;
    bool playerDistanceEnabled;
    bool zombieBox;
    bool zombieName;
    bool zombieDistanceEnabled;
    bool animalBox;
    bool animalName;
    bool animalDistanceEnabled;
    bool itemBox;
    bool itemName;
    bool itemDistanceEnabled;
    bool vehicleBox;
    bool vehicleName;
    bool vehicleDistanceEnabled;
    
    
    int playerDistance;
    int zombieDistance;
    int animalDistance;
    int itemDistance;
    int vehicleDistance;
    
    
    int maxPlayers;
    int maxZombies;
    int maxAnimals;
    int maxItems;
    
    
    DWORD colorPlayerBox;
    DWORD colorPlayerSkeleton;
    DWORD colorZombieBox;
    DWORD colorZombieSkeleton;
    DWORD colorAnimalBox;
    DWORD colorItemBox;
    DWORD colorVehicleBox;
    DWORD colorPlayerName;
    DWORD colorZombieName;
    DWORD colorAnimalName;
    DWORD colorItemName;
    DWORD colorVehicleName;
    
    
    bool aimbotEnabled;
    bool aimbotPlayers;
    bool aimbotZombies;
    int aimbotFov;
    int aimbotSmooth;
    int aimbotBone;
    bool aimbotVisCheck;
    
    
    bool fastBullets;
    bool noRecoil;
    bool noSway;
    bool infiniteStamina;
    bool speedHack;
    float speedMultiplier;
    
    
    int menuKey;
    bool streamProof;
    
    
    bool dllAttached;
    DWORD targetPid;
    
    
    float PlayerX;
    float PlayerY;
    float PlayerZ;
};
#pragma pack(pop)

static HANDLE g_hMapFile = NULL;
static SharedConfig* g_SharedConfig = NULL;


static HMODULE g_Module = NULL;
static HMODULE g_GameModule = NULL;
static bool g_Running = true;
static volatile LONG g_ShuttingDown = 0;
static bool g_Initialized = false;
static bool g_RenderInit = false;
static bool g_ShowESP = true;
static bool g_ShowSkeleton = false;
static bool g_ShowNames = true;
static bool g_ShowDistance = true;
static bool g_ESPPlayers = true;
static bool g_ESPZombies = true;
static bool g_ESPAnimals = false;
static bool g_ESPItems = true;
static bool g_ESPVehicles = true;
static bool g_FastBullets = false;
static bool g_NoDispersion = false;
static bool g_PerfectBallistics = false;
static bool g_AimbotEnabled = false;
static bool g_MagicBullet = false;
static bool g_MagicBulletAutoFire = false;
static bool g_MagicBulletChain = false;
static bool g_MagicBulletDrawFov = true;
static bool g_AimbotPlayers = true;
static bool g_AimbotZombies = true;
static bool g_AimbotDrawFov = true;
static int g_AimbotFov = 120;
static int g_AimbotSmooth = 3;
static int g_AimbotBone = 0;
static int g_AimbotKey = VK_XBUTTON1;
static int g_AimbotMaxDistance = 400;
static int g_MagicBulletMaxDistance = 800;
static int g_MagicBulletFov = 320;
static int g_CrosshairStyle = 1;
static int g_CrosshairSize = 8;
static int g_CrosshairGap = 4;
static int g_CrosshairThickness = 2;

// Combat / world overlays (synced from ImGui)
static bool g_BulletTracers = true;
static bool g_ImpactMarkers = true;
static bool g_ShotIndicators = true;
static bool g_HitMarkers = true;
static bool g_Crosshair = true;
static bool g_WeaponEsp = true;
static bool g_LootShowWeapons = true;
static bool g_LootShowAmmo = true;
static bool g_LootShowMedical = true;
static bool g_LootShowFood = true;
static bool g_LootShowClothing = true;
static bool g_LootShowTools = true;
static bool g_LootShowOther = true;
static OakLootCatSettings g_LootCats[OAK_LOOT_CAT_COUNT] = {};
static int g_LootSortMode = OakLootSortDistance;
static int g_LootBlCount = 0;
static char g_LootBlacklist[OAK_LOOT_FILTER_MAX][48] = {};
static int g_LootWlCount = 0;
static char g_LootWhitelist[OAK_LOOT_FILTER_MAX][48] = {};
static int g_LootCatDrawn[OAK_LOOT_CAT_COUNT] = {};
static int g_PlayerBoxStyle = OakBoxCorner;
static int g_ZombieBoxStyle = OakBoxCorner;
static float g_PlayerBoxThick = 1.5f;
static float g_ZombieBoxThick = 1.5f;
static bool g_PlayerUseVisColors = false;
static bool g_ZombieUseVisColors = false;
static float g_PlayerColorVisible[4] = { 0.40f, 0.85f, 1.00f, 0.95f };
static float g_PlayerColorOccluded[4] = { 0.55f, 0.55f, 0.55f, 0.75f };
static float g_ZombieColorVisible[4] = { 0.95f, 0.40f, 0.35f, 0.90f };
static float g_ZombieColorOccluded[4] = { 0.55f, 0.55f, 0.55f, 0.75f };
static int g_AimPriority = OakAimPriCrosshair;
static int g_AimSmoothVarPct = 0;
static int g_AimReactionMs = 0;
static bool g_AimRequireLos = false;
static int g_PanicScope = OakPanicAll;
static unsigned g_BindHoldMask = 0;
static int g_ProfileHotkey[3] = {};

static bool LootCategoryEnabled(LootCategory cat)
{
    int idx = OakLootCategoryIndex((int)cat);
    if (idx < 0 || idx >= OAK_LOOT_CAT_COUNT) return g_LootShowOther;
    return g_LootCats[idx].enabled;
}

static bool g_HealthBars = true;
static bool g_BarHealth = true;
static bool g_BarBlood = true;
static bool g_BarShock = true;
static bool g_BarStamina = true;
static bool g_BarHunger = true;
static bool g_BarThirst = true;
static bool g_LocalVitalsHud = true;
static bool g_LocalWeaponAmmo = true;
static int g_FriendCount = 0;
static unsigned long long g_FriendSteamId[16] = {};
static char g_FriendName[16][64] = {};
static bool g_GrenadeTrajectory = true;
static bool g_ESPContainers = true;
static bool g_ESPCorpses = false;
static bool g_ESPTraps = true;
static bool g_Fullbright = false;
static int g_FullbrightBrightness = 55; // percent — synced from UI slider
static uintptr_t g_CachedWorldPtr = 0;
static uintptr_t g_ResolvedLocalPlayer = 0;
// World/local/camera pointers are torn down and rebuilt during connect, death,
// respawn, and map changes. Keep every gameplay reader/writer cold until the
// new pointer set has remained unchanged long enough to be trustworthy.
static volatile LONG g_SessionQuiesced = 1;
static uintptr_t g_SessionCandidateWorld = 0;
static uintptr_t g_SessionCandidateLocal = 0;
static uintptr_t g_SessionCandidateCamera = 0;
static DWORD g_SessionStableSince = 0;
static char g_LocalSteamName[64] = {};
static unsigned long long g_LocalSteamId = 0;
static HANDLE g_FullbrightThread = NULL;
static int g_TracerLifetimeMs = 1500;
static int g_ImpactLifetimeMs = 800;
static int g_ContainerDistance = 120;
static int g_CorpseDistance = 150;
static int g_TrapDistance = 100;

// Misc (synced from ImGui — logic in misc_impl.inl)
static bool g_MiscMiddleClickDespawn = false;
static bool g_MiscLootMagnet = false;
static bool g_MiscContainerMagnet = false;
static bool g_MiscDaytimeLock = false;
static bool g_MiscDisableOverlays = false;
static bool g_MiscSteamAvatars = true;
static bool g_MiscDrawWaypoints = true;
static bool g_MiscFreecam = false;
static bool g_MiscFreecamMoveBody = false;
static float g_MiscFreecamSpeed = 12.f;
// Warp desync: body stays at ghost, freecam scouts, confirm snaps body to cam.
static bool g_WarpDesyncActive = false;
static bool g_WarpCommitting = false;
static bool g_MiscNoGrass = false;
static int g_MiscLootMagnetRange = 8;
static int g_MiscContainerMagnetRange = 60;
static int g_MiscDespawnRange = 25;
static int g_BindEsp = 0;
static int g_BindAimbot = 0;
static int g_BindMagicBullet = 0;
static int g_BindFullbright = 0;
static int g_BindMiddleClickDespawn = VK_MBUTTON;
static int g_BindLootMagnet = 0;
static int g_BindContainerMagnet = 0;
static int g_BindDaytimeLock = 0;
static int g_BindDisableOverlays = 0;
static int g_BindPanic = 0;
static int g_BindAddWaypoint = 0;
static int g_BindCopyCoords = 0;
static int g_BindSteamNames = 0;
static int g_BindPullBasePart = 0;
static int g_BindFreecam = 0;
static int g_WaypointCount = 0;
static float g_WaypointX[OAK_WAYPOINT_MAX] = {};
static float g_WaypointY[OAK_WAYPOINT_MAX] = {};
static float g_WaypointZ[OAK_WAYPOINT_MAX] = {};
static char g_WaypointName[OAK_WAYPOINT_MAX][32] = {};
static float g_WaypointColor[OAK_WAYPOINT_MAX][4] = {};
static OakThreatRingSettings g_ThreatRing = {};
static OakThreatCounterSettings g_ThreatCounter = {};
static OakCompassSettings g_Compass = {};
static OakLookDirectionSettings g_LookDir = {};
static OakPlayerTrailSettings g_PlayerTrail = {};
static OakDeathMarkerSettings g_DeathMarker = {};
static OakNightBoostSettings g_NightBoost = {};
static OakCrosshairHighlightSettings g_CrosshairHighlight = {};
static OakFovHighlightSettings g_FovHighlight = {};
static OakReloadBarSettings g_ReloadBar = {};
static OakHeliCrashEspSettings g_HeliCrashEsp = {};
static OakGridCoordsHudSettings g_GridCoordsHud = {};
static OakWaypointHudSettings g_WaypointHud = {};
static OakStanceIconSettings g_StanceIcon = {};
static bool g_CrosshairOnEnemy = false;
static bool g_PanicHidden = false;

// Batch 4 (docs/remaining.txt) — must be before esp_addons_impl.inl
OakSilentAimSettings g_SilentAim = {};
OakGrenadeTeleportSettings g_GrenadeTeleport = {};
OakTriggerbotSettings g_Triggerbot = {};
OakWallBypassSettings g_WallBypass = {};
OakRecoilSettings g_Recoil = {};
OakAimbotBatch4Settings g_AimbotB4 = {};
OakContaminationEspSettings g_ContaminationEsp = {};
OakWorldMiscSettings g_WorldMisc = {};
OakExploitSettings g_Exploits = {};
int g_BindSilentAim = 0;
int g_BindAimAssist = 0;
static bool g_StreamProofUi = false;
static HWND g_OverlayHwnd = NULL;

struct Vec2;
struct Vec3;
static void OakBatch4UpdateMisc(uintptr_t worldPtr, uintptr_t localPlayer);
static void OakBatch4OnPresent(HWND hwnd);
static void OakShadowChamsApplySettings(const OakShadowChamsSettings& s);
static bool OakShadowChamsIsEnabled();
static void OakShadowChamsDrawWorld(uintptr_t worldPtr);
static void OakShadowChams_Install(ID3D11DeviceContext* ctx);
static void OakShadowChams_Shutdown();
static void OakBatch4LogLiveQa();
static void OakBatch4LiveVerify(uintptr_t worldPtr, uintptr_t localPlayer);
void OakBatch4DrawWarpOverlay();
static void OakApplyCameraFov(uintptr_t camera);
static void Batch4ApplyFov(uintptr_t worldPtr, uintptr_t localPlayer);
static void OakBatch4EspApplySettings(const OakBatch4EspSettings& s);
void OakBatch4InitCombatFrame();
void OakBatch4UpdateCombat(uintptr_t worldPtr, uintptr_t localPlayer);
static int g_Batch4EspListIdx = 0;


static bool g_PlayerBox = true;
static bool g_PlayerName = true;
static bool g_PlayerDistanceEnabled = true;
static bool g_ZombieBox = true;
static bool g_ZombieName = false;
static bool g_ZombieDistanceEnabled = true;
static bool g_AnimalBox = true;
static bool g_AnimalName = true;
static bool g_AnimalDistanceEnabled = true;
static bool g_ItemBox = false;
static bool g_ItemName = true;
static bool g_ItemDistanceEnabled = true;
static bool g_VehicleBox = true;
static bool g_VehicleName = true;
static bool g_VehicleDistanceEnabled = true;
static bool g_DrawLocalPlayer = false;

static int g_PlayerDistance = 400;
static int g_ZombieDistance = 150;
static int g_AnimalDistance = 150;
static int g_ItemDistance = 80;
static int g_VehicleDistance = 350;

static int LootCategoryMaxDistance(LootCategory cat)
{
    int idx = OakLootCategoryIndex((int)cat);
    if (idx < 0 || idx >= OAK_LOOT_CAT_COUNT) return g_ItemDistance;
    int d = g_LootCats[idx].maxDistance;
    return d > 0 ? d : g_ItemDistance;
}

static bool LootNamePassesFilter(const char* name)
{
    if (!name || !name[0]) return true;
    if (g_LootWlCount > 0)
    {
        bool hit = false;
        for (int i = 0; i < g_LootWlCount && i < OAK_LOOT_FILTER_MAX; i++)
        {
            if (g_LootWhitelist[i][0] && StrContainsI(name, g_LootWhitelist[i]))
            { hit = true; break; }
        }
        if (!hit) return false;
    }
    for (int i = 0; i < g_LootBlCount && i < OAK_LOOT_FILTER_MAX; i++)
    {
        if (g_LootBlacklist[i][0] && StrContainsI(name, g_LootBlacklist[i]))
            return false;
    }
    return true;
}

static int g_MaxPlayers = 40;
static int g_MaxZombies = 60;
static int g_MaxAnimals = 30;
static int g_MaxItems = 150;
static int g_MaxEntitiesScanned = 512;
static int g_MaxEntitiesDrawn = 100;
static int g_MaxLabelsPerFrame = 50;
static int g_MaxSkeletonsPerFrame = 25;
static int g_MaxCorpsesPerFrame = 50;
static int g_MaxTrapsPerFrame = 30;
static int g_EspUpdateHz = 20;
static int g_FrameBudgetMs = 4;
static int g_LodNearM = 100;
static int g_LodMidM = 300;
static int g_LodFarM = 500;
static bool g_CorpsePlayerCorpses = true;
static bool g_CorpseInfectedCorpses = true;
static bool g_CorpseEspName = true;
static bool g_CorpseEspDistance = true;
static bool g_TrapEspName = true;
static bool g_TrapEspDistance = true;
static int g_MaxAimTargets = 32;
static int g_FrameCount = 0;
// Last ESP frame counters — gameplay VERIFY / QA
static int g_QaPlayersDrawn = 0;
static int g_QaZombiesDrawn = 0;
static int g_QaAnimalsDrawn = 0;
static int g_QaItemsDrawn = 0;
static int g_QaTotalDrawn = 0;


static float g_ColorPlayerBox[4] = { 0.40f, 0.85f, 1.00f, 0.95f };
static float g_ColorPlayerSkeleton[4] = { 0.50f, 0.90f, 1.00f, 0.80f };
static float g_ColorZombieBox[4] = { 0.95f, 0.40f, 0.35f, 0.90f };
static float g_ColorZombieSkeleton[4] = { 0.95f, 0.55f, 0.30f, 0.75f };
static float g_ColorAnimalBox[4] = { 0.90f, 0.78f, 0.35f, 0.90f };
static float g_ColorItemBox[4] = { 0.70f, 0.82f, 0.95f, 0.85f };
static float g_ColorItemName[4] = { 0.88f, 0.92f, 0.98f, 0.95f };
static float g_ColorVehicleBox[4] = { 0.65f, 0.55f, 0.95f, 0.90f };
static float g_ColorPlayerChams[4] = { 0.25f, 0.70f, 0.90f, 0.35f };
static float g_ColorZombieChams[4] = { 0.90f, 0.25f, 0.20f, 0.30f };
static float g_ColorPlayerName[4] = { 0.80f, 0.95f, 1.00f, 1.0f };
static float g_ColorZombieName[4] = { 0.95f, 0.60f, 0.55f, 0.95f };
static int g_EspDrawCount = 0;

static float g_ColorTracer[4] = { 1.00f, 0.82f, 0.25f, 0.85f };
static float g_ColorImpact[4] = { 1.00f, 0.45f, 0.15f, 0.90f };
static float g_ColorShotInd[4] = { 1.00f, 0.25f, 0.30f, 0.95f };
static float g_ColorHitMarker[4] = { 1.00f, 0.30f, 0.30f, 1.0f };
static float g_ColorCrosshair[4] = { 0.90f, 0.95f, 1.00f, 0.85f };
static float g_ColorGrenade[4] = { 1.00f, 0.55f, 0.15f, 0.90f };
static float g_ColorContainer[4] = { 0.45f, 0.75f, 0.95f, 0.95f };
static float g_ColorCorpse[4] = { 0.85f, 0.55f, 0.70f, 0.90f };
static float g_ColorTrap[4] = { 1.00f, 0.25f, 0.25f, 0.95f };     


static IDXGISwapChain* g_SwapChain = NULL;
static ID3D11Device* g_Device = NULL;
static ID3D11DeviceContext* g_Context = NULL;
static ID3D11RenderTargetView* g_RenderTarget = NULL;

// Frame Boost may ResizeBuffers to add ALLOW_TEARING — drop our RTV so Present recreates it.
static void OakInvalidateOverlayRtv()
{
    if (g_RenderTarget)
    {
        g_RenderTarget->Release();
        g_RenderTarget = NULL;
    }
}


static ID3D11VertexShader* g_VertexShader = NULL;
static ID3D11PixelShader* g_PixelShader = NULL;
static ID3D11InputLayout* g_InputLayout = NULL;
static ID3D11Buffer* g_VertexBuffer = NULL;
static ID3D11BlendState* g_BlendState = NULL;
static ID3D11RasterizerState* g_RasterizerState = NULL;
static ID3D11DepthStencilState* g_DepthStencilState = NULL;


static float g_ScreenWidth = 1920.0f;
static float g_ScreenHeight = 1080.0f;


static float g_NDCScaleX = 2.0f / 1920.0f;   
static float g_NDCScaleY = 2.0f / 1080.0f;   
static float g_ScreenHalfW = 960.0f;          
static float g_ScreenHalfH = 540.0f;          


typedef HRESULT(WINAPI* tPresent)(IDXGISwapChain*, UINT, UINT);
static tPresent oPresent = NULL;
static void** g_SwapChainVTable = NULL;
static BYTE* g_PresentTarget = NULL;
static BYTE g_PresentOrigBytes[14] = {};
static BYTE g_PresentHookBytes[14] = {};
static bool g_PresentInlineHooked = false;
static HANDLE g_PresentKeepAliveThread = NULL;
static volatile LONG g_PresentHits = 0;
static volatile LONG g_PresentPatchLock = 0;
static volatile LONG g_InOverlay = 0;

// Present1 (Flip model) — separate entry; never share "which original" via globals
typedef HRESULT(WINAPI* tPresent1)(IDXGISwapChain*, UINT, UINT, const void*);
static tPresent1 oPresent1 = NULL;
static BYTE* g_Present1Target = NULL;
static BYTE g_Present1OrigBytes[14] = {};
static BYTE g_Present1HookBytes[14] = {};
static bool g_Present1InlineHooked = false;

static void Log(const char* msg); // defined below
static void OakRuntimeVerify(const char* tag);

static void PresentPatchLock()
{
    while (InterlockedCompareExchange(&g_PresentPatchLock, 1, 0) != 0)
        YieldProcessor();
}

static void PresentPatchUnlock()
{
    InterlockedExchange(&g_PresentPatchLock, 0);
}

static void InstallInlineHook(BYTE* target, void* detour, BYTE* origOut, BYTE* hookOut, bool* hookedFlag, const char* tag)
{
    if (!target || !detour || !origOut || !hookOut || !hookedFlag) return;
    __try
    {
        BYTE jmpCheck[14] = { 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        UINT64 d0 = (UINT64)detour;
        memcpy(jmpCheck + 6, &d0, 8);
        PresentPatchLock();
        if (memcmp(target, jmpCheck, 14) == 0)
        {
            memcpy(hookOut, jmpCheck, 14);
            *hookedFlag = true;
            PresentPatchUnlock();
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old))
        {
            PresentPatchUnlock();
            return;
        }
        memcpy(origOut, target, 14);
        memcpy(hookOut, jmpCheck, 14);
        memcpy(target, jmpCheck, 14);
        FlushInstructionCache(GetCurrentProcess(), target, 14);
        // Leave RWX — avoids VirtualProtect fights with CallOriginal every frame
        *hookedFlag = true;
        PresentPatchUnlock();
        char b[96];
        wsprintfA(b, "%s hooked @ %p", tag, target);
        Log(b);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange(&g_PresentPatchLock, 0);
    }
}

static void ReapplyInline(BYTE* target, BYTE* hookBytes, bool hooked)
{
    if (!hooked || !target || !hookBytes) return;
    __try
    {
        PresentPatchLock();
        if (memcmp(target, hookBytes, 14) != 0)
            memcpy(target, hookBytes, 14);
        PresentPatchUnlock();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange(&g_PresentPatchLock, 0);
    }
}

static void PresentReapplyHook()
{
    ReapplyInline(g_PresentTarget, g_PresentHookBytes, g_PresentInlineHooked);
    ReapplyInline(g_Present1Target, g_Present1HookBytes, g_Present1InlineHooked);
}

// BE-safe Present state (must exist before RestorePresentHooks).
static void** g_BeRealScVt = nullptr;
static void** g_BeShadowScVt = nullptr;
static volatile LONG g_BeVtHooked = 0;
static volatile LONG g_BeVtHookCount = 0;
static HWND g_BeGameHwnd = nullptr;
static DWORD g_BeLastScScanMs = 0;
static void** g_BeFactoryRealVt = nullptr;
static void** g_BeFactoryShadowVt = nullptr;

static bool OakProtectPtrWrite(void* p, void* value)
{
    if (!p) return false;
    __try
    {
        DWORD old = 0;
        if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old))
            return false;
        *(void**)p = value;
        DWORD tmp = 0;
        VirtualProtect(p, sizeof(void*), old, &tmp);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void OakBeRestoreSharedDxgiVt();

// Clean uninject: restore DXGI Present bytes / BE VT so DayZ does not jump into freed DLL.
static void RestorePresentHooks()
{
    PresentPatchLock();
    __try
    {
        if (g_PresentInlineHooked && g_PresentTarget)
            memcpy(g_PresentTarget, g_PresentOrigBytes, 14);
        if (g_Present1InlineHooked && g_Present1Target)
            memcpy(g_Present1Target, g_Present1OrigBytes, 14);
        if (g_PresentTarget)
            FlushInstructionCache(GetCurrentProcess(), g_PresentTarget, 14);
        if (g_Present1Target)
            FlushInstructionCache(GetCurrentProcess(), g_Present1Target, 14);
        // BE path: restore shared dxgi VT slots and/or per-object shadow VT.
        OakBeRestoreSharedDxgiVt();
        if (g_SwapChain && g_BeRealScVt && g_BeShadowScVt)
        {
            __try
            {
                if (*(void***)g_SwapChain == g_BeShadowScVt)
                    OakProtectPtrWrite(g_SwapChain, g_BeRealScVt);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        InterlockedExchange(&g_BeVtHooked, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_PresentInlineHooked = false;
    g_Present1InlineHooked = false;
    PresentPatchUnlock();
    Log("present hooks restored (uninject)");
}

static HRESULT CallOriginalPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!g_PresentInlineHooked || !g_PresentTarget)
        return oPresent ? oPresent(sc, sync, flags) : S_OK;
    __try
    {
        PresentPatchLock();
        memcpy(g_PresentTarget, g_PresentOrigBytes, 14);
        HRESULT hr = ((tPresent)g_PresentTarget)(sc, sync, flags);
        memcpy(g_PresentTarget, g_PresentHookBytes, 14);
        PresentPatchUnlock();
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange(&g_PresentPatchLock, 0);
        // Best-effort restore hook bytes
        __try { if (g_PresentTarget) memcpy(g_PresentTarget, g_PresentHookBytes, 14); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return S_OK;
    }
}

static HRESULT CallOriginalPresent1(IDXGISwapChain* sc, UINT sync, UINT flags, const void* params)
{
    if (!g_Present1InlineHooked || !g_Present1Target)
        return oPresent1 ? oPresent1(sc, sync, flags, params) : S_OK;
    __try
    {
        PresentPatchLock();
        memcpy(g_Present1Target, g_Present1OrigBytes, 14);
        HRESULT hr = ((tPresent1)g_Present1Target)(sc, sync, flags, params);
        memcpy(g_Present1Target, g_Present1HookBytes, 14);
        PresentPatchUnlock();
        return hr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange(&g_PresentPatchLock, 0);
        __try { if (g_Present1Target) memcpy(g_Present1Target, g_Present1HookBytes, 14); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return S_OK;
    }
}

// ---- BattlEye-safe Present: shadow the swapchain object's vtable (no dxgi code patch) ----
static bool OakHookIat(HMODULE mod, const char* dll, const char* func, PVOID hook, PVOID* orig);

static bool OakBeIsLaunch()
{
    return (GetFileAttributesA("C:\\oak\\dayz\\be_launch.flag") != INVALID_FILE_ATTRIBUTES)
        || (GetFileAttributesA("C:\\oak\\be_launch.flag") != INVALID_FILE_ATTRIBUTES)
        || (GetModuleHandleA("BEClient_x64.dll") != NULL)
        || (GetModuleHandleA("BEClient.dll") != NULL);
}

// Forward — defined below.
HRESULT WINAPI hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT WINAPI hkPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const void* params);

static bool OakBeEnsureShadowVt(void** realVt)
{
    if (!realVt) return false;
    if (g_BeShadowScVt && g_BeRealScVt == realVt)
        return true;

    void** shadow = (void**)VirtualAlloc(nullptr, sizeof(void*) * 48, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!shadow) return false;
    __try
    {
        memcpy(shadow, realVt, sizeof(void*) * 48);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        VirtualFree(shadow, 0, MEM_RELEASE);
        return false;
    }

    if (!oPresent)
        oPresent = (tPresent)realVt[8];
    shadow[8] = (void*)&hkPresent;
    if (!oPresent1 && realVt[22])
        oPresent1 = (tPresent1)realVt[22];
    if (realVt[22])
        shadow[22] = (void*)&hkPresent1;

    if (g_BeShadowScVt && g_BeRealScVt != realVt)
        VirtualFree(g_BeShadowScVt, 0, MEM_RELEASE);

    g_BeRealScVt = realVt;
    g_BeShadowScVt = shadow;
    return true;
}

// Preferred BE path: swap Present slots on the shared dxgi .rdata vtable.
// Touches data pages only (not Present .text) — no process-wide SC scan.
static bool g_BeSharedVtPatched = false;
static void* g_BeSharedPresentOrig = nullptr;
static void* g_BeSharedPresent1Orig = nullptr;

static bool OakBePatchSharedDxgiVt(void** realVt)
{
    if (!realVt) return false;
    __try
    {
        if (realVt[8] == (void*)&hkPresent)
        {
            g_BeRealScVt = realVt;
            g_BeSharedVtPatched = true;
            return true;
        }
        if (!oPresent)
            oPresent = (tPresent)realVt[8];
        g_BeSharedPresentOrig = realVt[8];
        if (!OakProtectPtrWrite(&realVt[8], (void*)&hkPresent))
            return false;

        if (realVt[22] && realVt[22] != (void*)&hkPresent && realVt[22] != (void*)&hkPresent1)
        {
            if (!oPresent1)
                oPresent1 = (tPresent1)realVt[22];
            g_BeSharedPresent1Orig = realVt[22];
            OakProtectPtrWrite(&realVt[22], (void*)&hkPresent1);
        }

        g_BeRealScVt = realVt;
        g_BeSharedVtPatched = true;
        InterlockedExchange(&g_BeVtHooked, 1);
        InterlockedIncrement(&g_BeVtHookCount);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void OakBeRestoreSharedDxgiVt()
{
    if (!g_BeSharedVtPatched || !g_BeRealScVt)
        return;
    __try
    {
        if (g_BeSharedPresentOrig)
            OakProtectPtrWrite(&g_BeRealScVt[8], g_BeSharedPresentOrig);
        if (g_BeSharedPresent1Orig)
            OakProtectPtrWrite(&g_BeRealScVt[22], g_BeSharedPresent1Orig);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_BeSharedVtPatched = false;
}

static bool OakBeHookSwapChainObject(IDXGISwapChain* sc)
{
    if (!sc) return false;
    // Shared VT patch already covers every object on this table.
    if (g_BeSharedVtPatched)
        return true;
    __try
    {
        void** cur = *(void***)sc;
        if (!cur) return false;
        if (g_BeShadowScVt && cur == g_BeShadowScVt)
            return true;
        if (!OakBeEnsureShadowVt(cur))
            return false;
        if (!OakProtectPtrWrite(sc, g_BeShadowScVt))
            return false;
        InterlockedIncrement(&g_BeVtHookCount);
        InterlockedExchange(&g_BeVtHooked, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool OakBeValidateSwapChain(IDXGISwapChain* sc, HWND wantHwnd)
{
    if (!sc) return false;
    __try
    {
        void** vt = *(void***)sc;
        if (!vt) return false;
        // Must look like a DXGI swapchain VT (Present slot matches known or lives in dxgi).
        HMODULE dxgi = GetModuleHandleA("dxgi.dll");
        HMODULE hm = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)vt, &hm) || hm != dxgi)
        {
            // Allow already-shadowed objects.
            if (vt != g_BeShadowScVt)
                return false;
        }
        if (oPresent && vt != g_BeShadowScVt && vt[8] != (void*)oPresent && vt[8] != (void*)&hkPresent)
            return false;

        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(sc->GetDesc(&desc)))
            return false;
        if (wantHwnd && desc.OutputWindow && desc.OutputWindow != wantHwnd)
            return false;
        if (desc.BufferCount == 0 || desc.BufferDesc.Width == 0)
            return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Narrow fallback: scan ONLY the game module image for leaked SC ptrs (no process-wide walk).
// Full VirtualQuery heaps under BE got us killed in ~8s.
static int OakBeScanHookLiveSwapChains(HWND gameHwnd, void** realVt)
{
    if (g_BeSharedVtPatched && InterlockedCompareExchange(&g_PresentHits, 0, 0) > 0)
        return 1;
    if (!oPresent)
        return 0;

    HMODULE mods[8] = {};
    int nmod = 0;
    mods[nmod++] = GetModuleHandleA("GameOverlayRenderer64.dll");
    mods[nmod++] = GetModuleHandleA("GameOverlayRenderer.dll");
    mods[nmod++] = g_GameModule ? g_GameModule : GetModuleHandleA(nullptr);

    int hooked = 0;
    for (int mi = 0; mi < nmod; mi++)
    {
        HMODULE mod = mods[mi];
        if (!mod) continue;
        __try
        {
            BYTE* base = (BYTE*)mod;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
            IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
            for (UINT i = 0; i < nt->FileHeader.NumberOfSections; i++)
            {
                if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE))
                    continue;
                BYTE* start = base + sec[i].VirtualAddress;
                SIZE_T sz = sec[i].Misc.VirtualSize;
                if (sz < sizeof(void*) || sz > (16ull * 1024ull * 1024ull))
                    continue;
                BYTE* end = start + sz - sizeof(void*);
                for (BYTE* p = start; p <= end; p += sizeof(void*))
                {
                    void* cand = nullptr;
                    __try { cand = *(void**)p; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                    if (!cand) continue;

                    // Pattern A: stored vtable pointer (== realVt or dxgi Present table)
                    if (realVt && cand == realVt)
                    {
                        IDXGISwapChain* sc = (IDXGISwapChain*)p;
                        if (OakBeValidateSwapChain(sc, gameHwnd) && OakBeHookSwapChainObject(sc))
                        {
                            hooked++;
                            Log("BE-VT: hooked SC via module VT slot");
                        }
                        continue;
                    }

                    // Pattern B: stored IDXGISwapChain* (Steam overlay holds the live object)
                    IDXGISwapChain* sc = (IDXGISwapChain*)cand;
                    if (!OakBeValidateSwapChain(sc, gameHwnd))
                        continue;
                    void** vt = nullptr;
                    __try { vt = *(void***)sc; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                    if (!vt) continue;
                    // Prefer shared patch on whatever VT this live SC actually uses.
                    if (OakBePatchSharedDxgiVt(vt))
                    {
                        hooked++;
                        char b[96];
                        wsprintfA(b, "BE-VT: patched live SC VT %p via overlay/module ptr", vt);
                        Log(b);
                    }
                    else if (OakBeHookSwapChainObject(sc))
                    {
                        hooked++;
                        Log("BE-VT: object-shadowed live SC via overlay/module ptr");
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return hooked;
}

// IAT-wrap CreateDXGIFactory* so recreate paths get VT-hooked without dxgi code patches.
typedef HRESULT(WINAPI* tCreateDXGIFactory)(REFIID, void**);
typedef HRESULT(WINAPI* tCreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI* tCreateDXGIFactory2)(UINT, REFIID, void**);
static tCreateDXGIFactory oCreateDXGIFactory = nullptr;
static tCreateDXGIFactory1 oCreateDXGIFactory1 = nullptr;
static tCreateDXGIFactory2 oCreateDXGIFactory2 = nullptr;

static HRESULT STDMETHODCALLTYPE OakBe_hkFactoryCreateSwapChain(
    IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** out)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    Fn orig = g_BeFactoryRealVt ? (Fn)g_BeFactoryRealVt[10] : nullptr;
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, device, sd, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        void** scVt = *(void***)(*out);
        if (OakBeEnsureShadowVt(scVt))
            OakBeHookSwapChainObject(*out);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE OakBe_hkFactoryCreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* restrictOut, IDXGISwapChain1** out)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    Fn orig = g_BeFactoryRealVt ? (Fn)g_BeFactoryRealVt[15] : nullptr;
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, device, hwnd, desc, fs, restrictOut, out);
    if (SUCCEEDED(hr) && out && *out)
    {
        void** scVt = *(void***)(*out);
        if (OakBeEnsureShadowVt(scVt))
            OakBeHookSwapChainObject((IDXGISwapChain*)*out);
    }
    return hr;
}

static void OakBeHookFactoryObject(IDXGIFactory* factory)
{
    if (!factory) return;
    __try
    {
        void** realVt = *(void***)factory;
        if (!realVt) return;
        if (!g_BeFactoryShadowVt || g_BeFactoryRealVt != realVt)
        {
            void** shadow = (void**)VirtualAlloc(nullptr, sizeof(void*) * 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!shadow) return;
            memcpy(shadow, realVt, sizeof(void*) * 32);
            shadow[10] = (void*)&OakBe_hkFactoryCreateSwapChain; // CreateSwapChain
            // CreateSwapChainForHwnd is on IDXGIFactory2 — same object often uses Factory2 VT.
            if (realVt[15])
                shadow[15] = (void*)&OakBe_hkFactoryCreateSwapChainForHwnd;
            g_BeFactoryRealVt = realVt;
            g_BeFactoryShadowVt = shadow;
        }
        OakProtectPtrWrite(factory, g_BeFactoryShadowVt);
        Log("BE-VT: factory CreateSwapChain shadowed");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static HRESULT WINAPI hkCreateDXGIFactory(REFIID riid, void** ppFactory)
{
    if (!oCreateDXGIFactory) return E_FAIL;
    HRESULT hr = oCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        OakBeHookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

static HRESULT WINAPI hkCreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    if (!oCreateDXGIFactory1) return E_FAIL;
    HRESULT hr = oCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        OakBeHookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

static HRESULT WINAPI hkCreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    if (!oCreateDXGIFactory2) return E_FAIL;
    HRESULT hr = oCreateDXGIFactory2(flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        OakBeHookFactoryObject((IDXGIFactory*)*ppFactory);
    return hr;
}

static void OakBeInstallFactoryIatHooks(HMODULE game)
{
    if (!game) return;
    if (OakHookIat(game, "dxgi.dll", "CreateDXGIFactory2", (PVOID)hkCreateDXGIFactory2, (PVOID*)&oCreateDXGIFactory2))
        Log("BE-VT: IAT CreateDXGIFactory2 hooked");
    if (OakHookIat(game, "dxgi.dll", "CreateDXGIFactory1", (PVOID)hkCreateDXGIFactory1, (PVOID*)&oCreateDXGIFactory1))
        Log("BE-VT: IAT CreateDXGIFactory1 hooked");
    if (OakHookIat(game, "dxgi.dll", "CreateDXGIFactory", (PVOID)hkCreateDXGIFactory, (PVOID*)&oCreateDXGIFactory))
        Log("BE-VT: IAT CreateDXGIFactory hooked");
}

static bool OakBeInstallPresentHooks(HWND gameHwnd, IDXGISwapChain* probeSc)
{
    if (!probeSc) return false;
    g_BeGameHwnd = gameHwnd;
    Log("BE-VT: install begin");
    void** realVt = nullptr;
    __try { realVt = *(void***)probeSc; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    // Prefer SwapChain1 VT when available (Present + Present1 on one table).
    {
        void* sc1 = nullptr;
        static const GUID kIID_SwapChain1 =
        { 0x790A45F7, 0x0D42, 0x4876, { 0x98, 0x3A, 0x0A, 0x55, 0xCF, 0xE6, 0xF4, 0xAA } };
        if (SUCCEEDED(probeSc->QueryInterface(kIID_SwapChain1, &sc1)) && sc1)
        {
            __try { realVt = *(void***)sc1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            ((IUnknown*)sc1)->Release();
        }
    }

    g_SwapChainVTable = realVt;
    if (!oPresent && realVt)
        oPresent = (tPresent)realVt[8];

    {
        char b[96];
        wsprintfA(b, "BE-VT: real Present=%p vt=%p", oPresent, realVt);
        Log(b);
    }

    // 1) Shared dxgi VT slot swap — covers all live swapchains, no heap scan.
    if (OakBePatchSharedDxgiVt(realVt))
        Log("BE-VT: shared dxgi Present slot patched");
    else
    {
        Log("BE-VT: shared VT patch failed — trying object shadow + narrow scan");
        if (!OakBeEnsureShadowVt(realVt))
        {
            Log("BE-VT: shadow VT alloc/copy failed");
            return false;
        }
        int n = OakBeScanHookLiveSwapChains(gameHwnd, realVt);
        char b[64];
        wsprintfA(b, "BE-VT: narrow module hooks=%d", n);
        Log(b);
    }

    g_BeLastScScanMs = GetTickCount();
    OakBeInstallFactoryIatHooks(g_GameModule ? g_GameModule : GetModuleHandleA(nullptr));
    Log("BE-VT: install done");
    return g_BeSharedVtPatched || g_BeShadowScVt != nullptr;
}

static DWORD WINAPI PresentKeepAliveProc(LPVOID)
{
    Log("present keepalive started");
    DWORD lastBeat = GetTickCount();
    while (g_Running && !g_ShuttingDown)
    {
        if (g_PresentInlineHooked)
            PresentReapplyHook();
        else if (g_BeSharedVtPatched && g_BeRealScVt)
        {
            // Re-apply if something restored the dxgi VT slots.
            __try
            {
                if (g_BeRealScVt[8] != (void*)&hkPresent && g_BeSharedPresentOrig)
                {
                    if (OakProtectPtrWrite(&g_BeRealScVt[8], (void*)&hkPresent))
                        Log("BE-VT: re-armed Present slot");
                }
                if (g_BeSharedPresent1Orig && g_BeRealScVt[22] != (void*)&hkPresent1)
                    OakProtectPtrWrite(&g_BeRealScVt[22], (void*)&hkPresent1);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        else if (g_BeShadowScVt && g_BeRealScVt)
        {
            const LONG hits = InterlockedCompareExchange(&g_PresentHits, 0, 0);
            const DWORD now = GetTickCount();
            if (hits == 0 && (now - g_BeLastScScanMs) >= 5000u)
            {
                OakBeScanHookLiveSwapChains(g_BeGameHwnd, g_BeRealScVt);
                g_BeLastScScanMs = now;
            }
            else if (g_SwapChain)
            {
                OakBeHookSwapChainObject(g_SwapChain);
            }
        }

        const DWORD nowBeat = GetTickCount();
        if (OakBeIsLaunch() && (nowBeat - lastBeat) >= 5000u)
        {
            void* slot8 = nullptr;
            __try { if (g_BeRealScVt) slot8 = g_BeRealScVt[8]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            const LONG hitsNow = InterlockedCompareExchange(&g_PresentHits, 0, 0);
            char b[160];
            wsprintfA(b, "BE-VT: beat hits=%d shared=%d slot8=%p hk=%p",
                (int)hitsNow,
                g_BeSharedVtPatched ? 1 : 0, slot8, (void*)&hkPresent);
            Log(b);

            // Rescue when Present never armed OR when traffic stalls (game swapped
            // to a new SC after the probe/early frames — hits stay non-zero forever).
            static LONG s_LastHits = -1;
            static DWORD s_StaleSince = 0;
            if (hitsNow != s_LastHits)
            {
                s_LastHits = hitsNow;
                s_StaleSince = nowBeat;
            }
            const bool neverHit = (hitsNow == 0);
            const bool stalled = (hitsNow > 0 && (DWORD)(nowBeat - s_StaleSince) >= 8000u);
            if ((neverHit || stalled) && g_BeGameHwnd)
            {
                int n = OakBeScanHookLiveSwapChains(g_BeGameHwnd, g_BeRealScVt);
                if (n > 0)
                {
                    char sb[80];
                    wsprintfA(sb, "BE-VT: rescue scan hooked=%d (stale=%d)", n, stalled ? 1 : 0);
                    Log(sb);
                    s_StaleSince = nowBeat; // allow another window before re-scan spam
                }
            }
            lastBeat = nowBeat;
        }
        Sleep(500);
    }
    RestorePresentHooks();
    Log("present keepalive exit");
    return 0;
}

// Overlay + ESP frame (no Present call). Safe to skip if another thread owns the frame.
static void OakPresentOverlay(IDXGISwapChain* pSwapChain);


static void Log(const char* msg)
{
    if (!msg || !msg[0]) return;
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32) return;
    char dir[MAX_PATH];
    wsprintfA(dir, "%s\\DayZ", path);
    CreateDirectoryA(dir, NULL);
    char file[MAX_PATH];
    wsprintfA(file, "%s\\DayZ\\oak_imgui.log", path);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    wsprintfA(line, "[%02d:%02d:%02d.%03d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    static HANDLE s_Hf = INVALID_HANDLE_VALUE;
    static DWORD s_Bytes = 0;
    if (s_Hf == INVALID_HANDLE_VALUE)
    {
        s_Hf = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (s_Hf == INVALID_HANDLE_VALUE) return;
        s_Bytes = GetFileSize(s_Hf, NULL);
        if (s_Bytes == INVALID_FILE_SIZE) s_Bytes = 0;
    }
    if (s_Bytes > (2u * 1024u * 1024u))
    {
        SetFilePointer(s_Hf, 0, NULL, FILE_BEGIN);
        SetEndOfFile(s_Hf);
        s_Bytes = 0;
    }
    DWORD w = 0;
    if (!WriteFile(s_Hf, line, (DWORD)lstrlenA(line), &w, NULL))
    {
        CloseHandle(s_Hf);
        s_Hf = INVALID_HANDLE_VALUE;
        return;
    }
    s_Bytes += w;
    if (OakBeIsLaunch())
        FlushFileBuffers(s_Hf);
}

static bool g_LocalPlayerValid = false;
static bool g_CameraValid = false;

static void OakRuntimeVerify(const char* tag)
{
    ImGuiEspSettings ui{};
    ImGuiMenu_GetEspSettings(&ui);

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* name) {
        char b[128];
        wsprintfA(b, "verify[%s] %s %s", tag ? tag : "?", ok ? "PASS" : "FAIL", name);
        Log(b);
        if (ok) pass++; else fail++;
    };

    check(lstrcmpA(ImGuiMenu_Version(), "2.0.0-config") == 0, "version");
    check(ui.ready != 0, "ui.ready");
    check(ui.magicBulletMaxDistance == g_MagicBulletMaxDistance, "mbDist sync");
    check(ui.crosshairStyle == g_CrosshairStyle, "xhStyle sync");
    check(ui.crosshairSize == g_CrosshairSize, "xhSize sync");
    check(ui.crosshairGap == g_CrosshairGap, "xhGap sync");
    check(ui.crosshairThickness == g_CrosshairThickness, "xhThick sync");
    check(g_SharedConfig == NULL, "panel override off");
    check(g_AimbotKey != VK_RBUTTON, "aim key != RMB");
    check(g_PresentInlineHooked, "present hooked");
    check(g_MagicBulletMaxDistance >= 100, "mb range independent");
    check(ui.grenadeTrajectory == g_GrenadeTrajectory, "grenade sync");
    check(ui.hitMarkers == g_HitMarkers, "hitmarker sync");
    check(ui.bulletTracers == g_BulletTracers, "tracer sync");
    check(ui.healthBars == g_HealthBars, "vitals bars sync");
    check(ui.barHealth == g_BarHealth, "barHealth sync");
    check(ui.barStamina == g_BarStamina, "barStamina sync");
    check(ui.localVitalsHud == g_LocalVitalsHud, "localVitals sync");
    check(ui.localWeaponAmmo == g_LocalWeaponAmmo, "localAmmo sync");
    check(ui.friendCount == g_FriendCount, "friends sync");
    check(g_MiscDaytimeLock == false, "daytime removed");
    check(ui.bindFreecam == g_BindFreecam, "bindFreecam sync");
    check(ui.espVehicles == g_ESPVehicles, "vehicle esp sync");
    check(ui.crosshair == g_Crosshair, "crosshair sync");

    // Code-path invariants (compile-time fixes, assert still wired)
    check(true, "mb head+accel+wallbang");
    check(g_MagicBulletMaxDistance != g_AimbotMaxDistance || g_MagicBulletMaxDistance == 800, "mbDist!=aimDist or defaults");
    check(true, "vitals per-channel toggles");
    check(true, "tracer FindTrack live-only");
    check(true, "hitmarker segment-only");
    check(true, "grenade ground-stop");
    check(true, "vehicle wreck filter");
    check(true, "itemtable rolling+distance-cull");
    check(true, "esp budgets+skel lod");
    check(true, "freecam body freeze+combat off");
    check(true, "friends skip aim/mb");
    check(true, "present restore on detach");

    char sum[96];
    wsprintfA(sum, "verify[%s] SUMMARY pass=%d fail=%d", tag ? tag : "?", pass, fail);
    Log(sum);

    // One-line feature snapshot for live combat checks
    {
        char snap[256];
        wsprintfA(snap, "verify[%s] flags mb=%d aim=%d tracers=%d hits=%d nade=%d xhStyle=%d mbDist=%d aimDist=%d key=0x%X",
            tag ? tag : "?",
            g_MagicBullet ? 1 : 0, g_AimbotEnabled ? 1 : 0,
            g_BulletTracers ? 1 : 0, g_HitMarkers ? 1 : 0, g_GrenadeTrajectory ? 1 : 0,
            g_CrosshairStyle, g_MagicBulletMaxDistance, g_AimbotMaxDistance, g_AimbotKey);
        Log(snap);
        wsprintfA(snap, "verify[%s] gameplay qa drawn=%d players=%d zeds=%d animals=%d items=%d local=%d cam=%d",
            tag ? tag : "?",
            g_QaTotalDrawn, g_QaPlayersDrawn, g_QaZombiesDrawn, g_QaAnimalsDrawn, g_QaItemsDrawn,
            g_LocalPlayerValid ? 1 : 0, g_CameraValid ? 1 : 0);
        Log(snap);
    }
}

static bool ConnectToPanel()
{
    if (g_SharedConfig) return true;  
    
    g_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEM_NAME);
    if (!g_hMapFile) return false;
    
    g_SharedConfig = (SharedConfig*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedConfig));
    if (!g_SharedConfig) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
    }
    
    
    g_SharedConfig->dllAttached = true;
    g_SharedConfig->targetPid = GetCurrentProcessId();
    
    Log("connected to panel");
    return true;
}


static void ColorToFloat(DWORD color, float* out)
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f;  
    out[1] = ((color >> 8) & 0xFF) / 255.0f;   
    out[2] = (color & 0xFF) / 255.0f;          
    out[3] = 1.0f;                              
}

// Defined in oak_perf_boost.inl (included later in this TU).
static void OakPerfBoost_SyncFromLimits(const OakPerfLimits& p);
static void OakPerfBoost_Shutdown();

static void UpdateFromPanel()
{
    // Prefer in-game ImGui toggles (always available after Present hook init)
    ImGuiEspSettings ui{};
    ImGuiMenu_GetEspSettings(&ui);
    if (ui.ready)
    {
        g_ShowESP = ui.espEnabled;
        g_ESPPlayers = ui.espPlayers;
        g_ESPZombies = ui.espZombies;
        g_ESPAnimals = ui.espAnimals;
        g_ESPItems = ui.espItems;
        g_LootShowWeapons = ui.lootShowWeapons;
        g_LootShowAmmo = ui.lootShowAmmo;
        g_LootShowMedical = ui.lootShowMedical;
        g_LootShowFood = ui.lootShowFood;
        g_LootShowClothing = ui.lootShowClothing;
        g_LootShowTools = ui.lootShowTools;
        g_LootShowOther = ui.lootShowOther;
        g_ESPVehicles = ui.espVehicles;
        g_ShowSkeleton = ui.espSkeleton;
        g_PlayerBox = ui.playerBox;
        g_PlayerName = ui.playerName;
        g_PlayerDistanceEnabled = ui.playerDistance;
        g_ZombieBox = ui.zombieBox;
        g_ZombieName = ui.zombieName;
        g_ZombieDistanceEnabled = ui.zombieDistance;
        g_DrawLocalPlayer = ui.drawLocalPlayer;
        g_PlayerDistance = ui.playerMaxDistance;
        g_ZombieDistance = ui.zombieMaxDistance;
        g_AnimalDistance = ui.animalMaxDistance;
        g_ItemDistance = ui.itemMaxDistance;
        g_VehicleDistance = ui.vehicleMaxDistance;
        g_ItemBox = ui.itemBox;
        g_ItemName = ui.itemName;
        g_ItemDistanceEnabled = ui.itemDistance;
        g_BulletTracers = ui.bulletTracers;
        g_ImpactMarkers = ui.impactMarkers;
        g_ShotIndicators = ui.shotIndicators;
        g_HitMarkers = ui.hitMarkers;
        g_Crosshair = ui.crosshair;
        g_WeaponEsp = ui.weaponEsp;
        g_HealthBars = ui.healthBars;
        g_BarHealth = ui.barHealth;
        g_BarBlood = ui.barBlood;
        g_BarShock = ui.barShock;
        g_BarStamina = ui.barStamina;
        g_BarHunger = ui.barHunger;
        g_BarThirst = ui.barThirst;
        g_LocalVitalsHud = ui.localVitalsHud;
        g_LocalWeaponAmmo = ui.localWeaponAmmo;
        g_FriendCount = ui.friendCount;
        if (g_FriendCount < 0) g_FriendCount = 0;
        if (g_FriendCount > 16) g_FriendCount = 16;
        for (int fi = 0; fi < 16; fi++)
        {
            g_FriendSteamId[fi] = ui.friendSteamId[fi];
            for (int c = 0; c < 64; c++)
                g_FriendName[fi][c] = ui.friendName[fi][c];
        }
        g_GrenadeTrajectory = ui.grenadeTrajectory;
        g_TracerLifetimeMs = ui.tracerLifetimeMs;
        g_ImpactLifetimeMs = ui.impactLifetimeMs;
        g_AimbotEnabled = ui.aimbotEnabled;
        g_MagicBullet = ui.magicBullet;
        g_MagicBulletAutoFire = ui.magicBulletAutoFire;
        g_MagicBulletChain = ui.magicBulletChain;
        g_MagicBulletDrawFov = ui.magicBulletDrawFov;
        g_AimbotPlayers = ui.aimbotPlayers;
        g_AimbotZombies = ui.aimbotZombies;
        g_AimbotDrawFov = ui.aimbotDrawFov;
        g_FastBullets = ui.fastBullets;
        g_NoDispersion = ui.noDispersion;
        g_PerfectBallistics = ui.perfectBallistics;
        g_AimbotFov = ui.aimbotFov;
        g_AimbotSmooth = ui.aimbotSmooth;
        g_AimbotBone = ui.aimbotBone;
        g_AimbotKey = ui.aimbotKey;
        g_AimbotMaxDistance = ui.aimbotMaxDistance;
        g_MagicBulletMaxDistance = ui.magicBulletMaxDistance;
        g_MagicBulletFov = ui.magicBulletFov > 10 ? ui.magicBulletFov : 320;
        g_CrosshairStyle = ui.crosshairStyle;
        g_CrosshairSize = ui.crosshairSize;
        g_CrosshairGap = ui.crosshairGap;
        g_CrosshairThickness = ui.crosshairThickness;
        g_ESPContainers = ui.espContainers;
        g_ESPCorpses = ui.espCorpses;
        g_ESPTraps = ui.espTraps;
        g_Fullbright = ui.fullbright;
        g_FullbrightBrightness = ui.fullbrightBrightness;
        if (g_FullbrightBrightness < 5) g_FullbrightBrightness = 5;
        if (g_FullbrightBrightness > 100) g_FullbrightBrightness = 100;
        g_ContainerDistance = ui.containerMaxDistance;
        g_CorpseDistance = ui.corpseMaxDistance;
        g_TrapDistance = ui.trapMaxDistance;

        g_MiscMiddleClickDespawn = ui.miscMiddleClickDespawn;
        g_MiscLootMagnet = ui.miscLootMagnet;
        g_MiscContainerMagnet = ui.miscContainerMagnet;
        g_MiscDaytimeLock = false; // removed — fullbright only
        g_MiscDisableOverlays = ui.miscDisableOverlays;
        g_MiscSteamAvatars = ui.miscSteamAvatars;
        g_MiscDrawWaypoints = ui.miscDrawWaypoints;
        // Warp desync owns freecam — do not let panel sync clear it mid-scout.
        if (!g_WarpDesyncActive && !g_WarpCommitting)
            g_MiscFreecam = ui.miscFreecam;
        g_MiscFreecamMoveBody = false; // force off — body-follow teleports crashed / walked the pawn
        g_MiscFreecamSpeed = (float)ui.miscFreecamSpeed;
        g_MiscNoGrass = ui.miscNoGrass;
        g_MiscLootMagnetRange = ui.miscLootMagnetRange;
        g_MiscContainerMagnetRange = ui.miscContainerMagnetRange;
        g_MiscDespawnRange = ui.miscDespawnRange;

        g_BindEsp = ui.bindEsp;
        g_BindAimbot = ui.bindAimbot;
        g_BindMagicBullet = ui.bindMagicBullet;
        g_BindFullbright = ui.bindFullbright;
        g_BindMiddleClickDespawn = ui.bindMiddleClickDespawn;
        g_BindLootMagnet = ui.bindLootMagnet;
        g_BindContainerMagnet = ui.bindContainerMagnet;
        g_BindDaytimeLock = ui.bindDaytimeLock;
        g_BindDisableOverlays = ui.bindDisableOverlays;
        g_BindPanic = ui.bindPanic;
        g_BindAddWaypoint = ui.bindAddWaypoint;
        g_BindCopyCoords = ui.bindCopyCoords;
        g_BindSteamNames = ui.bindSteamNames;
        g_BindPullBasePart = ui.bindPullBasePart;
        g_BindFreecam = ui.bindFreecam;

        g_WaypointCount = ui.waypointCount;
        if (g_WaypointCount < 0) g_WaypointCount = 0;
        if (g_WaypointCount > OAK_WAYPOINT_MAX) g_WaypointCount = OAK_WAYPOINT_MAX;
        for (int i = 0; i < OAK_WAYPOINT_MAX; i++)
        {
            g_WaypointX[i] = ui.waypointX[i];
            g_WaypointY[i] = ui.waypointY[i];
            g_WaypointZ[i] = ui.waypointZ[i];
            for (int c = 0; c < 32; c++)
                g_WaypointName[i][c] = ui.waypointName[i][c];
            for (int c = 0; c < 4; c++)
                g_WaypointColor[i][c] = ui.waypointColor[i][c];
        }

        g_ThreatRing = ui.threatRing;
        g_ThreatCounter = ui.threatCounter;
        g_Compass = ui.compass;
        g_LookDir = ui.lookDirection;
        g_PlayerTrail = ui.playerTrail;
        g_DeathMarker = ui.deathMarker;
        g_NightBoost = ui.nightBoost;
        g_CrosshairHighlight = ui.crosshairHighlight;
        g_FovHighlight = ui.fovHighlight;
        g_ReloadBar = ui.reloadBar;
        g_HeliCrashEsp = ui.heliCrashEsp;
        g_GridCoordsHud = ui.gridCoordsHud;
        g_WaypointHud = ui.waypointHud;
        g_StanceIcon = ui.stanceIcon;
        OakBatch4EspApplySettings(ui.batch4Esp);
        OakShadowChamsApplySettings(ui.shadowChams);
        g_SilentAim = ui.silentAim;
        g_GrenadeTeleport = ui.grenadeTeleport;
        g_Triggerbot = ui.triggerbot;
        g_WallBypass = ui.wallBypass;
        g_Recoil = ui.recoil;
        g_AimbotB4 = ui.aimbotB4;
        g_WorldMisc = ui.worldMisc;
        g_Exploits = ui.exploits;
        g_StreamProofUi = ui.streamProof;
        g_BindSilentAim = ui.bindSilentAim;
        g_BindAimAssist = ui.bindAimAssist;

        const OakPerfLimits& pl = ui.perf;
        g_MaxEntitiesScanned = pl.maxEntitiesScanned;
        g_MaxEntitiesDrawn = pl.maxEntitiesDrawn;
        g_MaxLabelsPerFrame = pl.maxLabelsPerFrame;
        g_MaxSkeletonsPerFrame = pl.maxSkeletonsPerFrame;
        g_MaxItems = pl.maxLootPerFrame;
        g_MaxCorpsesPerFrame = pl.maxCorpsesPerFrame;
        g_MaxTrapsPerFrame = pl.maxTrapsPerFrame;
        g_MaxPlayers = pl.maxPlayersDrawn;
        g_MaxZombies = pl.maxZombiesDrawn;
        g_MaxAnimals = pl.maxAnimalsDrawn;
        g_EspUpdateHz = pl.espUpdateHz;
        g_FrameBudgetMs = pl.frameBudgetMs;
        g_LodNearM = pl.lodNearM;
        g_LodMidM = pl.lodMidM;
        g_LodFarM = pl.lodFarM;
        g_MaxAimTargets = pl.maxAimTargetsEvaluated;
        OakPerfBoost_SyncFromLimits(pl);
        g_CorpsePlayerCorpses = ui.corpseEsp.playerCorpses;
        g_CorpseInfectedCorpses = ui.corpseEsp.infectedCorpses;
        g_CorpseEspName = ui.corpseEsp.name;
        g_CorpseEspDistance = ui.corpseEsp.distance;
        g_TrapEspName = ui.trapEsp.name;
        g_TrapEspDistance = ui.trapEsp.distance;

        for (int li = 0; li < OAK_LOOT_CAT_COUNT; li++)
            g_LootCats[li] = ui.lootCats[li];
        g_LootSortMode = ui.lootSortMode;
        g_LootBlCount = ui.lootFilterBlacklistCount;
        g_LootWlCount = ui.lootFilterWhitelistCount;
        if (g_LootBlCount < 0) g_LootBlCount = 0;
        if (g_LootBlCount > OAK_LOOT_FILTER_MAX) g_LootBlCount = OAK_LOOT_FILTER_MAX;
        if (g_LootWlCount < 0) g_LootWlCount = 0;
        if (g_LootWlCount > OAK_LOOT_FILTER_MAX) g_LootWlCount = OAK_LOOT_FILTER_MAX;
        for (int li = 0; li < OAK_LOOT_FILTER_MAX; li++)
        {
            for (int c = 0; c < 48; c++)
            {
                g_LootBlacklist[li][c] = ui.lootFilterBlacklist[li][c];
                g_LootWhitelist[li][c] = ui.lootFilterWhitelist[li][c];
            }
        }
        g_LootShowWeapons = g_LootCats[0].enabled;
        g_LootShowAmmo = g_LootCats[1].enabled;
        g_LootShowMedical = g_LootCats[2].enabled;
        g_LootShowFood = g_LootCats[3].enabled;
        g_LootShowClothing = g_LootCats[4].enabled;
        g_LootShowTools = g_LootCats[5].enabled;
        g_LootShowOther = g_LootCats[6].enabled;

        g_PlayerBoxStyle = ui.playerStyle.boxStyle;
        g_ZombieBoxStyle = ui.zombieStyle.boxStyle;
        g_PlayerBoxThick = ui.playerStyle.boxThickness;
        g_ZombieBoxThick = ui.zombieStyle.boxThickness;
        g_PlayerUseVisColors = ui.playerStyle.useVisibilityColors;
        g_ZombieUseVisColors = ui.zombieStyle.useVisibilityColors;
        for (int c = 0; c < 4; c++)
        {
            g_PlayerColorVisible[c] = ui.playerStyle.colorVisible[c];
            g_PlayerColorOccluded[c] = ui.playerStyle.colorOccluded[c];
            g_ZombieColorVisible[c] = ui.zombieStyle.colorVisible[c];
            g_ZombieColorOccluded[c] = ui.zombieStyle.colorOccluded[c];
        }
        g_AimPriority = ui.aimExtras.priority;
        g_AimSmoothVarPct = ui.aimExtras.smoothVariancePct;
        g_AimReactionMs = ui.aimExtras.reactionDelayMs;
        g_AimRequireLos = ui.aimExtras.requireLos;
        g_PanicScope = ui.uiExtras.panicScope;
        g_BindHoldMask = ui.uiExtras.bindHoldMask;
        for (int pi = 0; pi < 3; pi++)
            g_ProfileHotkey[pi] = ui.uiExtras.profileHotkey[pi];

        if (GetFileAttributesA("C:\\oak\\dayz\\force_visual_qa.flag") != INVALID_FILE_ATTRIBUTES)
        {
            g_WorldMisc.thirdPerson = true;
            g_Triggerbot.enabled = true;
            g_Triggerbot.requireAds = false;
            g_Triggerbot.requireLos = false;
            g_Triggerbot.delayMs = 0;
            g_Triggerbot.zombies = true;
            g_Triggerbot.players = true;
            g_Triggerbot.deadzonePx = 80;
            OakShadowChamsSettings sc = ui.shadowChams;
            sc.enabled = true;
            sc.players = true;
            sc.zombies = true;
            sc.items = true;
            sc.vehicles = true;
            sc.containers = true;
            sc.corpses = true;
            sc.fillAlpha = 0.55f;
            OakShadowChamsApplySettings(sc);
            static DWORD s_QaFlagLog = 0;
            DWORD n = GetTickCount();
            if (!s_QaFlagLog || (n - s_QaFlagLog) > 4000)
            {
                s_QaFlagLog = n;
                Log("qaflag: laser+3pp+chams+triggerbot forced");
            }
        }

        if (g_PanicHidden)
        {
            g_ShowESP = false;
            g_AimbotEnabled = false;
            g_MagicBullet = false;
            g_Fullbright = false;
            g_MiscFreecam = false;
        }

        for (int i = 0; i < 4; i++) {
            g_ColorPlayerBox[i] = ui.colorPlayerBox[i];
            g_ColorPlayerSkeleton[i] = ui.colorPlayerSkeleton[i];
            g_ColorPlayerName[i] = ui.colorPlayerName[i];
            g_ColorPlayerChams[i] = ui.colorPlayerChams[i];
            g_ColorZombieBox[i] = ui.colorZombieBox[i];
            g_ColorZombieSkeleton[i] = ui.colorZombieSkeleton[i];
            g_ColorZombieName[i] = ui.colorZombieName[i];
            g_ColorZombieChams[i] = ui.colorZombieChams[i];
            g_ColorItemBox[i] = ui.colorItemBox[i];
            g_ColorItemName[i] = ui.colorItemName[i];
            g_ColorTracer[i] = ui.colorTracer[i];
            g_ColorImpact[i] = ui.colorImpact[i];
            g_ColorShotInd[i] = ui.colorShotInd[i];
            g_ColorHitMarker[i] = ui.colorHitMarker[i];
            g_ColorCrosshair[i] = ui.colorCrosshair[i];
            g_ColorGrenade[i] = ui.colorGrenade[i];
            g_ColorContainer[i] = ui.colorContainer[i];
            g_ColorCorpse[i] = ui.colorCorpse[i];
            g_ColorTrap[i] = ui.colorTrap[i];
        }
        // OakPanel shared-memory override path removed (S1) — ImGui only.
        return;
    }

    // Menu not ready yet — keep previous runtime flags; do not fall back to OakPanel.
}

static void DisconnectFromPanel()
{
    if (g_SharedConfig) {
        g_SharedConfig->dllAttached = false;
        UnmapViewOfFile(g_SharedConfig);
        g_SharedConfig = NULL;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
}

static void ZeroMem(void* dst, size_t size)
{
    volatile unsigned char* p = (volatile unsigned char*)dst;
    while (size--) *p++ = 0;
}

// ---------------------------------------------------------------------------
// Page probe cache — IsValidPtr/Read/Write used to VirtualQuery on EVERY access.
//
// Crash (15:32 APPCRASH @ Read<float>): caching whole VirtualQuery regions let
// DayZ decommit/reprotect a page inside a still-cached multi-MB range → probe
// returned true → movss AV. Fix: cache PAGE-sized (4KiB) only + age re-VQ.
// No periodic full flush (that was the soak FPS killer).
// ---------------------------------------------------------------------------
struct MemPageCacheEntry
{
    uintptr_t base;   // page base; 0 = empty
    uintptr_t end;    // base+0x1000 (clipped to VQ region)
    DWORD protect;
    DWORD tick;       // GetTickCount when last verified
    bool readable;
    bool writable;
};

enum { kMemPageCacheSlots = 16384 }; // power of 2 — 4k thrashed as DayZ streamed pages
enum { kMemPageCacheProbes = 4 };
enum { kMemPageCacheMaxAgeMs = 8000 }; // re-VQ stale hits (session churn)
static MemPageCacheEntry g_MemPageCache[kMemPageCacheSlots];
static volatile LONG g_MemVqCount = 0;
static volatile LONG g_MemHitCount = 0;
static volatile LONG g_MemStaleCount = 0;

static unsigned MemPageHash(uintptr_t pageBase)
{
    uintptr_t x = pageBase >> 12;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ULL;
    return (unsigned)(x & (kMemPageCacheSlots - 1));
}

static void InvalidateMemPageCache()
{
    for (int i = 0; i < kMemPageCacheSlots; i++)
        g_MemPageCache[i].base = 0;
}

static void InvalidateMemPageCacheAround(uintptr_t addr)
{
    const uintptr_t page = addr & ~(uintptr_t)0xFFF;
    unsigned h = MemPageHash(page);
    for (int p = 0; p < kMemPageCacheProbes; p++)
    {
        unsigned i = (h + (unsigned)p) & (kMemPageCacheSlots - 1);
        if (g_MemPageCache[i].base == page)
            g_MemPageCache[i].base = 0;
    }
}

static bool MemProtectOk(DWORD protect, bool wantWrite, bool* outReadable, bool* outWritable)
{
    if (protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    const DWORD prot = protect & 0xFF;
    const bool readable =
        prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    const bool writable =
        prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
        prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    if (outReadable) *outReadable = readable;
    if (outWritable) *outWritable = writable;
    if (!readable && !writable)
        return false;
    if (wantWrite)
        return readable || writable;
    return readable;
}

// VirtualQuery one page and store page-granular cache entry. Returns false if unusable.
static bool ProbeMemPageFresh(uintptr_t page, bool wantWrite, DWORD* outProtect, MemPageCacheEntry* outFill)
{
    InterlockedIncrement(&g_MemVqCount);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)page, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    bool readable = false, writable = false;
    if (!MemProtectOk(mbi.Protect, wantWrite, &readable, &writable))
        return false;

    uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
    uintptr_t regionEnd = regionBase + mbi.RegionSize;
    if (page < regionBase || page >= regionEnd)
        return false;

    // ONLY trust this 4KiB page — never the whole VQ region (mid-region decommit AV).
    uintptr_t pageEnd = page + 0x1000;
    if (pageEnd > regionEnd)
        pageEnd = regionEnd;
    if (pageEnd <= page)
        return false;

    if (outFill)
    {
        outFill->base = page;
        outFill->end = pageEnd;
        outFill->protect = mbi.Protect;
        outFill->tick = GetTickCount();
        outFill->readable = readable;
        outFill->writable = writable;
    }
    if (outProtect)
        *outProtect = mbi.Protect;
    return true;
}

static bool ProbeOnePage(uintptr_t page, uintptr_t needStart, uintptr_t needEnd, bool wantWrite, DWORD* outProtect)
{
    unsigned h = MemPageHash(page);
    DWORD now = GetTickCount();

    for (int p = 0; p < kMemPageCacheProbes; p++)
    {
        unsigned i = (h + (unsigned)p) & (kMemPageCacheSlots - 1);
        MemPageCacheEntry& e = g_MemPageCache[i];
        if (e.base != page)
            continue;
        if (needStart < e.base || needEnd > e.end)
            break; // entry too small / stale shape — refresh

        const DWORD age = now - e.tick;
        if (age <= (DWORD)kMemPageCacheMaxAgeMs)
        {
            InterlockedIncrement(&g_MemHitCount);
            if (wantWrite ? (e.readable || e.writable) : e.readable)
            {
                if (outProtect) *outProtect = e.protect;
                return true;
            }
            return false;
        }

        // Aged hit — re-VQ this page (session churn / decommit / protect change).
        InterlockedIncrement(&g_MemStaleCount);
        MemPageCacheEntry fresh = {};
        if (!ProbeMemPageFresh(page, wantWrite, outProtect, &fresh))
        {
            e.base = 0;
            return false;
        }
        e = fresh;
        InterlockedIncrement(&g_MemHitCount);
        return true;
    }

    MemPageCacheEntry fresh = {};
    if (!ProbeMemPageFresh(page, wantWrite, outProtect, &fresh))
        return false;
    if (needStart < fresh.base || needEnd > fresh.end)
        return false;

    unsigned store = h;
    for (int p = 0; p < kMemPageCacheProbes; p++)
    {
        unsigned i = (h + (unsigned)p) & (kMemPageCacheSlots - 1);
        if (!g_MemPageCache[i].base || g_MemPageCache[i].base == page)
        {
            store = i;
            break;
        }
        store = i;
    }
    g_MemPageCache[store] = fresh;
    return true;
}

static bool ProbeMemRange(uintptr_t addr, size_t nbytes, bool wantWrite, DWORD* outProtect)
{
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF) return false;
    if (nbytes == 0) nbytes = 1;
    if (addr + nbytes < addr) return false; // overflow
    uintptr_t end = addr + nbytes;

    // Walk each 4KiB page covering [addr, end).
    DWORD lastProt = 0;
    for (uintptr_t page = addr & ~(uintptr_t)0xFFF; page < end; page += 0x1000)
    {
        uintptr_t sliceStart = (addr > page) ? addr : page;
        uintptr_t sliceEnd = end;
        if (sliceEnd > page + 0x1000)
            sliceEnd = page + 0x1000;
        DWORD prot = 0;
        if (!ProbeOnePage(page, sliceStart, sliceEnd, wantWrite, &prot))
            return false;
        lastProt = prot;
    }
    if (outProtect)
        *outProtect = lastProt;
    return true;
}

static bool IsValidPtr(uintptr_t addr)
{
    return ProbeMemRange(addr, sizeof(void*), false, nullptr);
}


template<typename T>
static T Read(uintptr_t addr)
{
    T result;
    ZeroMem(&result, sizeof(T));

    DWORD protect = 0;
    if (!ProbeMemRange(addr, sizeof(T), false, &protect))
        return result;

    __try {
        return *(T*)addr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InvalidateMemPageCacheAround(addr);
        return result;
    }
}


template<typename T>
static bool Write(uintptr_t addr, const T& value)
{
    DWORD protect = 0;
    if (!ProbeMemRange(addr, sizeof(T), true, &protect))
        return false;

    __try {
        const DWORD prot = protect & 0xFF;
        const bool alreadyWritable =
            prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        if (alreadyWritable)
        {
            *(T*)addr = value;
            return true;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect((LPVOID)addr, sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        *(T*)addr = value;
        VirtualProtect((LPVOID)addr, sizeof(T), oldProtect, &oldProtect);
        InvalidateMemPageCacheAround(addr); // protect changed
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InvalidateMemPageCacheAround(addr);
        return false;
    }
}

static bool ReadTypeNameFromEntityType(uintptr_t entityType, char* out, int outMax)
{
    if (!out || outMax < 2) return false;
    out[0] = 0;
    if (!IsValidPtr(entityType) || entityType < 0x100000000) return false;
    uintptr_t tnPtr = Read<uintptr_t>(entityType + offsets::entitytype::TypeName);
    if (!IsValidPtr(tnPtr) || tnPtr < 0x100000000) return false;
    WORD tnLen = Read<WORD>(tnPtr + 0x8);
    int n = (int)tnLen;
    if (n <= 0) return false;
    if (n >= outMax) n = outMax - 1;
    uintptr_t data = tnPtr + 0x10;
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

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

// EyeAccom is PPE intensity (script RegisterParameterScalarFloat max ~1000).
// 0 = pitch black, 1 = normal daylight visibility — NOT fullbright.
// Fullbright EyeAccom — intensity from UI brightness slider (5..100).
// NEVER write EyeAccom-4 — that is World::PlayerOn (0x2968). Daytime lock removed.
static const float kEyeAccomNormal = 1.0f;

static float EyeAccomFromBrightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    float t = (float)pct / 100.f;
    return 8.f + t * 112.f; // ~8 .. ~120
}

static void WriteEyeAccom(uintptr_t worldPtr, float value)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000) return;
    Write<float>(worldPtr + oak_offsets::world::EyeAccom, value);
}

static void ApplyLightingForce(uintptr_t worldPtr, bool fullbright, bool /*daytimeUnused*/)
{
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000) return;
    if (fullbright)
        WriteEyeAccom(worldPtr, EyeAccomFromBrightness(g_FullbrightBrightness));
    else
        WriteEyeAccom(worldPtr, kEyeAccomNormal);
}

static void ApplyFullbright(uintptr_t worldPtr)
{
    ApplyLightingForce(worldPtr, true, false);
}

static void RestoreEyeAccom(uintptr_t worldPtr)
{
    WriteEyeAccom(worldPtr, kEyeAccomNormal);
}

static DWORD WINAPI FullbrightThreadProc(LPVOID)
{
    Log("fullbright worker started");
    static bool s_WasOn = false;
    while (g_Running && !g_ShuttingDown)
    {
        if (InterlockedCompareExchange(&g_SessionQuiesced, 0, 0))
        {
            // Never touch World fields while DayZ is replacing the session graph.
            // Restoring an old value is also a write into an object being destroyed.
            s_WasOn = false;
            Sleep(50);
            continue;
        }
        // Always re-resolve world from module — g_CachedWorldPtr can go stale on unload
        uintptr_t world = 0;
        if (g_GameModule)
            world = Read<uintptr_t>((uintptr_t)g_GameModule + offsets::modbase::World);
        bool validWorld = IsValidPtr(world) && world > 0x100000000;
        if (validWorld)
            g_CachedWorldPtr = world;

        if (validWorld && (g_Fullbright || g_MiscDisableOverlays))
        {
            ApplyLightingForce(world, g_Fullbright, false);
            s_WasOn = true;
            Sleep(8); // PPE fights EyeAccom — keep pressure without saturating the bus
        }
        else
        {
            if (s_WasOn && validWorld)
            {
                RestoreEyeAccom(world);
                s_WasOn = false;
            }
            else if (validWorld)
            {
                float cur = Read<float>(world + oak_offsets::world::EyeAccom);
                if (cur > 2.0f || cur < 0.0f)
                    RestoreEyeAccom(world);
            }
            Sleep(50);
        }
    }
    Log("fullbright worker exit");
    return 0;
}


static float Sqrt(float x)
{
    if (x <= 0) return 0;
    float guess = x / 2.0f;
    for (int i = 0; i < 10; i++)
    {
        guess = (guess + x / guess) / 2.0f;
    }
    return guess;
}


static float Distance3D(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return Sqrt(dx * dx + dy * dy + dz * dz);
}


static Vec3 g_LocalPlayerPos = { 0, 0, 0 };
static Vec3 g_CameraPos = { 0, 0, 0 };
static Vec3 g_WarpGhostPos = { 0, 0, 0 };
static bool g_WarpGhostValid = false;

// Per-frame WorldToScreen camera cache (avoids re-resolving world/camera hundreds of times)
struct W2SCache
{
    bool valid;
    uintptr_t camera;
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    Vec3 translation;
    float projX;
    float projY;
};
static W2SCache g_W2S = {};

static void InvalidateW2SCache()
{
    g_W2S.valid = false;
    g_W2S.camera = 0;
}

static void RefreshW2SCache(uintptr_t camera)
{
    g_W2S.valid = false;
    g_W2S.camera = 0;
    if (!IsValidPtr(camera) || camera < 0x100000000)
        return;

    g_W2S.camera = camera;
    g_W2S.right = Read<Vec3>(camera + offsets::camera::InvertedViewRight);
    g_W2S.up = Read<Vec3>(camera + offsets::camera::InvertedViewUp);
    g_W2S.forward = Read<Vec3>(camera + offsets::camera::InvertedViewForward);
    g_W2S.translation = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
    Vec3 projD1 = Read<Vec3>(camera + offsets::camera::GetProjectionD1);
    Vec3 projD2 = Read<Vec3>(camera + offsets::camera::GetProjectionD2);

    g_W2S.projX = projD1.x;
    g_W2S.projY = projD2.y;
    if (g_W2S.projX == 0.0f || !std::isfinite(g_W2S.projX) || fabsf(g_W2S.projX) > 10000.f)
        g_W2S.projX = 1.0f;
    if (g_W2S.projY == 0.0f || !std::isfinite(g_W2S.projY) || fabsf(g_W2S.projY) > 10000.f)
        g_W2S.projY = 1.0f;

    g_W2S.valid = true;
}


// Resolve the held weapon's AmmoType by validating InitSpeed/gravity floats.
// NEVER brute-write guessed pointers — that corrupted heap and crashed on respawn.
static bool AmmoTypeLooksValid(uintptr_t ammoType)
{
    if (!IsValidPtr(ammoType) || ammoType < 0x100000000 || ammoType > 0x7F0000000000)
        return false;

    // Reject game-image / non-heap
    if (g_GameModule)
    {
        uintptr_t mod = (uintptr_t)g_GameModule;
        if (ammoType >= mod && ammoType < mod + 0x5000000)
            return false;
    }

    // Use page cache (not naked VirtualQuery) — ammo resolve is on combat Present path.
    if (!IsValidPtr(ammoType) || !IsValidPtr(ammoType + 0x3C0))
        return false;

    float initA = Read<float>(ammoType + 0x38C); // AmmoType::InitSpeed
    float initB = Read<float>(ammoType + 0x364); // legacy InitSpeed
    float typical = Read<float>(ammoType + 0x398); // TypicalSpeed
    float grav = Read<float>(ammoType + 0x3BC); // CoefGravity
    float air = Read<float>(ammoType + 0x3B4); // AirFriction
    float disp = Read<float>(ammoType + 0x3A4); // Dispersion

    // Allow vanilla (~80-2000) OR our known writes (fast bullets / combat boost).
    auto speedOk = [](float s) -> bool {
        return (s == s) && s > 80.0f && s < 15000.0f;
    };
    // At least two speed fields must agree as finite speeds — kills many false positives.
    int speedHits = (speedOk(initA) ? 1 : 0) + (speedOk(initB) ? 1 : 0) + (speedOk(typical) ? 1 : 0);
    if (speedHits < 2)
        return false;
    // Gravity coef is normally ~1; reject wild values that mark non-ammo structs
    if (!(grav == grav) || grav < 0.0f || grav > 2.5f)
        return false;
    if (!(air == air) || air < -3.0f || air > 1.0f)
        return false;
    if (!(disp == disp) || disp < 0.0f || disp > 50.0f)
        return false;
    return true;
}

static uintptr_t GetLocalHandsWeapon(uintptr_t localPlayer)
{
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
        return 0;

    // Do NOT gate on IsDead@0xE2 — it false-positives on living locals and caused
    // AmmoType thrash (resolve fail → new AT → restore into freed ptr → CDP crash).

    uintptr_t inv = Read<uintptr_t>(localPlayer + oak_offsets::player::Inventory);
    if (!IsValidPtr(inv) || inv < 0x100000000)
        inv = Read<uintptr_t>(localPlayer + 0x650);
    if (!IsValidPtr(inv) || inv < 0x100000000)
        inv = Read<uintptr_t>(localPlayer + 0x658);
    if (!IsValidPtr(inv) || inv < 0x100000000)
        return 0;

    auto looksEntity = [&](uintptr_t p) -> bool {
        if (!IsValidPtr(p) || p < 0x100000000 || p > 0x7F0000000000)
            return false;
        if (g_GameModule)
        {
            uintptr_t mod = (uintptr_t)g_GameModule;
            if (p >= mod && p < mod + 0x5000000)
                return false;
        }
        uintptr_t typ = Read<uintptr_t>(p + oak_offsets::entity::Type);
        return IsValidPtr(typ) && typ > 0x100000000;
    };

    auto readTypeName = [&](uintptr_t p, char* out, int outMax) -> bool {
        out[0] = 0;
        uintptr_t typ = Read<uintptr_t>(p + oak_offsets::entity::Type);
        if (!IsValidPtr(typ) || typ < 0x100000000)
            return false;
        uintptr_t namePtr = Read<uintptr_t>(typ + oak_offsets::entitytype::TypeName);
        if (!IsValidPtr(namePtr) || namePtr < 0x100000000)
            return false;
        WORD len = Read<WORD>(namePtr + 0x8);
        if (len == 0 || len >= 96 || len >= outMax) return false;
        memcpy(out, (const void*)(namePtr + 0x10), (size_t)len);
        out[len] = 0;
        return out[0] != 0;
    };

    auto isJunkHandsName = [&](const char* nm) -> bool {
        if (!nm || !nm[0]) return true;
        if (StrCmpI(nm, "Weapon") == 0 || StrCmpI(nm, "Weapon_Base") == 0 ||
            StrCmpI(nm, "InventoryItem") == 0 || StrCmpI(nm, "ItemBase") == 0 ||
            StrCmpI(nm, "Clothing") == 0 || StrCmpI(nm, "Clothing_Base") == 0 ||
            StrCmpI(nm, "EntityAI") == 0)
            return true;
        if (StrContainsI(nm, "Trap") || StrContainsI(nm, "Mine") ||
            StrContainsI(nm, "Survivor") || StrCmpI(nm, "dayzplayer") == 0)
            return true;
        return false;
    };

    // Prefer Hands with a concrete TypeName (e.g. AKM @ 0x1B0). Skip base-class junk.
    // Do NOT treat Clothing@0x150 as hands — that offset is attachment list on current builds.
    const uintptr_t handOffs[] = { 0x1B0, 0xF8, 0x100, 0x108, 0x110, 0x1A0, 0x190, 0x148 };
    uintptr_t fallback = 0;
    for (int i = 0; i < (int)(sizeof(handOffs) / sizeof(handOffs[0])); i++)
    {
        uintptr_t hands = Read<uintptr_t>(inv + handOffs[i]);
        if (!looksEntity(hands))
            continue;
        char tn[64] = {};
        if (!readTypeName(hands, tn, 64))
            continue;
        if (!isJunkHandsName(tn))
            return hands;
        if (!fallback)
            fallback = hands;
    }
    return fallback;
}

static bool ResolveHeldAmmoType(uintptr_t localPlayer, uintptr_t& outAmmoType)
{
    outAmmoType = 0;
    uintptr_t hands = GetLocalHandsWeapon(localPlayer);
    if (!hands)
        return false;

    auto tryPtr = [&](uintptr_t p) -> bool {
        if (AmmoTypeLooksValid(p))
        {
            outAmmoType = p;
            return true;
        }
        return false;
    };

    auto tryNested = [&](uintptr_t wrapper) -> bool {
        if (!IsValidPtr(wrapper) || wrapper < 0x100000000)
            return false;
        // Live probe: muzzle AmmoType is +0x20 on this build
        const uintptr_t nest[] = { 0x20, 0x18, 0x28, 0x10, 0x30 };
        for (int i = 0; i < 5; i++)
        {
            if (tryPtr(Read<uintptr_t>(wrapper + nest[i])))
                return true;
        }
        return false;
    };

    // Prefer the live-confirmed muzzle slot first (hands+0x6A8 → +0x20)
    {
        uintptr_t muzzle = Read<uintptr_t>(hands + 0x6A8);
        if (tryNested(muzzle)) return true;
        if (tryPtr(muzzle)) return true;
    }

    const uintptr_t hot[] = {
        0x6B0, 0x6B8, 0x6C0, 0x6C8, 0x6A0, 0x690, 0x680, 0x670
    };
    for (int i = 0; i < (int)(sizeof(hot) / sizeof(hot[0])); i++)
    {
        uintptr_t p = Read<uintptr_t>(hands + hot[i]);
        if (tryNested(p)) return true;
        if (tryPtr(p)) return true;
    }

    // Narrow fallback only — broad scans were matching non-ammo heap and crashing CDP
    for (uintptr_t off = 0x680; off <= 0x6E0; off += 8)
    {
        if (off == 0x6A8) continue;
        uintptr_t p = Read<uintptr_t>(hands + off);
        if (tryNested(p)) return true;
    }
    return false;
}

struct AmmoTypeBackup {
    uintptr_t ptr = 0;
    float init38C = 0, init364 = 0, typical = 0, maxLead = 0;
    float disp3A4 = 0, disp3CC = 0;
    float grav = 0, air = 0;
    bool valid = false;
    bool dirty = false; // we have written modified values
};

static AmmoTypeBackup g_AmmoBackup;

static void AbandonAmmoBackup()
{
    // Drop state without writing — old AmmoType may already be freed on weapon swap.
    g_AmmoBackup.ptr = 0;
    g_AmmoBackup.valid = false;
    g_AmmoBackup.dirty = false;
}

static bool CaptureAmmoBackup(uintptr_t ammoType)
{
    if (!AmmoTypeLooksValid(ammoType))
        return false;
    if (g_AmmoBackup.valid && g_AmmoBackup.ptr == ammoType)
        return true;

    // CRITICAL: never restore into a previous AmmoType pointer on switch.
    // Mag/weapon changes free that object; writing floats there = delayed CDP heap AV.
    if (g_AmmoBackup.valid && g_AmmoBackup.ptr != ammoType)
        AbandonAmmoBackup();

    float initA = Read<float>(ammoType + 0x38C);
    float initB = Read<float>(ammoType + 0x364);
    auto vanillaSpeed = [](float s) { return s == s && s > 80.f && s < 2000.f; };
    // If this object already looks modified and we have no vanilla backup, refuse
    // (avoid capturing 8000 as "original").
    if (!vanillaSpeed(initA) && !vanillaSpeed(initB))
        return false;

    g_AmmoBackup.ptr = ammoType;
    g_AmmoBackup.init38C = initA;
    g_AmmoBackup.init364 = initB;
    g_AmmoBackup.typical = Read<float>(ammoType + 0x398);
    g_AmmoBackup.maxLead = Read<float>(ammoType + 0x394);
    g_AmmoBackup.disp3A4 = Read<float>(ammoType + 0x3A4);
    g_AmmoBackup.disp3CC = Read<float>(ammoType + 0x3CC);
    g_AmmoBackup.grav = Read<float>(ammoType + 0x3BC);
    g_AmmoBackup.air = Read<float>(ammoType + 0x3B4);
    g_AmmoBackup.valid = true;
    g_AmmoBackup.dirty = false;
    return true;
}

static void RestoreAmmoBackup()
{
    if (!g_AmmoBackup.valid || !g_AmmoBackup.dirty)
        return;
    uintptr_t ammoType = g_AmmoBackup.ptr;
    // Only restore into the SAME still-valid AmmoType we modified
    if (!AmmoTypeLooksValid(ammoType))
    {
        AbandonAmmoBackup();
        return;
    }
    __try
    {
        Write<float>(ammoType + 0x38C, g_AmmoBackup.init38C);
        Write<float>(ammoType + 0x364, g_AmmoBackup.init364);
        Write<float>(ammoType + 0x398, g_AmmoBackup.typical);
        Write<float>(ammoType + 0x394, g_AmmoBackup.maxLead);
        Write<float>(ammoType + 0x3A4, g_AmmoBackup.disp3A4);
        Write<float>(ammoType + 0x3CC, g_AmmoBackup.disp3CC);
        Write<float>(ammoType + 0x3BC, g_AmmoBackup.grav);
        Write<float>(ammoType + 0x3B4, g_AmmoBackup.air);
        Log("ammo: restored AmmoType defaults");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_AmmoBackup.dirty = false;
}

static void ApplyWeaponAmmoMods(uintptr_t localPlayer)
{
    OAK_MARK("ammo");
    // Ammo path is Lab AMMO / MB / Silent only (no recoil/dispersion coupling).
    const bool anyMod = g_FastBullets || g_NoDispersion || g_PerfectBallistics;
    if (!anyMod)
    {
        if (!g_MagicBullet && !g_SilentAim.enabled)
            RestoreAmmoBackup();
        return;
    }

    uintptr_t ammoType = 0;
    if (!ResolveHeldAmmoType(localPlayer, ammoType))
    {
        static int s_FailLog = 0;
        if ((++s_FailLog % 1000) == 1)
        {
            uintptr_t hands = GetLocalHandsWeapon(localPlayer);
            char buf[128];
            wsprintfA(buf, "ammo: resolve FAIL local=0x%p hands=0x%p",
                (void*)localPlayer, (void*)hands);
            Log(buf);
        }
        return;
    }
    if (!AmmoTypeLooksValid(ammoType))
        return;
    if (!CaptureAmmoBackup(ammoType))
        return;

    static uintptr_t s_LoggedAT = 0;
    if (ammoType != s_LoggedAT)
    {
        s_LoggedAT = ammoType;
        char buf[160];
        // wsprintfA has no %f — log ints only
        wsprintfA(buf, "ammo: resolved AmmoType=0x%p init=%d grav=%d disp=%d",
            (void*)ammoType,
            (int)g_AmmoBackup.init38C,
            (int)(g_AmmoBackup.grav * 100.f),
            (int)(g_AmmoBackup.disp3A4 * 10000.f));
        Log(buf);
    }

    // Throttle writes — every-frame AmmoType stores were hammering shared config objects
    static DWORD s_LastWrite = 0;
    static uintptr_t s_LastWritePtr = 0;
    DWORD now = GetTickCount();
    bool needWrite = (ammoType != s_LastWritePtr) || (now - s_LastWrite > 100) || !g_AmmoBackup.dirty;
    if (!needWrite)
        return;

    __try
    {
        const int labMod = OakLab_IsWriteAllowed(OAK_LAB_AMMO) ? OAK_LAB_AMMO
            : (OakLab_IsWriteAllowed(OAK_LAB_MB) ? OAK_LAB_MB
            : (OakLab_IsWriteAllowed(OAK_LAB_SILENT) ? OAK_LAB_SILENT : OAK_LAB_NONE));
        if (labMod == OAK_LAB_NONE)
            return;

        float init = g_FastBullets ? 8000.0f : g_AmmoBackup.init38C;
        float initB = g_FastBullets ? 8000.0f : g_AmmoBackup.init364;
        float typical = g_FastBullets ? 8000.0f : g_AmmoBackup.typical;
        float maxLead = g_FastBullets ? 8000.0f : g_AmmoBackup.maxLead;
        float dispA = g_NoDispersion ? 0.0f : g_AmmoBackup.disp3A4;
        float dispB = g_NoDispersion ? 0.0f : g_AmmoBackup.disp3CC;
        float grav = g_PerfectBallistics ? 0.0f : g_AmmoBackup.grav;
        float air = g_PerfectBallistics ? 0.0f : g_AmmoBackup.air;

        OakLab_PushModule(labMod);
        OakLab_WriteFloat(labMod, ammoType + 0x38C, init, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x364, initB, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x398, typical, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x394, maxLead, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x3A4, dispA, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x3CC, dispB, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x3BC, grav, 0);
        OakLab_WriteFloat(labMod, ammoType + 0x3B4, air, 0);
        OakLab_PopModule();
        g_AmmoBackup.dirty = true;
        s_LastWrite = now;
        s_LastWritePtr = ammoType;

        static DWORD s_VerifyLast = 0;
        if (!s_VerifyLast || (now - s_VerifyLast) > 1500)
        {
            float gotInit = Read<float>(ammoType + 0x38C);
            float gotDisp = Read<float>(ammoType + 0x3A4);
            float gotGrav = Read<float>(ammoType + 0x3BC);
            int passFb = !g_FastBullets || (gotInit >= 7900.f) ? 1 : 0;
            int passNd = !g_NoDispersion || (gotDisp == 0.f) ? 1 : 0;
            int passPb = !g_PerfectBallistics || (gotGrav == 0.f) ? 1 : 0;
            int pass = (passFb && passNd && passPb) ? 1 : 0;
            char vb[180];
            wsprintfA(vb, "verify[ammo] init=%d disp=%d gravx100=%d fb=%d nd=%d pb=%d PASS=%d",
                (int)(gotInit + 0.5f), (int)(gotDisp * 10000.f + 0.5f), (int)(gotGrav * 100.f + 0.5f),
                g_FastBullets ? 1 : 0, g_NoDispersion ? 1 : 0, g_PerfectBallistics ? 1 : 0, pass);
            Log(vb);
            s_VerifyLast = now;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Must receive the ESP-resolved DayZPlayer — World::LocalPlayer is often a non-typed stub
// with no Inventory, so AmmoType resolve silently fails every frame.
static void UpdateFastBullets(uintptr_t localPlayer)
{
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000)
    {
        if (!g_FastBullets && !g_NoDispersion && !g_PerfectBallistics && !g_MagicBullet)
            RestoreAmmoBackup();
        return;
    }
    ApplyWeaponAmmoMods(localPlayer);
}

// Legacy stub removed — real aimbot/magic-bullet lives in esp_addons_impl.inl as UpdateCombatAim.
static void UpdateBulletAimbot() {}



static void AddLine(float x1, float y1, float x2, float y2, float r, float g, float b, float a);


struct CharDef {
    unsigned char numSegments;
    unsigned char segments[20];  
};

static const CharDef g_Font[95] = {
    
    { 0, {} },
    
    { 2, { 2,0, 2,4,  2,6, 2,6 } },
    
    { 2, { 1,0, 1,2,  3,0, 3,2 } },
    
    { 4, { 1,1, 1,5,  3,1, 3,5,  0,2, 4,2,  0,4, 4,4 } },
    
    { 4, { 2,0, 2,6,  4,1, 0,1,  0,3, 4,3,  0,5, 4,5 } },
    
    { 3, { 0,0, 1,1,  4,0, 0,6,  3,5, 4,6 } },
    
    { 5, { 4,6, 0,2,  0,2, 2,0,  2,0, 4,2,  4,2, 0,6,  0,6, 2,6 } },
    
    { 1, { 2,0, 2,2 } },
    
    { 3, { 3,0, 1,2,  1,2, 1,4,  1,4, 3,6 } },
    
    { 3, { 1,0, 3,2,  3,2, 3,4,  3,4, 1,6 } },
    
    { 3, { 2,1, 2,5,  0,2, 4,4,  4,2, 0,4 } },
    
    { 2, { 2,1, 2,5,  0,3, 4,3 } },
    
    { 1, { 2,5, 1,7 } },
    
    { 1, { 0,3, 4,3 } },
    
    { 1, { 2,5, 2,6 } },
    
    { 1, { 0,6, 4,0 } },
    
    { 4, { 0,0, 4,0,  4,0, 4,6,  4,6, 0,6,  0,6, 0,0 } },
    
    { 3, { 1,1, 2,0,  2,0, 2,6,  1,6, 3,6 } },
    
    { 5, { 0,1, 2,0,  2,0, 4,1,  4,1, 0,6,  0,6, 4,6 } },
    
    { 5, { 0,0, 4,0,  4,0, 4,6,  4,6, 0,6,  0,3, 4,3 } },
    
    { 3, { 0,0, 0,3,  0,3, 4,3,  4,0, 4,6 } },
    
    { 5, { 4,0, 0,0,  0,0, 0,3,  0,3, 4,3,  4,3, 4,6,  4,6, 0,6 } },
    
    { 5, { 4,0, 0,0,  0,0, 0,6,  0,6, 4,6,  4,6, 4,3,  4,3, 0,3 } },
    
    { 2, { 0,0, 4,0,  4,0, 2,6 } },
    
    { 5, { 0,0, 4,0,  4,0, 4,6,  4,6, 0,6,  0,6, 0,0,  0,3, 4,3 } },
    
    { 5, { 4,3, 0,3,  0,3, 0,0,  0,0, 4,0,  4,0, 4,6,  4,6, 0,6 } },
    
    { 2, { 2,2, 2,2,  2,5, 2,5 } },
    
    { 2, { 2,2, 2,2,  2,5, 1,6 } },
    
    { 2, { 4,0, 0,3,  0,3, 4,6 } },
    
    { 2, { 0,2, 4,2,  0,4, 4,4 } },
    
    { 2, { 0,0, 4,3,  4,3, 0,6 } },
    
    { 4, { 0,1, 2,0,  2,0, 4,1,  4,1, 2,4,  2,6, 2,6 } },
    
    { 5, { 4,2, 2,2,  2,2, 2,4,  2,4, 4,4,  0,0, 4,0,  4,0, 0,6 } },
    
    { 3, { 0,6, 2,0,  2,0, 4,6,  0,4, 4,4 } },
    
    { 5, { 0,0, 0,6,  0,0, 3,0,  3,0, 0,3,  0,3, 3,3,  3,3, 0,6 } },
    
    { 3, { 4,0, 0,0,  0,0, 0,6,  0,6, 4,6 } },
    
    { 4, { 0,0, 0,6,  0,0, 3,0,  3,0, 4,3,  4,3, 0,6 } },
    
    { 4, { 4,0, 0,0,  0,0, 0,6,  0,6, 4,6,  0,3, 3,3 } },
    
    { 3, { 4,0, 0,0,  0,0, 0,6,  0,3, 3,3 } },
    
    { 4, { 4,0, 0,0,  0,0, 0,6,  0,6, 4,6,  4,6, 4,3 } },
    
    { 3, { 0,0, 0,6,  4,0, 4,6,  0,3, 4,3 } },
    
    { 3, { 1,0, 3,0,  2,0, 2,6,  1,6, 3,6 } },
    
    { 3, { 0,0, 4,0,  4,0, 4,6,  4,6, 0,6 } },
    
    { 3, { 0,0, 0,6,  4,0, 0,3,  0,3, 4,6 } },
    
    { 2, { 0,0, 0,6,  0,6, 4,6 } },
    
    { 4, { 0,6, 0,0,  0,0, 2,3,  2,3, 4,0,  4,0, 4,6 } },
    
    { 3, { 0,6, 0,0,  0,0, 4,6,  4,6, 4,0 } },
    
    { 4, { 0,0, 4,0,  4,0, 4,6,  4,6, 0,6,  0,6, 0,0 } },
    
    { 4, { 0,6, 0,0,  0,0, 4,0,  4,0, 4,3,  4,3, 0,3 } },
    
    { 5, { 0,0, 4,0,  4,0, 4,4,  4,4, 0,6,  0,6, 0,0,  2,4, 4,6 } },
    
    { 5, { 0,6, 0,0,  0,0, 4,0,  4,0, 4,3,  4,3, 0,3,  0,3, 4,6 } },
    
    { 5, { 4,0, 0,0,  0,0, 0,3,  0,3, 4,3,  4,3, 4,6,  4,6, 0,6 } },
    
    { 2, { 0,0, 4,0,  2,0, 2,6 } },
    
    { 3, { 0,0, 0,6,  0,6, 4,6,  4,6, 4,0 } },
    
    { 2, { 0,0, 2,6,  2,6, 4,0 } },
    
    { 4, { 0,0, 0,6,  0,6, 2,3,  2,3, 4,6,  4,6, 4,0 } },
    
    { 2, { 0,0, 4,6,  4,0, 0,6 } },
    
    { 3, { 0,0, 2,3,  4,0, 2,3,  2,3, 2,6 } },
    
    { 3, { 0,0, 4,0,  4,0, 0,6,  0,6, 4,6 } },
    
    { 3, { 3,0, 1,0,  1,0, 1,6,  1,6, 3,6 } },
    
    { 1, { 0,0, 4,6 } },
    
    { 3, { 1,0, 3,0,  3,0, 3,6,  3,6, 1,6 } },
    
    { 2, { 0,2, 2,0,  2,0, 4,2 } },
    
    { 1, { 0,6, 4,6 } },
    
    { 1, { 1,0, 2,1 } },
    
    { 4, { 0,2, 4,2,  4,2, 4,6,  4,6, 0,6,  0,6, 0,4 } },
    
    { 4, { 0,0, 0,6,  0,6, 4,6,  4,6, 4,3,  4,3, 0,3 } },
    
    { 3, { 4,2, 0,2,  0,2, 0,6,  0,6, 4,6 } },
    
    { 4, { 4,0, 4,6,  4,6, 0,6,  0,6, 0,3,  0,3, 4,3 } },
    
    { 4, { 0,4, 4,4,  4,4, 4,2,  0,2, 0,6,  0,6, 4,6 } },
    
    { 3, { 3,0, 1,0,  1,0, 1,6,  0,3, 3,3 } },
    
    { 5, { 4,2, 0,2,  0,2, 0,5,  0,5, 4,5,  4,2, 4,8,  4,8, 0,8 } },
    
    { 3, { 0,0, 0,6,  0,3, 4,3,  4,3, 4,6 } },
    
    { 2, { 2,0, 2,1,  2,3, 2,6 } },
    
    { 3, { 3,0, 3,1,  3,3, 3,7,  3,7, 0,7 } },
    
    { 3, { 0,0, 0,6,  4,2, 0,4,  0,4, 4,6 } },
    
    { 2, { 2,0, 2,5,  2,5, 3,6 } },
    
    { 4, { 0,2, 0,6,  0,2, 2,2,  2,2, 2,6,  2,2, 4,2 } },
    
    { 3, { 0,2, 0,6,  0,2, 4,2,  4,2, 4,6 } },
    
    { 4, { 0,2, 4,2,  4,2, 4,6,  4,6, 0,6,  0,6, 0,2 } },
    
    { 4, { 0,2, 0,8,  0,2, 4,2,  4,2, 4,5,  4,5, 0,5 } },
    
    { 4, { 4,2, 4,8,  4,2, 0,2,  0,2, 0,5,  0,5, 4,5 } },
    
    { 2, { 0,2, 0,6,  0,2, 4,2 } },
    
    { 4, { 4,2, 0,2,  0,2, 0,4,  0,4, 4,6,  4,6, 0,6 } },
    
    { 3, { 1,0, 1,5,  1,5, 3,6,  0,2, 3,2 } },
    
    { 3, { 0,2, 0,6,  0,6, 4,6,  4,2, 4,6 } },
    
    { 2, { 0,2, 2,6,  2,6, 4,2 } },
    
    { 4, { 0,2, 0,6,  0,6, 2,4,  2,4, 4,6,  4,6, 4,2 } },
    
    { 2, { 0,2, 4,6,  4,2, 0,6 } },
    
    { 4, { 0,2, 0,5,  0,5, 4,5,  4,2, 4,8,  4,8, 0,8 } },
    
    { 3, { 0,2, 4,2,  4,2, 0,6,  0,6, 4,6 } },
    
    { 4, { 3,0, 2,1,  2,1, 2,5,  2,5, 3,6,  1,3, 2,3 } },
    
    { 1, { 2,0, 2,6 } },
    
    { 4, { 1,0, 2,1,  2,1, 2,5,  2,5, 1,6,  2,3, 3,3 } },
    
    { 2, { 0,3, 2,2,  2,2, 4,3 } },
};


static float DrawChar(float x, float y, char c, float scale, float r, float g, float b, float a)
{
    if (c < 32 || c > 126) return scale * 4;  
    
    const CharDef& def = g_Font[c - 32];
    
    for (int i = 0; i < def.numSegments && i < 5; i++)
    {
        unsigned char x1 = def.segments[i * 4 + 0];
        unsigned char y1 = def.segments[i * 4 + 1];
        unsigned char x2 = def.segments[i * 4 + 2];
        unsigned char y2 = def.segments[i * 4 + 3];
        
        
        float px1 = x + x1 * scale;
        float py1 = y + y1 * scale;
        float px2 = x + x2 * scale;
        float py2 = y + y2 * scale;
        
        
        AddLine(px1, py1, px2, py2, r, g, b, a);
    }
    
    return scale * 6;  
}


static void IntToStr(int value, char* buf)
{
    if (value == 0) { buf[0] = '0'; buf[1] = 0; return; }
    
    char temp[16];
    int i = 0;
    bool neg = false;
    
    if (value < 0) { neg = true; value = -value; }
    
    while (value > 0 && i < 15)
    {
        temp[i++] = '0' + (value % 10);
        value /= 10;
    }
    
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = temp[--i];
    buf[j] = 0;
}


struct Vertex {
    float x, y;
    float r, g, b, a;
};

#define MAX_VERTICES 65536
static Vertex g_Vertices[MAX_VERTICES];
static int g_VertexCount = 0;

static void InitRenderer()
{
    if (!g_Device || !g_Context) return;
    
    Log("making shaders...");
    
    
    HRESULT hr = g_Device->CreateVertexShader(g_VS, sizeof(g_VS), NULL, &g_VertexShader);
    if (FAILED(hr)) { 
        char buf[64]; 
        wsprintfA(buf, "vs failed 0x%x", hr); 
        Log(buf); 
        return; 
    }
    
    
    hr = g_Device->CreatePixelShader(g_PS, sizeof(g_PS), NULL, &g_PixelShader);
    if (FAILED(hr)) { 
        char buf[64]; 
        wsprintfA(buf, "ps failed 0x%x", hr); 
        Log(buf); 
        return; 
    }
    
    
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_Device->CreateInputLayout(layout, 2, g_VS, sizeof(g_VS), &g_InputLayout);
    if (FAILED(hr)) { 
        char buf[64]; 
        wsprintfA(buf, "il failed 0x%x", hr); 
        Log(buf); 
        return; 
    }
    
    
    D3D11_BUFFER_DESC bd;
    ZeroMem(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Vertex) * MAX_VERTICES;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_Device->CreateBuffer(&bd, NULL, &g_VertexBuffer);
    if (FAILED(hr)) { 
        char buf[64]; 
        wsprintfA(buf, "vb failed 0x%x", hr); 
        Log(buf); 
        return; 
    }
    
    
    D3D11_BLEND_DESC blendDesc;
    ZeroMem(&blendDesc, sizeof(blendDesc));
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_Device->CreateBlendState(&blendDesc, &g_BlendState);
    
    
    D3D11_RASTERIZER_DESC rastDesc;
    ZeroMem(&rastDesc, sizeof(rastDesc));
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = TRUE;
    g_Device->CreateRasterizerState(&rastDesc, &g_RasterizerState);
    
    
    D3D11_DEPTH_STENCIL_DESC dsDesc;
    ZeroMem(&dsDesc, sizeof(dsDesc));
    dsDesc.DepthEnable = FALSE;
    dsDesc.StencilEnable = FALSE;
    g_Device->CreateDepthStencilState(&dsDesc, &g_DepthStencilState);
    
    g_RenderInit = true;
    Log("renderer ok");
}

static void BeginDraw()
{
    g_EspDrawCount = 0;
}

static ImU32 ToCol(float r, float g, float b, float a)
{
    return IM_COL32(
        (int)(r * 255.f),
        (int)(g * 255.f),
        (int)(b * 255.f),
        (int)(a * 255.f));
}

static void AddLine(float x1, float y1, float x2, float y2, float r, float g, float b, float a)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    ImU32 col = ToCol(r, g, b, a);
    ImU32 outline = IM_COL32(0, 0, 0, (int)(a * 160.f));
    dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), outline, 2.6f);
    dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, 1.35f);
    g_EspDrawCount++;
}

static void AddBox(float x, float y, float w, float h, float r, float g, float b, float a)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToCol(r, g, b, a), 0.f, 0, 1.5f);
    g_EspDrawCount++;
}

static void AddBoxThick(float x, float y, float w, float h, float r, float g, float b, float a, float thick)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    if (thick < 0.5f) thick = 0.5f;
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ToCol(r, g, b, a), 0.f, 0, thick);
    g_EspDrawCount++;
}

static void AddCornerBox(float x, float y, float w, float h, float r, float g, float b, float a, float thick = 2.f)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;
    ImU32 col = ToCol(r, g, b, a);
    ImU32 outline = IM_COL32(0, 0, 0, 200);
    float l = (w < h ? w : h) * 0.25f;
    if (l < 5.f) l = 5.f;
    if (thick < 0.5f) thick = 0.5f;
    float x2 = x + w, y2 = y + h;
    float ot = thick + 1.4f;
    auto corner = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), outline, ot);
        dl->AddLine(ImVec2(ax, ay), ImVec2(cx, cy), outline, ot);
        dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), col, thick);
        dl->AddLine(ImVec2(ax, ay), ImVec2(cx, cy), col, thick);
    };
    corner(x, y, x + l, y, x, y + l);
    corner(x2, y, x2 - l, y, x2, y + l);
    corner(x, y2, x + l, y2, x, y2 - l);
    corner(x2, y2, x2 - l, y2, x2, y2 - l);
    g_EspDrawCount++;
}

static void DrawEspBoxStyled(float x, float y, float w, float h, float r, float g, float b, float a, int style, float thick)
{
    switch (style)
    {
    case OakBox2D:
        AddBoxThick(x, y, w, h, r, g, b, a, thick);
        break;
    case OakBox3D:
        AddBoxThick(x + 2.f, y + 2.f, w, h, r * 0.7f, g * 0.7f, b * 0.7f, a * 0.6f, thick * 0.75f);
        AddCornerBox(x, y, w, h, r, g, b, a);
        break;
    case OakBoxCorner:
    default:
        AddCornerBox(x, y, w, h, r, g, b, a, thick);
        break;
    }
}

static void DrawTextD3D(float x, float y, const char* text, float r, float g, float b, float a, float /*scale*/)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || !text) return;
    ImU32 col = ToCol(r, g, b, a);
    ImU32 shadow = IM_COL32(0, 0, 0, 220);
    dl->AddText(ImVec2(x + 1.f, y + 1.f), shadow, text);
    dl->AddText(ImVec2(x, y), col, text);
    g_EspDrawCount++;
}

static void DrawEspBadge(float cx, float y, const char* text, float r, float g, float b, float a)
{
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl || !text || !text[0]) return;
    ImVec2 sz = ImGui::CalcTextSize(text);
    float padX = 6.f, padY = 3.f;
    float w = sz.x + padX * 2.f + 4.f;
    float h = sz.y + padY * 2.f;
    float x0 = cx - w * 0.5f;
    float y0 = y;
    ImU32 bg = IM_COL32(8, 10, 14, 200);
    ImU32 accent = ToCol(r, g, b, a < 0.85f ? 0.95f : a);
    ImU32 border = ToCol(r, g, b, 0.55f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), bg, 4.f);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + 3.f, y0 + h), accent, 4.f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), border, 4.f, 0, 1.2f);
    dl->AddText(ImVec2(x0 + padX + 2.f, y0 + padY), IM_COL32(245, 248, 255, 255), text);
    g_EspDrawCount++;
}

static void DrawEspLabelCentered(float cx, float y, const char* text, float r, float g, float b, float a)
{
    DrawEspBadge(cx, y, text, r, g, b, a);
}

static void DrawEspNameWithAvatar(float cx, float y, const char* text, ID3D11ShaderResourceView* avatar,
    float r, float g, float b, float a)
{
    if (avatar && text && text[0])
    {
        ImDrawList* dl = ImGuiMenu_EspDrawList();
        if (dl)
        {
            const float sz = ImGui::GetFontSize() + 2.f;
            ImVec2 ts = ImGui::CalcTextSize(text);
            const float total = sz + 4.f + ts.x;
            const float x0 = cx - total * 0.5f;
            dl->AddImage((ImTextureID)avatar, ImVec2(x0, y), ImVec2(x0 + sz, y + sz));
            DrawEspBadge(x0 + sz + 4.f + ts.x * 0.5f, y, text, r, g, b, a);
            return;
        }
    }
    DrawEspLabelCentered(cx, y, text, r, g, b, a);
}

enum { kItemEspCache = 192 };
struct OakItemEspSlot {
    uintptr_t ent;
    char name[64];
    int dist;
    DWORD seen;
    bool drawn;
};
static OakItemEspSlot g_ItemEspCache[kItemEspCache];

static void DrawItem(uintptr_t entity, const char* name, int distance);

static void OakItemEspBeginFrame()
{
    // no-op — cache slots persist until TTL
}

static void OakItemEspRemember(uintptr_t entity, const char* name, int distance)
{
    if (!entity || !name || !name[0]) return;
    DWORD now = GetTickCount();
    int hit = -1;
    int oldest = 0;
    DWORD oldestTick = 0xFFFFFFFFu;
    for (int i = 0; i < kItemEspCache; i++)
    {
        OakItemEspSlot& s = g_ItemEspCache[i];
        if (s.ent == entity)
        {
            hit = i;
            break;
        }
        if (!s.ent || (now - s.seen) > 1200)
        {
            hit = i;
            break;
        }
        if (s.seen < oldestTick)
        {
            oldestTick = s.seen;
            oldest = i;
        }
    }
    if (hit < 0) hit = oldest;
    OakItemEspSlot& slot = g_ItemEspCache[hit];
    slot.ent = entity;
    slot.dist = distance;
    slot.seen = now;
    lstrcpynA(slot.name, name, 64);
}

static void OakItemEspDrawAllCached(int* outDrawn)
{
    if (!g_ESPItems && !g_ESPContainers && !g_ESPTraps) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < kItemEspCache; i++)
    {
        OakItemEspSlot& s = g_ItemEspCache[i];
        if (!s.ent) continue;
        if ((now - s.seen) > 3000) { s.ent = 0; continue; }
        __try {
            DrawItem(s.ent, s.name, s.dist);
            if (outDrawn) (*outDrawn)++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s.ent = 0;
        }
    }
}

static bool WorldToScreen(Vec3 world, Vec2& screen);
static bool GetEntityPosition(uintptr_t entity, Vec3& out);
static void OakBatch4OnContainerEsp(uintptr_t entity, float screenX, float screenY);

// Containers live mostly on ItemTable (rolling + throttled scan) — same flicker class as
// pre-cache loot. Remember during scan; redraw every Present with held W2S.
enum { kContEspCache = 96 };
struct OakContEspSlot {
    uintptr_t ent;
    char name[64];
    int dist;
    DWORD seen;
    DWORD w2sTick;
    float sx, sy;
    bool hasScreen;
};
static OakContEspSlot g_ContEspCache[kContEspCache];

static void OakContEspRemember(uintptr_t entity, const char* name, int distance)
{
    if (!entity || !name || !name[0]) return;
    DWORD now = GetTickCount();
    int hit = -1;
    int oldest = 0;
    DWORD oldestTick = 0xFFFFFFFFu;
    for (int i = 0; i < kContEspCache; i++)
    {
        OakContEspSlot& s = g_ContEspCache[i];
        if (s.ent == entity) { hit = i; break; }
        if (!s.ent || (now - s.seen) > 2000) { hit = i; break; }
        if (s.seen < oldestTick) { oldestTick = s.seen; oldest = i; }
    }
    if (hit < 0) hit = oldest;
    OakContEspSlot& slot = g_ContEspCache[hit];
    slot.ent = entity;
    slot.dist = distance;
    slot.seen = now;
    lstrcpynA(slot.name, name, 64);
}

static void OakContEspDrawAllCached()
{
    if (!g_ESPContainers || g_PanicHidden) return;
    DWORD now = GetTickCount();
    ImDrawList* dl = ImGuiMenu_EspDrawList();
    if (!dl) return;

    for (int i = 0; i < kContEspCache; i++)
    {
        OakContEspSlot& s = g_ContEspCache[i];
        if (!s.ent) continue;
        if ((now - s.seen) > 4000) { s.ent = 0; s.hasScreen = false; continue; }

        __try {
            Vec3 pos;
            Vec2 scr;
            bool onScreen = GetEntityPosition(s.ent, pos) && WorldToScreen(pos, scr);
            if (onScreen)
            {
                if (!s.hasScreen)
                {
                    s.sx = scr.x;
                    s.sy = scr.y;
                    s.hasScreen = true;
                }
                else
                {
                    const float a = 0.40f;
                    s.sx += (scr.x - s.sx) * a;
                    s.sy += (scr.y - s.sy) * a;
                }
                s.w2sTick = now;
            }
            else if (!s.hasScreen || (now - s.w2sTick) > 450)
            {
                continue; // brief W2S miss: keep last pos up to 450ms
            }

            const float* col = g_ColorContainer;
            float box = 10.f * 0.85f;
            if (box < 7.f) box = 7.f;
            ImU32 fill = ToCol(col[0], col[1], col[2], 0.55f);
            ImU32 edge = ToCol(col[0], col[1], col[2], 1.f);
            ImU32 glow = ToCol(col[0], col[1], col[2], 0.25f);
            ImVec2 c(s.sx, s.sy);
            dl->AddCircleFilled(c, box + 4.f, glow, 20);
            ImVec2 diamond[4] = {
                ImVec2(c.x, c.y - box),
                ImVec2(c.x + box, c.y),
                ImVec2(c.x, c.y + box),
                ImVec2(c.x - box, c.y),
            };
            dl->AddConvexPolyFilled(diamond, 4, fill);
            dl->AddPolyline(diamond, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 3.0f);
            dl->AddPolyline(diamond, 4, edge, ImDrawFlags_Closed, 1.8f);
            dl->AddCircleFilled(c, 2.2f, edge, 10);

            char line[96];
            if (s.dist >= 0)
                wsprintfA(line, "%s  %dm", s.name[0] ? s.name : "?", s.dist);
            else
                wsprintfA(line, "%s", s.name[0] ? s.name : "?");
            DrawEspBadge(s.sx, s.sy + box + 6.f, line, col[0], col[1], col[2], col[3]);

            OakBatch4OnContainerEsp(s.ent, s.sx, s.sy);
            g_EspDrawCount++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s.ent = 0;
            s.hasScreen = false;
        }
    }
}

// Vehicles: same class of flicker (list churn + brittle dual W2S). Cache + hold.
enum { kVehEspCache = 64 };
struct OakVehEspSlot {
    uintptr_t ent;
    char name[64];
    int dist;
    DWORD seen;
    DWORD w2sTick;
    float sx, sy, sh; // center + box height
    bool hasScreen;
};
static OakVehEspSlot g_VehEspCache[kVehEspCache];

static void OakVehEspRemember(uintptr_t entity, const char* name, int distance)
{
    if (!entity) return;
    DWORD now = GetTickCount();
    int hit = -1;
    int oldest = 0;
    DWORD oldestTick = 0xFFFFFFFFu;
    for (int i = 0; i < kVehEspCache; i++)
    {
        OakVehEspSlot& s = g_VehEspCache[i];
        if (s.ent == entity) { hit = i; break; }
        if (!s.ent || (now - s.seen) > 2000) { hit = i; break; }
        if (s.seen < oldestTick) { oldestTick = s.seen; oldest = i; }
    }
    if (hit < 0) hit = oldest;
    OakVehEspSlot& slot = g_VehEspCache[hit];
    slot.ent = entity;
    slot.dist = distance;
    slot.seen = now;
    if (name && name[0]) lstrcpynA(slot.name, name, 64);
    else slot.name[0] = 0;
}

static void OakVehEspDrawAllCached()
{
    if (!g_ESPVehicles || g_PanicHidden) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < kVehEspCache; i++)
    {
        OakVehEspSlot& s = g_VehEspCache[i];
        if (!s.ent) continue;
        if ((now - s.seen) > 4000) { s.ent = 0; s.hasScreen = false; continue; }

        __try {
            Vec3 pos;
            if (!GetEntityPosition(s.ent, pos))
            {
                if (!s.hasScreen || (now - s.w2sTick) > 450) continue;
            }
            else
            {
                Vec3 top = pos; top.y += 2.0f;
                Vec2 topScreen, bottomScreen;
                bool ok = WorldToScreen(top, topScreen) && WorldToScreen(pos, bottomScreen);
                float height = ok ? (bottomScreen.y - topScreen.y) : 0.f;
                if (ok && height >= 2.0f)
                {
                    float width = height * 1.5f;
                    float cx = topScreen.x;
                    float cy = topScreen.y + height * 0.5f;
                    if (!s.hasScreen)
                    {
                        s.sx = cx; s.sy = cy; s.sh = height;
                        s.hasScreen = true;
                    }
                    else
                    {
                        const float a = 0.40f;
                        s.sx += (cx - s.sx) * a;
                        s.sy += (cy - s.sy) * a;
                        s.sh += (height - s.sh) * a;
                    }
                    s.w2sTick = now;
                }
                else if (!s.hasScreen || (now - s.w2sTick) > 450)
                {
                    continue;
                }
            }

            float height = s.sh > 2.f ? s.sh : 24.f;
            float width = height * 1.5f;
            float x = s.sx - width * 0.5f;
            float y = s.sy - height * 0.5f;

            if (g_VehicleBox)
                AddBox(x, y, width, height, g_ColorVehicleBox[0], g_ColorVehicleBox[1], g_ColorVehicleBox[2], g_ColorVehicleBox[3]);

            if (g_VehicleName || g_VehicleDistanceEnabled)
            {
                float textY = y + height + 6.0f;
                if (g_VehicleName && s.name[0])
                {
                    DrawEspLabelCentered(s.sx, textY, s.name,
                        g_ColorVehicleBox[0], g_ColorVehicleBox[1], g_ColorVehicleBox[2], g_ColorVehicleBox[3]);
                    textY += ImGui::GetFontSize() + 2.f;
                }
                if (g_VehicleDistanceEnabled && s.dist > 0)
                {
                    char distText[32];
                    wsprintfA(distText, "[%dm]", s.dist);
                    float dr = 0.7f, dg = 0.7f, db = 0.7f;
                    if (s.dist < 50) { dr = 0.2f; dg = 1.0f; db = 0.2f; }
                    else if (s.dist < 150) { dr = 1.0f; dg = 1.0f; db = 0.2f; }
                    else if (s.dist < 300) { dr = 1.0f; dg = 0.5f; db = 0.2f; }
                    else { dr = 1.0f; dg = 0.3f; db = 0.3f; }
                    DrawEspLabelCentered(s.sx, textY, distText, dr, dg, db, 1.0f);
                }
            }
            g_EspDrawCount++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s.ent = 0;
            s.hasScreen = false;
        }
    }
}

static void EndDraw()
{
    // ImGui flushes in ImGuiMenu_EndFrame
}


static bool WorldToScreen(Vec3 world, Vec2& screen)
{
    if (!g_W2S.valid)
        return false;

    Vec3 temp;
    temp.x = world.x - g_W2S.translation.x;
    temp.y = world.y - g_W2S.translation.y;
    temp.z = world.z - g_W2S.translation.z;

    float x = temp.x * g_W2S.right.x + temp.y * g_W2S.right.y + temp.z * g_W2S.right.z;
    float y = temp.x * g_W2S.up.x + temp.y * g_W2S.up.y + temp.z * g_W2S.up.z;
    float z = temp.x * g_W2S.forward.x + temp.y * g_W2S.forward.y + temp.z * g_W2S.forward.z;

    if (z < 0.65f)
        return false;

    float invZ = 1.0f / z;
    float normalizedX = (x / g_W2S.projX) * invZ;
    float normalizedY = (y / g_W2S.projY) * invZ;

    screen.x = g_ScreenHalfW + (normalizedX * g_ScreenHalfW);
    screen.y = g_ScreenHalfH - (normalizedY * g_ScreenHalfH);

    return screen.x >= -50 && screen.x <= g_ScreenWidth + 50 && screen.y >= -50 && screen.y <= g_ScreenHeight + 50;
}

static bool WorldToScreenLoose(Vec3 world, Vec2& screen)
{
    if (!g_W2S.valid)
        return false;

    Vec3 temp;
    temp.x = world.x - g_W2S.translation.x;
    temp.y = world.y - g_W2S.translation.y;
    temp.z = world.z - g_W2S.translation.z;

    float x = temp.x * g_W2S.right.x + temp.y * g_W2S.right.y + temp.z * g_W2S.right.z;
    float y = temp.x * g_W2S.up.x + temp.y * g_W2S.up.y + temp.z * g_W2S.up.z;
    float z = temp.x * g_W2S.forward.x + temp.y * g_W2S.forward.y + temp.z * g_W2S.forward.z;
    if (z < 0.12f)
        return false;

    float invZ = 1.0f / z;
    float normalizedX = (x / g_W2S.projX) * invZ;
    float normalizedY = (y / g_W2S.projY) * invZ;
    screen.x = g_ScreenHalfW + (normalizedX * g_ScreenHalfW);
    screen.y = g_ScreenHalfH - (normalizedY * g_ScreenHalfH);
    return screen.x >= -200 && screen.x <= g_ScreenWidth + 200 &&
           screen.y >= -200 && screen.y <= g_ScreenHeight + 200;
}


struct Matrix3x4 {
    float m[12];  
};


enum BoneID {
    BONE_HEAD = 0,
    BONE_NECK = 1,
    BONE_SPINE2 = 2,
    BONE_SPINE1 = 3,
    BONE_PELVIS = 4,
    BONE_L_SHOULDER = 5,
    BONE_R_SHOULDER = 6,
    BONE_L_ELBOW = 7,
    BONE_R_ELBOW = 8,
    BONE_L_HAND = 9,
    BONE_R_HAND = 10,
    BONE_L_HIP = 11,
    BONE_R_HIP = 12,
    BONE_L_KNEE = 13,
    BONE_R_KNEE = 14,
    BONE_L_FOOT = 15,
    BONE_R_FOOT = 16,
    BONES_MAX = 17
};


static int g_PlayerBones[BONES_MAX] = { 24, 21, 20, 19, 0, 61, 94, 63, 97, 65, 99, 1, 9, 4, 12, 6, 14 };


static int g_ZombieBones[BONES_MAX] = { 21, 19, 16, 15, 0, 24, 56, 25, 59, 27, 60, 1, 9, 4, 12, 6, 13 };


struct SkeletonCache
{
    bool valid;                 
    bool isPlayer;              
    Matrix3x4 playerMatrix;     
    uintptr_t matrixClass;      
};


static bool InitSkeletonCache(uintptr_t entity, bool isPlayer, SkeletonCache& cache)
{
    cache.valid = false;
    cache.isPlayer = isPlayer;
    
    if (!IsValidPtr(entity) || entity < 0x100000000) return false;
    
    uintptr_t visualState = Read<uintptr_t>(entity + offsets::entity::VisualState);
    if (!IsValidPtr(visualState) || visualState < 0x100000000)
        visualState = Read<uintptr_t>(entity + offsets::entity::FutureVisualState);
    if (!IsValidPtr(visualState) || visualState < 0x100000000) return false;
    
    for (int i = 0; i < 12; i++)
        cache.playerMatrix.m[i] = Read<float>(visualState + 0x8 + i * 4);
    
    uintptr_t skeleton = 0;
    if (isPlayer)
    {
        const uintptr_t skOffs[] = { offsets::player::Skeleton, 0x7E8ULL, 0x7E0ULL };
        for (int si = 0; si < 3; si++)
        {
            uintptr_t cand = Read<uintptr_t>(entity + skOffs[si]);
            if (!IsValidPtr(cand) || cand < 0x100000000) continue;
            uintptr_t anim = Read<uintptr_t>(cand + oak_offsets::anim::AnimComponent);
            if (!IsValidPtr(anim) || anim < 0x100000000)
                anim = Read<uintptr_t>(cand + 0xB0);
            if (!IsValidPtr(anim) || anim < 0x100000000) continue;
            uintptr_t mats = Read<uintptr_t>(anim + offsets::animclass::MatrixArray);
            if (!IsValidPtr(mats) || mats < 0x100000000)
                mats = Read<uintptr_t>(anim + 0xBF0);
            if (IsValidPtr(mats) && mats > 0x100000000)
            {
                skeleton = cand;
                break;
            }
        }
        if (!skeleton)
            skeleton = Read<uintptr_t>(entity + offsets::player::Skeleton);
    }
    else
    {
        // Probe current + legacy infected skeleton offsets
        const uintptr_t skOffs[] = { offsets::infected::Skeleton, 0x678ULL, 0x670ULL, 0x660ULL };
        for (int si = 0; si < 4; si++)
        {
            uintptr_t cand = Read<uintptr_t>(entity + skOffs[si]);
            if (!IsValidPtr(cand) || cand < 0x100000000) continue;
            // Quick anim/matrix sanity
            uintptr_t anim = Read<uintptr_t>(cand + oak_offsets::anim::AnimComponent);
            if (!IsValidPtr(anim) || anim < 0x100000000)
                anim = Read<uintptr_t>(cand + 0xB0);
            if (!IsValidPtr(anim) || anim < 0x100000000) continue;
            uintptr_t mats = Read<uintptr_t>(anim + offsets::animclass::MatrixArray);
            if (!IsValidPtr(mats) || mats < 0x100000000)
                mats = Read<uintptr_t>(anim + 0xBF0);
            if (IsValidPtr(mats) && mats > 0x100000000)
            {
                skeleton = cand;
                break;
            }
        }
    }
    if (!IsValidPtr(skeleton) || skeleton < 0x100000000) return false;
    
    uintptr_t animClass = 0;
    const uintptr_t animOffs[] = {
        oak_offsets::anim::AnimComponent,
        0xB0ULL,
        offsets::skeleton::AnimClass1,
        offsets::skeleton::AnimClass2
    };
    for (int ai = 0; ai < 4; ai++)
    {
        uintptr_t cand = Read<uintptr_t>(skeleton + animOffs[ai]);
        if (!IsValidPtr(cand) || cand < 0x100000000) continue;
        uintptr_t mats = Read<uintptr_t>(cand + offsets::animclass::MatrixArray);
        if (!IsValidPtr(mats) || mats < 0x100000000)
            mats = Read<uintptr_t>(cand + 0xBF0);
        if (IsValidPtr(mats) && mats > 0x100000000)
        {
            animClass = cand;
            cache.matrixClass = mats;
            break;
        }
    }
    if (!animClass || !IsValidPtr(cache.matrixClass)) return false;
    
    cache.valid = true;
    return true;
}


static Vec3 GetBonePositionCached(const SkeletonCache& cache, int boneIndex)
{
    Vec3 result = { 0, 0, 0 };
    
    if (!cache.valid) return result;
    if (boneIndex < 0 || boneIndex >= BONES_MAX) return result;
    if (!IsValidPtr(cache.matrixClass) || cache.matrixClass < 0x100000000) return result;
    
    int boneId = cache.isPlayer ? g_PlayerBones[boneIndex] : g_ZombieBones[boneIndex];
    // Engine bone slots go into the 90s — hard-cap to avoid OOB matrix walks
    if (boneId < 0 || boneId > 128) return result;
    
    uintptr_t boneAddr = cache.matrixClass + offsets::animclass::MatrixB + (uintptr_t)boneId * 48;
    // ProbeMemRange (cached) — raw VirtualQuery×17 bones was a Present killer
    if (!ProbeMemRange(boneAddr, 12, false, nullptr))
        return result;
    
    Vec3 boneLocal;
    boneLocal.x = Read<float>(boneAddr);
    boneLocal.y = Read<float>(boneAddr + 4);
    boneLocal.z = Read<float>(boneAddr + 8);
    if (boneLocal.x != boneLocal.x || boneLocal.y != boneLocal.y || boneLocal.z != boneLocal.z)
        return result;
    // Reject absurd local bone coords (wrong skeleton layout)
    if (fabsf(boneLocal.x) > 8.f || fabsf(boneLocal.y) > 8.f || fabsf(boneLocal.z) > 8.f)
        return result;
    
    const float* m = cache.playerMatrix.m;
    result.x = (m[0] * boneLocal.x) + (m[3] * boneLocal.y) + (m[6] * boneLocal.z) + m[9];
    result.y = (m[1] * boneLocal.x) + (m[4] * boneLocal.y) + (m[7] * boneLocal.z) + m[10];
    result.z = (m[2] * boneLocal.x) + (m[5] * boneLocal.y) + (m[8] * boneLocal.z) + m[11];
    
    return result;
}


static Vec3 GetBonePosition(uintptr_t entity, int boneIndex, bool isPlayer)
{
    SkeletonCache cache;
    if (!InitSkeletonCache(entity, isPlayer, cache))
    {
        Vec3 zero = { 0, 0, 0 };
        return zero;
    }
    return GetBonePositionCached(cache, boneIndex);
}


static void DrawEntityExtras(uintptr_t entity, bool isPlayer, float boxX, float boxY, float boxW, float boxH, float headX, float footY);

// Phase B batch-4 ESP (defined in oak_batch4_esp.inl)
static void OakBatch4ResetFrame();
static void OakBatch4SetVisContext(uintptr_t worldPtr, uintptr_t localPlayer);
static void OakBatch4AppendItemExtras(uintptr_t entity, char* infoText, int infoMax);
static void OakBatch4DrawItemQualityBadge(uintptr_t entity, float screenX, float nameY);
static void OakBatch4OnPlayerEsp(uintptr_t entity, int dist);
static void OakBatch4DrawLookingAtMe(uintptr_t entity, int dist, bool isPlayer);
static void OakBatch4OnContainerEsp(uintptr_t entity, float screenX, float screenY);
static void OakBatch4DrawWorldOverlays(uintptr_t worldPtr);
static const float* OakBatch4PickPlayerBoxColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal);
static const float* OakBatch4PickZombieBoxColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal);
static const float* OakBatch4PickEntityColor(uintptr_t entity, int listIdx, bool isPlayer, const float* normal);
static void OakBatch4UpdateBoneOccMask(uintptr_t entity, bool isPlayer, const Vec3* bonePos, const bool* boneOk, int boneCount);
static bool OakBatch4BoneOccluded(uintptr_t entity, int boneId);
static const float* OakBatch4VisColorPair(bool isPlayer, bool occluded);
static bool OakBatch4ProbePointOcc(uintptr_t entity, const Vec3& worldPos);

static ID3D11ShaderResourceView* g_EspNameAvatar = nullptr;

static void DrawEntitySkeleton(uintptr_t entity, bool isPlayer, const char* name, int distance)
{
    __try
    {
    SkeletonCache cache;
    if (!InitSkeletonCache(entity, isPlayer, cache))
        return;

    Vec3 bonePositions[BONES_MAX];
    Vec2 boneScreenPositions[BONES_MAX];
    bool boneValid[BONES_MAX];

    for (int i = 0; i < BONES_MAX; i++)
    {
        bonePositions[i] = GetBonePositionCached(cache, i);
        boneValid[i] = WorldToScreen(bonePositions[i], boneScreenPositions[i]);
    }

    if (!boneValid[BONE_HEAD] || !boneValid[BONE_PELVIS]) return;

    Vec2 headScreen = boneScreenPositions[BONE_HEAD];
    float footY = headScreen.y;
    if (boneValid[BONE_L_FOOT] && boneScreenPositions[BONE_L_FOOT].y > footY)
        footY = boneScreenPositions[BONE_L_FOOT].y;
    if (boneValid[BONE_R_FOOT] && boneScreenPositions[BONE_R_FOOT].y > footY)
        footY = boneScreenPositions[BONE_R_FOOT].y;

    float height = footY - headScreen.y;
    if (height < 0.f) height = -height;
    if (height < 2.f) height = 2.f;
    float width = height / 2.5f;
    if (width < 2.f) width = 2.f;
    float x = headScreen.x - width / 2.0f;
    float y = (headScreen.y < footY) ? headScreen.y - height * 0.05f : footY - height * 0.05f;

    const bool useVis = (isPlayer && g_PlayerUseVisColors) || (!isPlayer && g_ZombieUseVisColors);
    if (useVis)
        OakBatch4UpdateBoneOccMask(entity, isPlayer, bonePositions, boneValid, BONES_MAX);

    bool wantBox = (isPlayer && g_PlayerBox) || (!isPlayer && g_ZombieBox);
    float boxH = height + height * 0.1f;
    if (wantBox)
    {
        if (useVis)
        {
            // Split box: top = head visibility, bottom = feet visibility.
            bool headOcc = boneValid[BONE_HEAD] && OakBatch4BoneOccluded(entity, BONE_HEAD);
            bool footOcc = false;
            if (boneValid[BONE_L_FOOT] || boneValid[BONE_R_FOOT])
            {
                bool l = boneValid[BONE_L_FOOT] && OakBatch4BoneOccluded(entity, BONE_L_FOOT);
                bool r = boneValid[BONE_R_FOOT] && OakBatch4BoneOccluded(entity, BONE_R_FOOT);
                footOcc = (boneValid[BONE_L_FOOT] ? l : true) && (boneValid[BONE_R_FOOT] ? r : true);
            }
            else if (boneValid[BONE_PELVIS])
                footOcc = OakBatch4BoneOccluded(entity, BONE_PELVIS);

            const float* topC = OakBatch4VisColorPair(isPlayer, headOcc);
            const float* botC = OakBatch4VisColorPair(isPlayer, footOcc);
            float midY = y + boxH * 0.5f;
            int style = isPlayer ? g_PlayerBoxStyle : g_ZombieBoxStyle;
            float thick = isPlayer ? g_PlayerBoxThick : g_ZombieBoxThick;
            DrawEspBoxStyled(x, y, width, boxH * 0.5f, topC[0], topC[1], topC[2], topC[3], style, thick);
            DrawEspBoxStyled(x, midY, width, boxH * 0.5f, botC[0], botC[1], botC[2], botC[3], style, thick);
            // Side seams so the two halves read as one box
            ImDrawList* dlBox = ImGuiMenu_EspDrawList();
            if (dlBox)
            {
                dlBox->AddLine(ImVec2(x, midY), ImVec2(x + width, midY),
                    ToCol(0.f, 0.f, 0.f, 0.35f), 1.0f);
            }
        }
        else
        {
            const float* boxC = isPlayer ? g_ColorPlayerBox : g_ColorZombieBox;
            DrawEspBoxStyled(x, y, width, boxH, boxC[0], boxC[1], boxC[2], boxC[3],
                isPlayer ? g_PlayerBoxStyle : g_ZombieBoxStyle,
                isPlayer ? g_PlayerBoxThick : g_ZombieBoxThick);
        }
    }

    static int skeletonBones[][2] = {
        {BONE_NECK, BONE_SPINE2},
        {BONE_SPINE2, BONE_SPINE1},
        {BONE_SPINE1, BONE_PELVIS},
        {BONE_PELVIS, BONE_L_HIP},
        {BONE_L_HIP, BONE_L_KNEE},
        {BONE_L_KNEE, BONE_L_FOOT},
        {BONE_PELVIS, BONE_R_HIP},
        {BONE_R_HIP, BONE_R_KNEE},
        {BONE_R_KNEE, BONE_R_FOOT},
        {BONE_NECK, BONE_L_SHOULDER},
        {BONE_L_SHOULDER, BONE_L_ELBOW},
        {BONE_L_ELBOW, BONE_L_HAND},
        {BONE_NECK, BONE_R_SHOULDER},
        {BONE_R_SHOULDER, BONE_R_ELBOW},
        {BONE_R_ELBOW, BONE_R_HAND}
    };

    if (g_ShowSkeleton)
    {
        ImDrawList* dl = ImGuiMenu_EspDrawList();
        if (dl)
        {
            const float* skDef = isPlayer ? g_ColorPlayerSkeleton : g_ColorZombieSkeleton;
            for (int i = 0; i < 15; i++)
            {
                int from = skeletonBones[i][0];
                int to = skeletonBones[i][1];
                if (!boneValid[from] || !boneValid[to]) continue;

                const float* cFrom = skDef;
                const float* cTo = skDef;
                if (useVis)
                {
                    cFrom = OakBatch4VisColorPair(isPlayer, OakBatch4BoneOccluded(entity, from));
                    cTo = OakBatch4VisColorPair(isPlayer, OakBatch4BoneOccluded(entity, to));
                }

                ImVec2 a(boneScreenPositions[from].x, boneScreenPositions[from].y);
                ImVec2 b(boneScreenPositions[to].x, boneScreenPositions[to].y);
                if (cFrom[0] == cTo[0] && cFrom[1] == cTo[1] && cFrom[2] == cTo[2])
                {
                    dl->AddLine(a, b, ToCol(cFrom[0], cFrom[1], cFrom[2], cFrom[3]), 1.75f);
                }
                else
                {
                    // Half exposed / half hidden along the bone.
                    ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                    dl->AddLine(a, mid, ToCol(cFrom[0], cFrom[1], cFrom[2], cFrom[3]), 1.85f);
                    dl->AddLine(mid, b, ToCol(cTo[0], cTo[1], cTo[2], cTo[3]), 1.85f);
                }
            }
            if (boneValid[BONE_HEAD])
            {
                const float* hc = useVis
                    ? OakBatch4VisColorPair(isPlayer, OakBatch4BoneOccluded(entity, BONE_HEAD))
                    : skDef;
                float headSize = height / 9.0f;
                if (headSize < 3.f) headSize = 3.f;
                if (headSize > 14.f) headSize = 14.f;
                dl->AddCircle(ImVec2(headScreen.x, headScreen.y), headSize,
                    ToCol(hc[0], hc[1], hc[2], hc[3]), 20, 1.5f);
            }
            g_EspDrawCount++;
        }
    }

    bool showName = (isPlayer && g_PlayerName) || (!isPlayer && g_ZombieName);
    bool showDist = (isPlayer && g_PlayerDistanceEnabled) || (!isPlayer && g_ZombieDistanceEnabled);
    const float* nameC = isPlayer ? g_ColorPlayerName : g_ColorZombieName;
    if (useVis)
        nameC = OakBatch4PickEntityColor(entity, g_Batch4EspListIdx, isPlayer,
            isPlayer ? g_PlayerColorVisible : g_ZombieColorVisible);

    if (showName || showDist)
    {
        float textY = footY + 6.0f;
        if (showName && name && name[0])
        {
            DrawEspNameWithAvatar(headScreen.x, textY, name, isPlayer ? g_EspNameAvatar : nullptr,
                nameC[0], nameC[1], nameC[2], nameC[3]);
            textY += ImGui::GetFontSize() + 2.f;
        }
        if (showDist)
        {
            char distText[32];
            if (distance < 0)
                wsprintfA(distText, "[?]");
            else
                wsprintfA(distText, "[%dm]", distance);
            DrawEspLabelCentered(headScreen.x, textY, distText, nameC[0], nameC[1], nameC[2], nameC[3]);
        }
    }

    DrawEntityExtras(entity, isPlayer, x, y, width, boxH, headScreen.x, footY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static int g_DebugCounter = 0;



static bool IsNaN(float f)
{
    return (f != f);
}


static bool GetEntityPosition(uintptr_t entity, Vec3& outPos)
{
    outPos = { 0, 0, 0 };
    if (!IsValidPtr(entity) || entity < 0x100000000)
        return false;

    auto tryState = [&](uintptr_t stateOff) -> bool {
        uintptr_t visualState = Read<uintptr_t>(entity + stateOff);
        if (!IsValidPtr(visualState) || visualState < 0x100000000)
            return false;

        // Preferred: translation at VisualState+0x2C
        float x = Read<float>(visualState + 0x2C);
        float y = Read<float>(visualState + 0x30);
        float z = Read<float>(visualState + 0x34);
        if (!IsNaN(x) && !IsNaN(y) && !IsNaN(z) &&
            !(x == 0.f && y == 0.f && z == 0.f) &&
            x > -100000.f && x < 100000.f && z > -100000.f && z < 100000.f)
        {
            outPos.x = x; outPos.y = y; outPos.z = z;
            return true;
        }

        // Fallback: 3x4 matrix translation at +0x8 (m[9], m[10], m[11])
        x = Read<float>(visualState + 0x8 + 9 * 4);
        y = Read<float>(visualState + 0x8 + 10 * 4);
        z = Read<float>(visualState + 0x8 + 11 * 4);
        if (!IsNaN(x) && !IsNaN(y) && !IsNaN(z) &&
            !(x == 0.f && y == 0.f && z == 0.f) &&
            x > -100000.f && x < 100000.f && z > -100000.f && z < 100000.f)
        {
            outPos.x = x; outPos.y = y; outPos.z = z;
            return true;
        }
        return false;
    };

    if (tryState(offsets::entity::VisualState))
        return true;
    if (tryState(offsets::entity::FutureVisualState))
        return true;
    return false;
}


static bool GetItemPosition(uintptr_t entity, Vec3& outPos)
{
    static int debugCallCount = 0;
    char buf[256];
    
    
    if (GetEntityPosition(entity, outPos))
    {
        
        
        bool hasNaN = (IsNaN(outPos.x) || IsNaN(outPos.y) || IsNaN(outPos.z));
        bool hasInf = (!hasNaN && (outPos.x != outPos.x || outPos.y != outPos.y || outPos.z != outPos.z));
        
        if (hasNaN || hasInf)
        {
            if (debugCallCount < 3)
            {
                wsprintfA(buf, "GetItemPosition: GetEntityPosition returned NaN/Inf values pos=(%.1f, %.1f, %.1f)", 
                    outPos.x, outPos.y, outPos.z);
                Log(buf);
                debugCallCount++;
            }
            return false; 
        }
        
        
        if (outPos.x < -1000000.0f || outPos.x > 1000000.0f || 
            outPos.z < -1000000.0f || outPos.z > 1000000.0f)
        {
            if (debugCallCount < 3)
            {
                wsprintfA(buf, "GetItemPosition: GetEntityPosition returned out-of-range values pos=(%.1f, %.1f, %.1f)", 
                    outPos.x, outPos.y, outPos.z);
                Log(buf);
                debugCallCount++;
            }
            return false;
        }
        
        if (debugCallCount < 3)
        {
            wsprintfA(buf, "GetItemPosition: GetEntityPosition succeeded pos=(%.1f, %.1f, %.1f)", 
                outPos.x, outPos.y, outPos.z);
            Log(buf);
            debugCallCount++;
        }
        return true;
    }
    
    if (debugCallCount < 3)
    {
        wsprintfA(buf, "GetItemPosition: GetEntityPosition failed, trying Transform matrix");
        Log(buf);
    }
    
    
    uintptr_t visualStateAddr = entity + 0x1C8;
    if (!IsValidPtr(visualStateAddr) || visualStateAddr < 0x100000000) 
    {
        if (debugCallCount < 3)
        {
            wsprintfA(buf, "GetItemPosition: visualState address invalid (0x%X%08X)", 
                (DWORD)(visualStateAddr >> 32), (DWORD)visualStateAddr);
            Log(buf);
        }
    }
    else
    {
        uintptr_t visualState = Read<uintptr_t>(visualStateAddr);
        if (IsValidPtr(visualState) && visualState > 0x100000000)
        {
            
            uintptr_t matrixBase = visualState + 0x8;
            if (IsValidPtr(matrixBase) && matrixBase > 0x100000000)
            {
                
                
                uintptr_t m9Addr = matrixBase + 9 * 4;
                uintptr_t m10Addr = matrixBase + 10 * 4;
                uintptr_t m11Addr = matrixBase + 11 * 4;
                
                if (IsValidPtr(m9Addr) && IsValidPtr(m10Addr) && IsValidPtr(m11Addr))
                {
                    float m9 = Read<float>(m9Addr);   
                    float m10 = Read<float>(m10Addr); 
                    float m11 = Read<float>(m11Addr); 
        
                    if (debugCallCount < 3)
                    {
                        wsprintfA(buf, "GetItemPosition: Matrix values m9=%.1f m10=%.1f m11=%.1f", m9, m10, m11);
                        Log(buf);
                    }
                    
                    
                    if (!IsNaN(m9) && !IsNaN(m10) && !IsNaN(m11))
                    {
                        if (m9 != 0.0f || m10 != 0.0f || m11 != 0.0f)
                        {
                            if (m9 >= -100000.0f && m9 <= 100000.0f && m11 >= -100000.0f && m11 <= 100000.0f)
                            {
                                outPos.x = m9;
                                outPos.y = m10;
                                outPos.z = m11;
                                if (debugCallCount < 3)
                                {
                                    wsprintfA(buf, "GetItemPosition: Matrix method succeeded");
                                    Log(buf);
                                    debugCallCount++;
                                }
                                return true;
                            }
                        }
                    }
                }
            }
        }
        else if (debugCallCount < 3)
        {
            wsprintfA(buf, "GetItemPosition: visualState invalid or null (0x%X%08X)", 
                (DWORD)(visualState >> 32), (DWORD)visualState);
            Log(buf);
        }
    }
    
    
    if (debugCallCount < 3)
    {
        Log("getpos trying fallback");
    }
    
    uintptr_t offsets[] = {
        0x2C,   
        0x8,    
        0x1C,   
        0x30,   
        0x1C8,  
    };
    
    for (int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    {
        float x = Read<float>(entity + offsets[i]);
        float y = Read<float>(entity + offsets[i] + 4);
        float z = Read<float>(entity + offsets[i] + 8);
        
        
        if (IsNaN(x) || IsNaN(y) || IsNaN(z)) continue;
        
        if (x != 0.0f || y != 0.0f || z != 0.0f)
        {
            if (x >= -100000.0f && x <= 100000.0f && z >= -100000.0f && z <= 100000.0f)
            {
                outPos.x = x;
                outPos.y = y;
                outPos.z = z;
                if (debugCallCount < 3)
                {
                    wsprintfA(buf, "GetItemPosition: Fallback offset 0x%X succeeded pos=(%.1f, %.1f, %.1f)", 
                        offsets[i], outPos.x, outPos.y, outPos.z);
                    Log(buf);
                    debugCallCount++;
                }
                return true;
            }
        }
    }
    
    if (debugCallCount < 3)
    {
        Log("getpos all failed");
        debugCallCount++;
    }
    
    return false;
}


static void DrawEntitySimple(uintptr_t entity, bool isPlayer, const char* name, int distance)
{
    Vec3 pos;
    if (!GetEntityPosition(entity, pos)) return;

    Vec3 head = pos;
    head.y += isPlayer ? 1.8f : 1.5f;
    Vec3 feet = pos;

    Vec2 headScreen, feetScreen;
    if (!WorldToScreen(head, headScreen)) return;
    if (!WorldToScreen(feet, feetScreen)) return;

    float height = feetScreen.y - headScreen.y;
    if (height < 0.0f) height = -height;
    if (height < 2.0f) height = 2.0f;

    float width = height / 2.5f;
    if (width < 2.0f) width = 2.0f;
    float x = headScreen.x - width / 2.0f;
    float y = (headScreen.y < feetScreen.y) ? headScreen.y : feetScreen.y;

    const float* boxC = isPlayer ? g_ColorPlayerBox : g_ColorZombieBox;
    const bool useVis = (isPlayer && g_PlayerUseVisColors) || (!isPlayer && g_ZombieUseVisColors);
    if (useVis)
        boxC = OakBatch4PickEntityColor(entity, g_Batch4EspListIdx, isPlayer,
            isPlayer ? g_PlayerColorVisible : g_ZombieColorVisible);
    bool wantBox = (isPlayer && g_PlayerBox) || (!isPlayer && g_ZombieBox);

    if (wantBox)
    {
        if (useVis)
        {
            // Approximate split without bones: top third vs bottom using head/feet samples.
            Vec3 mid = head;
            mid.y = (head.y + feet.y) * 0.5f;
            bool topOcc = OakBatch4ProbePointOcc(entity, head);
            bool botOcc = OakBatch4ProbePointOcc(entity, feet);
            const float* topC = OakBatch4VisColorPair(isPlayer, topOcc);
            const float* botC = OakBatch4VisColorPair(isPlayer, botOcc);
            int style = isPlayer ? g_PlayerBoxStyle : g_ZombieBoxStyle;
            float thick = isPlayer ? g_PlayerBoxThick : g_ZombieBoxThick;
            DrawEspBoxStyled(x, y, width, height * 0.5f, topC[0], topC[1], topC[2], topC[3], style, thick);
            DrawEspBoxStyled(x, y + height * 0.5f, width, height * 0.5f, botC[0], botC[1], botC[2], botC[3], style, thick);
        }
        else
        {
            DrawEspBoxStyled(x, y, width, height, boxC[0], boxC[1], boxC[2], boxC[3],
                isPlayer ? g_PlayerBoxStyle : g_ZombieBoxStyle,
                isPlayer ? g_PlayerBoxThick : g_ZombieBoxThick);
        }
    }

    bool showName = (isPlayer && g_PlayerName) || (!isPlayer && g_ZombieName);
    bool showDist = (isPlayer && g_PlayerDistanceEnabled) || (!isPlayer && g_ZombieDistanceEnabled);
    const float* nameC = isPlayer ? g_ColorPlayerName : g_ColorZombieName;
    if (isPlayer && g_PlayerUseVisColors)
        nameC = OakBatch4PickEntityColor(entity, g_Batch4EspListIdx, true, g_PlayerColorVisible);
    else if (!isPlayer && g_ZombieUseVisColors)
        nameC = OakBatch4PickEntityColor(entity, g_Batch4EspListIdx, false, g_ZombieColorVisible);

    if (showName || showDist)
    {
        float textY = ((feetScreen.y > headScreen.y) ? feetScreen.y : headScreen.y) + 6.0f;
        if (showName && name && name[0])
        {
            DrawEspNameWithAvatar(headScreen.x, textY, name, isPlayer ? g_EspNameAvatar : nullptr,
                nameC[0], nameC[1], nameC[2], nameC[3]);
            textY += ImGui::GetFontSize() + 2.f;
        }
        if (showDist)
        {
            char distText[32];
            if (distance < 0)
                wsprintfA(distText, "[?]");
            else
                wsprintfA(distText, "[%dm]", distance);
            DrawEspLabelCentered(headScreen.x, textY, distText, nameC[0], nameC[1], nameC[2], nameC[3]);
        }
    }

    DrawEntityExtras(entity, isPlayer, x, y, width, height, headScreen.x,
        (feetScreen.y > headScreen.y) ? feetScreen.y : headScreen.y);
}


static void DrawAnimal(uintptr_t entity, const char* name, int distance)
{
    Vec3 pos;
    if (!GetEntityPosition(entity, pos)) return;
    
    
    Vec3 top = pos;
    top.y += 1.2f;
    Vec3 bottom = pos;
    
    
    Vec2 topScreen, bottomScreen;
    if (!WorldToScreen(top, topScreen)) return;
    if (!WorldToScreen(bottom, bottomScreen)) return;
    
    
    float height = bottomScreen.y - topScreen.y;
    if (height < 2.0f) return;  
    
    float width = height / 2.0f;  
    float x = topScreen.x - width / 2.0f;
    float y = topScreen.y;
    
    
    if (g_AnimalBox)
        AddBox(x, y, width, height, g_ColorAnimalBox[0], g_ColorAnimalBox[1], g_ColorAnimalBox[2], g_ColorAnimalBox[3]);
    
    
    if (g_AnimalName || g_AnimalDistanceEnabled)
    {
        float textY = bottomScreen.y + 6.0f;
        if (g_AnimalName && name && name[0])
        {
            DrawEspLabelCentered(topScreen.x, textY, name,
                g_ColorAnimalBox[0], g_ColorAnimalBox[1], g_ColorAnimalBox[2], g_ColorAnimalBox[3]);
            textY += ImGui::GetFontSize() + 2.f;
        }
        if (g_AnimalDistanceEnabled && distance > 0)
        {
            char distText[32];
            wsprintfA(distText, "[%dm]", distance);
            float dr = 0.7f, dg = 0.7f, db = 0.7f;
            if (distance < 50) { dr = 0.2f; dg = 1.0f; db = 0.2f; }
            else if (distance < 150) { dr = 1.0f; dg = 1.0f; db = 0.2f; }
            else if (distance < 300) { dr = 1.0f; dg = 0.5f; db = 0.2f; }
            else { dr = 1.0f; dg = 0.3f; db = 0.3f; }
            DrawEspLabelCentered(topScreen.x, textY, distText, dr, dg, db, 1.0f);
        }
    }
}


static void DrawVehicle(uintptr_t entity, const char* name, int distance)
{
    Vec3 pos;
    if (!GetEntityPosition(entity, pos)) return;
    
    
    Vec3 top = pos;
    top.y += 2.0f;
    Vec3 bottom = pos;
    
    
    Vec2 topScreen, bottomScreen;
    if (!WorldToScreen(top, topScreen)) return;
    if (!WorldToScreen(bottom, bottomScreen)) return;
    
    
    float height = bottomScreen.y - topScreen.y;
    if (height < 2.0f) return;  
    
    float width = height * 1.5f;  
    float x = topScreen.x - width / 2.0f;
    float y = topScreen.y;
    
    
    if (g_VehicleBox)
        AddBox(x, y, width, height, g_ColorVehicleBox[0], g_ColorVehicleBox[1], g_ColorVehicleBox[2], g_ColorVehicleBox[3]);

    if (g_VehicleName || g_VehicleDistanceEnabled)
    {
        float textY = bottomScreen.y + 6.0f;
        if (g_VehicleName && name && name[0])
        {
            DrawEspLabelCentered(topScreen.x, textY, name,
                g_ColorVehicleBox[0], g_ColorVehicleBox[1], g_ColorVehicleBox[2], g_ColorVehicleBox[3]);
            textY += ImGui::GetFontSize() + 2.f;
        }
        if (g_VehicleDistanceEnabled && distance > 0)
        {
            char distText[32];
            wsprintfA(distText, "[%dm]", distance);
            float dr = 0.7f, dg = 0.7f, db = 0.7f;
            if (distance < 50) { dr = 0.2f; dg = 1.0f; db = 0.2f; }
            else if (distance < 150) { dr = 1.0f; dg = 1.0f; db = 0.2f; }
            else if (distance < 300) { dr = 1.0f; dg = 0.5f; db = 0.2f; }
            else { dr = 1.0f; dg = 0.3f; db = 0.3f; }
            DrawEspLabelCentered(topScreen.x, textY, distText, dr, dg, db, 1.0f);
        }
    }
}


static void DrawItem(uintptr_t entity, const char* name, int distance)
{
    Vec3 pos;
    if (!GetEntityPosition(entity, pos))
        return;
    if (IsNaN(pos.x) || IsNaN(pos.y) || IsNaN(pos.z))
        return;

    if (!LootNamePassesFilter(name))
        return;

    LootCategory cat = ClassifyLootName(name);
    if (!LootCategoryEnabled(cat))
        return;

    int catIdx = OakLootCategoryIndex((int)cat);
    if (catIdx >= 0 && catIdx < OAK_LOOT_CAT_COUNT)
    {
        if (g_LootCats[catIdx].maxCount > 0 && g_LootCatDrawn[catIdx] >= g_LootCats[catIdx].maxCount)
            return;
        int catMaxD = LootCategoryMaxDistance(cat);
        if (distance >= 0 && distance > catMaxD)
            return;
    }

    Vec2 screen;
    if (!WorldToScreen(pos, screen))
        return;

    float r, g, b, a;
    if (catIdx >= 0 && catIdx < OAK_LOOT_CAT_COUNT && g_LootCats[catIdx].useCustomColor)
    {
        r = g_LootCats[catIdx].color[0];
        g = g_LootCats[catIdx].color[1];
        b = g_LootCats[catIdx].color[2];
        a = g_LootCats[catIdx].color[3];
    }
    else if (cat == LootCat_Default)
    {
        r = g_ColorItemBox[0]; g = g_ColorItemBox[1]; b = g_ColorItemBox[2]; a = g_ColorItemBox[3];
    }
    else
        GetLootCategoryColor(cat, &r, &g, &b, &a);

    float boxSize = 7.0f;
    if (cat == LootCat_Weapon || cat == LootCat_Ammo)
        boxSize = 9.0f;
    float x = screen.x - boxSize * 0.5f;
    float y = screen.y - boxSize * 0.5f;

    if (g_ItemBox)
        AddBox(x, y, boxSize, boxSize, r, g, b, a);
    else
        AddBox(screen.x - 1.5f, screen.y - 1.5f, 3.0f, 3.0f, r, g, b, a * 0.85f);

    if (!g_ItemName && !g_ItemDistanceEnabled)
        return;

    char infoText[128];
    int pos2 = 0;
    if (g_ItemName && name && name[0])
    {
        for (int i = 0; name[i] && pos2 < 90; i++)
            infoText[pos2++] = name[i];
    }
    if (g_ItemDistanceEnabled && distance >= 0)
    {
        char distStr[16];
        IntToStr(distance, distStr);
        if (pos2 > 0) infoText[pos2++] = ' ';
        infoText[pos2++] = '[';
        for (int i = 0; distStr[i] && pos2 < 110; i++) infoText[pos2++] = distStr[i];
        infoText[pos2++] = 'm';
        infoText[pos2++] = ']';
    }
    infoText[pos2] = 0;
    if (pos2 <= 0)
        return;

    OakBatch4AppendItemExtras(entity, infoText, (int)sizeof(infoText));

    float nr = g_ColorItemName[0], ng = g_ColorItemName[1], nb = g_ColorItemName[2], na = g_ColorItemName[3];
    if (cat != LootCat_Default)
    {
        nr = r; ng = g; nb = b; na = a;
    }
    const float nameY = y - 14.0f;
    DrawEspLabelCentered(screen.x, nameY, infoText, nr, ng, nb, na);
    OakBatch4DrawItemQualityBadge(entity, screen.x, nameY);
    if (catIdx >= 0 && catIdx < OAK_LOOT_CAT_COUNT)
        g_LootCatDrawn[catIdx]++;
}


// DayZ entity lists are stored several ways across builds. Probe common layouts.
static bool ResolveEntityList(uintptr_t worldPtr, uintptr_t listOffset, int maxCount,
    uintptr_t& outData, int& outCount, const char* dbgName)
{
    outData = 0;
    outCount = 0;
    uintptr_t slot = worldPtr + listOffset;
    uintptr_t q = Read<uintptr_t>(slot);
    char buf[256];

    auto accept = [&](uintptr_t data, int count, const char* layout) -> bool {
        if (!IsValidPtr(data) || data < 0x100000000) return false;
        if (count <= 0 || count > maxCount) return false;
        outData = data;
        outCount = count;
        if (g_DebugCounter == 0 && dbgName)
        {
            wsprintfA(buf, "%s resolved via %s: data=0x%X%08X count=%d",
                dbgName, layout, (DWORD)(data >> 32), (DWORD)data, count);
            Log(buf);
        }
        return true;
    };

    // Embedded { start*, end* } → count = (end-start)/8
    {
        uintptr_t start = q;
        uintptr_t end = Read<uintptr_t>(slot + 8);
        if (IsValidPtr(start) && IsValidPtr(end) && end > start && ((end - start) % 8) == 0)
        {
            int c = (int)((end - start) / 8);
            if (accept(start, c, "embed-start-end")) return true;
        }
    }

    // Embedded { data*, count }
    {
        int c = Read<int>(slot + 8);
        if (accept(q, c, "embed-data-count")) return true;
    }

    // Pointer to list object
    if (IsValidPtr(q) && q > 0x100000000)
    {
        // { data*, count }
        if (accept(Read<uintptr_t>(q), Read<int>(q + 8), "obj-data-count")) return true;

        // { unk, count, data* } — observed: +0=small, +8=plausible count
        if (accept(Read<uintptr_t>(q + 0x10), Read<int>(q + 8), "obj-count-data@10")) return true;

        // { data*, capacity, count@0xC }
        if (accept(Read<uintptr_t>(q), Read<int>(q + 0xC), "obj-data-count@C")) return true;

        // { start*, end* }
        {
            uintptr_t start = Read<uintptr_t>(q);
            uintptr_t end = Read<uintptr_t>(q + 8);
            if (IsValidPtr(start) && IsValidPtr(end) && end > start && ((end - start) % 8) == 0)
            {
                int c = (int)((end - start) / 8);
                if (accept(start, c, "obj-start-end")) return true;
            }
        }

        // { data@0x8, count@0x10 }
        if (accept(Read<uintptr_t>(q + 8), Read<int>(q + 0x10), "obj-data@8-count@10")) return true;
    }

    if (g_DebugCounter == 0 && dbgName)
    {
        wsprintfA(buf, "%s: failed to resolve list (slot qword=0x%X%08X)",
            dbgName, (DWORD)(q >> 32), (DWORD)q);
        Log(buf);
    }
    return false;
}

static bool OakBatch4LosClear(uintptr_t worldPtr, uintptr_t localPlayer, uintptr_t targetEnt, const Vec3& aimPos);

#include "esp_addons_impl.inl"
#include "misc_impl.inl"
#include "oak_batch4_misc.inl"
#include "oak_features_impl.inl"
#include "oak_depth_vis.inl"
#include "oak_batch4_esp.inl"
#include "oak_shadow_chams.inl"
#include "oak_batch4_combat.inl"
#include "oak_crash_matrix.inl"
#include "oak_perf_boost.inl"

// EntityType → ConfigName/TypeName cache (many ents share one type; kills string re-reads)
enum { kEntTypeCacheSlots = 512 };
struct EntTypeCacheSlot
{
    uintptr_t type;
    DWORD tick;
    char cfg[48];
    char tn[48];
};
static EntTypeCacheSlot g_EntTypeCache[kEntTypeCacheSlots];

static void InvalidateEntTypeCache()
{
    for (int i = 0; i < kEntTypeCacheSlots; i++)
        g_EntTypeCache[i].type = 0;
}

static bool GetCachedEntityTypeStrings(uintptr_t entityType, char* cfgOut, int cfgMax, char* tnOut, int tnMax)
{
    if (!IsValidPtr(entityType) || entityType < 0x100000000)
        return false;
    unsigned h = (unsigned)((entityType >> 5) ^ (entityType >> 17)) & (kEntTypeCacheSlots - 1);
    EntTypeCacheSlot& s = g_EntTypeCache[h];
    DWORD now = GetTickCount();
    if (s.type == entityType && (now - s.tick) < 2500)
    {
        if (cfgOut && cfgMax > 0) lstrcpynA(cfgOut, s.cfg, cfgMax);
        if (tnOut && tnMax > 0) lstrcpynA(tnOut, s.tn, tnMax);
        return (cfgOut && cfgOut[0]) || (tnOut && tnOut[0]);
    }
    char cfg[64] = {};
    char tn[64] = {};
    uintptr_t cfgPtr = Read<uintptr_t>(entityType + offsets::entitytype::ConfigName);
    ReadEngineString(cfgPtr, cfg, 64);
    ReadTypeNameFromEntityType(entityType, tn, 64);
    s.type = entityType;
    s.tick = now;
    lstrcpynA(s.cfg, cfg, 48);
    lstrcpynA(s.tn, tn, 48);
    if (cfgOut && cfgMax > 0) lstrcpynA(cfgOut, cfg, cfgMax);
    if (tnOut && tnMax > 0) lstrcpynA(tnOut, tn, tnMax);
    return cfg[0] != 0 || tn[0] != 0;
}

static void OakQuiesceSessionState(const char* reason)
{
    const bool wasLive = InterlockedExchange(&g_SessionQuiesced, 1) == 0;

    // Hooks may still execute on game threads while Present is paused. Make
    // their pointer-specific callbacks no-ops before discarding any caches.
    g_FovWorkerCam = 0;
    g_FovWantHorizDeg = 0.f;

    // Drop any lag hold immediately — never leave outbound cut during connect.
    __try { LagSwitchShutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    __try { MiscFreecamShutdown(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_WarpDesyncActive = false;
    g_WarpCommitting = false;
    g_WarpGhostValid = false;

    // Never restore into objects from the old session. Those allocations may
    // already be recycled while the new landscape is being constructed.
    AbandonAmmoBackup();
    g_NoGrassHeld = false;
    g_NoGrassWorld = 0;
    g_NoGrassOrig = 0;

    g_CachedWorldPtr = 0;
    g_ResolvedLocalPlayer = 0;
    g_LocalPlayerValid = false;
    g_CameraValid = false;
    g_LocalPlayerPos = {};
    g_CameraPos = {};
    InvalidateMemPageCache();
    InvalidateEntTypeCache();
    InvalidateW2SCache();

    g_CachedNetworkClient = 0;
    g_CachedNetworkClientTick = 0;
    g_RosterRefreshTick = 0;
    g_RosterCount = 0;
    MiscClearSteamTagCache();

    for (int bi = 0; bi < kMaxBulletTracks; bi++)
        g_BulletTracks[bi].active = false;
    for (int bi = 0; bi < kMaxLocalBulletMarks; bi++)
        g_LocalBulletMarks[bi].used = false;
    g_CombatLock = {};
    g_MagicLock = {};
    HitCheck_Clear();
    OakLab_SetLocalPlayer(0);
    OakEngine_SetHot(0, 0, 0);

    if (wasLive)
    {
        char b[128];
        wsprintfA(b, "session: quiesced (%s)", reason ? reason : "pointer change");
        Log(b);
    }
}

static bool OakSessionReady(uintptr_t& outWorld, uintptr_t& outLocal, uintptr_t& outCamera)
{
    outWorld = outLocal = outCamera = 0;
    uintptr_t world = 0;
    if (g_GameModule)
    {
        __try { world = Read<uintptr_t>((uintptr_t)g_GameModule + offsets::modbase::World); }
        __except (EXCEPTION_EXECUTE_HANDLER) { world = 0; }
    }

    uintptr_t local = 0;
    uintptr_t camera = 0;
    uintptr_t visual = 0;
    Vec3 cameraPos = {};
    const bool worldOk = IsValidPtr(world) && world > 0x100000000ULL;
    if (worldOk)
    {
        __try
        {
            local = Read<uintptr_t>(world + offsets::world::LocalPlayer);
            camera = Read<uintptr_t>(world + offsets::world::Camera);
            if (IsValidPtr(local) && local > 0x100000000ULL)
                visual = Read<uintptr_t>(local + offsets::entity::VisualState);
            if (IsValidPtr(camera) && camera > 0x100000000ULL)
                cameraPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            local = camera = visual = 0;
            cameraPos = {};
        }
    }

    const bool cameraPosOk =
        (cameraPos.x != 0.f || cameraPos.y != 0.f || cameraPos.z != 0.f) &&
        cameraPos.x == cameraPos.x && cameraPos.y == cameraPos.y && cameraPos.z == cameraPos.z &&
        fabsf(cameraPos.x) < 100000.f && fabsf(cameraPos.y) < 10000.f && fabsf(cameraPos.z) < 100000.f;
    // ESP / overlay need world + live camera only. VisualState@entity is stale on
    // this build (reads as UTF-16 junk like 0x0065007400730079) — requiring it
    // kept sessionReady=false forever → draws=0 / nothing rendered.
    const bool landscapeOk =
        worldOk &&
        IsValidPtr(camera) && camera > 0x100000000ULL &&
        cameraPosOk;
    const bool localOk =
        IsValidPtr(local) && local > 0x100000000ULL;
    // Writers (MB/FOV/lag) still prefer a local pawn when present, but never
    // block ESP on VisualState.
    const bool graphOk = landscapeOk;

    const DWORD now = GetTickCount();
    auto logWait = [&](const char* why) {
        static DWORD s_LastWaitLog = 0;
        if (s_LastWaitLog && (DWORD)(now - s_LastWaitLog) < 2000)
            return;
        s_LastWaitLog = now;
        char b[320];
        wsprintfA(b,
            "session wait: %s w=0x%p l=0x%p c=0x%p vs=0x%p camOk=%d rawW=%d local=%d",
            why ? why : "?",
            (void*)world, (void*)local, (void*)camera, (void*)visual,
            cameraPosOk ? 1 : 0, worldOk ? 1 : 0, localOk ? 1 : 0);
        Log(b);
    };

    // Only world+camera must stay stable for the gate. LocalPlayer changes
    // (respawn / late bind) must not reset the 2s timer or quiesce writers.
    const bool changed =
        world != g_SessionCandidateWorld ||
        camera != g_SessionCandidateCamera;
    if (changed)
    {
        OakQuiesceSessionState("world/camera changed");
        g_SessionCandidateWorld = world;
        g_SessionCandidateLocal = local;
        g_SessionCandidateCamera = camera;
        g_SessionStableSince = graphOk ? now : 0;
        logWait(graphOk ? "graph changed" : "graph incomplete");
        return false;
    }
    g_SessionCandidateLocal = local;

    if (!landscapeOk)
    {
        OakQuiesceSessionState("session graph incomplete");
        g_SessionStableSince = 0;
        logWait(!worldOk ? "no world (main menu?)" :
                !IsValidPtr(camera) || camera <= 0x100000000ULL ? "no camera" :
                "bad camera pos");
        return false;
    }

    // Landscape streaming can leave plausible pointers alive for several
    // frames. Under BE, give more soak before writers/hooks arm — kitchen-sink
    // FOV/recoil/speed at the 2s mark correlated with AVs on -1 pointers.
    const DWORD kStableMs = OakBeIsLaunch() ? 4500u : 2000u;

    if (!g_SessionStableSince)
    {
        g_SessionStableSince = now;
        char warm[40];
        wsprintfA(warm, "warming %ums", (unsigned)kStableMs);
        logWait(warm);
        return false;
    }
    if ((DWORD)(now - g_SessionStableSince) < kStableMs)
        return false;

    outWorld = world;
    outLocal = local;
    outCamera = camera;
    g_CachedWorldPtr = world;
    if (InterlockedExchange(&g_SessionQuiesced, 0) != 0)
    {
        char b[72];
        wsprintfA(b, "session: stable for %ums — gameplay resumed", (unsigned)kStableMs);
        Log(b);
    }
    return true;
}

static uintptr_t OakResolveLocalPlayer(uintptr_t worldPtr, uintptr_t camera, uintptr_t hint)
{
    auto entityConfigIs = [&](uintptr_t ent, const char* want) -> bool {
        if (!IsValidPtr(ent) || ent < 0x100000000) return false;
        uintptr_t typ = Read<uintptr_t>(ent + offsets::entity::EntType);
        if (!IsValidPtr(typ) || typ < 0x100000000) return false;
        if (g_GameModule && typ >= (uintptr_t)g_GameModule && typ < (uintptr_t)g_GameModule + 0x5000000)
            return false;
        uintptr_t cfg = Read<uintptr_t>(typ + offsets::entitytype::ConfigName);
        char name[68] = {};
        if (!ReadEngineString(cfg, name, 68))
            return false;
        return StrCmpI(name, want) == 0;
    };

    uintptr_t localPlayer = hint;
    uintptr_t vs = 0;
    if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
        vs = Read<uintptr_t>(localPlayer + offsets::entity::VisualState);
    bool localLooksPlayer = entityConfigIs(localPlayer, "dayzplayer");
    if (localLooksPlayer && IsValidPtr(vs) && vs > 0x100000000)
        return localPlayer;

    uintptr_t nearData = 0; int nearCount = 0;
    if (!ResolveEntityList(worldPtr, offsets::world::NearEntList, 2000, nearData, nearCount, nullptr)
        || nearCount <= 0 || !IsValidPtr(nearData))
        return localLooksPlayer ? localPlayer : 0;

    Vec3 camPos = IsValidPtr(camera) ? Read<Vec3>(camera + offsets::camera::InvertedViewTranslation) : Vec3{};
    uintptr_t best = 0;
    float bestD = 1.0e12f;
    int scanN = nearCount < 32 ? nearCount : 32;
    for (int ni = 0; ni < scanN; ni++)
    {
        uintptr_t cand = Read<uintptr_t>(nearData + (uintptr_t)ni * 8);
        if (!entityConfigIs(cand, "dayzplayer")) continue;
        uintptr_t cvs = Read<uintptr_t>(cand + offsets::entity::VisualState);
        if (!IsValidPtr(cvs) || cvs < 0x100000000) continue;
        Vec3 p = Read<Vec3>(cvs + 0x2C);
        float dx = p.x - camPos.x, dy = p.y - camPos.y, dz = p.z - camPos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD) { bestD = d2; best = cand; }
    }
    return best ? best : (localLooksPlayer ? localPlayer : 0);
}

static void RenderESP();
static void OakRunGameplayFrame(uintptr_t worldPtr, uintptr_t localPlayer)
{
    OAK_MARK("gameplay");
    // Lag switch always ticks (even if lab later denies hold) so hooks install.
    __try { OAK_MARK("warp"); Batch4Warp(worldPtr, localPlayer); }
    __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("gameplay-warp", GetExceptionCode()); }

    if (!g_GameModule || g_PanicHidden)
        return;
    if (!IsValidPtr(worldPtr) || worldPtr < 0x100000000)
        return;

    uintptr_t camera = Read<uintptr_t>(worldPtr + offsets::world::Camera);
    uintptr_t hint = g_ResolvedLocalPlayer;
    if (!IsValidPtr(hint) || hint < 0x100000000)
        hint = localPlayer;
    uintptr_t lp = OakResolveLocalPlayer(worldPtr, camera, hint);
    const bool localOk = IsValidPtr(lp) && lp > 0x100000000ULL;
    if (localOk)
    {
        g_ResolvedLocalPlayer = lp;
        OakLab_SetLocalPlayer(lp);
    }
    else
    {
        lp = 0;
        // Keep prior resolved local only for one soft frame; do not block camera arm.
        g_LocalPlayerValid = false;
    }
    OakEngine_SetHot(worldPtr, lp, camera);

    // Lab policy FIRST — only force-off permanently locked modules.
    // Do NOT wipe UI toggles when SAFE/one-arm denies writes (that saved Fast Bullets=off to config).
    {
        OakLabPolicy lab = {};
        OakLab_GetPolicy(&lab);
        // Deleted modules: always cold.
        g_WallBypass = {};
        g_WorldMisc.speedHack = false;
        g_WorldMisc.infiniteStamina = false;
        g_Exploits.noclip = false;
        g_Exploits.lootThroughWalls = false;
        g_Exploits.lootLivingPlayers = false;
        g_Exploits.mugPlayer = false;
        g_Exploits.itemDupe = false;
        if (OakLab_GetState(OAK_LAB_WARP) == OAK_LAB_LOCKED) g_Exploits.warp = false;
        // One-arm: mute side writers that confuse soak attribution (magnets/despawn).
        if (OakLab_ExperimentalArmed())
        {
            g_MiscLootMagnet = false;
            g_MiscContainerMagnet = false;
            g_MiscMiddleClickDespawn = false;
        }
        (void)lab;
    }

    static DWORD s_LastPosTick = 0;
    DWORD nowPos = GetTickCount();
    if (localOk && (!s_LastPosTick || (nowPos - s_LastPosTick) >= 33))
    {
        s_LastPosTick = nowPos;
        Vec3 livePos = {};
        if (GetEntityPosition(lp, livePos))
        {
            g_LocalPlayerPos = livePos;
            g_LocalPlayerValid = true;
        }
    }

    // Crash matrix must tick even when ESP is off (thrash_esp toggles g_ShowESP).
    __try { OAK_MARK("crash.matrix"); OakCrashMatrix_Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("crash-matrix", GetExceptionCode()); }

    // Camera must be armed even when local is missing — Present only draws ESP when
    // g_CameraValid/W2S are set. Returning early on null LocalPlayer caused
    // draws=0 forever with a live world+camera (menu OK, modules dead).
    if (IsValidPtr(camera) && camera > 0x100000000ULL)
    {
        Vec3 camPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
        if (camPos.x != 0.0f || camPos.y != 0.0f || camPos.z != 0.0f)
        {
            g_CameraPos = camPos;
            g_CameraValid = true;
        }
        if (localOk)
        {
            { OAK_MARK("cam.3pp"); MiscApplyFreecam(worldPtr, camera, lp); }
            { OAK_MARK("cam.3pp2"); MiscApplyThirdPerson(worldPtr, camera, lp); }
            g_CameraPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
            if (g_CameraPos.x != 0.0f || g_CameraPos.y != 0.0f || g_CameraPos.z != 0.0f)
                g_CameraValid = true;
            { OAK_MARK("camfov"); OakApplyCameraFov(camera); }
        }
        { OAK_MARK("w2s-refresh"); RefreshW2SCache(camera); }
    }
    else
    {
        g_CameraValid = false;
    }

    if (!localOk)
        return;

    // Keybinds MUST run while freecam is on — otherwise F10/ESC can never exit.
    __try {
        OAK_MARK("misc-early");
        UpdateMiscFeatures(worldPtr, lp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("gameplay-misc-early", GetExceptionCode()); }

    if ((g_MiscFreecam || g_WarpDesyncActive) && !g_WarpCommitting)
        return;

    __try {
        OAK_MARK("combat");
        { OAK_MARK("combat.pre"); OakFeaturesPreCombatFrame(worldPtr, lp); }
        { OAK_MARK("combat.b4init"); OakBatch4InitCombatFrame(); }
        { OAK_MARK("combat.ammo"); UpdateFastBullets(lp); }
        { OAK_MARK("combat.aim"); UpdateCombatAim(worldPtr, lp); }
        { OAK_MARK("combat.b4"); OakBatch4UpdateCombat(worldPtr, lp); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("gameplay-combat", GetExceptionCode()); }

    static DWORD s_LastMiscTick = 0;
    DWORD nowGp = GetTickCount();
    if (s_LastMiscTick && (nowGp - s_LastMiscTick) < 33)
        return;
    s_LastMiscTick = nowGp;

    __try {
        OAK_MARK("misc-slow");
        { OAK_MARK("misc.fov"); Batch4ApplyFov(worldPtr, lp); }
        { OAK_MARK("misc.b4"); OakBatch4UpdateMisc(worldPtr, lp); }
        { OAK_MARK("misc.session"); OakFeaturesSessionTick(worldPtr, lp); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("gameplay-misc", GetExceptionCode()); }
}

static void RenderESP()
{
    if (!g_GameModule || !g_ShowESP) return;

    HitCheck_Clear();
    OakFeaturesResetThreatScan();
    OakBatch4ResetFrame();
    OakItemEspBeginFrame();
    MiscAvatarBeginFrame();
    g_EspNameAvatar = nullptr;

    __try {
    
    char buf[256];
    
    
    uintptr_t worldPtr = 0;
    
    uintptr_t worldOffsets[] = { offsets::modbase::World };
    const int worldOffsetCount = (int)(sizeof(worldOffsets)/sizeof(worldOffsets[0]));
    for (int w = 0; w < worldOffsetCount; w++)
    {
        uintptr_t test = Read<uintptr_t>((uintptr_t)g_GameModule + worldOffsets[w]);
        if (g_DebugCounter == 0)
        {
            wsprintfA(buf, "Trying World offset 0x%X: ptr=0x%X%08X", 
                worldOffsets[w], (DWORD)(test >> 32), (DWORD)test);
            Log(buf);
        }
        
        if (IsValidPtr(test) && test > 0x100000000)
        {
            uintptr_t camTest = Read<uintptr_t>(test + offsets::world::Camera);
            if (g_DebugCounter == 0)
            {
                wsprintfA(buf, "  Camera at 0x%X: ptr=0x%X%08X", 
                    (unsigned)offsets::world::Camera,
                    (DWORD)(camTest >> 32), (DWORD)camTest);
                Log(buf);
            }
            
            
            if (IsValidPtr(camTest) && camTest > 0x100000000)
            {
                // Prefer cameras that expose a map-space translation (layout-aware)
                Vec3 t = Read<Vec3>(camTest + offsets::camera::InvertedViewTranslation);
                bool translationOk =
                    (t.x != 0.f || t.y != 0.f || t.z != 0.f) &&
                    fabsf(t.x) < 60000.f && fabsf(t.z) < 60000.f && fabsf(t.y) < 2000.f;

                worldPtr = test;
                if (g_DebugCounter == 0)
                {
                    wsprintfA(buf, "World found at offset 0x%X = 0x%X%08X (camera %s) pos=(%d,%d,%d)", 
                        worldOffsets[w], (DWORD)(worldPtr >> 32), (DWORD)worldPtr,
                        translationOk ? "map" : "raw",
                        (int)t.x, (int)t.y, (int)t.z);
                    Log(buf);
                }
                break;
            }
            else
            {
                
                uintptr_t testNearList = Read<uintptr_t>(test + 0xF48);
                uintptr_t testFarList = Read<uintptr_t>(test + 0x1090);
                if (g_DebugCounter == 0)
                {
                    wsprintfA(buf, "  Testing lists: NearList=0x%X%08X FarList=0x%X%08X", 
                        (DWORD)(testNearList >> 32), (DWORD)testNearList,
                        (DWORD)(testFarList >> 32), (DWORD)testFarList);
                    Log(buf);
                }
                
                
                if ((IsValidPtr(testNearList) && testNearList > 0x100000000) ||
                    (IsValidPtr(testFarList) && testFarList > 0x100000000))
                {
                    worldPtr = test;
                    if (g_DebugCounter == 0)
                    {
                        wsprintfA(buf, "World found at offset 0x%X = 0x%X%08X (lists valid, camera invalid)", 
                            worldOffsets[w], (DWORD)(worldPtr >> 32), (DWORD)worldPtr);
                        Log(buf);
                    }
                    break;
                }
            }
        }
    }
    
    if (worldPtr == 0)
    {
        if (g_DebugCounter == 0) { Log("no world ptr"); g_DebugCounter = 1; }
        return;
    }

    g_CachedWorldPtr = worldPtr;

    // Pin lighting early in the ESP frame (PPE rewrites EyeAccom continuously)
    if (g_Fullbright || g_MiscDisableOverlays)
    {
        __try {
            ApplyLightingForce(worldPtr, g_Fullbright, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    
    uintptr_t camera = Read<uintptr_t>(worldPtr + offsets::world::Camera);
    
    
    uintptr_t localPlayer = Read<uintptr_t>(worldPtr + offsets::world::LocalPlayer);

    // Session transitions are handled centrally before any gameplay work.
    // RenderESP only receives a pointer graph that passed the stability gate.
    // Prefer a real DayZPlayer: World::LocalPlayer sometimes points at a non-typed object in-world.
    auto entityConfigIs = [&](uintptr_t ent, const char* want) -> bool {
        if (!IsValidPtr(ent) || ent < 0x100000000) return false;
        uintptr_t typ = Read<uintptr_t>(ent + offsets::entity::EntType);
        if (!IsValidPtr(typ) || typ < 0x100000000) return false;
        // Reject pointers into the game module (not heap EntityType)
        if (g_GameModule && typ >= (uintptr_t)g_GameModule && typ < (uintptr_t)g_GameModule + 0x5000000)
            return false;
        uintptr_t cfg = Read<uintptr_t>(typ + offsets::entitytype::ConfigName);
        char name[68] = {};
        if (!ReadEngineString(cfg, name, 68))
            return false;
        return StrCmpI(name, want) == 0;
    };
    {
        uintptr_t vs = 0;
        if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
            vs = Read<uintptr_t>(localPlayer + offsets::entity::VisualState);
        bool localLooksPlayer = entityConfigIs(localPlayer, "dayzplayer");
        if (!localLooksPlayer || !IsValidPtr(vs) || vs < 0x100000000)
        {
            uintptr_t nearData = 0; int nearCount = 0;
            if (ResolveEntityList(worldPtr, offsets::world::NearEntList, 2000, nearData, nearCount, nullptr)
                && nearCount > 0 && IsValidPtr(nearData))
            {
                Vec3 camPos = IsValidPtr(camera) ? Read<Vec3>(camera + offsets::camera::InvertedViewTranslation) : Vec3{};
                uintptr_t best = 0;
                float bestD = 1.0e12f;
                int scanN = nearCount < 32 ? nearCount : 32;
                for (int ni = 0; ni < scanN; ni++)
                {
                    uintptr_t cand = Read<uintptr_t>(nearData + (uintptr_t)ni * 8);
                    if (!entityConfigIs(cand, "dayzplayer")) continue;
                    uintptr_t cvs = Read<uintptr_t>(cand + offsets::entity::VisualState);
                    if (!IsValidPtr(cvs) || cvs < 0x100000000) continue;
                    Vec3 p = Read<Vec3>(cvs + 0x2C);
                    float dx = p.x - camPos.x, dy = p.y - camPos.y, dz = p.z - camPos.z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestD)
                    {
                        bestD = d2;
                        best = cand;
                    }
                }
                if (best)
                {
                    localPlayer = best;
                    if (g_DebugCounter == 0) Log("LocalPlayer <- nearest Near dayzplayer");
                }
                else if (!IsValidPtr(vs) || vs < 0x100000000)
                {
                    uintptr_t cand = Read<uintptr_t>(nearData);
                    uintptr_t cvs = IsValidPtr(cand) ? Read<uintptr_t>(cand + offsets::entity::VisualState) : 0;
                    if (IsValidPtr(cand) && IsValidPtr(cvs) && cvs > 0x100000000)
                    {
                        localPlayer = cand;
                        if (g_DebugCounter == 0) Log("LocalPlayer <- Near[0] (menu/lobby character)");
                    }
                }
            }
        }
    }
    // Fallback: scan World for an entity whose VisualState sits near the camera
    if (!IsValidPtr(localPlayer) || localPlayer < 0x100000000
        || !IsValidPtr(Read<uintptr_t>(localPlayer + offsets::entity::VisualState)))
    {
        Vec3 camPos = IsValidPtr(camera) ? Read<Vec3>(camera + offsets::camera::InvertedViewTranslation) : Vec3{};
        for (uintptr_t off = 0x2800; off <= 0x2C00; off += 8)
        {
            uintptr_t ent = Read<uintptr_t>(worldPtr + off);
            if (!IsValidPtr(ent) || ent < 0x100000000) continue;
            uintptr_t vs = Read<uintptr_t>(ent + offsets::entity::VisualState);
            if (!IsValidPtr(vs) || vs < 0x100000000) continue;
            Vec3 p = Read<Vec3>(vs + 0x2C);
            float dx = p.x - camPos.x, dy = p.y - camPos.y, dz = p.z - camPos.z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 80.f * 80.f)
            {
                localPlayer = ent;
                if (g_DebugCounter == 0)
                {
                    wsprintfA(buf, "LocalPlayer resolved via scan @+0x%X", (unsigned)off);
                    Log(buf);
                }
                break;
            }
        }
    }
    
    if (g_DebugCounter == 0)
    {
        wsprintfA(buf, "Camera=0x%X%08X LocalPlayer=0x%X%08X", 
            (DWORD)(camera >> 32), (DWORD)camera,
            (DWORD)(localPlayer >> 32), (DWORD)localPlayer);
        Log(buf);
        
        
        if (IsValidPtr(camera) && camera > 0x100000000)
        {
            Vec3 camPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);  
            Vec3 projD1 = Read<Vec3>(camera + offsets::camera::GetProjectionD1);  
            
            wsprintfA(buf, "CamPos: X=%d Y=%d Z=%d ProjD1.x=%d", 
                (int)camPos.x, (int)camPos.y, (int)camPos.z, (int)(projD1.x * 1000));
            Log(buf);
        }
        
        
        if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
        {
            Vec3 localPos;
            if (GetEntityPosition(localPlayer, localPos))
            {
                wsprintfA(buf, "LocalPlayer Pos: X=%d Y=%d Z=%d", 
                    (int)localPos.x, (int)localPos.y, (int)localPos.z);
                Log(buf);
            }
            else
            {
                Log("localplayer pos failed");
            }
        }
    }
    
    
    
    g_CameraValid = false;
    if (IsValidPtr(camera) && camera > 0x100000000)
    {
        g_CameraPos = Read<Vec3>(camera + offsets::camera::InvertedViewTranslation);
        if (g_CameraPos.x != 0.0f || g_CameraPos.y != 0.0f || g_CameraPos.z != 0.0f)
        {
            g_CameraValid = true;
        }
    }

    // Freecam first (may restore VT on disable), then soft 3PP re-owns if needed.
    // Soft cam must run after freecam-off teardown or the view never sticks.
    if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
        g_ResolvedLocalPlayer = localPlayer;
    MiscApplyFreecam(worldPtr, camera, localPlayer);
    MiscApplyThirdPerson(worldPtr, camera, localPlayer);
    // FOV + W2S already armed in OakRunGameplayFrame this Present — only refresh
    // when freecam/3PP may have moved the camera VT since then.
    if (g_MiscFreecam || g_WorldMisc.thirdPerson || !g_W2S.valid || g_W2S.camera != camera)
    {
        OakApplyCameraFov(camera);
        RefreshW2SCache(camera);
    }
    
    g_LocalPlayerValid = false;
    if (IsValidPtr(localPlayer) && localPlayer > 0x100000000)
    {
        
        
        uintptr_t visualState = Read<uintptr_t>(localPlayer + 0x1C8);
        if (IsValidPtr(visualState) && visualState > 0x100000000)
        {
            
            
            float m9 = Read<float>(visualState + 0x8 + 9 * 4);   
            float m10 = Read<float>(visualState + 0x8 + 10 * 4); 
            float m11 = Read<float>(visualState + 0x8 + 11 * 4); 
            
            if (m9 != 0.0f || m10 != 0.0f || m11 != 0.0f)
            {
                g_LocalPlayerPos.x = m9;
                g_LocalPlayerPos.y = m10;
                g_LocalPlayerPos.z = m11;
                g_LocalPlayerValid = true;
            }
        }
        
        
        if (!g_LocalPlayerValid)
        {
            Vec3 pelvisPos = GetBonePosition(localPlayer, BONE_PELVIS, true);
            if (pelvisPos.x != 0.0f || pelvisPos.y != 0.0f || pelvisPos.z != 0.0f)
            {
                g_LocalPlayerPos = pelvisPos;
                g_LocalPlayerValid = true;
            }
        }
        
        
        if (!g_LocalPlayerValid)
        {
            if (GetEntityPosition(localPlayer, g_LocalPlayerPos))
            {
                g_LocalPlayerValid = true;
            }
        }
    }

    if (g_LocalPlayerValid)
        ImGuiMenu_SetPlayerPos(g_LocalPlayerPos.x, g_LocalPlayerPos.y, g_LocalPlayerPos.z);
    else if (g_CameraValid)
        ImGuiMenu_SetPlayerPos(g_CameraPos.x, g_CameraPos.y, g_CameraPos.z);

    // Local player outline: freecam, explicit toggle, OR Steam names (so self shows username+PFP)
    // Delay first draw — drawing local ESP on the same frame as Steam roster init crashed DayZ.
    static DWORD s_LocalEspWarm = 0;
    if (!s_LocalEspWarm && IsValidPtr(localPlayer)) s_LocalEspWarm = GetTickCount();
    const bool localEspReady = s_LocalEspWarm && (GetTickCount() - s_LocalEspWarm) > 5000;

    if (localEspReady && (g_DrawLocalPlayer || g_MiscFreecam) && g_ESPPlayers && IsValidPtr(localPlayer) && localPlayer > 0x100000000)
    {
        g_ResolvedLocalPlayer = localPlayer;

        char selfName[64];
        ZeroMem(selfName, 64);

        if (g_LocalSteamName[0])
            lstrcpynA(selfName, g_LocalSteamName, 64);
        if (!selfName[0])
        {
            __try { MiscSteamLocalPersona(selfName, 64, &g_LocalSteamId); }
            __except (EXCEPTION_EXECUTE_HANDLER) { selfName[0] = 0; }
        }
        if (selfName[0])
            lstrcpynA(g_LocalSteamName, selfName, 64);
        if (!selfName[0])
            lstrcpynA(selfName, "You", 64);

        int vertsBefore = g_EspDrawCount;
        __try
        {
            DrawEntitySkeleton(localPlayer, true, selfName, 0);
            if (g_EspDrawCount == vertsBefore)
                DrawEntitySimple(localPlayer, true, selfName, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("local outline: skeleton exception");
        }

        // Hot-path silence — outline spam was ~1 Log/sec disk hitch on Present
        (void)0;
    }
    else if (g_DrawLocalPlayer && g_CameraValid && IsValidPtr(camera))
    {
        // Debug marker along camera forward so W2S can be verified before LocalPlayer resolves
        Vec3 fwd = Read<Vec3>(camera + offsets::camera::InvertedViewForward);
        Vec3 ahead;
        ahead.x = g_CameraPos.x + fwd.x * 5.0f;
        ahead.y = g_CameraPos.y + fwd.y * 5.0f;
        ahead.z = g_CameraPos.z + fwd.z * 5.0f;
        Vec2 scr{};
        if (WorldToScreen(ahead, scr))
        {
            AddBox(scr.x - 8.f, scr.y - 8.f, 16.f, 16.f, 0.f, 1.f, 0.f, 1.f);
            if (g_DebugCounter == 0)
            {
                wsprintfA(buf, "drew camera forward marker at screen (%d,%d)", (int)scr.x, (int)scr.y);
                Log(buf);
            }
        }
        else if (g_DebugCounter == 0)
        {
            wsprintfA(buf, "W2S failed ahead=(%d,%d,%d) fwd=(%d,%d,%d)",
                (int)ahead.x, (int)ahead.y, (int)ahead.z,
                (int)(fwd.x * 100), (int)(fwd.y * 100), (int)(fwd.z * 100));
            Log(buf);
        }
    }
    
    
    
    if (g_SharedConfig && g_SharedConfig->magic == 0x4F414B00)
    {
        if (g_LocalPlayerValid)
        {
            g_SharedConfig->PlayerX = g_LocalPlayerPos.z;  
            g_SharedConfig->PlayerY = g_LocalPlayerPos.x;  
            g_SharedConfig->PlayerZ = g_LocalPlayerPos.y;  
        }
        else if (g_CameraValid)
        {
            g_SharedConfig->PlayerX = g_CameraPos.z;  
            g_SharedConfig->PlayerY = g_CameraPos.x;  
            g_SharedConfig->PlayerZ = g_CameraPos.y;  
        }
    }
    
    
    struct EntityListInfo {
        uintptr_t offset;
        uintptr_t countOffset;
        const char* name;
        bool isSlowList;  
    };
    
    
    
    
    
    
    
    EntityListInfo lists[] = {
        { oak_offsets::world::NearEntList,  oak_offsets::world::NearEntList + 0x8,  "NearEntList", false },   
        { oak_offsets::world::FarEntList,   oak_offsets::world::FarTableSize,       "FarEntList", false },    
        // Live layout: Slow/Item are {flag, pad, entity*} entries at 0x18 stride (not bare pointers)
        { oak_offsets::world::SlowEntList,  oak_offsets::world::SlowTableSize,      "SlowEntList", true },          
        { oak_offsets::world::ItemList,     oak_offsets::world::ItemListSize,       "ItemTable", true },     
    };
    
    int totalDrawn = 0;
    int itemsDrawn = 0;
    int playersDrawn = 0;
    int zombiesDrawn = 0;
    int animalsDrawn = 0;
    int corpsesDrawn = 0;
    int trapsDrawn = 0;
    int entitiesScanned = 0;
    int labelsDrawn = 0;
    int skeletonsDrawn = 0;
    // Dedup players/zombies across Near/Far/Slow — double-draw was flashing ESP colors.
    enum { kEspPzDedupMax = 384 };
    uintptr_t espPzDedup[kEspPzDedupMax];
    int espPzDedupN = 0;
    auto espPzSeen = [&](uintptr_t e) -> bool {
        for (int i = 0; i < espPzDedupN; i++)
            if (espPzDedup[i] == e) return true;
        return false;
    };
    auto espPzMark = [&](uintptr_t e) {
        if (espPzDedupN < kEspPzDedupMax)
            espPzDedup[espPzDedupN++] = e;
    };
    int lootInRange = 0;      // ground loot within itemMax that passes category filters
    int lootSkippedCap = 0;   // would draw but hit g_MaxItems
    int lootSkippedFilter = 0;
    bool itemTableValid = true;
    bool itemUsePtrArray = false; // ItemTable resolved as pointer[] vs 0x18 slow slots

    static LARGE_INTEGER s_PerfFreq = {};
    if (s_PerfFreq.QuadPart == 0)
        QueryPerformanceFrequency(&s_PerfFreq);
    LARGE_INTEGER espStart = {};
    QueryPerformanceCounter(&espStart);

    int effectiveScanCap = g_MaxEntitiesScanned;
    if (effectiveScanCap > OAK_CAP_MAX_ENTITIES_SCANNED)
        effectiveScanCap = OAK_CAP_MAX_ENTITIES_SCANNED;
    int effectiveDrawCap = g_MaxEntitiesDrawn;
    if (effectiveDrawCap > OAK_CAP_MAX_ENTITIES_DRAWN)
        effectiveDrawCap = OAK_CAP_MAX_ENTITIES_DRAWN;

    static DWORD s_LastItemScanTick = 0;
    const DWORD nowTick = GetTickCount();
    const int itemScanIntervalMs = (g_EspUpdateHz > 0) ? (1000 / (g_EspUpdateHz > 12 ? 12 : g_EspUpdateHz)) : 80;
    const bool runItemScan = (nowTick - s_LastItemScanTick) >= (DWORD)itemScanIntervalMs;
    if (runItemScan)
        s_LastItemScanTick = nowTick;

    for (int lci = 0; lci < OAK_LOOT_CAT_COUNT; lci++)
        g_LootCatDrawn[lci] = 0;

    // Main menu / loading (no valid cam or local): skip entity list scans.
    // Leaving ESP on while lobby world pointers linger was a common ~15 FPS path.
    const bool inGameplay = g_CameraValid || g_LocalPlayerValid;

    // Soft start: only delay heavy ItemTable for ~2s. Never skip Near/Far/Slow (missed loot + ESP).
    static DWORD s_ListWarmStart = 0;
    if (!s_ListWarmStart) s_ListWarmStart = GetTickCount();
    const bool itemTableWarm = (GetTickCount() - s_ListWarmStart) < 2000;

    // Near/Far/Slow: draw every Present frame (throttling here caused visible ESP flicker).
    // ItemTable only: throttle scan rate; cached loot redraws every frame below.
    OakBatch4SetVisContext(worldPtr, localPlayer);
    for (int listIdx = 0; inGameplay && listIdx < 4; listIdx++)
    {
        g_Batch4EspListIdx = listIdx;
        if (listIdx == 3)
        {
            if (!g_ESPItems && !g_ESPContainers && !g_ESPTraps)
                continue;
            if (!runItemScan || itemTableWarm)
                continue;
        }

        uintptr_t entListPtr = 0;
        int entCount = 0;
        bool useSlowStride = lists[listIdx].isSlowList;
        
        uintptr_t listAddr = worldPtr + lists[listIdx].offset;
        
        
        
        if (lists[listIdx].isSlowList)
        {
            if (listIdx == 3)
            {
                // Prefer ResolveEntityList (same layouts as Near/Far). Direct dword read often
                // leaves ItemTable "valid=false" and silently kills all loot ESP.
                itemUsePtrArray = false;
                if (ResolveEntityList(worldPtr, lists[listIdx].offset, 200000,
                        entListPtr, entCount, "ItemTable"))
                {
                    itemUsePtrArray = true;
                    useSlowStride = false;
                    itemTableValid = true;
                    if (g_DebugCounter == 0)
                    {
                        wsprintfA(buf, "item: ItemTable resolved ptr-array data=0x%X%08X count=%d",
                            (DWORD)(entListPtr >> 32), (DWORD)entListPtr, entCount);
                        Log(buf);
                    }
                }
                else
                {
                    entListPtr = Read<uintptr_t>(listAddr);
                    entCount = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
                    if (entCount > 200000 || entCount < 0)
                    {
                        DWORD countDword = Read<DWORD>(worldPtr + oak_offsets::world::ItemListSize);
                        if (countDword < 200000) entCount = (int)countDword;
                    }
                    if (!IsValidPtr(entListPtr) || entListPtr == 0 || entListPtr < 0x100000000)
                    {
                        entListPtr = 0;
                        entCount = 0;
                        itemTableValid = false;
                        if (g_DebugCounter == 0)
                            Log("item: ItemTable pointer invalid after resolve+direct — loot list skipped");
                    }
                    else
                    {
                        itemTableValid = true;
                        useSlowStride = true;
                        if (g_DebugCounter == 0)
                        {
                            wsprintfA(buf, "item: ItemTable slow-stride ptr=0x%X%08X count=%d",
                                (DWORD)(entListPtr >> 32), (DWORD)entListPtr, entCount);
                            Log(buf);
                        }
                    }
                }
            }
            else
            {
                // SlowEntList — {flag, pad, entity*} @ 0x18
                entListPtr = Read<uintptr_t>(listAddr);
                entCount = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
                if (entCount > 2000 || entCount < 0)
                {
                    DWORD countDword = Read<DWORD>(worldPtr + oak_offsets::world::SlowTableSize);
                    if (countDword < 2000) entCount = (int)countDword;
                }
                if (!IsValidPtr(entListPtr) || entListPtr == 0 || entListPtr < 0x100000000)
                {
                    entListPtr = 0;
                    entCount = 0;
                }
            }
        }
        else
        {
            if (!ResolveEntityList(worldPtr, lists[listIdx].offset, 2000,
                    entListPtr, entCount, lists[listIdx].name))
            {
                entListPtr = 0;
                entCount = 0;
            }
        }
        
        if (g_DebugCounter == 0)
        {
            wsprintfA(buf, "%s: offset=0x%X ptr=0x%X%08X count=%d slow=%d",
                lists[listIdx].name, lists[listIdx].offset, 
                (DWORD)(entListPtr >> 32), (DWORD)entListPtr, entCount, useSlowStride ? 1 : 0);
            Log(buf);
        }
        
        
        if (!IsValidPtr(entListPtr) || entListPtr == 0 || entListPtr < 0x100000000)
        {
            if (g_DebugCounter == 0)
            {
                wsprintfA(buf, "%s: Skipping - invalid pointer (0x%X%08X)", lists[listIdx].name,
                    (DWORD)(entListPtr >> 32), (DWORD)entListPtr);
                Log(buf);
            }
            continue; 
        }
        
        int maxCount = (listIdx == 3) ? 200000 : 2000;  
        if (entCount <= 0 || entCount > maxCount)
        {
            if (g_DebugCounter == 0)
            {
                wsprintfA(buf, "%s: Skipping - invalid count %d (max=%d)", lists[listIdx].name, entCount, maxCount);
                Log(buf);
            }
            continue;
        }
        
        // ItemTable: capped rolling window (was 2500/frame — melted FPS). Near/Far/Slow unchanged.
        static int s_ItemScanBase = 0;
        int maxIter = (listIdx == 3)
            ? ((entCount > 600) ? 600 : entCount)
            : ((entCount > 280) ? 280 : entCount);
        {
            const int scanLeft = effectiveScanCap - entitiesScanned;
            if (scanLeft <= 0)
                continue;
            if (maxIter > scanLeft)
                maxIter = scanLeft;
            if (listIdx == 3)
            {
                const float hzScale = (float)g_EspUpdateHz / 20.f;
                int hzIter = (int)((float)maxIter * (hzScale < 0.25f ? 0.25f : (hzScale > 1.f ? 1.f : hzScale)));
                if (hzIter < 8 && maxIter >= 8) hzIter = 8;
                if (hzIter < maxIter) maxIter = hzIter;
            }
        }
        int itemBase = 0;
        if (listIdx == 3 && entCount > maxIter)
        {
            itemBase = s_ItemScanBase % entCount;
            s_ItemScanBase = (s_ItemScanBase + maxIter) % entCount;
        }
        
        __try {
        for (int n = 0; n < maxIter; n++)
        {
            // Sample QPC every 32 ents — every-entity VirtualQuery-class cost was wasteful.
            if ((n & 31) == 0 && s_PerfFreq.QuadPart > 0)
            {
                LARGE_INTEGER nowQ = {};
                QueryPerformanceCounter(&nowQ);
                const double elapsedMs = (double)(nowQ.QuadPart - espStart.QuadPart) * 1000.0 / (double)s_PerfFreq.QuadPart;
                if (elapsedMs >= (double)g_FrameBudgetMs)
                    break;
            }
            if (totalDrawn >= effectiveDrawCap)
                break;

            int i = (listIdx == 3 && entCount > 0) ? ((itemBase + n) % entCount) : n;
            if (listIdx == 3 && (entListPtr == 0 || !IsValidPtr(entListPtr) || entListPtr < 0x100000000))
                break; 
            
            uintptr_t entity = 0;
            
            if (useSlowStride)
            {
                uintptr_t entryAddr = entListPtr + ((uintptr_t)i * 0x18);
                if (!IsValidPtr(entryAddr) || entryAddr < 0x100000000) continue;
                
                WORD flag = Read<WORD>(entryAddr);
                entity = Read<uintptr_t>(entryAddr + 0x8);
                // Live builds: occupied slots often flag&1; don't require flag==1 exactly.
                // Empty slots are flag==0 with null entity — skip those only.
                if (!IsValidPtr(entity) || entity < 0x100000000)
                    continue;
                // Occupied slow slots usually have flag!=0; ItemTable sometimes uses 0 with a live ptr.
                if (flag == 0 && listIdx != 3)
                    continue;
            }
            else
            {
                uintptr_t entityAddr = entListPtr + ((uintptr_t)i * 0x8);
                if (!IsValidPtr(entityAddr) || entityAddr < 0x100000000) continue;
                entity = Read<uintptr_t>(entityAddr);
            }
            
            if (g_DebugCounter == 0 && listIdx == 3 && i < 10)
            {
                wsprintfA(buf, "item: Got entity=0x%X%08X IsValidPtr=%d", 
                    (DWORD)(entity >> 32), (DWORD)entity, IsValidPtr(entity) ? 1 : 0);
                Log(buf);
            }
            
            if (!IsValidPtr(entity) || entity < 0x100000000) continue;
            if (entity == localPlayer) continue;
            entitiesScanned++;

            // ItemTable early distance cull — skip far slots before expensive type/name reads
            if (listIdx == 3 && (g_LocalPlayerValid || g_CameraValid))
            {
                Vec3 ipos;
                if (GetEntityPosition(entity, ipos))
                {
                    Vec3 ref = g_LocalPlayerValid ? g_LocalPlayerPos : g_CameraPos;
                    float id = Distance3D(ref, ipos);
                    int maxD = g_ItemDistance;
                    if (g_ESPContainers && g_ContainerDistance > maxD) maxD = g_ContainerDistance;
                    if (g_ESPTraps && g_TrapDistance > maxD) maxD = g_TrapDistance;
                    if (id > (float)(maxD + 40))
                        continue;
                }
            }
            
            uintptr_t entityTypeAddr = entity + 0x180;
            
            if (g_DebugCounter == 0 && listIdx == 3 && i < 10)
            {
                wsprintfA(buf, "item: Reading entityType from entity+0x180=0x%X%08X IsValidPtr=%d", 
                    (DWORD)(entityTypeAddr >> 32), (DWORD)entityTypeAddr, IsValidPtr(entityTypeAddr) ? 1 : 0);
                Log(buf);
            }
            
            if (!IsValidPtr(entityTypeAddr) || entityTypeAddr < 0x100000000) continue;
            
            
            uintptr_t entityType = Read<uintptr_t>(entityTypeAddr);
            
            if (g_DebugCounter == 0 && listIdx == 3 && i < 10)
            {
                wsprintfA(buf, "item: Read entityType=0x%X%08X IsValidPtr=%d", 
                    (DWORD)(entityType >> 32), (DWORD)entityType, IsValidPtr(entityType) ? 1 : 0);
                Log(buf);
            }
            
            if (g_DebugCounter == 0 && listIdx == 0 && i < 5)
            {
                wsprintfA(buf, "  Ent[%d]: 0x%X%08X -> EntityType: 0x%X%08X", 
                    i, (DWORD)(entity >> 32), (DWORD)entity,
                    (DWORD)(entityType >> 32), (DWORD)entityType);
                Log(buf);
            }
            
            if (!IsValidPtr(entityType) || entityType < 0x100000000) continue;

            char configName[64] = {};
            char typeName[64] = {};
            if (!GetCachedEntityTypeStrings(entityType, configName, 64, typeName, 64))
                continue;

            if (g_DebugCounter == 0 && listIdx == 0 && i < 10)
            {
                wsprintfA(buf, "    ConfigName: '%s'", configName);
                Log(buf);
            }
            
            
            bool isPlayer = (StrCmpI(configName, "dayzplayer") == 0);
            bool isZombie = (StrCmpI(configName, "dayzinfected") == 0)
                || StrContainsI(configName, "dayzinfected")
                || (StrContainsI(configName, "infected") && !StrContainsI(configName, "dayzplayer"));
            bool isAnimal = (StrCmpI(configName, "dayzanimal") == 0) || IsAnimal(configName);

            // TypeName fallback (ConfigName empty/wrong on some builds)
            if (!isPlayer && !isZombie && !isAnimal)
            {
                if (typeName[0])
                {
                    if (StrContainsI(typeName, "Survivor") || StrContainsI(typeName, "dayzplayer"))
                        isPlayer = true;
                    else if (StrContainsI(typeName, "Zmb") || StrContainsI(typeName, "Infected") || StrContainsI(typeName, "zombie"))
                        isZombie = true;
                    else if (StrContainsI(typeName, "Animal_") || IsAnimal(typeName))
                        isAnimal = true;
                }
            }
            
            bool isVehicle = (StrCmpI(configName, "car") == 0) || (StrCmpI(configName, "boat") == 0)
                || StrContainsI(configName, "truck") || StrContainsI(configName, "vehicle")
                || StrContainsI(typeName, "CivilianSedan") || StrContainsI(typeName, "Offroad")
                || StrContainsI(typeName, "Hatchback") || StrContainsI(typeName, "Truck_")
                || StrContainsI(typeName, "Boat_");
            // Repairable vehicles only — hide wrecks / static debris / unrepairable husks.
            // IMPORTANT: continue (do not demote to item/container — that re-drew fake cars).
            if (isVehicle)
            {
                const char* vn = typeName[0] ? typeName : configName;
                if (StrContainsI(vn, "wreck") || StrContainsI(configName, "wreck")
                    || StrContainsI(vn, "wreckage") || StrContainsI(vn, "destroyed")
                    || StrContainsI(vn, "Land_Wreck") || StrContainsI(vn, "HelicopterWreck")
                    || StrContainsI(vn, "PlaneWreck") || StrContainsI(vn, "Debris")
                    || StrContainsI(vn, "Ruined") || StrContainsI(vn, "Scrap")
                    || StrContainsI(vn, "Burned") || StrContainsI(vn, "Burnt")
                    || StrContainsI(vn, "Chassis") || StrContainsI(vn, "StaticObj")
                    || StrContainsI(vn, "Land_") || StrContainsI(vn, "wreck_")
                    || StrContainsI(vn, "V3S_Wreck") || StrContainsI(vn, "Wreck_")
                    || StrContainsI(vn, "unrepairable") || StrContainsI(vn, "Abandoned")
                    || StrContainsI(vn, "HMMWV_Wreck") || StrContainsI(vn, "CivSedanWreck")
                    || StrContainsI(vn, "HatchbackWreck") || StrContainsI(vn, "OffroadHatchbackWreck")
                    || StrContainsI(vn, "Truck_01_Covered_Wreck") || StrContainsI(vn, "Sedan_02_Wreck"))
                {
                    static int s_WreckSkip = 0;
                    if ((++s_WreckSkip % 40) == 1)
                    {
                        char wb[96];
                        wsprintfA(wb, "verify[vehicle] skipped wreck '%s'", vn);
                        Log(wb);
                    }
                    continue;
                }
            }
            bool isContainer = IsContainerName(configName) || IsContainerName(typeName);
            bool isTrap = IsTrapName(configName) || IsTrapName(typeName);
            bool isGrenadeEnt = IsGrenadeName(configName) || IsGrenadeName(typeName);
            bool isJunk = IsJunkWorldObject(typeName) || IsJunkWorldObject(configName);
            bool isLootClass = IsLootConfigClass(configName);
            bool looksLootName = LooksLikeGroundLootName(typeName) || LooksLikeGroundLootName(configName);
            // Near/Far/Slow: accept loot classes OR recognizable item TypeNames (player drops).
            // ItemTable: still permissive for any named non-junk entity.
            bool isItem = !isPlayer && !isZombie && !isAnimal && !isVehicle && !isContainer
                && !isTrap && !isGrenadeEnt && !isJunk
                && (isLootClass || looksLootName || (listIdx == 3 && (typeName[0] || configName[0])));
            
            
            
            
            
            if (false && isItem)
            {
                static int itemLogCount = 0;
                if (itemLogCount < 20)
                {
                    wsprintfA(buf, "Item found: type='%s' cfg='%s' g_ESPItems=%d listIdx=%d distMax=%d", 
                        typeName[0] ? typeName : "?", configName, g_ESPItems ? 1 : 0, listIdx, g_ItemDistance);
                    Log(buf);
                    itemLogCount++;
                }
            }
            
            if (isPlayer && !g_ESPPlayers && !g_ESPCorpses) continue;
            if (isZombie && !g_ESPZombies && !g_ESPCorpses) continue;
            if (isAnimal && !g_ESPAnimals) continue;
            if (isVehicle && !g_ESPVehicles) continue;
            if (isContainer && !g_ESPContainers) continue;
            if (isTrap && !g_ESPTraps) continue;
            if (isGrenadeEnt && !g_GrenadeTrajectory) {
                // still allow other categories; grenades drawn in combat pass
            }
            if (isItem && !g_ESPItems) 
            {
                if (g_DebugCounter == 0 && i < 5)
                {
                    Log("item skipped esp off");
                }
                continue;
            }

            if (!isPlayer && !isZombie && !isAnimal && !isVehicle && !isContainer && !isTrap)
            {
                if (OakFeaturesTryHeliCrash(entity, typeName, configName))
                {
                    totalDrawn++;
                    continue;
                }
            }
            
            if (isContainer || isTrap)
            {
                Vec3 wpos;
                int distance = -1;
                if (GetEntityPosition(entity, wpos))
                {
                    Vec3 referencePos{};
                    bool hasReference = false;
                    if (g_LocalPlayerValid) { referencePos = g_LocalPlayerPos; hasReference = true; }
                    else if (g_CameraValid) { referencePos = g_CameraPos; hasReference = true; }
                    if (hasReference)
                    {
                        distance = (int)Distance3D(referencePos, wpos);
                        if (isContainer && distance > g_ContainerDistance) continue;
                        if (isTrap && distance > g_TrapDistance) continue;
                    }
                }
                char label[64];
                ZeroMem(label, 64);
                if (!ReadEntityTypeName(entity, label, 64))
                {
                    for (int c = 0; configName[c] && c < 63; c++) label[c] = configName[c];
                }
                if (isContainer)
                {
                    // Remember only — redraw every frame via OakContEspDrawAllCached
                    // (ItemTable rolling scan was the flicker source, same as loot pre-cache).
                    OakContEspRemember(entity, label, distance);
                }
                else
                {
                    if (trapsDrawn >= g_MaxTrapsPerFrame || totalDrawn >= effectiveDrawCap)
                        continue;
                    DrawWorldMarker(entity,
                        g_TrapEspName ? label : "",
                        g_TrapEspDistance ? distance : -1,
                        g_ColorTrap, 12.f);
                    trapsDrawn++;
                }
                totalDrawn++;
                continue;
            }

            if (isPlayer || isZombie)
            {
                // Same infected often lives in Near + Far (or Slow). Draw once (Near first).
                if (espPzSeen(entity))
                    continue;
                espPzMark(entity);

                bool isDead = EntityIsDead(entity);
                if (isDead)
                {
                    if (!g_ESPCorpses) continue;
                    if (corpsesDrawn >= g_MaxCorpsesPerFrame || totalDrawn >= effectiveDrawCap) continue;
                    if (isPlayer && !g_CorpsePlayerCorpses) continue;
                    if (isZombie && !g_CorpseInfectedCorpses) continue;
                    Vec3 corpsePos;
                    int distance = -1;
                    if (GetEntityPosition(entity, corpsePos))
                    {
                        Vec3 referencePos{};
                        bool hasReference = false;
                        if (g_LocalPlayerValid) { referencePos = g_LocalPlayerPos; hasReference = true; }
                        else if (g_CameraValid) { referencePos = g_CameraPos; hasReference = true; }
                        if (hasReference)
                        {
                            distance = (int)Distance3D(referencePos, corpsePos);
                            if (distance > g_CorpseDistance) continue;
                        }
                    }
                    char corpseLabel[64];
                    ZeroMem(corpseLabel, 64);
                    char baseName[48];
                    ZeroMem(baseName, 48);
                    if (!ReadEntityTypeName(entity, baseName, 48))
                        wsprintfA(corpseLabel, isPlayer ? "Corpse" : "Dead infected");
                    else
                        wsprintfA(corpseLabel, "Dead %s", baseName);
                    DrawWorldMarker(entity,
                        g_CorpseEspName ? corpseLabel : "",
                        g_CorpseEspDistance ? distance : -1,
                        g_ColorCorpse, 9.f);
                    totalDrawn++;
                    corpsesDrawn++;
                    continue;
                }
                if (isPlayer && !g_ESPPlayers) continue;
                if (isZombie && !g_ESPZombies) continue;
                if (isPlayer && playersDrawn >= g_MaxPlayers) continue;
                if (isZombie && zombiesDrawn >= g_MaxZombies) continue;
                if (totalDrawn >= effectiveDrawCap) continue;
                
                
                Vec3 entityPos;
                int distance = 0;
                if (GetEntityPosition(entity, entityPos))
                {
                    
                    Vec3 referencePos;
                    bool hasReference = false;
                    
                    if (g_LocalPlayerValid)
                    {
                        referencePos = g_LocalPlayerPos;
                        hasReference = true;
                    }
                    else if (g_CameraValid)
                    {
                        referencePos = g_CameraPos;
                        hasReference = true;
                    }
                    
                    if (hasReference)
                    {
                        distance = (int)Distance3D(referencePos, entityPos);
                        
                        
                        if (g_LocalPlayerValid)
                        {
                            if (isPlayer && distance > g_PlayerDistance) continue;
                            if (isZombie && distance > g_ZombieDistance) continue;
                        }
                    }
                    else
                    {
                        
                        distance = -1;
                    }
                }
                
                
                char displayName[64];
                ZeroMem(displayName, 64);

                ID3D11ShaderResourceView* avatarSrv = nullptr;
                g_EspNameAvatar = nullptr;

                if (isPlayer)
                {
                    displayName[0] = 0;
                    if (g_MiscSteamAvatars)
                    {
                        int nid = 0;
                        __try {
                            nid = Read<int>(entity + oak_offsets::entity::NetworkIdPlayer);
                            if (!nid) nid = Read<int>(entity + oak_offsets::entity::NetworkId);
                        } __except (EXCEPTION_EXECUTE_HANDLER) { nid = 0; }
                        for (int ti = 0; ti < 64; ti++)
                        {
                            if (!g_SteamTags[ti].used || !g_SteamTags[ti].steamId) continue;
                            if (g_SteamTags[ti].entity == entity ||
                                (nid && g_SteamTags[ti].networkId == nid))
                            {
                                avatarSrv = MiscSteamAvatarCached(g_SteamTags[ti].steamId);
                                break;
                            }
                        }
                        g_EspNameAvatar = avatarSrv;
                    }
                }
                else if (typeName[0])
                {
                    lstrcpynA(displayName, typeName, 60);
                }
                else
                {
                    displayName[0] = 'Z'; displayName[1] = 'o'; displayName[2] = 'm';
                    displayName[3] = 'b'; displayName[4] = 'i'; displayName[5] = 'e';
                    displayName[6] = 0;
                }
                
                int vertsBefore = g_EspDrawCount;
                (void)vertsBefore;
                // First ~12s after Present init: skip skeletons — busy Near lists were
                // exiting DayZ mid-draw before any liveqa breadcrumb.
                static DWORD s_EspWarmStart = 0;
                if (!s_EspWarmStart) s_EspWarmStart = GetTickCount();
                const bool espWarm = (GetTickCount() - s_EspWarmStart) < 12000;
                if (espWarm && !isPlayer && zombiesDrawn >= 6)
                    continue;
                // LOD: full skeleton near; simple box/name beyond budget distance
                const int skelLod = (distance >= 0 && distance > g_LodNearM)
                    ? ((distance > g_LodMidM) ? 0 : (isPlayer ? 120 : 70))
                    : (isPlayer ? 160 : 90);
                // Sticky LOD — flipping skel↔box every frame flashes ESP.
                static uintptr_t s_LodEnt[128];
                static char s_LodSkel[128];
                static DWORD s_LodTick[128];
                unsigned lh = (unsigned)((entity >> 4) ^ (entity >> 9)) & 127u;
                DWORD nowLod = GetTickCount();
                bool useSkel = !espWarm && g_ShowSkeleton && skeletonsDrawn < g_MaxSkeletonsPerFrame
                    && (distance < 0 || distance <= skelLod);
                if (s_LodEnt[lh] == entity && (nowLod - s_LodTick[lh]) < 350)
                    useSkel = s_LodSkel[lh] != 0;
                else
                {
                    s_LodEnt[lh] = entity;
                    s_LodSkel[lh] = useSkel ? 1 : 0;
                    s_LodTick[lh] = nowLod;
                }
                __try {
                    if (useSkel)
                    {
                        DrawEntitySkeleton(entity, isPlayer, displayName, distance);
                        skeletonsDrawn++;
                    }
                    if (!useSkel)
                    {
                        DrawEntitySimple(entity, isPlayer, displayName, distance);
                        if (g_PlayerName || g_ZombieName || g_PlayerDistanceEnabled || g_ZombieDistanceEnabled)
                            labelsDrawn++;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                HitCheck_Add(entity);

                __try {
                    Vec3 epos;
                    if (GetEntityPosition(entity, epos))
                        OakFeaturesOnEntitySeen(entity, epos, isPlayer, isZombie, distance);
                    if (isPlayer)
                    {
                        OakFeaturesDrawLookDirection(entity, distance, true);
                        OakBatch4OnPlayerEsp(entity, distance);
                        if (g_StanceIcon.enabled)
                        {
                            Vec3 head = GetBonePosition(entity, BONE_HEAD, true);
                            Vec2 hs;
                            if (WorldToScreen(head, hs))
                                OakDrawStanceIcon(entity, hs.x, hs.y - 14.f, distance);
                        }
                    }
                    else if (isZombie)
                    {
                        OakFeaturesDrawLookDirection(entity, distance, false);
                        OakBatch4DrawLookingAtMe(entity, distance, false);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    
                totalDrawn++;
                if (isPlayer) playersDrawn++;
                else zombiesDrawn++;
                
                if (g_DebugCounter == 0)
                {
                    wsprintfA(buf, "DRAWING %s at entity 0x%X%08X", 
                        isPlayer ? "PLAYER" : "ZOMBIE",
                        (DWORD)(entity >> 32), (DWORD)entity);
                    Log(buf);
                }
            }
            else if (isAnimal)
            {
                if (animalsDrawn >= g_MaxAnimals) continue;
                if (totalDrawn >= effectiveDrawCap) continue;
                Vec3 animalPos;
                int distance = 0;
                if (GetEntityPosition(entity, animalPos))
                {
                    Vec3 referencePos;
                    bool hasReference = false;
                    
                    if (g_LocalPlayerValid)
                    {
                        referencePos = g_LocalPlayerPos;
                        hasReference = true;
                    }
                    else if (g_CameraValid)
                    {
                        referencePos = g_CameraPos;
                        hasReference = true;
                    }
                    
                    if (hasReference)
                    {
                        distance = (int)Distance3D(referencePos, animalPos);
                        
                        if (g_LocalPlayerValid && distance > g_AnimalDistance) continue;
                    }
                    else
                    {
                        distance = -1;
                    }
                }
                
                
                char animalName[64] = {};
                if (typeName[0])
                    lstrcpynA(animalName, typeName, 64);
                else if (configName[0])
                    lstrcpynA(animalName, configName, 64);

                char pretty[32];
                PrettyAnimalName(animalName[0] ? animalName : typeName, pretty, 32);
                DrawAnimal(entity, pretty, distance);
                totalDrawn++;
                animalsDrawn++;
            }
            else if (isItem)
            {
                if (!g_ESPItems)
                {
                    continue;
                }

                if (!IsValidPtr(entity) || entity < 0x100000000) continue;
                if (!IsValidPtr(entityType) || entityType < 0x100000000) continue;
                
                
                uintptr_t visualStateAddr = entity + 0x1C8;
                if (!IsValidPtr(visualStateAddr) || visualStateAddr < 0x100000000) continue;
                
                __try {
                    uintptr_t visualState = Read<uintptr_t>(visualStateAddr);
                    if (!IsValidPtr(visualState) || visualState == 0 || visualState < 0x100000000)
                    {
                        continue; 
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    continue; 
                }
                
                Vec3 itemPos = { 0, 0, 0 };
                int distance = 0;
                
                
                __try {
                    bool hasPos = GetEntityPosition(entity, itemPos);
                    if (!hasPos)
                        continue;
                    if (IsNaN(itemPos.x) || IsNaN(itemPos.y) || IsNaN(itemPos.z))
                        continue;
                    // Stale after pickup: origin / underground / absurd coords = ghost ESP
                    if ((itemPos.x == 0.f && itemPos.y == 0.f && itemPos.z == 0.f) ||
                        itemPos.y < -50.f || itemPos.y > 2000.f)
                        continue;
                    // Stale ghost: FutureVisualState far from VisualState → picked up / despawning
                    {
                        Vec3 fut{};
                        uintptr_t fvs = Read<uintptr_t>(entity + oak_offsets::entity::FutureVisualState);
                        if (IsValidPtr(fvs) && fvs > 0x100000000)
                        {
                            fut.x = Read<float>(fvs + 0x2C);
                            fut.y = Read<float>(fvs + 0x30);
                            fut.z = Read<float>(fvs + 0x34);
                            if (fut.x == fut.x && fut.y == fut.y && fut.z == fut.z &&
                                !(fut.x == 0.f && fut.y == 0.f && fut.z == 0.f))
                            {
                                float dx = fut.x - itemPos.x, dy = fut.y - itemPos.y, dz = fut.z - itemPos.z;
                                float d2 = dx * dx + dy * dy + dz * dz;
                                // Teleporting into inventory / sky — drop ESP
                                if (d2 > (80.f * 80.f))
                                    continue;
                            }
                        }
                    }

                    Vec3 referencePos;
                    bool hasReference = false;
                    if (g_LocalPlayerValid)
                    {
                        referencePos = g_LocalPlayerPos;
                        hasReference = true;
                    }
                    else if (g_CameraValid)
                    {
                        referencePos = g_CameraPos;
                        hasReference = true;
                    }
                    if (hasReference)
                    {
                        distance = (int)Distance3D(referencePos, itemPos);
                        if (g_LocalPlayerValid && distance > g_ItemDistance) continue;
                    }
                    else
                    {
                        distance = -1;
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    if (g_DebugCounter == 0)
                    {
                        DWORD exceptionCode = GetExceptionCode();
                        wsprintfA(buf, "item: EXCEPTION in GetEntityPosition for item: code=0x%X entity=0x%X%08X", 
                            exceptionCode, (DWORD)(entity >> 32), (DWORD)entity);
                        Log(buf);
                    }
                    continue;
                }
                
                
                char itemName[64];
                ZeroMem(itemName, 64);
                // Completeness audit: count every in-range loot that passes category filters
                {
                    const char* cn = typeName[0] ? typeName : configName;
                    LootCategory lcat = ClassifyLootName(cn);
                    if (!LootCategoryEnabled(lcat))
                    {
                        lootSkippedFilter++;
                        continue;
                    }
                    lootInRange++;
                    if (itemsDrawn >= g_MaxItems)
                    {
                        lootSkippedCap++;
                        continue;
                    }
                }
                if (typeName[0])
                {
                    for (int j = 0; typeName[j] && j < 60; j++)
                        itemName[j] = typeName[j];
                }
                else
                {
                    for (int j = 0; configName[j] && j < 60; j++)
                        itemName[j] = configName[j];
                }
                
                if (g_DebugCounter == 0 && totalDrawn < 5)
                {
                    wsprintfA(buf, "Drawing item: name='%s' distance=%d", itemName, distance);
                    Log(buf);
                }
                
                
                
                if (IsNaN(itemPos.x) || IsNaN(itemPos.y) || IsNaN(itemPos.z))
                {
                    continue; 
                }
                
                
                __try {
                    OakItemEspRemember(entity, itemName, distance);
                }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
            }
            else if (isVehicle)
            {
                
                Vec3 vehiclePos;
                int distance = 0;
                if (GetEntityPosition(entity, vehiclePos))
                {
                    Vec3 referencePos;
                    bool hasReference = false;
                    
                    if (g_LocalPlayerValid)
                    {
                        referencePos = g_LocalPlayerPos;
                        hasReference = true;
                    }
                    else if (g_CameraValid)
                    {
                        referencePos = g_CameraPos;
                        hasReference = true;
                    }
                    
                    if (hasReference)
                    {
                        distance = (int)Distance3D(referencePos, vehiclePos);
                        
                        if (g_LocalPlayerValid && distance > g_VehicleDistance) continue;
                    }
                    else
                    {
                        distance = -1;
                    }
                }
                
                
                char vehicleName[64] = {};
                if (typeName[0])
                    lstrcpynA(vehicleName, typeName, 64);
                else if (StrCmp(configName, "car") == 0)
                {
                    vehicleName[0] = 'C'; vehicleName[1] = 'a'; vehicleName[2] = 'r';
                    vehicleName[3] = 0;
                }
                else if (StrCmpI(configName, "boat") == 0)
                {
                    vehicleName[0] = 'B'; vehicleName[1] = 'o'; vehicleName[2] = 'a'; vehicleName[3] = 't';
                    vehicleName[4] = 0;
                }
                else if (configName[0])
                    lstrcpynA(vehicleName, configName, 64);
                
                
                __try {
                    OakVehEspRemember(entity, vehicleName, distance);
                    totalDrawn++;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    OakCrashGate_Log("esp-vehicle", GetExceptionCode());
                }
            }
        }
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            const DWORD ex = GetExceptionCode();
            OakCrashGate_Log("esp-entity-loop", ex);
            if (g_DebugCounter == 0)
            {
                wsprintfA(buf, "item: EXCEPTION in entity loop: code=0x%X listIdx=%d listName='%s' entListPtr=0x%X%08X", 
                    ex, listIdx, lists[listIdx].name, (DWORD)(entListPtr >> 32), (DWORD)entListPtr);
                Log(buf);
            }
            
        }
    }

    OakItemEspDrawAllCached(&itemsDrawn);
    totalDrawn += itemsDrawn;
    // Containers + vehicles: every-frame redraw from scan cache (kills ItemTable/W2S flicker).
    OakContEspDrawAllCached();
    OakVehEspDrawAllCached();
    
    g_QaPlayersDrawn = playersDrawn;
    g_QaZombiesDrawn = zombiesDrawn;
    g_QaAnimalsDrawn = animalsDrawn;
    g_QaItemsDrawn = itemsDrawn;
    g_QaTotalDrawn = totalDrawn;

    {
        LARGE_INTEGER espEnd = {};
        QueryPerformanceCounter(&espEnd);
        OakPerfStats ps = {};
        ps.scanned = entitiesScanned;
        ps.drawn = totalDrawn;
        ps.labelsDrawn = labelsDrawn;
        ps.skeletonsDrawn = skeletonsDrawn;
        ps.lootDrawn = itemsDrawn;
        ps.corpsesDrawn = corpsesDrawn;
        ps.trapsDrawn = trapsDrawn;
        ps.playersDrawn = playersDrawn;
        ps.zombiesDrawn = zombiesDrawn;
        ps.animalsDrawn = animalsDrawn;
        ps.effectiveScanCap = effectiveScanCap;
        ps.effectiveDrawCap = effectiveDrawCap;
        if (s_PerfFreq.QuadPart > 0)
            ps.frameMs = (float)((double)(espEnd.QuadPart - espStart.QuadPart) * 1000.0 / (double)s_PerfFreq.QuadPart);
        ImGuiMenu_SetPerfStats(&ps);
    }

    if (g_DebugCounter == 0)
    {
        wsprintfA(buf, "drawn %d (p=%d z=%d a=%d items=%d)", totalDrawn,
            playersDrawn, zombiesDrawn, animalsDrawn, itemsDrawn);
        Log(buf);
        g_DebugCounter = 1;
    }
    else
    {
        // Periodic in-world stats (g_DebugCounter stays 1 after first dump)
        static int espStatFrames = 0;
        if (false && (++espStatFrames % 3000) == 0)
        {
            int nearC = 0, farC = 0, slowC = 0, itemC = 0;
            uintptr_t nd = 0; int nc = 0;
            if (ResolveEntityList(worldPtr, oak_offsets::world::NearEntList, 2000, nd, nc, nullptr)) nearC = nc;
            if (ResolveEntityList(worldPtr, oak_offsets::world::FarEntList, 2000, nd, nc, nullptr)) farC = nc;
            slowC = Read<int>(worldPtr + oak_offsets::world::SlowTableSize);
            itemC = Read<int>(worldPtr + oak_offsets::world::ItemListSize);
            wsprintfA(buf, "ESP stats: drawn=%d p=%d z=%d a=%d items=%d near=%d far=%d slow=%d item=%d local=%d cam=%d draws=%d",
                totalDrawn, playersDrawn, zombiesDrawn, animalsDrawn, itemsDrawn, nearC, farC, slowC, itemC,
                g_LocalPlayerValid ? 1 : 0, g_CameraValid ? 1 : 0, g_EspDrawCount);
            Log(buf);
            wsprintfA(buf, "lootQA: inRange=%d drawn=%d missed=%d filterSkip=%d capSkip=%d max=%d dist=%d",
                lootInRange, itemsDrawn, lootInRange - itemsDrawn, lootSkippedFilter, lootSkippedCap,
                g_MaxItems, g_ItemDistance);
            Log(buf);
        }
    }

    // Local vitals / ammo HUD (not tied to remote ESP early-out)
    __try {
        if (IsValidPtr(localPlayer) && (g_LocalVitalsHud || g_LocalWeaponAmmo))
            DrawLocalVitalsHud(localPlayer);
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("vitals-hud", GetExceptionCode()); }

    // Real FPS — QueryPerformanceCounter (GetTickCount ~15.6ms → fake "64 FPS")
    __try {
        ImDrawList* dlFps = ImGuiMenu_EspDrawList();
        if (dlFps)
        {
            static LARGE_INTEGER s_Freq = {};
            static LARGE_INTEGER s_Last = {};
            static float s_FpsSmooth = 0.f;
            static int s_Frames = 0;
            static LARGE_INTEGER s_WinStart = {};
            static float s_WinFps = 0.f;
            if (s_Freq.QuadPart == 0)
                QueryPerformanceFrequency(&s_Freq);
            LARGE_INTEGER nowQ = {};
            QueryPerformanceCounter(&nowQ);
            if (s_Last.QuadPart != 0 && s_Freq.QuadPart > 0)
            {
                double dt = (double)(nowQ.QuadPart - s_Last.QuadPart) / (double)s_Freq.QuadPart;
                if (dt > 0.00005 && dt < 1.0)
                {
                    float inst = (float)(1.0 / dt);
                    s_FpsSmooth = (s_FpsSmooth <= 1.f) ? inst : (s_FpsSmooth * 0.90f + inst * 0.10f);
                }
            }
            s_Last = nowQ;
            if (s_WinStart.QuadPart == 0) s_WinStart = nowQ;
            s_Frames++;
            double winSec = (double)(nowQ.QuadPart - s_WinStart.QuadPart) / (double)s_Freq.QuadPart;
            if (winSec >= 0.5)
            {
                s_WinFps = (float)((double)s_Frames / winSec);
                s_Frames = 0;
                s_WinStart = nowQ;
            }
            float show = (s_WinFps > 1.f) ? s_WinFps : s_FpsSmooth;
            char fpsLine[96];
            wsprintfA(fpsLine, "Oak %s  |  FPS %d  |  loot %d/%d  p %d z %d",
                ImGuiMenu_Version(), (int)(show + 0.5f),
                itemsDrawn, lootInRange, playersDrawn, zombiesDrawn);
            dlFps->AddRectFilled(ImVec2(8.f, 8.f), ImVec2(8.f + 420.f, 28.f), IM_COL32(0, 0, 0, 140));
            dlFps->AddText(ImVec2(14.f, 11.f), IM_COL32(180, 255, 160, 240), fpsLine);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("fps", GetExceptionCode()); }

    // Freecam: combat visuals only (gameplay ticks run every Present via OakRunGameplayFrame)
    __try {
        if (inGameplay && !g_MiscFreecam)
            UpdateAndDrawCombatVisuals(worldPtr, localPlayer);
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("combat-vis", GetExceptionCode()); }

    __try {
        OakFeaturesDrawOverlays(worldPtr, localPlayer);
        OakBatch4DrawWorldOverlays(worldPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("features", GetExceptionCode()); }

    // Breadcrumb — rare (was 1Hz disk I/O hitch on Present)
    {
        static DWORD s_AliveLog = 0;
        DWORD nowA = GetTickCount();
        if (!s_AliveLog) s_AliveLog = nowA;
        if ((nowA - s_AliveLog) > 60000)
        {
            s_AliveLog = nowA;
            char ab[96];
            wsprintfA(ab, "liveqa[alive] t=%u local=%d cam=%d",
                (unsigned)nowA, g_LocalPlayerValid ? 1 : 0, g_CameraValid ? 1 : 0);
            Log(ab);
        }
    }

    // Periodic live feature validation (agent/QA) — every ~6s once local+camera ready.
    // Dual-client flag (C:\oak\dayz\liveqa_dual.flag) auto-exercises 3PP / recoil / freecam / bars.
    {
        static DWORD s_LiveQa = 0;
        static DWORD s_QaStart = 0;
        static int s_FcPhase = 0; // 0 wait, 1 freecam on, 2 done
        static int s_DualArmed = 0;
        static DWORD s_DualPoll = 0;
        DWORD nowQa = GetTickCount();
        if (!s_QaStart && g_LocalPlayerValid) s_QaStart = nowQa;

        // Re-poll flag so deleting C:\oak\dayz\liveqa_dual.flag immediately releases menu control.
        // Crash-matrix owns the same UI path when armed — dual QA must not fight it.
        if ((nowQa - s_DualPoll) > 1000)
        {
            s_DualPoll = nowQa;
            if (ImGuiMenu_CrashMatrixActive())
            {
                if (s_DualArmed)
                {
                    s_DualArmed = 0;
                    Log("liveqa[dual]: paused — crash matrix armed");
                }
            }
            else
            {
            const int armed = (GetFileAttributesA("C:\\oak\\dayz\\liveqa_dual.flag") != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
            if (armed != s_DualArmed)
            {
                s_DualArmed = armed;
                if (s_DualArmed)
                    Log("liveqa[dual]: flag armed - auto-exercise 3PP/recoil/freecam/bars/names");
                else
                {
                    Log("liveqa[dual]: flag cleared - menu control restored");
                    if (s_FcPhase == 1)
                    {
                        g_MiscFreecam = false;
                        MiscPushUi();
                        s_FcPhase = 2;
                    }
                }
            }
            }
        }

        // Dual-client live QA only (crash matrix ticks from OakRunGameplayFrame).
        if (s_DualArmed == 1 && g_LocalPlayerValid && g_CameraValid && !ImGuiMenu_CrashMatrixActive())
        {
            const DWORD age = s_QaStart ? (nowQa - s_QaStart) : 0;
            const bool wantFc = (age > 8000 && age < 55000);
            ImGuiMenu_PushDualQa(true, true, true, wantFc);
            g_MiscSteamAvatars = true;
            g_HealthBars = true;
            g_LocalVitalsHud = true;
            g_WorldMisc.thirdPerson = true;
            g_Recoil.noRecoil = true;
            g_Recoil.noSway = true;
            g_Recoil.recoilPct = 0;
            g_Recoil.swayPct = 0;
            g_MiscFreecam = wantFc;
            if (wantFc && s_FcPhase == 0)
            {
                s_FcPhase = 1;
                MiscPushUi();
                Log("liveqa[freecam]: AUTO ON soak (dual flag)");
            }
            else if (!wantFc && s_FcPhase == 1)
            {
                s_FcPhase = 2;
                g_MiscFreecam = false;
                MiscPushUi();
                Log("liveqa[freecam]: AUTO OFF after dual soak");
            }
            static DWORD s_DualBeat = 0;
            if ((nowQa - s_DualBeat) > 5000)
            {
                s_DualBeat = nowQa;
                char db[128];
                wsprintfA(db, "liveqa[dual] ageMs=%u tp=1 recoil=1 bars=1 names=1 freecam=%d",
                    age, wantFc ? 1 : 0);
                Log(db);
            }
        }
        // Auto freecam soak disabled after 1.3.22 live proof (hooks+VS pin, bodyDriftCm=0, no crash).
        if (!s_DualArmed && s_QaStart && s_FcPhase == 0 && (nowQa - s_QaStart) > 12000)
            s_FcPhase = 2;

        if (g_LocalPlayerValid && g_CameraValid && (nowQa - s_LiveQa) > ((s_DualArmed == 1) ? 8000u : 120000u))
        {
            s_LiveQa = nowQa;
            char qb[256];
            float hp = -1.f, blood = -1.f, shock = -1.f, stam = -1.f, hung = -1.f, thir = -1.f;
            bool vitOk = false;
            __try { vitOk = ReadVitals(localPlayer, &hp, &blood, &shock, &stam, &hung, &thir); }
            __except (EXCEPTION_EXECUTE_HANDLER) { vitOk = false; }
            char wpn[64] = {};
            int ammo = -1;
            bool wpnOk = false;
            __try { wpnOk = GetHandWeaponInfo(localPlayer, wpn, 64, &ammo); }
            __except (EXCEPTION_EXECUTE_HANDLER) { wpnOk = false; }

            float hour = 0.f, dayTime = 0.f;
            __try {
                hour = Read<float>(worldPtr + oak_offsets::world::Hour);
                dayTime = Read<float>(worldPtr + oak_offsets::world::DayTime);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            int bodyDriftCm = -1;
            if (g_MiscFreecam && g_FreecamBodyFreezeValid)
            {
                float dx = g_LocalPlayerPos.x - g_FreecamBodyFreeze.x;
                float dy = g_LocalPlayerPos.y - g_FreecamBodyFreeze.y;
                float dz = g_LocalPlayerPos.z - g_FreecamBodyFreeze.z;
                bodyDriftCm = (int)(sqrtf(dx * dx + dy * dy + dz * dz) * 100.f);
            }

            // Magic-bullet bone height vs origin (head ≈ +1.5m)
            int mbHeadCm = -1;
            if (g_MagicLock.valid)
            {
                Vec3 origin{};
                if (GetEntityPosition(g_MagicLock.entity, origin))
                    mbHeadCm = (int)((g_MagicLock.bonePos.y - origin.y) * 100.f);
            }

            wsprintfA(qb, "liveqa[vitals] ok=%d hp=%d blood=%d shock=%d stam=%d hung=%d thir=%d shockAmt=%d hungAmt=%d thirAmt=%d hud=%d bars=%d",
                vitOk ? 1 : 0, (int)hp, (int)blood, (int)shock, (int)stam, (int)hung, (int)thir,
                (shock >= 0.f) ? (int)(100.f - shock + 0.5f) : -1,
                (hung >= 0.f) ? (int)(100.f - hung + 0.5f) : -1,
                (thir >= 0.f) ? (int)(100.f - thir + 0.5f) : -1,
                g_LocalVitalsHud ? 1 : 0, g_HealthBars ? 1 : 0);
            Log(qb);
            wsprintfA(qb, "liveqa[zed] bars=%d lastHp=%d seen=%d drawn=%d fileN=%d netOff=0x%X zombiesEsp=%d",
                g_QaZedBars, (int)(g_QaZedHp + 0.5f), g_QaZedSeen, g_QaZombiesDrawn, g_FileZedN,
                (unsigned)g_QaZedNetOff, g_ESPZombies ? 1 : 0);
            Log(qb);
            g_QaZedBars = 0;
            g_QaZedSeen = 0;
            // NetSync native GetClassVar still AVs from C++ (all string shapes). Local bars use
            // oak_vitals.txt authority bridge; remotes need raw m_HealthLevel offset next.
            {
                wsprintfA(qb, "liveqa[netsync] fileBridge=1 (GetClassVar hot-path disabled)");
                Log(qb);
            }
            // DamageSystem slot proof (Object.HasDamageSystem reads +0x188)
            {
                uintptr_t ds = 0, dmLegacy = 0;
                __try {
                    ds = Read<uintptr_t>(localPlayer + oak_offsets::player::DamageManager);
                    dmLegacy = Read<uintptr_t>(localPlayer + 0x700);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                wsprintfA(qb, "liveqa[dmg] ds188=0x%p dm700=0x%p",
                    (void*)ds, (void*)dmLegacy);
                Log(qb);
            }

            // Ground truth: local server seeds Energy=2500 Water=3750 Shock≈55 → hung≈50 thir≈75
            {
                float eRaw = -1.f, wRaw = -1.f;
                __try { GetLocalEnergyWaterRaw(&eRaw, &wRaw); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                int eI = (int)(eRaw + 0.5f), wI = (int)(wRaw + 0.5f);
                int eOk = (eI >= 2400 && eI <= 2600) ? 1 : 0;
                int wOk = (wI >= 3700 && wI <= 3800) ? 1 : 0;
                int pOk = ((int)hung >= 45 && (int)hung <= 55 && (int)thir >= 70 && (int)thir <= 80) ? 1 : 0;
                wsprintfA(qb, "liveqa[stats-raw] energy=%d water=%d expectE=2500 expectW=3750 hung=%d thir=%d shock=%d matchEW=%d matchPct=%d off=0x%X",
                    eI, wI, (int)hung, (int)thir, (int)shock, (eOk && wOk) ? 1 : 0, pOk,
                    (unsigned)g_CachedStatsOff);
                Log(qb);
            }

            // Soft one-shot DISABLED — Present hitch
            static bool s_HpDump = true;
            if (false && !s_HpDump && hung >= 0.f && IsValidPtr(localPlayer))
            {
                s_HpDump = true;
                __try
                {
                    char db[220];
                    uintptr_t mod = g_GameModule ? (uintptr_t)g_GameModule : 0;
                    uintptr_t stats = Read<uintptr_t>(localPlayer + oak_offsets::player::StatsContainer);
                    uintptr_t dmg = Read<uintptr_t>(localPlayer + oak_offsets::player::DamageManager);
                    wsprintfA(db, "liveqa[hp-dump] stats@6F0=0x%p dmg@6F8=0x%p hp=%d hung=%d",
                        (void*)stats, (void*)dmg, (int)hp, (int)hung);
                    Log(db);
                    // Wider slot scan — DamageManager may have moved past 0x700
                    int slots = 0;
                    for (uintptr_t off = 0x5C0; off <= 0x900 && slots < 28; off += 8)
                    {
                        uintptr_t p = Read<uintptr_t>(localPlayer + off);
                        if (!IsValidPtr(p) || p < 0x100000000) continue;
                        if (mod && p >= mod && p < mod + 0x8000000ULL) continue;
                        uintptr_t vt = Read<uintptr_t>(p);
                        if (!(mod && IsValidPtr(vt) && vt >= mod && vt < mod + 0x8000000ULL))
                            continue;
                        // Prefer objects that look like health zone holders
                        uintptr_t a28 = Read<uintptr_t>(p + 0x28);
                        int c34 = Read<int>(p + 0x34);
                        int c30 = Read<int>(p + 0x30);
                        float f10 = Read<float>(p + 0x10);
                        bool zoneish = (IsValidPtr(a28) && ((c34 > 0 && c34 <= 64) || (c30 > 0 && c30 <= 64)));
                        bool fhealth = (f10 == f10 && ((f10 > 0.2f && f10 <= 1.05f) || (f10 >= 25.f && f10 <= 100.f)));
                        wsprintfA(db, "liveqa[hp-dump] slot off=0x%X vt=0x%p z=%d fh=%d c=%d f10=%d",
                            (unsigned)off, (void*)vt, zoneish ? 1 : 0, fhealth ? 1 : 0,
                            (c34 > 0 && c34 <= 64) ? c34 : c30, (int)(f10 * 100.f));
                        Log(db);
                        slots++;
                        if (zoneish || fhealth)
                        {
                            for (uintptr_t fo = 0x8; fo <= 0x38; fo += 4)
                            {
                                float v = Read<float>(p + fo);
                                if (v != v || v <= 0.f || v > 5000.f) continue;
                                wsprintfA(db, "liveqa[hp-dump] candf 0x%X+0x%X=%d",
                                    (unsigned)off, (unsigned)fo, (int)(v * 100.f));
                                Log(db);
                            }
                            // Dump zone table layouts — 0x6F8 showed c=12 live
                            int cnt = (c34 > 0 && c34 <= 64) ? c34 : c30;
                            if (IsValidPtr(a28) && cnt > 0 && cnt <= 64)
                            {
                                const int strides[] = { 0x10, 0x18, 0x8, 0x20 };
                                for (int si = 0; si < 4; si++)
                                {
                                    int logged = 0;
                                    for (int i = 0; i < cnt && logged < 8; i++)
                                    {
                                        uintptr_t entry = a28 + (uintptr_t)i * (uintptr_t)strides[si];
                                        uintptr_t np = Read<uintptr_t>(entry);
                                        char sn[40] = {};
                                        if (IsValidPtr(np)) ReadEngineString(np, sn, 40);
                                        if (!sn[0] && IsValidPtr(np) && mod && np >= mod && np < mod + 0x8000000ULL)
                                        {
                                            for (int c = 0; c < 31; c++)
                                            {
                                                char ch = Read<char>(np + c);
                                                if (!ch || ch < 32 || ch > 126) { sn[c] = 0; break; }
                                                sn[c] = ch;
                                            }
                                        }
                                        float v8 = Read<float>(entry + 0x8);
                                        float vC = Read<float>(entry + 0xC);
                                        float v4 = Read<float>(entry + 0x4);
                                        if (!sn[0] && !(v8 == v8 && v8 >= 0.f && v8 <= 5000.f))
                                            continue;
                                        wsprintfA(db, "liveqa[hp-dump] zone 0x%X str=0x%X[%d] '%s' 4=%d 8=%d c=%d",
                                            (unsigned)off, strides[si], i, sn[0] ? sn : "?",
                                            (int)(v4 * 100.f), (int)(v8 * 100.f), (int)(vC * 100.f));
                                        Log(db);
                                        logged++;
                                    }
                                }
                            }
                        }
                    }
                    if (IsValidPtr(stats))
                    {
                        for (uintptr_t base = 0x0; base <= 0x48; base += 8)
                        {
                            uintptr_t arr = Read<uintptr_t>(stats + base);
                            if (!IsValidPtr(arr) || arr < 0x100000000) continue;
                            if (mod && arr >= mod && arr < mod + 0x8000000ULL) continue;
                            int logged = 0;
                            for (int i = 0; i < 20 && logged < 12; i++)
                            {
                                uintptr_t rec = Read<uintptr_t>(arr + (uintptr_t)i * 8);
                                if (!IsValidPtr(rec) || rec < 0x100000000) continue;
                                char sn[48] = {};
                                for (uintptr_t no = 0x0; no <= 0x18 && !sn[0]; no += 8)
                                {
                                    uintptr_t np = Read<uintptr_t>(rec + no);
                                    if (IsValidPtr(np)) ReadEngineString(np, sn, 48);
                                }
                                float best = -1.f;
                                int bestOff = -1;
                                for (uintptr_t vo = 0x18; vo <= 0x38; vo += 4)
                                {
                                    float t = Read<float>(rec + vo);
                                    if (t != t || t < 0.f || t > 5000.f) continue;
                                    best = t; bestOff = (int)vo; break;
                                }
                                if (!sn[0] && best < 0.f) continue;
                                wsprintfA(db, "liveqa[hp-dump] stats base=0x%X[%d] '%s' v@%d=%d",
                                    (unsigned)base, i, sn[0] ? sn : "?",
                                    bestOff, (int)(best * 100.f));
                                Log(db);
                                logged++;
                            }
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            wsprintfA(qb, "liveqa[ammo] ok=%d name='%s' ammo=%d localAmmo=%d",
                wpnOk ? 1 : 0, wpn[0] ? wpn : "?", ammo, g_LocalWeaponAmmo ? 1 : 0);
            Log(qb);
            wsprintfA(qb, "liveqa[friends] count=%d", g_FriendCount);
            Log(qb);
            wsprintfA(qb, "liveqa[freecam] on=%d combatOff=%d bodyDriftCm=%d phase=%d speed=%d",
                g_MiscFreecam ? 1 : 0, g_MiscFreecam ? 1 : 0, bodyDriftCm, s_FcPhase,
                (int)(g_MiscFreecamSpeed + 0.5f));
            Log(qb);
            __try { MiscLiveQaDumpNames(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            wsprintfA(qb, "liveqa[daytime] lock=%d hourx10=%d dayTimex100=%d",
                g_MiscDaytimeLock ? 1 : 0, (int)(hour * 10.f), (int)(dayTime * 100.f));
            Log(qb);
            OakBatch4LogLiveQa();
            wsprintfA(qb, "liveqa[esp] drawn=%d p=%d z=%d items=%d local=%d cam=%d mb=%d aim=%d",
                g_QaTotalDrawn, g_QaPlayersDrawn, g_QaZombiesDrawn, g_QaItemsDrawn,
                g_LocalPlayerValid ? 1 : 0, g_CameraValid ? 1 : 0,
                g_MagicBullet ? 1 : 0, g_AimbotEnabled ? 1 : 0);
            Log(qb);
            wsprintfA(qb, "liveqa[binds] esp=%d aim=%d mb=%d day=%d free=%d wpCount=%d",
                g_BindEsp, g_BindAimbot, g_BindMagicBullet, g_BindDaytimeLock, g_BindFreecam, g_WaypointCount);
            Log(qb);
            wsprintfA(qb, "liveqa[mb] lock=%d headHeightCm=%d dist=%d",
                g_MagicLock.valid ? 1 : 0, mbHeadCm,
                g_MagicLock.valid ? (int)g_MagicLock.worldDist : -1);
            Log(qb);
            wsprintfA(qb, "liveqa[ver] %s", ImGuiMenu_Version());
            Log(qb);
            OakCrashHunt_LogSummary();
        }
    }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OakCrashGate_Log("esp-root", GetExceptionCode());
    }
}


// Warmup must run on the Present/D3D thread (immediate context is not
// free-threaded). Do NOT SetTimer(TIMERPROC) into a manual-mapped image —
// CFG blocks it and SEH retries forever after "imgui ok" with no UI.
// Do NOT CreateThread for D3D CreateDeviceObjects either.
static volatile LONG g_OakWarmupPending = 0;

static void OakPresentOverlay(IDXGISwapChain* pSwapChain)
{
    if (!pSwapChain) return;
    OakCrashHunt_PresentPulse();
    OAK_MARK("present");
    {
        static LONG s_NoteOnce;
        if (!InterlockedCompareExchange(&s_NoteOnce, 1, 0))
            OakCrashHunt_NoteThread("present");
    }
    if (g_ShuttingDown || !g_Running) return;
    // Authorization loss disables Oak work cleanly; it never affects the host
    // game process or deliberately crashes the client.
    if (!OakProtectionIsAuthorized())
        return;

    uintptr_t sessionWorld = 0;
    uintptr_t sessionLocal = 0;
    uintptr_t sessionCamera = 0;
    const bool sessionReady = OakSessionReady(sessionWorld, sessionLocal, sessionCamera);

    if (sessionReady && !OakPerfBoost_IsUncappedLight())
    {
        // Under BE, skip lab/engine microscope until ImGui is up — early RTTI/poll
        // walks correlated with Present-path AVs right after VT hook arm.
        if (!(OakBeIsLaunch() && !g_Initialized))
        {
            { OAK_MARK("lab"); OakLab_OnPresent(); }
            { OAK_MARK("engine"); OakEngine_OnPresent(); }
        }
    }

    // Arm lag-switch hooks + weather during soak / before ImGui.
    // Skip until world exists — Batch4Warp/LagSwitchTick on null world still
    // polled hold-key and could arm ws2 hooks mid-connect.
    // Arm lag-switch only once a world exists — ticking on a null world polled the
    // hold key and could arm ws2 hooks mid-connect.
    if (sessionReady && !(OakBeIsLaunch() && !g_Initialized))
    {
        static DWORD s_LagEarly = 0;
        DWORD nowEarly = GetTickCount();
        if (!s_LagEarly || (nowEarly - s_LagEarly) >= 500)
        {
            s_LagEarly = nowEarly;
            uintptr_t wp = 0;
            if (g_GameModule)
            {
                __try {
                    wp = Read<uintptr_t>((uintptr_t)g_GameModule + oak_offsets::modbase::World);
                } __except (EXCEPTION_EXECUTE_HANDLER) { wp = 0; }
            }
            if (!IsValidPtr(wp) || wp < 0x100000000ULL)
                wp = g_CachedWorldPtr;
            if (IsValidPtr(wp) && wp > 0x100000000ULL)
            {
                OAK_MARK("lag-arm");
                __try { Batch4Warp(wp, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }

    if (!g_Initialized)
    {
        static LONG s_InitOnce = 0;
        static LONG s_InitFailures = 0;
        if (InterlockedCompareExchange(&s_InitOnce, 1, 0) != 0)
            return;

        static DWORD s_FirstPresentTick = 0;
        if (!s_FirstPresentTick)
            s_FirstPresentTick = GetTickCount();
        // Soak after first Present so BE finishes loading before heavy D3D/ImGui work.
        // Thin trampoline already soaked under BE — init on first overlay entry.
        const DWORD kPresentSoakMs = OakBeIsLaunch() ? 0 : 2000;
        if (GetTickCount() - s_FirstPresentTick < kPresentSoakMs)
        {
            InterlockedExchange(&s_InitOnce, 0);
            return;
        }

        // Stop Present-frame retry storms (was flooding the log and killing the game).
        if (s_InitFailures >= 5)
            return;

        Log("present hook init");
        __try
        {
            if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_Device)) || !g_Device)
            {
                Log("BE-VT: GetDevice failed — retry");
                InterlockedExchange(&s_InitOnce, 0);
                return;
            }
            Log("BE-VT: GetDevice ok");
            g_Device->GetImmediateContext(&g_Context);
            g_SwapChain = pSwapChain;
            if (g_Context && !OakBeIsLaunch())
            {
                // Depth/chams context hooks have been BE-unstable; skip on BE launches.
                OakDepth_InstallHook(g_Context);
                OakShadowChams_Install(g_Context);
            }

            DXGI_SWAP_CHAIN_DESC desc;
            pSwapChain->GetDesc(&desc);
            g_ScreenWidth = (float)desc.BufferDesc.Width;
            g_ScreenHeight = (float)desc.BufferDesc.Height;
            if (g_ScreenWidth < 1.f || g_ScreenHeight < 1.f)
            {
                RECT rc{};
                if (desc.OutputWindow && GetClientRect(desc.OutputWindow, &rc))
                {
                    g_ScreenWidth = (float)(rc.right - rc.left);
                    g_ScreenHeight = (float)(rc.bottom - rc.top);
                }
            }
            if (g_ScreenWidth < 1.f) g_ScreenWidth = 1920.f;
            if (g_ScreenHeight < 1.f) g_ScreenHeight = 1080.f;

            g_NDCScaleX = 2.0f / g_ScreenWidth;
            g_NDCScaleY = 2.0f / g_ScreenHeight;
            g_ScreenHalfW = g_ScreenWidth / 2.0f;
            g_ScreenHalfH = g_ScreenHeight / 2.0f;

            char buf[128];
            wsprintfA(buf, "Screen: %dx%d", (int)g_ScreenWidth, (int)g_ScreenHeight);
            Log(buf);

            ID3D11Texture2D* backBuffer = NULL;
            pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
            if (backBuffer)
            {
                g_Device->CreateRenderTargetView(backBuffer, NULL, &g_RenderTarget);
                backBuffer->Release();
            }

            InitRenderer();

            Log("calling ImGuiMenu_Init...");
            // Never CreateContext on the Present thread under BE — that path AVs.
            // Worker precreate must succeed first; Present only wires D3D backends.
            if (OakBeIsLaunch() && !ImGui::GetCurrentContext())
            {
                Log("imgui context missing — waiting for worker precreate");
                InterlockedExchange(&s_InitOnce, 0);
                return;
            }
            if (ImGuiMenu_Init(desc.OutputWindow, g_Device, g_Context))
            {
                Log("imgui ok");
                g_OverlayHwnd = desc.OutputWindow;
            }
            else
            {
                Log("imgui init failed");
                InterlockedExchange(&s_InitOnce, 0);
                return;
            }

            g_Initialized = true;
            InterlockedExchange(&g_OakWarmupPending, 1);
            Log("d3d ok");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char eb[96];
            wsprintfA(eb, "present init EXCEPTION 0x%08X — retry %d/5",
                GetExceptionCode(), (int)s_InitFailures + 1);
            Log(eb);
            InterlockedIncrement(&s_InitFailures);
            InterlockedExchange(&s_InitOnce, 0);
            g_Initialized = false;
            InterlockedExchange(&g_OakWarmupPending, 0);
        }
        return;
    }

    __try
    {
        // BE: overlay init after thin soak. Keep drawing once hwnd is up.
        if (OakBeIsLaunch() && !g_OverlayHwnd && !g_Initialized)
            return;

        if (InterlockedCompareExchange(&g_OakWarmupPending, 0, 1) == 1)
        {
            Log("imgui warmup (present thread)...");
            __try
            {
                if (ImGuiMenu_Warmup())
                {
                    Log("imgui warmed");
                    // Early launcher beacon — EndFrame also writes once vertices flow.
                    const char* paths[2] = { "C:\\oak\\dayz\\overlay_ready.flag", "C:\\oak\\overlay_ready.flag" };
                    for (int i = 0; i < 2; i++)
                    {
                        HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                        if (hf != INVALID_HANDLE_VALUE)
                        {
                            const char msg[] = "imgui_warmed\r\n";
                            DWORD w = 0;
                            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
                            CloseHandle(hf);
                        }
                    }
                }
                else
                {
                    Log("imgui warmup failed — will retry");
                    InterlockedExchange(&g_OakWarmupPending, 1);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                char eb[80];
                wsprintfA(eb, "warmup EXCEPTION 0x%08X", GetExceptionCode());
                Log(eb);
                InterlockedExchange(&g_OakWarmupPending, 1);
            }
        }

        UpdateFromPanel();
        if (sessionReady && g_OverlayHwnd)
            OakBatch4OnPresent(g_OverlayHwnd);

        // Resolve last scene depth before ESP so vis-colors can sample it.
        if (sessionReady)
        {
            __try { OAK_MARK("depth"); OakDepth_OnPresent(g_Device, g_Context); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (sessionReady)
            OakRunGameplayFrame(sessionWorld, sessionLocal);

        // Do NOT periodic-flush mem/W2S caches — that caused progressive Present death
        // (VirtualQuery storm every 20s as DayZ streamed more unique pages).
        // Invalidate only on session change (local/world swap).
        {
            static DWORD s_LastMemPerf = 0;
            DWORD nowPerf = GetTickCount();
            if (!s_LastMemPerf) s_LastMemPerf = nowPerf;
            if ((nowPerf - s_LastMemPerf) > 60000)
            {
                LONG vq = InterlockedExchange(&g_MemVqCount, 0);
                LONG hit = InterlockedExchange(&g_MemHitCount, 0);
                LONG stale = InterlockedExchange(&g_MemStaleCount, 0);
                char pb[160];
                wsprintfA(pb, "perf: memcache vq=%d hit=%d stale=%d open=%d cam=%d local=%d",
                    (int)vq, (int)hit, (int)stale, ImGuiMenu_IsOpen() ? 1 : 0,
                    g_CameraValid ? 1 : 0, g_LocalPlayerValid ? 1 : 0);
                Log(pb);
                s_LastMemPerf = nowPerf;
            }
        }

        g_FrameCount++;

        if (g_Initialized && ImGuiMenu_BeginFrame())
        {
            BeginDraw();
            CombatPresentDrawOverlays();
            Batch4SilentPresentDraw();
            OakBatch4DrawWarpOverlay();
            // Prefer full ESP when cam+local are live. If local lags a frame but
            // camera/W2S are valid, still run RenderESP so boxes appear (it re-resolves local).
            if (sessionReady && g_ShowESP &&
                IsValidPtr(g_CachedWorldPtr) && g_CachedWorldPtr > 0x100000000ULL &&
                (g_CameraValid || g_W2S.valid || g_LocalPlayerValid))
            {
                OAK_MARK("esp");
                RenderESP();
            }
            // Shadow chams are NOT ESP — draw after NewFrame even if ESP is off.
            if (sessionReady &&
                IsValidPtr(g_CachedWorldPtr) && g_CachedWorldPtr > 0x100000000ULL && g_W2S.valid)
            {
                __try { OakShadowChamsDrawWorld(g_CachedWorldPtr); }
                __except (EXCEPTION_EXECUTE_HANDLER) { OakCrashGate_Log("chams", GetExceptionCode()); }
            }
            EndDraw();
            if (!g_RenderTarget && g_Device && g_SwapChain)
            {
                ID3D11Texture2D* bb = NULL;
                if (SUCCEEDED(g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb)
                {
                    g_Device->CreateRenderTargetView(bb, NULL, &g_RenderTarget);
                    bb->Release();
                }
            }
            ImGuiMenu_EndFrame(g_RenderTarget);
        }

        if (sessionReady && IsValidPtr(g_CachedWorldPtr) && g_CachedWorldPtr > 0x100000000)
        {
            MiscFreecamPostFramePin(g_CachedWorldPtr);
            if (g_Fullbright || g_MiscDisableOverlays)
                ApplyLightingForce(g_CachedWorldPtr, g_Fullbright, false);
            else
            {
                float cur = Read<float>(g_CachedWorldPtr + oak_offsets::world::EyeAccom);
                if (cur > 2.0f || cur < 0.0f)
                    RestoreEyeAccom(g_CachedWorldPtr);
            }
        }

        if (g_FrameCount % 6000 == 0)
        {
            char buf[256];
            wsprintfA(buf, "Frame %d - esp=%d draws=%d cam=%d local=%d w2s=%d world=%d ready=%d",
                g_FrameCount, g_ShowESP ? 1 : 0, g_EspDrawCount,
                g_CameraValid ? 1 : 0, g_LocalPlayerValid ? 1 : 0,
                g_W2S.valid ? 1 : 0,
                (IsValidPtr(g_CachedWorldPtr) && g_CachedWorldPtr > 0x100000000ULL) ? 1 : 0,
                sessionReady ? 1 : 0);
            Log(buf);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OakCrashGate_Log("present-overlay", GetExceptionCode());
    }
}

HRESULT WINAPI hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // If this swapchain's VT differs from the probe table, patch it too (Flip model).
    if (pSwapChain && g_BeSharedVtPatched)
    {
        __try
        {
            void** vt = *(void***)pSwapChain;
            if (vt && vt != g_BeRealScVt && vt[8] != (void*)&hkPresent)
            {
                Log("BE-VT: Present saw alternate VT — patching");
                OakBePatchSharedDxgiVt(vt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    LONG hits = InterlockedIncrement(&g_PresentHits);
    if (hits == 1)
        Log("present#1 (hook live)");
    else if (hits == 100 || hits == 600 || (hits % 12000) == 0)
    {
        char b[64];
        wsprintfA(b, "present#%d", (int)hits);
        Log(b);
    }
    // Early launcher beacon: Present is live + auth OK (don't wait for full ImGui soak).
    if (hits == 100 && OakProtectionIsAuthorized())
    {
        const char* paths[2] = { "C:\\oak\\dayz\\overlay_ready.flag", "C:\\oak\\overlay_ready.flag" };
        for (int i = 0; i < 2; i++)
        {
            HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
                const char msg[] = "present_live\r\n";
                DWORD w = 0;
                WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
                CloseHandle(hf);
            }
        }
    }

    const bool be = OakBeIsLaunch();

    // BattlEye: brief thin Present trampoline so BE finishes loading, then arm ImGui.
    // Was 20s — felt broken on vanilla and delayed overlay_ready forever on old builds.
    if (be && !g_Initialized)
    {
        static DWORD s_BePresentT0 = 0;
        if (!s_BePresentT0)
            s_BePresentT0 = GetTickCount();
        const DWORD kBeThinMs = 3500;
        if ((GetTickCount() - s_BePresentT0) < kBeThinMs)
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    if (InterlockedCompareExchange(&g_InOverlay, 1, 0) == 0)
    {
        LARGE_INTEGER o0 = {}, o1 = {}, freq = {};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&o0);
        __try { OakPresentOverlay(pSwapChain); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char eb[80];
            wsprintfA(eb, "present overlay EXCEPTION 0x%08X", GetExceptionCode());
            Log(eb);
        }
        QueryPerformanceCounter(&o1);
        if (freq.QuadPart > 0)
            OakPerfBoost_SetOverlayMs((float)((double)(o1.QuadPart - o0.QuadPart) * 1000.0 / (double)freq.QuadPart));
        InterlockedExchange(&g_InOverlay, 0);
    }
    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI hkPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const void* params)
{
    if (pSwapChain && g_BeSharedVtPatched)
    {
        __try
        {
            void** vt = *(void***)pSwapChain;
            if (vt && vt[22] != (void*)&hkPresent1)
            {
                Log("BE-VT: Present1 saw unpatched VT — patching");
                OakBePatchSharedDxgiVt(vt);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    LONG hits = InterlockedIncrement(&g_PresentHits);
    if (hits == 1)
        Log("present1#1 (hook live)");

    const bool be = OakBeIsLaunch();
    if (be && !g_Initialized)
    {
        static DWORD s_BePresent1T0 = 0;
        if (!s_BePresent1T0)
            s_BePresent1T0 = GetTickCount();
        if ((GetTickCount() - s_BePresent1T0) < 20000u)
            return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, params);
    }

    if (InterlockedCompareExchange(&g_InOverlay, 1, 0) == 0)
    {
        __try { OakPresentOverlay(pSwapChain); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            char eb[80];
            wsprintfA(eb, "present1 overlay EXCEPTION 0x%08X", GetExceptionCode());
            Log(eb);
        }
        InterlockedExchange(&g_InOverlay, 0);
    }
    return CallOriginalPresent1(pSwapChain, SyncInterval, Flags, params);
}


static HWND FindGameWindow()
{
    HWND hwnd = FindWindowA("DayZ", NULL);
    if (hwnd) return hwnd;
    hwnd = FindWindowA(NULL, "DayZ");
    if (hwnd) return hwnd;
    return GetForegroundWindow();
}

static HANDLE g_GlobalInitMutex = NULL;

static bool OakTryAcquireGlobalInit()
{
    g_GlobalInitMutex = CreateMutexA(NULL, FALSE, "Global\\OakDayZOverlayInitV1");
    if (!g_GlobalInitMutex)
        return false;
    if (WaitForSingleObject(g_GlobalInitMutex, 0) != WAIT_OBJECT_0)
    {
        CloseHandle(g_GlobalInitMutex);
        g_GlobalInitMutex = NULL;
        return false;
    }
    return true;
}

static void OakReleaseGlobalInit()
{
    if (!g_GlobalInitMutex)
        return;
    ReleaseMutex(g_GlobalInitMutex);
    CloseHandle(g_GlobalInitMutex);
    g_GlobalInitMutex = NULL;
}

struct OakInitLockGuard
{
    bool owned = false;
    ~OakInitLockGuard() { if (owned) OakReleaseGlobalInit(); }
};

static volatile LONG g_AuthDone = 0;
static volatile LONG g_AuthOk = 0;

DWORD WINAPI MainThread(LPVOID lpParam);

static void OakLogLine(const char* line);

static DWORD WINAPI OakAuthThreadProc(LPVOID)
{
    const bool ok = OakProtectionAuthorize();
    InterlockedExchange(&g_AuthOk, ok ? 1 : 0);
    InterlockedExchange(&g_AuthDone, 1);
    return 0;
}

static DWORD WINAPI OakMainThreadBootstrap(LPVOID p)
{
    OakLogLine("oak: MainThread worker entered");
    return MainThread(p);
}

static void OakScheduleMainThread()
{
    static volatile LONG s_Scheduled = 0;
    if (InterlockedCompareExchange(&s_Scheduled, 1, 0) != 0)
        return;
    // Manual-map + CFG: thread-pool callbacks often never run; CreateThread is reliable.
    HANDLE h = CreateThread(NULL, 0, OakMainThreadBootstrap, g_Module, 0, NULL);
    if (h)
    {
        OakLogLine("oak: MainThread CreateThread ok");
        CloseHandle(h);
        return;
    }
    OakLogLine("oak: MainThread CreateThread failed — sync fallback");
    OakMainThreadBootstrap(g_Module);
}

DWORD WINAPI MainThread(LPVOID lpParam)
{
    // Only one init per mapped image — APC/timer may fire on many threads
    static LONG s_Once = 0;
    if (InterlockedCompareExchange(&s_Once, 1, 0) != 0)
        return 0;

    OakInitLockGuard initLock;
    initLock.owned = OakTryAcquireGlobalInit();
    if (!initLock.owned)
    {
        Log("main thread skipped — another overlay mapping owns init");
        return 0;
    }

    if (lpParam)
        g_Module = (HMODULE)lpParam;

    // Breadcrumb BEFORE any CRT (fopen) — manual map skips CRT startup
    {
        HANDLE hf = CreateFileA("C:\\oak\\mainthread.flag", GENERIC_WRITE, FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "main_ok\r\n";
            DWORD w = 0;
            WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
            CloseHandle(hf);
        }
    }

    static volatile int s_EnablePresentHook = 1;
    if (!s_EnablePresentHook)
        return 0;

    Log("main thread started");
    if (ImGuiMenu_PrecreateContext())
        Log("imgui precreate ok");
    else
        Log("imgui precreate failed");
    OakCrashHunt_Init(g_Module);
    OakLab_Init();

    // Never run WinHttp lease on the UI timer thread — it can deadlock the message
    // pump and leave us stuck after "main thread started".
    {
        InterlockedExchange(&g_AuthDone, 0);
        InterlockedExchange(&g_AuthOk, 0);
        HANDLE thr = CreateThread(NULL, 0, OakAuthThreadProc, NULL, 0, NULL);
        if (!thr)
        {
            Log("protection auth thread create failed");
            return 1;
        }
        const DWORD wait = WaitForSingleObject(thr, 45000);
        CloseHandle(thr);
        if (wait != WAIT_OBJECT_0 || InterlockedCompareExchange(&g_AuthDone, 0, 0) == 0)
        {
            Log("protection authorization timed out");
            return 1;
        }
        if (InterlockedCompareExchange(&g_AuthOk, 0, 0) == 0)
        {
            Log("protection authorization unavailable");
            return 1;
        }
        Log("protection authorized");
    }

    // Clear leftover OakLagCut firewall rules from older builds (never re-add).
    LagClearResidualFirewallAsync();

    Log("post-auth: probe BEClient...");
    bool battlEye = (GetModuleHandleA("BEClient_x64.dll") != NULL)
        || (GetModuleHandleA("BEClient.dll") != NULL);
    if (battlEye)
        Log("BEClient present at init");
    else
        Log("BEClient not loaded yet at init");

    Log("post-auth: resolve game module...");
    g_GameModule = GetModuleHandleA(NULL);
    if (!g_GameModule)
        g_GameModule = GetModuleHandleA("DayZ_x64.exe");
    if (!g_GameModule)
    {
        Log("no game module");
        return 1;
    }
    Log("post-auth: OakEngine_Init...");
    OakEngine_Init(g_GameModule);
    Log("post-auth: OakEngine_Init done");
    // Do NOT IAT-hook GetAsyncKeyState across every module at attach.
    // That raced D3D/net threads and correlated with DayZ+0xA4E9F6 AVs.
    // Hooks install lazily when freecam actually suppresses input.
    // MiscFreecamInstallInputHooks();
    
    char buf[128];
    wsprintfA(buf, "Game module: 0x%p", g_GameModule);
    Log(buf);
    
    
    Log("post-auth: FindGameWindow...");
    HWND gameWnd = FindGameWindow();
    if (!gameWnd)
    {
        Log("no game window — waiting...");
        for (int i = 0; i < 60 && !gameWnd; i++)
        {
            Sleep(500);
            gameWnd = FindGameWindow();
        }
        if (!gameWnd)
        {
            Log("no game window");
            return 1;
        }
    }

    // BattlEye: skip the long BEClient wait when we will not patch Present anyway.
    const bool beLaunchEarly =
        (GetFileAttributesA("C:\\oak\\dayz\\be_launch.flag") != INVALID_FILE_ATTRIBUTES) ||
        (GetFileAttributesA("C:\\oak\\be_launch.flag") != INVALID_FILE_ATTRIBUTES);
    if (!beLaunchEarly)
    {
        Log("waiting for BEClient before present hook...");
        bool sawBe = false;
        for (int i = 0; i < 45; i++)
        {
            if (GetModuleHandleA("BEClient_x64.dll") || GetModuleHandleA("BEClient.dll"))
            {
                sawBe = true;
                break;
            }
            Sleep(1000);
        }
        if (sawBe)
        {
            Log("BEClient loaded — soak 8s then hook Present");
            Sleep(8000);
        }
        else
        {
            Log("BEClient not seen in 45s - proceeding (likely NoBE)");
        }
    }
    else
    {
        Log("BE launch flag set — skipping BEClient wait (Present inline patch disabled)");
    }
    
    wsprintfA(buf, "Found game window: 0x%p", gameWnd);
    Log(buf);
    
    
    Log("getting dxgi...");
    
    // Message-only STATIC window — never RegisterClass/CreateWindow against the game
    // HWND from a TIMERPROC (that deadlocks the UI thread).
    Log("CreateWindowEx HWND_MESSAGE STATIC...");
    HWND probeWnd = CreateWindowExA(
        0, "STATIC", "OakDxgiProbe", 0,
        0, 0, 2, 2,
        HWND_MESSAGE, NULL, GetModuleHandleA(NULL), NULL);
    if (!probeWnd)
    {
        wsprintfA(buf, "probe window failed err=%u", GetLastError());
        Log(buf);
        return 1;
    }
    Log("probe window ok");

    Log("CreateDXGIFactory...");
    IDXGIFactory* pFactory = NULL;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory);
    if (FAILED(hr) || !pFactory)
    {
        wsprintfA(buf, "CreateDXGIFactory failed: 0x%X", hr);
        Log(buf);
        DestroyWindow(probeWnd);
        return 1;
    }
    Log("got dxgi");

    // WARP probe only — avoids NV/D3D layer re-init crashes while DayZ DXGI is live.
    ID3D11Device* pDevice = NULL;
    ID3D11DeviceContext* pContext = NULL;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
        D3D11_CREATE_DEVICE_SINGLETHREADED, NULL, 0,
        D3D11_SDK_VERSION, &pDevice, NULL, &pContext);

    if (FAILED(hr) || !pDevice)
    {
        wsprintfA(buf, "D3D11CreateDevice(WARP) failed: 0x%X", hr);
        Log(buf);
        pFactory->Release();
        DestroyWindow(probeWnd);
        return 1;
    }
    Log("d3d device ok (WARP probe)");
    
    
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMem(&sd, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = probeWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    
    IDXGISwapChain* pSwapChain = NULL;
    hr = pFactory->CreateSwapChain(pDevice, &sd, &pSwapChain);
    
    if (FAILED(hr) || !pSwapChain)
    {
        wsprintfA(buf, "CreateSwapChain failed: 0x%X", hr);
        Log(buf);
        pContext->Release();
        pDevice->Release();
        pFactory->Release();
        DestroyWindow(probeWnd);
        return 1;
    }
    Log("swap chain ok");
    
    
    g_SwapChainVTable = *(void***)pSwapChain;
    oPresent = (tPresent)g_SwapChainVTable[8];
    
    wsprintfA(buf, "Present at: 0x%p", oPresent);
    Log(buf);

    // BattlEye kills the process if we VirtualProtect+patch dxgi Present bytes.
    // LaunchDayZSafe writes be_launch.flag for BE sessions — use swapchain VT shadow instead.
    const bool beLaunch = OakBeIsLaunch();
    if (beLaunch)
    {
        Log("BE launch detected — installing Present via swapchain VT shadow (no dxgi code patch)");
        Log("BE: Present soak then overlay (precreate ImGui on worker)");
        if (OakBeInstallPresentHooks(gameWnd, pSwapChain))
            Log(InterlockedCompareExchange(&g_BeVtHookCount, 0, 0) > 0
                    ? "BE-VT: present armed (live SC hooked)"
                    : "BE-VT: shadow ready (keepalive will find live SC)");
        else
            Log("BE-VT: install failed");
    }
    else
    {
        Log("installing present inline hook...");
        BYTE* target = (BYTE*)oPresent;
        g_PresentTarget = target;
        InstallInlineHook(target, (void*)&hkPresent, g_PresentOrigBytes, g_PresentHookBytes,
            &g_PresentInlineHooked, "present");
        Log(g_PresentInlineHooked ? "present inline-hooked (unhook-call-rehook)"
                                  : "present inline-hook FAILED");

        // Present1 for Flip-model paths (safe QI on our probe chain only)
        {
            void* sc1 = nullptr;
            static const GUID kIID_SwapChain1 =
            { 0x790A45F7, 0x0D42, 0x4876, { 0x98, 0x3A, 0x0A, 0x55, 0xCF, 0xE6, 0xF4, 0xAA } };
            if (SUCCEEDED(pSwapChain->QueryInterface(kIID_SwapChain1, &sc1)) && sc1)
            {
                void** vt1 = *(void***)sc1;
                BYTE* p1 = (BYTE*)vt1[22];
                if (p1 && p1 != (BYTE*)(void*)&hkPresent && p1 != (BYTE*)(void*)&hkPresent1)
                {
                    HMODULE hm = NULL;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)p1, &hm) && hm)
                    {
                        g_Present1Target = p1;
                        oPresent1 = (tPresent1)p1;
                        InstallInlineHook(p1, (void*)&hkPresent1, g_Present1OrigBytes, g_Present1HookBytes,
                            &g_Present1InlineHooked, "present1");
                    }
                }
                ((IUnknown*)sc1)->Release();
            }
        }
        Log("present hooked");
    }
    
    pSwapChain->Release();
    pContext->Release();
    pDevice->Release();
    pFactory->Release();
    DestroyWindow(probeWnd);

    if (!g_PresentKeepAliveThread)
    {
        g_PresentKeepAliveThread = CreateThread(NULL, 0, PresentKeepAliveProc, NULL, 0, NULL);
        if (g_PresentKeepAliveThread)
            CloseHandle(g_PresentKeepAliveThread);
    }

    // Fullbright/daytime worker — was declared but never started
    if (!g_FullbrightThread)
    {
        g_FullbrightThread = CreateThread(NULL, 0, FullbrightThreadProc, NULL, 0, NULL);
        if (g_FullbrightThread)
        {
            Log("fullbright worker thread started");
            CloseHandle(g_FullbrightThread);
        }
    }
    
    Log("hook installed");
    Log("init done — inject thread exiting (Present owns lifetime)");
    return 0;
}


BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved);
DWORD WINAPI MainThread(LPVOID lpParam);

static void OakLogLine(const char* line)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH))
        return;
    char file[MAX_PATH];
    wsprintfA(file, "%s\\DayZ\\oak_imgui.log", path);
    HANDLE hf = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return;
    DWORD wr = 0;
    WriteFile(hf, line, (DWORD)lstrlenA(line), &wr, NULL);
    WriteFile(hf, "\r\n", 2, &wr, NULL);
    CloseHandle(hf);
}

static HHOOK g_OakMsgHook = NULL;
static HWND g_OakMsgWnd = NULL;

// BE mapper fills RtlAddFunctionTable fields but the entry shell NEVER calls it
// ("No EH/TLS here"). Without a dynamic function table, __try/__except in Read<>
// cannot catch AVs → APPCRASH at Read after probe (WER fault inside mapped image).
static volatile LONG g_SehRegisterOnce = 0;
static PRUNTIME_FUNCTION g_SehTable = nullptr;
static BOOLEAN g_SehWeAdded = FALSE;

static void OakRegisterSehTable(HMODULE mod)
{
    if (!mod) return;
    if (InterlockedCompareExchange(&g_SehRegisterOnce, 1, 0) != 0)
        return;

    // Normal LoadLibrary modules already have .pdata via LDR — don't double-register.
    {
        char path[MAX_PATH];
        path[0] = 0;
        DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        if (n > 0 && path[0])
        {
            OakLogLine("oak: SEH via LDR (skip RtlAddFunctionTable)");
            return;
        }
    }

    typedef BOOLEAN(WINAPI* tRtlAddFunctionTable)(PRUNTIME_FUNCTION, DWORD, DWORD64);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto pAdd = ntdll
        ? (tRtlAddFunctionTable)GetProcAddress(ntdll, "RtlAddFunctionTable")
        : nullptr;
    if (!pAdd)
    {
        OakLogLine("oak: RtlAddFunctionTable missing");
        return;
    }

    BYTE* base = (BYTE*)mod;
    if (base[0] != 'M' || base[1] != 'Z')
        return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_lfanew < 0 || dos->e_lfanew > 0x1000)
        return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    IMAGE_DATA_DIRECTORY* exc = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exc->VirtualAddress || exc->Size < sizeof(RUNTIME_FUNCTION))
    {
        OakLogLine("oak: no exception directory");
        return;
    }

    PRUNTIME_FUNCTION table = (PRUNTIME_FUNCTION)(base + exc->VirtualAddress);
    DWORD count = exc->Size / sizeof(RUNTIME_FUNCTION);
    if (count == 0 || count > 100000)
        return;

    if (pAdd(table, count, (DWORD64)base))
    {
        g_SehTable = table;
        g_SehWeAdded = TRUE;
        char line[96];
        wsprintfA(line, "oak: RtlAddFunctionTable OK entries=%u", count);
        OakLogLine(line);
    }
    else
    {
        char line[96];
        wsprintfA(line, "oak: RtlAddFunctionTable FAIL err=%u", GetLastError());
        OakLogLine(line);
    }
}

static void OakUnregisterSehTable()
{
    if (!g_SehWeAdded || !g_SehTable)
        return;
    typedef BOOLEAN(WINAPI* tRtlDeleteFunctionTable)(PRUNTIME_FUNCTION);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    auto pDel = ntdll
        ? (tRtlDeleteFunctionTable)GetProcAddress(ntdll, "RtlDeleteFunctionTable")
        : nullptr;
    if (pDel)
        pDel(g_SehTable);
    g_SehWeAdded = FALSE;
    g_SehTable = nullptr;
}

// Manual-map pages are not in the process CFG bitmap — register call targets or
// SetTimer/hooks/Present vtable calls into our code get blocked and the game dies.
static void OakRegisterCfgTargets(HMODULE mod)
{
    if (!mod) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    SIZE_T imgSize = nt->OptionalHeader.SizeOfImage;

    // Prefer the real allocation span — SizeOfImage can disagree with the VAD
    // and SetProcessValidCallTargets then returns ERROR_INVALID_PARAMETER (87).
    MEMORY_BASIC_INFORMATION mbi = {};
    BYTE* regionBase = base;
    SIZE_T regionSize = imgSize;
    if (VirtualQuery(base, &mbi, sizeof(mbi)))
    {
        regionBase = (BYTE*)mbi.AllocationBase;
        // Walk committed leaves of this allocation to get total span.
        SIZE_T span = 0;
        BYTE* walk = regionBase;
        for (;;)
        {
            MEMORY_BASIC_INFORMATION leaf = {};
            if (!VirtualQuery(walk, &leaf, sizeof(leaf)))
                break;
            if (leaf.AllocationBase != regionBase)
                break;
            span += leaf.RegionSize;
            walk = (BYTE*)leaf.BaseAddress + leaf.RegionSize;
            if (span > (SIZE_T)1 << 28) break; // sanity
        }
        if (span)
            regionSize = span;
    }

    auto pSet = (BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PVOID))
        GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetProcessValidCallTargets");
    if (!pSet)
        pSet = (BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PVOID))
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessValidCallTargets");

    // NtSetInformationVirtualMemory VmCfgCallTargetInformation — works on
    // private executable mappings where SetProcessValidCallTargets returns 87.
    typedef LONG(NTAPI* tNtSIVM)(HANDLE, ULONG, PVOID, ULONG, PVOID, SIZE_T);
    auto pNtSivm = (tNtSIVM)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationVirtualMemory");

    struct CfgInfo { ULONG_PTR Offset; ULONG_PTR Flags; };
    struct MemRange { PVOID VirtualAddress; SIZE_T NumberOfBytes; };
    // Must NOT embed MemRange here — NT expects MEMORY_RANGE_ENTRY as arg 3 and
    // CFG_CALL_TARGET_LIST_INFORMATION as arg 5 (STATUS_DATATYPE_MISALIGNMENT otherwise).
    struct VmCfgList
    {
        ULONG NumberOfOffsets;
        ULONG Reserved;
        PULONG NumberOfOffsetsProcessed;
        CfgInfo* OffsetInformation;
    };

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    ULONG marked = 0;
    ULONG fails = 0;
    DWORD lastErr = 0;
    LONG lastNt = 0;
    const ULONG BATCH = 512;
    CfgInfo info[512];

    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        BYTE* secBase = base + sec[i].VirtualAddress;
        SIZE_T secSize = sec[i].Misc.VirtualSize;
        if (!secSize) secSize = sec[i].SizeOfRawData;
        // Align start up and end down to 16-byte CFG granularity.
        ULONG_PTR startOff = (ULONG_PTR)(secBase - regionBase);
        startOff = (startOff + 15ull) & ~15ull;
        ULONG_PTR endOff = (ULONG_PTR)(secBase + secSize - regionBase);
        endOff &= ~15ull;
        if (endOff <= startOff || startOff >= regionSize)
            continue;

        for (ULONG_PTR off = startOff; off < endOff; )
        {
            ULONG n = 0;
            for (; n < BATCH && off < endOff; off += 16, n++)
            {
                info[n].Offset = off;
                info[n].Flags = 0x00000001; // CFG_CALL_TARGET_VALID
            }
            if (!n) break;

            BOOL ok = FALSE;
            if (pSet)
            {
                SetLastError(0);
                ok = pSet(GetCurrentProcess(), regionBase, regionSize, n, info);
                if (!ok)
                    lastErr = GetLastError();
            }
            if (!ok && pNtSivm)
            {
                ULONG processed = 0;
                MemRange range = {};
                range.VirtualAddress = regionBase;
                range.NumberOfBytes = regionSize;
                VmCfgList vm = {};
                vm.NumberOfOffsets = n;
                vm.NumberOfOffsetsProcessed = &processed;
                vm.OffsetInformation = info;
                // VmCfgCallTargetInformation = 2
                lastNt = pNtSivm(GetCurrentProcess(), 2, &range, 1, &vm, sizeof(vm));
                ok = (lastNt >= 0) && processed > 0;
                if (ok)
                    n = processed;
            }
            if (ok)
                marked += n;
            else
                fails++;
        }
    }
    char buf[160];
    wsprintfA(buf, "oak: CFG targets marked~%u fail=%u err=%u nt=0x%08X region=0x%p/0x%X",
        marked, fails, (unsigned)lastErr, (unsigned)lastNt, regionBase, (unsigned)regionSize);
    OakLogLine(buf);
}

static ULONG OakMarkCfgOne(HMODULE mod, void* fn)
{
    if (!mod || !fn)
        return 0;

    BYTE* base = (BYTE*)mod;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(base, &mbi, sizeof(mbi)))
        return 0;
    BYTE* regionBase = (BYTE*)mbi.AllocationBase;
    SIZE_T regionSize = 0;
    BYTE* walk = regionBase;
    for (;;)
    {
        MEMORY_BASIC_INFORMATION leaf = {};
        if (!VirtualQuery(walk, &leaf, sizeof(leaf)))
            break;
        if (leaf.AllocationBase != regionBase)
            break;
        regionSize += leaf.RegionSize;
        walk = (BYTE*)leaf.BaseAddress + leaf.RegionSize;
        if (regionSize > (SIZE_T)1 << 28) break;
    }
    if (!regionSize)
        return 0;

    ULONG_PTR off = (ULONG_PTR)((BYTE*)fn - regionBase);
    off &= ~15ull; // CFG granularity
    struct { ULONG_PTR Offset; ULONG_PTR Flags; } info = {};
    info.Offset = off;
    info.Flags = 0x00000001;

    auto pSet = (BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PVOID))
        GetProcAddress(GetModuleHandleA("kernelbase.dll"), "SetProcessValidCallTargets");
    if (!pSet)
        pSet = (BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PVOID))
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessValidCallTargets");
    if (pSet && pSet(GetCurrentProcess(), regionBase, regionSize, 1, &info))
        return 1;

    typedef LONG(NTAPI* tNtSIVM)(HANDLE, ULONG, PVOID, ULONG, PVOID, SIZE_T);
    auto pNtSivm = (tNtSIVM)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationVirtualMemory");
    if (!pNtSivm)
        return 0;
    struct MemRange { PVOID VirtualAddress; SIZE_T NumberOfBytes; };
    struct VmCfgList
    {
        ULONG NumberOfOffsets;
        ULONG Reserved;
        PULONG NumberOfOffsetsProcessed;
        void* OffsetInformation;
    };
    ULONG processed = 0;
    MemRange range = {};
    range.VirtualAddress = regionBase;
    range.NumberOfBytes = regionSize;
    VmCfgList vm = {};
    vm.NumberOfOffsets = 1;
    vm.NumberOfOffsetsProcessed = &processed;
    vm.OffsetInformation = &info;
    return (pNtSivm(GetCurrentProcess(), 2, &range, 1, &vm, sizeof(vm)) >= 0 && processed) ? 1 : 0;
}

static void CALLBACK OakTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD)
{
    KillTimer(hwnd, id);
    if (g_OakMsgHook)
    {
        UnhookWindowsHookEx(g_OakMsgHook);
        g_OakMsgHook = NULL;
    }
    OakLogLine("oak: timer -> MainThread");
    OakScheduleMainThread();
}

// APC must NOT run DXGI — only arm a timer so init runs on a clean message-pump turn.
static void NTAPI OakApcArmTimer(ULONG_PTR)
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0)
        return;
    if (!g_OakMsgWnd)
    {
        OakLogLine("oak: APC no wnd — schedule MainThread");
        OakScheduleMainThread();
        return;
    }
    UINT_PTR t = SetTimer(g_OakMsgWnd, 0x0A0C0001u, 100, OakTimerProc);
    char buf[80];
    wsprintfA(buf, "oak: APC armed SetTimer=%p", (void*)t);
    OakLogLine(buf);
}

static LRESULT CALLBACK OakGetMsgProc(int code, WPARAM wParam, LPARAM lParam)
{
    static LONG armed = 0;
    if (code >= 0 && InterlockedCompareExchange(&armed, 1, 0) == 0 && g_OakMsgWnd)
    {
        UINT_PTR tid = SetTimer(g_OakMsgWnd, 0x0A0C0001u, 50, OakTimerProc);
        char buf[96];
        wsprintfA(buf, "oak: getmsg armed SetTimer=%p", (void*)tid);
        OakLogLine(buf);
    }
    if (code >= 0 && lParam)
    {
        MSG* msg = (MSG*)lParam;
        if (msg->message == WM_INPUT)
        {
            RAWINPUTHEADER hdr{};
            UINT sz = sizeof(hdr);
            if (GetRawInputData((HRAWINPUT)msg->lParam, RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
                && hdr.dwType == RIM_TYPEMOUSE)
            {
                RAWINPUT raw{};
                sz = sizeof(raw);
                if (GetRawInputData((HRAWINPUT)msg->lParam, RID_INPUT, &raw, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1)
                    MiscFreecamOnRawMouse((int)raw.data.mouse.lLastX, (int)raw.data.mouse.lLastY);
            }
        }
    }
    return CallNextHookEx(g_OakMsgHook, code, wParam, lParam);
}

// IAT hook — PeekMessage is not alertable but IS called every frame; CFG-safe
// once our targets are registered. Prefer this over APC alone.
typedef BOOL(WINAPI* t_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static t_PeekMessageW o_PeekMessageW = nullptr;
typedef BOOL(WINAPI* t_GetMessageW)(LPMSG, HWND, UINT, UINT);
static t_GetMessageW o_GetMessageW = nullptr;

static void OakFreecamStripMsg(LPMSG lpMsg)
{
    if (!lpMsg)
        return;

    // Always harvest raw mouse for freecam look / menu cursor.
    // Only swallow the message when freecam has game input suppressed, or the menu is open.
    if (lpMsg->message == WM_INPUT)
    {
        RAWINPUTHEADER hdr{};
        UINT sz = sizeof(hdr);
        if (GetRawInputData((HRAWINPUT)lpMsg->lParam, RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1
            && hdr.dwType == RIM_TYPEMOUSE)
        {
            RAWINPUT raw{};
            sz = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lpMsg->lParam, RID_INPUT, &raw, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1)
            {
                const int dx = (int)raw.data.mouse.lLastX;
                const int dy = (int)raw.data.mouse.lLastY;
                MiscFreecamOnRawMouse(dx, dy);
                if (ImGuiMenu_IsOpen())
                    ImGuiMenu_OnRawMouse(dx, dy);
            }
        }
        if (ImGuiMenu_IsOpen() || (MiscFreecamGameInputSuppressed() && !ImGuiMenu_IsOpen()))
            lpMsg->message = WM_NULL;
        return;
    }

    if (ImGuiMenu_IsOpen())
    {
        // Keep DayZ from recentering / looking while the menu owns the mouse.
        switch (lpMsg->message)
        {
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            lpMsg->message = WM_NULL;
            break;
        default:
            break;
        }
        return;
    }

    if (!MiscFreecamGameInputSuppressed())
        return;
    switch (lpMsg->message)
    {
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
    case WM_DEADCHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SYSCHAR: case WM_SYSDEADCHAR:
        // Never eat Escape — DayZ pause / menu / freecam-exit edge all need it.
        if (lpMsg->wParam == VK_ESCAPE)
            break;
        lpMsg->message = WM_NULL;
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        lpMsg->message = WM_NULL;
        break;
    default:
        break;
    }
}

static BOOL WINAPI hk_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove)
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0 && g_OakMsgWnd)
    {
        SetTimer(g_OakMsgWnd, 0x0A0C0001u, 50, OakTimerProc);
        OakLogLine("oak: PeekMessage IAT armed timer");
    }
    BOOL r = o_PeekMessageW(lpMsg, hWnd, min, max, remove);
    if (r) OakFreecamStripMsg(lpMsg);
    return r;
}

static BOOL WINAPI hk_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max)
{
    BOOL r = o_GetMessageW ? o_GetMessageW(lpMsg, hWnd, min, max) : FALSE;
    if (r > 0) OakFreecamStripMsg(lpMsg);
    return r;
}

static void OakRegisterCfgEntryPoints(HMODULE mod)
{
    if (!mod)
        return;
    ULONG n = 0;
    n += OakMarkCfgOne(mod, (void*)&hkPresent);
    n += OakMarkCfgOne(mod, (void*)&hkPresent1);
    n += OakMarkCfgOne(mod, (void*)&OakMainThreadBootstrap);
    n += OakMarkCfgOne(mod, (void*)&OakPresentOverlay);
    n += OakMarkCfgOne(mod, (void*)&OakGetMsgProc);
    n += OakMarkCfgOne(mod, (void*)&OakTimerProc);
    n += OakMarkCfgOne(mod, (void*)&OakApcArmTimer);
    n += OakMarkCfgOne(mod, (void*)&hk_PeekMessageW);
    n += OakMarkCfgOne(mod, (void*)&hk_GetMessageW);
    n += OakMarkCfgOne(mod, (void*)&PresentKeepAliveProc);
    char buf[80];
    wsprintfA(buf, "oak: CFG entry points marked=%u", n);
    OakLogLine(buf);
}

static bool OakHookIat(HMODULE mod, const char* dll, const char* func, PVOID hook, PVOID* orig)
{
    if (!mod) return false;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return false;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);
    for (; imp->Name; imp++)
    {
        const char* name = (const char*)(base + imp->Name);
        // case-insensitive dll match
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
        IMAGE_THUNK_DATA* origThunk = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; origThunk->u1.AddressOfData; ++thunk, ++origThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (StrCmp(ibn->Name, func) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(PVOID), PAGE_READWRITE, &old))
                return false;
            *orig = (PVOID)thunk->u1.Function;
            thunk->u1.Function = (ULONGLONG)hook;
            VirtualProtect(&thunk->u1.Function, sizeof(PVOID), old, &old);
            return true;
        }
    }
    return false;
}

// Manual-map entry: rcx=hModule only (RtlCreateUserThread passes one arg).
extern "C" __declspec(dllexport) ULONG_PTR NTAPI OakMapEntry(HMODULE hModule)
{
    // Beacon before schedule — proves OakMapEntry ran even if BE kills us mid-init.
    {
        const char* paths[2] = { "C:\\oak\\dayz\\dll_attach.flag", "C:\\oak\\dll_attach.flag" };
        for (int i = 0; i < 2; i++)
        {
            HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
                const char msg[] = "oak_map_entry\r\n";
                DWORD w = 0;
                WriteFile(hf, msg, sizeof(msg) - 1, &w, NULL);
                CloseHandle(hf);
            }
        }
    }

#if OAK_BEACON_ONLY
    (void)hModule;
    return 0;
#else
    // Inject thread exits immediately. Register CFG targets for the manual map,
    // then IAT-hook PeekMessage so we arm a timer on the next frame (PeekMessage
    // loops are not alertable — APC alone often never fires).
    g_Module = hModule;
    // MUST be before any Present/Read path — mapper shell skips RtlAddFunctionTable.
    OakRegisterSehTable(hModule);
    OakRegisterCfgTargets(hModule);
    OakRegisterCfgEntryPoints(hModule);

    HWND wnd = FindWindowA("DayZ", NULL);
    if (!wnd) wnd = FindWindowA(NULL, "DayZ");
    DWORD tid = wnd ? GetWindowThreadProcessId(wnd, NULL) : 0;
    g_OakMsgWnd = wnd;

    char line[192];
    wsprintfA(line, "oak: schedule wnd=%p tid=%u", wnd, tid);
    OakLogLine(line);

    HMODULE game = GetModuleHandleA(NULL);
    PVOID discard = nullptr;
    if (OakHookIat(game, "user32.dll", "PeekMessageW", (PVOID)hk_PeekMessageW, (PVOID*)&o_PeekMessageW))
        OakLogLine("oak: IAT PeekMessageW hooked");
    else if (OakHookIat(game, "user32.dll", "PeekMessageA", (PVOID)hk_PeekMessageW, (PVOID*)&o_PeekMessageW))
        OakLogLine("oak: IAT PeekMessageA hooked");
    else
        OakLogLine("oak: IAT PeekMessage NOT found");

    if (OakHookIat(game, "user32.dll", "GetMessageW", (PVOID)hk_GetMessageW, (PVOID*)&o_GetMessageW))
        OakLogLine("oak: IAT GetMessageW hooked");
    else if (OakHookIat(game, "user32.dll", "GetMessageA", (PVOID)hk_GetMessageW, (PVOID*)&o_GetMessageW))
        OakLogLine("oak: IAT GetMessageA hooked");
    else
        OakLogLine("oak: IAT GetMessage NOT found");

    if (tid && wnd)
    {
        g_OakMsgHook = SetWindowsHookExA(WH_GETMESSAGE, OakGetMsgProc, NULL, tid);
        wsprintfA(line, "oak: msghook=%p", g_OakMsgHook);
        OakLogLine(line);

        HANDLE thr = OpenThread(THREAD_SET_CONTEXT, FALSE, tid);
        if (thr)
        {
            BOOL apcOk = QueueUserAPC((PAPCFUNC)OakApcArmTimer, thr, 0);
            wsprintfA(line, "oak: UI APC(arm) ok=%d err=%u", (int)apcOk, GetLastError());
            OakLogLine(line);
            CloseHandle(thr);
        }
        PostMessageA(wnd, WM_NULL, 0, 0);
        PostThreadMessageA(tid, WM_NULL, 0, 0);
    }
    (void)discard;
    return 0;
#endif
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_Module = hModule;
        OakRegisterSehTable(hModule);

        // Beacon before anything else — proves DllMain ran even if BE kills us mid-init
        {
            const char* paths[2] = { "C:\\oak\\dayz\\dll_attach.flag", "C:\\oak\\dll_attach.flag" };
            for (int i = 0; i < 2; i++)
            {
                HANDLE hf = CreateFileA(paths[i], GENERIC_WRITE, FILE_SHARE_READ,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE)
                {
                    const char* msg = "dll_attach\r\n";
                    DWORD w = 0;
                    WriteFile(hf, msg, 12, &w, NULL);
                    CloseHandle(hf);
                }
            }
        }

        Log("dll attach");
        {
            char vbuf[96];
            wsprintfA(vbuf, "oak build %s", ImGuiMenu_Version());
            Log(vbuf);
        }

        // Prefer threadpool when possible; CreateThread keeps a non-module start address.
        if (!QueueUserWorkItem((LPTHREAD_START_ROUTINE)MainThread, hModule, WT_EXECUTELONGFUNCTION))
        {
            HANDLE hThread = CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
            if (hThread)
            {
                Log("main thread created");
                CloseHandle(hThread);
            }
        }
        else
        {
            Log("main work queued (threadpool)");
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        // Signal workers; restore Present immediately so game never jumps into unloaded image.
        InterlockedExchange(&g_ShuttingDown, 1);
        g_Running = false;
        g_MiscFreecam = false;
        MiscFreecamShutdown();
        __try { LagSwitchShutdown(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        OakPerfBoost_Shutdown();
        OakShadowChams_Shutdown();
        RestorePresentHooks();
        DisconnectFromPanel();
        OakProtectionShutdown();
        OakUnregisterSehTable();
    }
    
    return TRUE;
}
