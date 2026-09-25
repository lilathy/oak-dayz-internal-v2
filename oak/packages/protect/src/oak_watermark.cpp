#include "oak_protect.h"

#include <cstring>

// Placeholder rewritten by protect/tools/mutate_release.mjs per build.
// Keep exactly 80 bytes so the mutator can locate and patch it safely.
extern "C" volatile char g_OakBuildMark[80] =
    "OAKWMARK______________________________________________________________";

namespace
{
    std::uint32_t Mix(std::uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7FEB352Du;
        x ^= x >> 15;
        x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }
}

std::uint32_t OakProtectIntegrityTag()
{
    std::uint32_t tag = 0xA17C3E91u;
    for (int i = 0; i < 80; ++i)
        tag = Mix(tag ^ static_cast<std::uint8_t>(g_OakBuildMark[i]));
    for (const char* p = OAK_PRODUCT_SLUG; *p; ++p)
        tag = Mix(tag ^ static_cast<std::uint8_t>(*p));
    return tag ? tag : 1u;
}
