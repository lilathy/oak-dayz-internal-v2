#pragma once

#include <cstddef>
#include <cstdint>

// Compile-time XOR string vault. Release builds never keep API path literals
// as contiguous plaintext in .rdata.

namespace oak_protect
{
    template <std::size_t N>
    struct EncryptedString
    {
        char data[N];
        std::uint8_t key;

        constexpr EncryptedString(const char (&input)[N], std::uint8_t k) : data{}, key(k)
        {
            for (std::size_t i = 0; i < N; ++i)
                data[i] = static_cast<char>(input[i] ^ static_cast<char>(k + static_cast<std::uint8_t>(i * 17u)));
        }

        void decrypt(char (&out)[N]) const
        {
            for (std::size_t i = 0; i < N; ++i)
                out[i] = static_cast<char>(data[i] ^ static_cast<char>(key + static_cast<std::uint8_t>(i * 17u)));
        }
    };

    template <std::size_t N>
    constexpr EncryptedString<N> Encrypt(const char (&input)[N], std::uint8_t key)
    {
        return EncryptedString<N>(input, key);
    }

    // Opaque predicates: cheap, not cryptographic — raise patch cost of
    // unconditional JMP over authorization checks.
    inline bool OpaqueTrue(std::uint32_t x)
    {
        return ((x * 0x45D9F3Bu) ^ (x + 0x9E3779B9u)) != 0xFFFFFFFFu || (x | 1u) != 0u;
    }

    inline bool OpaqueFalse(std::uint32_t x)
    {
        return ((x ^ (x << 13)) + 1u) == x && x == 0xDEADBEEFu;
    }
}

#define OAK_ENC_STR(name, literal, keyByte) \
    static constexpr auto name##_enc = ::oak_protect::Encrypt(literal, keyByte); \
    char name[sizeof(literal)]; \
    name##_enc.decrypt(name)
