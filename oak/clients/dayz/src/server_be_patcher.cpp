// Oak DayZ Server BattlEye patcher
// Patches DayZServer_x64.exe so BattlEye init always succeeds / is skipped,
// and VAC reject checks are bypassed. Creates .bak backup first.

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cctype>

struct PatByte {
    bool wild;
    uint8_t v;
};

static bool ParsePattern(const char* sig, std::vector<PatByte>& out)
{
    out.clear();
    std::istringstream iss(sig);
    std::string tok;
    while (iss >> tok)
    {
        if (tok[0] == '?')
            out.push_back({ true, 0 });
        else
        {
            if (tok.size() != 2 || !isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1]))
                return false;
            out.push_back({ false, (uint8_t)strtoul(tok.c_str(), nullptr, 16) });
        }
    }
    return !out.empty();
}

static uint8_t* FindPattern(uint8_t* base, size_t size, const char* sig)
{
    std::vector<PatByte> pat;
    if (!ParsePattern(sig, pat)) return nullptr;
    if (pat.size() > size) return nullptr;
    for (size_t i = 0; i + pat.size() <= size; i++)
    {
        bool ok = true;
        for (size_t j = 0; j < pat.size(); j++)
        {
            if (!pat[j].wild && base[i + j] != pat[j].v) { ok = false; break; }
        }
        if (ok) return base + i;
    }
    return nullptr;
}

static bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

static bool WriteAll(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return (bool)f;
}

static bool PatchAt(std::vector<uint8_t>& buf, uint8_t* addr, const uint8_t* patch, size_t n)
{
    size_t off = (size_t)(addr - buf.data());
    if (off + n > buf.size()) return false;
    memcpy(buf.data() + off, patch, n);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("Usage: OakDayZServerBEPatcher.exe <path\\to\\DayZServer_x64.exe>\n");
        return 1;
    }

    std::string path = argv[1];
    std::string bak = path + ".bak";

    std::vector<uint8_t> bytes;
    if (!ReadAll(path, bytes))
    {
        printf("ERROR: cannot read %s\n", path.c_str());
        return 2;
    }
    printf("Loaded %zu bytes\n", bytes.size());

    if (!WriteAll(bak, bytes))
    {
        printf("ERROR: cannot write backup %s\n", bak.c_str());
        return 3;
    }
    printf("Backup: %s\n", bak.c_str());

    int patches = 0;

    // BattlEye init prologues (several game builds)
    const char* beInitSigs[] = {
        "40 53 55 56 57 41 54 48 81 EC ? ? ? ? 45 33 E4 48 8B D9 44 89",
        "48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 56 48 81 EC ? ? ? ? 45 33 F6 48 8B D9 44 89",
        "40 55 53 56 57 41 54 41 56 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05",
    };
    const uint8_t retal1[] = { 0xB0, 0x01, 0xC3 }; // mov al,1 ; ret

    bool bePatched = false;
    for (const char* sig : beInitSigs)
    {
        uint8_t* hit = FindPattern(bytes.data(), bytes.size(), sig);
        if (!hit) continue;
        if (!PatchAt(bytes, hit, retal1, sizeof(retal1))) continue;
        printf("Patched BattlEye init @ +0x%llX (%s)\n",
            (unsigned long long)(hit - bytes.data()), sig);
        bePatched = true;
        patches++;
        break;
    }
    if (!bePatched)
        printf("WARN: BattlEye init pattern not found (exe may already be patched or signatures changed)\n");

    // VAC / BE failure jz -> jmp
    const char* vacSigs[] = {
        "74 44 0F B7 C8 E8 ? ? ? ? 8B 13 44 0F B7 C0 44 89 4C 24 ? 48",
        "74 5C B8 ? ? ? ? 66 3B C3 75 09 E8 ? ? ? ? 84 C0 74 49 0F B7",
        "74 ? 0F B7 C8 E8 ? ? ? ? 8B 13",
    };
    bool vacPatched = false;
    for (const char* sig : vacSigs)
    {
        uint8_t* hit = FindPattern(bytes.data(), bytes.size(), sig);
        if (!hit) continue;
        *hit = 0xEB; // jmp
        printf("Patched VAC/BE fail check @ +0x%llX\n",
            (unsigned long long)(hit - bytes.data()));
        vacPatched = true;
        patches++;
        break;
    }
    if (!vacPatched)
        printf("WARN: VAC check pattern not found\n");

    // Extra: force common "BattlEye failed" branches when possible
    // Search for string refs isn't needed if init returns true.

    // Console title marker
    uint8_t* title = FindPattern(bytes.data(), bytes.size(),
        "43 6F 6E 73 6F 6C 65 20 76"); // "Console v"
    if (title)
    {
        const char* repl = "OakNoBE v";
        size_t n = strlen(repl);
        memcpy(title, repl, n);
        printf("Patched console title\n");
        patches++;
    }

    if (!WriteAll(path, bytes))
    {
        printf("ERROR: cannot write patched exe (is the server running?)\n");
        return 4;
    }

    printf("Done. Applied %d patch(es) to %s\n", patches, path.c_str());
    if (patches == 0)
    {
        printf("No patches applied — restore from .bak if needed.\n");
        return 5;
    }
    return 0;
}
