#include "game_profiles.h"
#include "image_headers.h"

#include <cstddef>
#include <cstring>
#include <windows.h>
#include <cwchar>
#include <limits>

namespace batman_mods {
namespace {

constexpr std::uint8_t Nibble(char value) noexcept {
    return static_cast<std::uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10);
}

template<std::size_t N>
constexpr std::array<std::uint8_t, 32> Sha256Literal(const char (&hex)[N]) noexcept {
    static_assert(N == 65, "A SHA-256 profile needs exactly 64 hexadecimal digits.");
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>((Nibble(hex[i * 2]) << 4) | Nibble(hex[i * 2 + 1]));
    return bytes;
}

// Generated exact-build identities and per-backend native targets.
constexpr GameProfile kProfiles[] = {
#include "generated_profiles.inc"
};

} // namespace

bool NativeHookMatches(const unsigned char* image, std::uint32_t imageSize,
                       const NativeHook& hook, std::uint64_t preferredImageBase,
                       void (*log)(const wchar_t*)) noexcept {
    constexpr std::size_t kRelocationBytes = sizeof(std::uintptr_t);
    constexpr std::size_t kCapacity = 16 + kRelocationBytes - 1;
    const std::size_t readBytes = NativeHookReadSize(hook);
    if (!image || !preferredImageBase || preferredImageBase > std::numeric_limits<std::uintptr_t>::max() ||
        imageSize < 4096 || !readBytes || readBytes > kCapacity || readBytes > imageSize ||
        hook.rva > imageSize - readBytes) return false;
    if (!NativeImageHeadersMatch(image, imageSize) ||
        !memory::Readable(image + hook.rva, readBytes)) return false;
    const auto delta = reinterpret_cast<std::uintptr_t>(image) -
                       static_cast<std::uintptr_t>(preferredImageBase);
    std::array<unsigned char, kCapacity> normalized{};
    std::memcpy(normalized.data(), image + hook.rva, readBytes);
    const auto observed = normalized;
    for (std::size_t i = 0; i < hook.relocationCount; ++i) {
        const auto offset = hook.relocations[i];
        if (offset >= hook.prologue.size()) return false;
        std::uintptr_t value = 0;
        std::memcpy(&value, normalized.data() + offset, sizeof(value));
        value -= delta;
        std::memcpy(normalized.data() + offset, &value, sizeof(value));
    }
    const bool matches = std::memcmp(normalized.data(), hook.prologue.data(), hook.prologue.size()) == 0;
    if (!matches && log) {
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        wchar_t expected[33]{}, raw[33]{}, actual[33]{}, message[512]{};
        for (std::size_t i = 0; i < 16; ++i) {
            expected[2*i] = hex[hook.prologue[i] >> 4];
            expected[2*i+1] = hex[hook.prologue[i] & 15];
            raw[2*i] = hex[observed[i] >> 4];
            raw[2*i+1] = hex[observed[i] & 15];
            actual[2*i] = hex[normalized[i] >> 4];
            actual[2*i+1] = hex[normalized[i] & 15];
        }
        swprintf_s(message, L"Signature mismatch RVA=0x%08X mapped=%p preferred=0x%llX delta=0x%llX expected=%ls raw=%ls normalized=%ls",
            hook.rva, image, static_cast<unsigned long long>(preferredImageBase),
            static_cast<unsigned long long>(delta), expected, raw, actual);
        try { log(message); } catch (...) {}
    }
    return matches;
}

const GameProfile* FindGameProfileBySha256(
    const std::array<std::uint8_t, 32>& sha256) noexcept {
    for (const auto& profile : kProfiles)
        if (profile.sha256 == sha256) return &profile;
    return nullptr;
}

} // namespace batman_mods
