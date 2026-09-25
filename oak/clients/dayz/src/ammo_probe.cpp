// Live RPM probe: find Inventory/Hands/AmmoType on current DayZ build.
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static DWORD FindPid(const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(s, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return pid;
}

static uintptr_t ModBase(HANDLE h, DWORD pid) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me{ sizeof(me) };
    uintptr_t base = 0;
    if (Module32FirstW(s, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"DayZ_x64.exe") == 0) { base = (uintptr_t)me.modBaseAddr; break; }
        } while (Module32NextW(s, &me));
    }
    CloseHandle(s);
    return base;
}

template<typename T>
static T R(HANDLE h, uintptr_t a) {
    T v{};
    SIZE_T n = 0;
    ReadProcessMemory(h, (LPCVOID)a, &v, sizeof(T), &n);
    return v;
}

static bool Valid(uintptr_t p) {
    return p > 0x100000000ULL && p < 0x7F0000000000ULL;
}

static void ReadName(HANDLE h, uintptr_t engStr, char* out, int max) {
    out[0] = 0;
    if (!Valid(engStr)) return;
    uintptr_t data = R<uintptr_t>(h, engStr + 0x10);
    if (!Valid(data)) data = engStr + 0x10;
    WORD len = R<WORD>(h, engStr + 0x8);
    if (len == 0 || len > 80) len = 32;
    char tmp[96]{};
    SIZE_T n = 0;
    ReadProcessMemory(h, (LPCVOID)data, tmp, len < 90 ? len : 90, &n);
    for (int i = 0; i < max - 1 && tmp[i]; i++) out[i] = tmp[i];
}

static bool GetTypeName(HANDLE h, uintptr_t ent, char* out, int max) {
    out[0] = 0;
    if (!Valid(ent)) return false;
    uintptr_t typ = R<uintptr_t>(h, ent + 0x180);
    if (!Valid(typ)) return false;
    uintptr_t nameOffs[] = { 0x98, 0x70, 0xA8, 0x68, 0x78, 0x48, 0x50, 0xD0 };
    for (int i = 0; i < 6; i++) {
        uintptr_t np = R<uintptr_t>(h, typ + nameOffs[i]);
        ReadName(h, np, out, max);
        if (out[0]) return true;
    }
    return false;
}

static bool SpeedOk(float s) { return s == s && s > 50.f && s < 20000.f; }

static bool LooksAmmo(HANDLE h, uintptr_t p, float* outInit, int* outOff) {
    if (!Valid(p)) return false;
    uintptr_t speedOffs[] = { 0x38C, 0x364, 0x398, 0x370, 0x380 };
    float best = 0;
    int bestOff = -1;
    for (int i = 0; i < 5; i++) {
        float s = R<float>(h, p + speedOffs[i]);
        if (SpeedOk(s)) { best = s; bestOff = (int)speedOffs[i]; break; }
    }
    if (bestOff < 0) return false;
    float grav = R<float>(h, p + 0x3BC);
    float air = R<float>(h, p + 0x3B4);
    float disp = R<float>(h, p + 0x3A4);
    // Soft checks — don't reject aggressively while probing
    if (grav == grav && (grav < -1.f || grav > 20.f)) return false;
    if (air == air && (air < -20.f || air > 20.f)) return false;
    if (outInit) *outInit = best;
    if (outOff) *outOff = bestOff;
    (void)disp;
    return true;
}

static void DumpAmmoNear(HANDLE h, uintptr_t hands, const char* tag) {
    int hits = 0;
    for (uintptr_t off = 0x80; off <= 0x900; off += 8) {
        uintptr_t p = R<uintptr_t>(h, hands + off);
        float init = 0; int soff = 0;
        if (LooksAmmo(h, p, &init, &soff)) {
            printf("  [%s] HIT hands+0x%X -> AT 0x%llX init=%.1f@+0x%X grav=%.3f disp=%.4f air=%.4f\n",
                tag, (unsigned)off, (unsigned long long)p, init, soff,
                R<float>(h, p + 0x3BC), R<float>(h, p + 0x3A4), R<float>(h, p + 0x3B4));
            hits++;
        }
        if (!Valid(p)) continue;
        uintptr_t nests[] = { 0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 };
        for (int ni = 0; ni < 11; ni++) {
            uintptr_t q = R<uintptr_t>(h, p + nests[ni]);
            if (LooksAmmo(h, q, &init, &soff)) {
                printf("  [%s] HIT hands+0x%X -> +0x%X -> AT 0x%llX init=%.1f@+0x%X\n",
                    tag, (unsigned)off, (unsigned)nests[ni], (unsigned long long)q, init, soff);
                hits++;
            }
        }
    }
    // mag capacity hints
    for (uintptr_t off = 0x100; off <= 0x800; off += 8) {
        uintptr_t mag = R<uintptr_t>(h, hands + off);
        if (!Valid(mag)) continue;
        int caps[] = { 0x6AC, 0x6B0, 0x6B4, 0x694, 0x698, 0x69C };
        for (int ci = 0; ci < 6; ci++) {
            int v = R<int>(h, mag + caps[ci]);
            if (v > 0 && v <= 200) {
                printf("  [%s] mag? hands+0x%X = 0x%llX +0x%X=%d\n",
                    tag, (unsigned)off, (unsigned long long)mag, (unsigned)caps[ci], v);
                break;
            }
        }
    }
    printf("  [%s] ammo hits=%d\n", tag, hits);
}

int main() {
    DWORD pid = FindPid(L"DayZ_x64.exe");
    if (!pid) { printf("DayZ not running\n"); return 1; }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { printf("OpenProcess failed %u\n", GetLastError()); return 1; }
    uintptr_t base = ModBase(h, pid);
    printf("pid=%u base=0x%llX\n", pid, (unsigned long long)base);

    uintptr_t world = R<uintptr_t>(h, base + 0x4262FE8);
    printf("world=0x%llX\n", (unsigned long long)world);
    if (!Valid(world)) { printf("bad world\n"); return 1; }

    uintptr_t local = R<uintptr_t>(h, world + 0x2960);
    printf("local=0x%llX\n", (unsigned long long)local);
    if (!Valid(local)) { printf("bad local\n"); return 1; }

    char lname[64]{};
    GetTypeName(h, local, lname, 64);
    printf("local type='%s' dead=%u\n", lname, (unsigned)R<BYTE>(h, local + 0xE2));

    // Dump candidate inventory pointers on local
    printf("--- inventory candidates on local ---\n");
    uintptr_t invCandidates[16]{};
    int nInv = 0;
    uintptr_t invOffs[] = { 0x650, 0x658, 0x648, 0x660, 0x640, 0x668, 0x670, 0x680 };
    for (int i = 0; i < 8; i++) {
        uintptr_t inv = R<uintptr_t>(h, local + invOffs[i]);
        printf("  local+0x%X = 0x%llX %s\n", (unsigned)invOffs[i], (unsigned long long)inv,
            Valid(inv) ? "OK" : "BAD");
        if (Valid(inv) && nInv < 16) invCandidates[nInv++] = inv;
    }
    // broader scan for inv that has hands-like entities
    for (uintptr_t off = 0x600; off <= 0x700; off += 8) {
        uintptr_t inv = R<uintptr_t>(h, local + off);
        if (!Valid(inv)) continue;
        uintptr_t handOffs[] = { 0xF8, 0x1B0, 0x150, 0x100 };
        for (int hi = 0; hi < 4; hi++) {
            uintptr_t hands = R<uintptr_t>(h, inv + handOffs[hi]);
            if (!Valid(hands)) continue;
            char hn[64]{};
            if (GetTypeName(h, hands, hn, 64) && hn[0]) {
                printf("  SCAN local+0x%X inv -> hands+0x%X '%s'\n",
                    (unsigned)off, (unsigned)handOffs[hi], hn);
                int found = 0;
                for (int k = 0; k < nInv; k++) if (invCandidates[k] == inv) found = 1;
                if (!found && nInv < 16) invCandidates[nInv++] = inv;
            }
        }
    }

    if (!nInv) { printf("no inventory candidates\n"); return 1; }

    for (int ii = 0; ii < nInv; ii++) {
        uintptr_t inv = invCandidates[ii];
        printf("--- inv[%d]=0x%llX ---\n", ii, (unsigned long long)inv);
        uintptr_t handOffs[] = { 0xF8, 0x1B0, 0x150, 0x148, 0x100, 0x108, 0x110, 0x1A0, 0x190, 0x1C0 };
        for (int hi = 0; hi < 10; hi++) {
            uintptr_t hands = R<uintptr_t>(h, inv + handOffs[hi]);
            if (!Valid(hands)) continue;
            char hn[64]{};
            GetTypeName(h, hands, hn, 64);
            printf("  hands inv+0x%X = 0x%llX type='%s'\n",
                (unsigned)handOffs[hi], (unsigned long long)hands, hn);
            if (hn[0]) {
                char tag[32];
                sprintf(tag, "i%d+0x%X", ii, (unsigned)handOffs[hi]);
                DumpAmmoNear(h, hands, tag);
            }
        }
    }

    CloseHandle(h);
    return 0;
}
