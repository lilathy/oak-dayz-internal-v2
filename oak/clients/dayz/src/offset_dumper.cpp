// OakOffsetDumper — usermode DayZ_x64 offset finder (ReadProcessMemory).
// Strategies:
//   1) RIP-relative MOV/LEA signature scan → candidate Modbase::World RVAs
//   2) Writable .data pointer sweep (only if sigs are weak)
//   3) Struct field discovery on the live World object
//
// Usage: OakOffsetDumper.exe [optional path for offsets.hpp]
// Requires DayZ_x64.exe running (ideally in-world / main menu with camera).

#include <Windows.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <map>

#pragma comment(lib, "psapi.lib")

struct ModuleInfo {
    uintptr_t base = 0;
    size_t size = 0;
    std::string path;
};

struct WorldHit {
    uintptr_t rva = 0;
    uintptr_t worldPtr = 0;
    uintptr_t camera = 0;
    int score = 0;
    int xrefs = 0;
    bool fromSig = false;
    const char* via = "";
};

struct StructOffsets {
    uintptr_t Camera = 0;
    uintptr_t NearEntList = 0;
    uintptr_t FarEntList = 0;
    uintptr_t SlowEntList = 0;
    uintptr_t BulletList = 0;
    uintptr_t LocalPlayer = 0;
    uintptr_t SlowEntValidCount = 0;
};

static DWORD FindPid(const wchar_t* name)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool Rpm(HANDLE h, uintptr_t addr, void* buf, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, buf, n, &got) && got == n;
}

template <typename T>
static bool RpmT(HANDLE h, uintptr_t addr, T& out)
{
    return Rpm(h, addr, &out, sizeof(T));
}

static bool IsUserPtr(uintptr_t p)
{
    return p >= 0x10000ULL && p < 0x00007FFFFFFFFFFFULL;
}

static bool InModule(uintptr_t p, const ModuleInfo& mi)
{
    return mi.base && p >= mi.base && p < mi.base + mi.size;
}

static bool IsCommitted(HANDLE h, uintptr_t p)
{
    if (!IsUserPtr(p)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQueryEx(h, (LPCVOID)p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

static bool IsFiniteF(float f)
{
    return std::isfinite(f) && fabsf(f) < 1.0e8f;
}

static bool LooksLikeCamera(HANDLE h, uintptr_t cam)
{
    if (!IsCommitted(h, cam) || !IsCommitted(h, cam + 0xE0)) return false;

    float m[12]{};
    if (!Rpm(h, cam + 0x8, m, sizeof(m))) return false;
    for (float v : m)
        if (!IsFiniteF(v)) return false;

    float pos[3]{};
    if (!Rpm(h, cam + 0x2C, pos, sizeof(pos))) return false;
    for (float v : pos)
        if (!IsFiniteF(v)) return false;
    if (fabsf(pos[0]) > 60000.f || fabsf(pos[1]) > 2000.f || fabsf(pos[2]) > 60000.f)
        return false;

    float proj[3]{};
    if (!Rpm(h, cam + 0xD0, proj, sizeof(proj))) return false;
    if (!IsFiniteF(proj[0]) || !IsFiniteF(proj[1])) return false;
    return true;
}

static int ScoreWorld(HANDLE h, const ModuleInfo& mi, uintptr_t world, StructOffsets* discovered)
{
    if (!IsCommitted(h, world) || !IsCommitted(h, world + 0x3000)) return 0;
    if (InModule(world, mi)) return 0;

    int score = 0;
    uintptr_t cam = 0;
    uintptr_t bestCamOff = 0;

    const uintptr_t camCandidates[] = { 0x1B8ULL, 0x1C0ULL, 0x1B0ULL, 0x200ULL };
    for (uintptr_t off : camCandidates) {
        uintptr_t c = 0;
        if (!RpmT(h, world + off, c)) continue;
        if (InModule(c, mi)) continue;
        if (LooksLikeCamera(h, c)) {
            cam = c;
            bestCamOff = off;
            score += 50;
            break;
        }
    }
    if (!cam) return 0;

    uintptr_t nearPtr = 0, farPtr = 0;
    int nearCount = 0, farCount = 0;
    if (!RpmT(h, world + 0xF48, nearPtr) || !RpmT(h, world + 0xF50, nearCount)) return 0;
    if (!RpmT(h, world + 0x1090, farPtr) || !RpmT(h, world + 0x1098, farCount)) return 0;

    bool nearOk = IsUserPtr(nearPtr) && !InModule(nearPtr, mi) && IsCommitted(h, nearPtr);
    bool farOk = IsUserPtr(farPtr) && !InModule(farPtr, mi) && IsCommitted(h, farPtr);
    if (!nearOk || !farOk) return 0;
    score += 40;
    if (nearCount >= 0 && nearCount < 5000) score += 10;
    if (farCount >= 0 && farCount < 20000) score += 10;

    uintptr_t bullet = 0, slow = 0;
    RpmT(h, world + 0xE00, bullet);
    RpmT(h, world + 0x2010, slow);
    if (IsUserPtr(bullet) && !InModule(bullet, mi) && IsCommitted(h, bullet)) score += 8;
    if (IsUserPtr(slow) && !InModule(slow, mi) && IsCommitted(h, slow)) score += 8;

    uintptr_t bestLocal = 0x2960;
    uintptr_t localAt2960 = 0;
    if (RpmT(h, world + 0x2960, localAt2960) && IsUserPtr(localAt2960) &&
        !InModule(localAt2960, mi) && IsCommitted(h, localAt2960)) {
        uintptr_t vs = 0;
        if (RpmT(h, localAt2960 + 0x1C8, vs) && IsCommitted(h, vs) && !InModule(vs, mi)) {
            bestLocal = 0x2960;
            score += 25;
        }
    } else {
        for (uintptr_t off = 0x28C0; off <= 0x2A00; off += 8) {
            uintptr_t ent = 0;
            if (!RpmT(h, world + off, ent)) continue;
            if (!IsUserPtr(ent) || InModule(ent, mi) || !IsCommitted(h, ent)) continue;
            uintptr_t vs = 0;
            if (!RpmT(h, ent + 0x1C8, vs) || !IsCommitted(h, vs) || InModule(vs, mi)) continue;
            float t[3]{};
            if (!Rpm(h, vs + 0x2C, t, sizeof(t))) continue;
            if (!IsFiniteF(t[0]) || !IsFiniteF(t[1]) || !IsFiniteF(t[2])) continue;
            bestLocal = off;
            score += 12;
            break;
        }
    }

    if (discovered) {
        discovered->Camera = bestCamOff;
        discovered->NearEntList = 0xF48;
        discovered->FarEntList = 0x1090;
        discovered->SlowEntList = 0x2010;
        discovered->BulletList = 0xE00;
        discovered->LocalPlayer = bestLocal;
        discovered->SlowEntValidCount = 0x1F90;
    }
    return score;
}

static ModuleInfo GetMainModule(HANDLE h)
{
    ModuleInfo mi;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_64BIT))
        return mi;

    char path[MAX_PATH]{};
    if (!GetModuleFileNameExA(h, mods[0], path, MAX_PATH))
        return mi;

    MODULEINFO info{};
    if (!GetModuleInformation(h, mods[0], &info, sizeof(info)))
        return mi;

    mi.base = (uintptr_t)info.lpBaseOfDll;
    mi.size = (size_t)info.SizeOfImage;
    mi.path = path;
    return mi;
}

static bool ReadImage(HANDLE h, const ModuleInfo& mi, std::vector<uint8_t>& out)
{
    out.assign(mi.size, 0);
    const size_t chunk = 0x10000;
    size_t ok = 0;
    for (size_t off = 0; off < mi.size; off += chunk) {
        size_t n = (std::min)(chunk, mi.size - off);
        SIZE_T got = 0;
        if (ReadProcessMemory(h, (LPCVOID)(mi.base + off), out.data() + off, n, &got) && got)
            ok += got;
    }
    return ok > mi.size / 4;
}

struct Section {
    char name[9]{};
    uintptr_t rva = 0;
    size_t size = 0;
    uint32_t chars = 0;
};

static std::vector<Section> ParseSections(const std::vector<uint8_t>& img)
{
    std::vector<Section> secs;
    if (img.size() < 0x200) return secs;
    auto* dos = (IMAGE_DOS_HEADER*)img.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return secs;
    auto* nt = (IMAGE_NT_HEADERS64*)(img.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return secs;
    auto* sh = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Section s{};
        memcpy(s.name, sh[i].Name, 8);
        s.rva = sh[i].VirtualAddress;
        s.size = sh[i].Misc.VirtualSize ? sh[i].Misc.VirtualSize : sh[i].SizeOfRawData;
        s.chars = sh[i].Characteristics;
        secs.push_back(s);
    }
    return secs;
}

static bool MatchPat(const uint8_t* data, size_t len, const char* pat)
{
    std::vector<int> bytes;
    for (const char* p = pat; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { bytes.push_back(-1); p += 2; continue; }
        unsigned v = 0;
        if (sscanf_s(p, "%02X", &v) != 1) return false;
        bytes.push_back((int)v);
        p += 2;
    }
    if (bytes.size() > len) return false;
    for (size_t i = 0; i < bytes.size(); i++)
        if (bytes[i] >= 0 && data[i] != (uint8_t)bytes[i]) return false;
    return true;
}

static uintptr_t ResolveRip(uintptr_t instrVA, int32_t disp, int instrLen)
{
    return instrVA + (uintptr_t)instrLen + (intptr_t)disp;
}

static bool IsWritableRva(const std::vector<Section>& secs, uintptr_t rva)
{
    for (const auto& s : secs) {
        if (rva >= s.rva && rva < s.rva + s.size)
            return (s.chars & IMAGE_SCN_MEM_WRITE) != 0;
    }
    return false;
}

static void ScanSignatures(HANDLE h, const ModuleInfo& mi, const std::vector<uint8_t>& img,
    std::vector<WorldHit>& hits)
{
    struct Sig {
        const char* pat;
        int dispAt;
        int len;
        const char* name;
    };
    Sig sigs[] = {
        { "48 8B 0D ?? ?? ?? ??", 3, 7, "mov rcx,[rip]" },
        { "48 8B 05 ?? ?? ?? ??", 3, 7, "mov rax,[rip]" },
        { "48 8B 15 ?? ?? ?? ??", 3, 7, "mov rdx,[rip]" },
        { "48 8B 1D ?? ?? ?? ??", 3, 7, "mov rbx,[rip]" },
        { "48 8B 3D ?? ?? ?? ??", 3, 7, "mov rdi,[rip]" },
        { "4C 8B 05 ?? ?? ?? ??", 3, 7, "mov r8,[rip]" },
        { "4C 8B 0D ?? ?? ?? ??", 3, 7, "mov r9,[rip]" },
        { "4C 8B 15 ?? ?? ?? ??", 3, 7, "mov r10,[rip]" },
        { "4C 8B 1D ?? ?? ?? ??", 3, 7, "mov r11,[rip]" },
    };

    auto secs = ParseSections(img);
    std::vector<std::pair<size_t, size_t>> codeRanges;
    for (const auto& s : secs) {
        if (s.chars & IMAGE_SCN_MEM_EXECUTE)
            codeRanges.push_back({ (size_t)s.rva, (size_t)s.rva + s.size });
    }
    if (codeRanges.empty())
        codeRanges.push_back({ 0x1000, img.size() });

    std::map<uintptr_t, WorldHit> merged;

    for (const auto& range : codeRanges) {
        size_t start = range.first;
        size_t end = (std::min)(range.second, img.size());
        if (end < 16 || start >= end) continue;

        for (size_t off = start; off + 16 < end; off++) {
            for (const auto& sig : sigs) {
                if (!MatchPat(img.data() + off, end - off, sig.pat)) continue;
                int32_t disp = 0;
                memcpy(&disp, img.data() + off + sig.dispAt, 4);
                uintptr_t targetVA = ResolveRip(mi.base + off, disp, sig.len);
                if (targetVA < mi.base || targetVA + 8 > mi.base + mi.size) continue;
                uintptr_t rva = targetVA - mi.base;
                if (rva < 0x1000 || rva > mi.size - 8) continue;
                if (!IsWritableRva(secs, rva)) continue;

                uintptr_t world = 0;
                if (!RpmT(h, targetVA, world)) continue;
                if (!IsUserPtr(world) || InModule(world, mi)) continue;

                StructOffsets so{};
                int sc = ScoreWorld(h, mi, world, &so);
                if (sc < 90) continue;

                auto it = merged.find(rva);
                if (it == merged.end()) {
                    WorldHit hit;
                    hit.rva = rva;
                    hit.worldPtr = world;
                    RpmT(h, world + so.Camera, hit.camera);
                    hit.score = sc;
                    hit.xrefs = 1;
                    hit.fromSig = true;
                    hit.via = sig.name;
                    merged[rva] = hit;
                } else {
                    it->second.xrefs++;
                    if (sc > it->second.score) {
                        it->second.score = sc;
                        it->second.worldPtr = world;
                        RpmT(h, world + so.Camera, it->second.camera);
                        it->second.via = sig.name;
                    }
                }
            }
        }
    }

    for (auto& kv : merged)
        hits.push_back(kv.second);
}

static void SweepDataSections(HANDLE h, const ModuleInfo& mi, const std::vector<uint8_t>& img,
    std::vector<WorldHit>& hits)
{
    bool haveStrongSig = false;
    for (const auto& hhit : hits)
        if (hhit.fromSig && hhit.score >= 100 && hhit.xrefs >= 2) { haveStrongSig = true; break; }
    if (haveStrongSig) {
        printf("[*] skipping data sweep (strong signature hit already)\n");
        return;
    }

    auto secs = ParseSections(img);
    for (const auto& s : secs) {
        if (!(s.chars & IMAGE_SCN_MEM_WRITE))
            continue;

        size_t start = (size_t)s.rva;
        size_t end = (std::min)((size_t)s.rva + s.size, img.size());
        printf("[*] Sweeping %s RVA 0x%zX..0x%zX\n", s.name, start, end);

        for (size_t off = start; off + 8 <= end; off += 8) {
            uintptr_t world = 0;
            if (!RpmT(h, mi.base + off, world)) continue;
            if (!IsUserPtr(world) || InModule(world, mi)) continue;

            StructOffsets so{};
            int sc = ScoreWorld(h, mi, world, &so);
            if (sc < 110) continue;

            WorldHit hit;
            hit.rva = (uintptr_t)off;
            hit.worldPtr = world;
            RpmT(h, world + so.Camera, hit.camera);
            hit.score = sc;
            hit.xrefs = 0;
            hit.fromSig = false;
            hit.via = "data-sweep";
            hits.push_back(hit);
            printf("    candidate RVA=0x%llX world=0x%llX score=%d\n",
                (unsigned long long)hit.rva, (unsigned long long)world, sc);
        }
    }
}

static void RankHits(std::vector<WorldHit>& hits)
{
    std::map<uintptr_t, WorldHit> merged;
    for (const auto& h : hits) {
        auto it = merged.find(h.rva);
        if (it == merged.end()) merged[h.rva] = h;
        else {
            it->second.xrefs += h.xrefs;
            it->second.fromSig = it->second.fromSig || h.fromSig;
            if (h.score > it->second.score) {
                it->second.score = h.score;
                it->second.worldPtr = h.worldPtr;
                it->second.camera = h.camera;
                it->second.via = h.via;
            }
        }
    }
    hits.clear();
    for (auto& kv : merged) hits.push_back(kv.second);

    std::sort(hits.begin(), hits.end(), [](const WorldHit& a, const WorldHit& b) {
        int ra = a.score + (a.fromSig ? 50 : 0) + (std::min)(a.xrefs, 50) * 2;
        int rb = b.score + (b.fromSig ? 50 : 0) + (std::min)(b.xrefs, 50) * 2;
        if (ra != rb) return ra > rb;
        return a.rva < b.rva;
    });
}

static void WriteOutputs(const ModuleInfo& mi, const WorldHit& best, const StructOffsets& so,
    const char* outPath)
{
    char localApp[MAX_PATH]{};
    GetEnvironmentVariableA("LOCALAPPDATA", localApp, MAX_PATH);
    std::string sessionDir = std::string(localApp) + "\\DayZ\\oak_sessions";
    CreateDirectoryA((std::string(localApp) + "\\DayZ").c_str(), nullptr);
    CreateDirectoryA(sessionDir.c_str(), nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[64];
    sprintf_s(stamp, "%04d-%02d-%02d_%02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::string txtPath = sessionDir + "\\offsets_" + stamp + ".txt";
    std::string hppPath = outPath && outPath[0] ? outPath : (sessionDir + "\\offsets_generated.hpp");

    {
        std::ofstream f(txtPath);
        f << "OakOffsetDumper results\n";
        f << "module: " << mi.path << "\n";
        f << std::hex;
        f << "base: 0x" << mi.base << " size=0x" << mi.size << "\n";
        f << "via: " << best.via << " score=" << std::dec << best.score
          << " xrefs=" << best.xrefs << "\n\n";
        f << std::hex;
        f << "[OFFSET: ] Modbase::World                       -> 0x" << best.rva << "\n";
        f << "[OFFSET: ] World::Camera                        -> 0x" << so.Camera << "\n";
        f << "[OFFSET: ] World::NearEntList                   -> 0x" << so.NearEntList << "\n";
        f << "[OFFSET: ] World::FarEntList                    -> 0x" << so.FarEntList << "\n";
        f << "[OFFSET: ] World::SlowEntList                   -> 0x" << so.SlowEntList << "\n";
        f << "[OFFSET: ] World::BulletList                    -> 0x" << so.BulletList << "\n";
        f << "[OFFSET: ] World::LocalPlayer                   -> 0x" << so.LocalPlayer << "\n";
        f << "[OFFSET: ] World::SlowEntValidCount             -> 0x" << so.SlowEntValidCount << "\n";
        f << "\nworldPtr=0x" << best.worldPtr << " camera=0x" << best.camera << "\n";
    }

    {
        std::ofstream f(hppPath);
        f << "#pragma once\n";
        f << "// Auto-generated by OakOffsetDumper — do not hand-edit\n";
        f << "// module base size 0x" << std::hex << mi.size << "\n\n";
        f << "namespace oak_offsets {\n";
        f << "namespace modbase {\n";
        f << "    constexpr uintptr_t World = 0x" << best.rva << ";\n";
        f << "}\n";
        f << "namespace world {\n";
        f << "    constexpr uintptr_t Camera = 0x" << so.Camera << ";\n";
        f << "    constexpr uintptr_t NearEntList = 0x" << so.NearEntList << ";\n";
        f << "    constexpr uintptr_t FarEntList = 0x" << so.FarEntList << ";\n";
        f << "    constexpr uintptr_t SlowEntList = 0x" << so.SlowEntList << ";\n";
        f << "    constexpr uintptr_t BulletList = 0x" << so.BulletList << ";\n";
        f << "    constexpr uintptr_t LocalPlayer = 0x" << so.LocalPlayer << ";\n";
        f << "    constexpr uintptr_t SlowEntValidCount = 0x" << so.SlowEntValidCount << ";\n";
        f << "}\n";
        f << "}\n";
    }

    printf("[+] wrote %s\n", txtPath.c_str());
    printf("[+] wrote %s\n", hppPath.c_str());
}

int main(int argc, char** argv)
{
    printf("OakOffsetDumper (usermode)\n");

    DWORD pid = FindPid(L"DayZ_x64.exe");
    if (!pid) {
        printf("[!] DayZ_x64.exe not running — launch with -noBattlEye first\n");
        return 1;
    }
    printf("[+] PID %lu\n", pid);

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        printf("[!] OpenProcess failed (%lu) — run as Administrator?\n", GetLastError());
        return 2;
    }

    ModuleInfo mi = GetMainModule(h);
    if (!mi.base) {
        printf("[!] failed to resolve main module\n");
        CloseHandle(h);
        return 3;
    }
    printf("[+] module base=0x%llX size=0x%zX\n", (unsigned long long)mi.base, mi.size);
    printf("[+] path: %s\n", mi.path.c_str());

    std::vector<uint8_t> img;
    printf("[*] reading image...\n");
    if (!ReadImage(h, mi, img)) {
        printf("[!] failed to read enough of the image\n");
        CloseHandle(h);
        return 4;
    }
    printf("[+] image buffer %zu bytes\n", img.size());

    std::vector<WorldHit> hits;
    printf("[*] signature scan...\n");
    ScanSignatures(h, mi, img, hits);
    printf("[+] sig candidates: %zu\n", hits.size());

    printf("[*] data-section sweep (conditional)...\n");
    SweepDataSections(h, mi, img, hits);

    RankHits(hits);
    printf("[+] unique World candidates: %zu\n", hits.size());

    if (hits.empty()) {
        printf("[!] no World candidates — enter the game world and retry\n");
        CloseHandle(h);
        return 5;
    }

    for (size_t i = 0; i < hits.size() && i < 10; i++) {
        printf("  #%zu RVA=0x%llX world=0x%llX score=%d xrefs=%d sig=%d via=%s\n",
            i, (unsigned long long)hits[i].rva, (unsigned long long)hits[i].worldPtr,
            hits[i].score, hits[i].xrefs, hits[i].fromSig ? 1 : 0, hits[i].via);
    }

    WorldHit best = hits[0];
    StructOffsets so{};
    ScoreWorld(h, mi, best.worldPtr, &so);

    printf("\n=== BEST ===\n");
    printf("Modbase::World     = 0x%llX\n", (unsigned long long)best.rva);
    printf("World::Camera      = 0x%llX\n", (unsigned long long)so.Camera);
    printf("World::NearEntList = 0x%llX\n", (unsigned long long)so.NearEntList);
    printf("World::FarEntList  = 0x%llX\n", (unsigned long long)so.FarEntList);
    printf("World::SlowEntList = 0x%llX\n", (unsigned long long)so.SlowEntList);
    printf("World::BulletList  = 0x%llX\n", (unsigned long long)so.BulletList);
    printf("World::LocalPlayer = 0x%llX\n", (unsigned long long)so.LocalPlayer);
    printf("worldPtr           = 0x%llX\n", (unsigned long long)best.worldPtr);
    printf("camera             = 0x%llX\n", (unsigned long long)best.camera);
    printf("xrefs              = %d (sig=%d)\n", best.xrefs, best.fromSig ? 1 : 0);

    const char* outHpp = (argc > 1) ? argv[1] : nullptr;
    char defaultHpp[MAX_PATH]{};
    if (!outHpp) {
        GetModuleFileNameA(nullptr, defaultHpp, MAX_PATH);
        char* slash = strrchr(defaultHpp, '\\');
        if (slash) {
            *(slash + 1) = 0;
            strcat_s(defaultHpp, "offsets_generated.hpp");
            outHpp = defaultHpp;
        }
    }
    WriteOutputs(mi, best, so, outHpp);

    CloseHandle(h);
    printf("[+] done\n");
    return 0;
}
