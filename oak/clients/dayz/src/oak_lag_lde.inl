// Minimal x64 instruction-length helper for API trampolines (common prologues).
// Returns size of one instruction at p, or 0 on failure. Caps at 15.
static int LagInsnLen(const BYTE* p)
{
    if (!p) return 0;
    int i = 0;
    bool op66 = false, rexW = false;
    // prefixes
    for (int n = 0; n < 4; n++)
    {
        BYTE b = p[i];
        if (b == 0x66) { op66 = true; i++; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3) { i++; continue; }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
        if ((b & 0xF0) == 0x40) { rexW = (b & 8) != 0; i++; continue; }
        break;
    }
    BYTE op = p[i++];
    auto modrm = [&]() -> int {
        if (i >= 15) return -1;
        BYTE m = p[i++];
        int mod = m >> 6, rm = m & 7;
        if (mod != 3 && rm == 4)
        {
            if (i >= 15) return -1;
            i++; // SIB
        }
        if (mod == 0 && rm == 5) i += 4; // disp32
        else if (mod == 1) i += 1;
        else if (mod == 2) i += 4;
        return 0;
    };

    // NOP / RET / simple
    if (op == 0x90 || op == 0xC3 || op == 0xCC) return i;
    if (op == 0xC2) { i += 2; return i; }

    // push/pop r
    if ((op & 0xF0) == 0x50) return i;

    // mov r64, imm64
    if (rexW && (op & 0xF8) == 0xB8) { i += 8; return i; }
    // mov r32, imm32
    if ((op & 0xF8) == 0xB8) { i += 4; return i; }

    // add/sub/xor/cmp/mov r/m, r  and  r, r/m  (0x01,0x03,0x29,0x2B,0x31,0x33,0x39,0x3B,0x89,0x8B)
    if (op == 0x01 || op == 0x03 || op == 0x29 || op == 0x2B || op == 0x31 || op == 0x33
        || op == 0x39 || op == 0x3B || op == 0x85 || op == 0x87 || op == 0x89 || op == 0x8B
        || op == 0x8D || op == 0x63)
    {
        if (modrm() < 0) return 0;
        return i;
    }

    // mov r/m, imm32 (C7 /0)
    if (op == 0xC7)
    {
        if (modrm() < 0) return 0;
        i += 4;
        return i;
    }
    // mov r/m8, imm8 (C6)
    if (op == 0xC6)
    {
        if (modrm() < 0) return 0;
        i += 1;
        return i;
    }

    // arithmetic imm: 81 / 83
    if (op == 0x81)
    {
        if (modrm() < 0) return 0;
        i += op66 ? 2 : 4;
        return i;
    }
    if (op == 0x83)
    {
        if (modrm() < 0) return 0;
        i += 1;
        return i;
    }

    // lea already covered as 0x8D

    // call/jmp rel32
    if (op == 0xE8 || op == 0xE9) { i += 4; return i; }
    // short jmp
    if (op == 0xEB) { i += 1; return i; }

    // FF /2 call, /4 jmp
    if (op == 0xFF)
    {
        if (modrm() < 0) return 0;
        return i;
    }

    // test al,imm8 / eax,imm32
    if (op == 0xA8) { i += 1; return i; }
    if (op == 0xA9) { i += op66 ? 2 : 4; return i; }
    if (op == 0xF6)
    {
        BYTE m = p[i];
        int reg = (m >> 3) & 7;
        if (modrm() < 0) return 0;
        if (reg == 0) i += 1; // test imm8
        return i;
    }
    if (op == 0xF7)
    {
        BYTE m = p[i];
        int reg = (m >> 3) & 7;
        if (modrm() < 0) return 0;
        if (reg == 0) i += op66 ? 2 : 4;
        return i;
    }

    // two-byte opcodes 0F ...
    if (op == 0x0F)
    {
        BYTE op2 = p[i++];
        // jcc rel32
        if (op2 >= 0x80 && op2 <= 0x8F) { i += 4; return i; }
        // movups/movaps etc with modrm
        if (op2 == 0x1F || op2 == 0xAE || op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF
            || op2 == 0xA3 || op2 == 0xAB || (op2 >= 0x40 && op2 <= 0x4F))
        {
            if (modrm() < 0) return 0;
            return i;
        }
        return 0;
    }

    // movzx-ish already via 0F

    // push imm8 / imm32
    if (op == 0x6A) { i += 1; return i; }
    if (op == 0x68) { i += 4; return i; }

    // sub rsp, imm8 : 48 83 EC xx already handled via 83 with rex

    // xor r,r : 33 / 31 handled

    return 0;
}

static int LagStealLen(const BYTE* p, int minBytes)
{
    int total = 0;
    while (total < minBytes)
    {
        int n = LagInsnLen(p + total);
        if (n <= 0 || n > 15) return 0;
        total += n;
        if (total > 32) return 0;
    }
    return total;
}
