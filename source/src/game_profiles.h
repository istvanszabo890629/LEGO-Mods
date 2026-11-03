#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace batman_mods {

enum class BuildId {
    None,
    SteamEnglish,
    GogEnglish,
    EpicEnglish,
    Indy2SteamEnglish,
    Indy1SteamEnglish,
    Indy1GogEnglish,
    Indy2GogEnglish,
    TcsGogEnglish,
};

enum class RedirectorBackend {
    BatmanVfs,
    NuFileDevice,
    NuFileDeviceSmart,
    NuFileDeviceSmartCString,
    NuFileDeviceSmart64,
    NuFileDeviceSmartCString64,
    NuFileDeviceShared64,
    NuFileExtendedRaw,
    NuFileExtendedIntrusive,
    NuFileExtendedShared,
    FlatVfs,
    NttFile64,
};

struct NativeHook {
    std::uint32_t rva;
    std::array<std::uint8_t, 16> prologue;
    // Base relocation fields beginning within the sampled prologue:
    // HIGHLOW DWORDs on x86, DIR64 QWORDs on x64. RIP-relative operands are not
    // image-base relocations and must not be listed here.
    std::array<std::uint8_t, 4> relocations{};
    std::uint8_t relocationCount = 0;
};

// A relocation field can extend beyond the comparison sample. Read the rest
// only when that field actually occurs; invalid metadata requests no read.
constexpr std::size_t NativeHookReadSize(const NativeHook& hook) noexcept {
    if (hook.relocationCount > hook.relocations.size()) return 0;
    std::size_t required = hook.prologue.size();
    for (std::size_t i = 0; i < hook.relocationCount; ++i) {
        const auto offset = hook.relocations[i];
        if (offset >= hook.prologue.size()) return 0;
        const auto end = offset + sizeof(std::uintptr_t);
        if (end > required) required = end;
    }
    return required;
}

struct GameProfile {
    BuildId id;
    const wchar_t* label;
    std::uint64_t fileSize;
    std::array<std::uint8_t, 32> sha256;
    std::uint32_t imageSize;
    // BatmanVfs: open, exists, size, whole-file, WAVE.
    // FlatVfs: the same first four cdecl contracts; no WAVE target.
    // NuFile: DAT open, DAT size, PC factory, bool offset, direct offset.
    // Shared64 and extended owned backends use existence in their fifth slot.
    // Extended profiles may omit metadata slots which have no native routine.
    std::array<NativeHook, 5> hooks;
    RedirectorBackend backend = RedirectorBackend::BatmanVfs;
    // Null for games without a separately verified native text dictionary.
    const wchar_t* textConfigFile = nullptr;
    std::uint16_t machine = 0x14c; // IMAGE_FILE_MACHINE_I386; AMD64 is 0x8664.
    std::uint32_t pcDeviceVtableRva = 0;
    NativeHook releaseControl{};
    NativeHook pcDeviceLookup{};
    std::uint32_t pcDeviceId = 1;
    // Immutable on-disk ImageBase. Windows can rewrite the mapped PE header.
    std::uint64_t preferredImageBase = 0;
    // Live-code evidence is valid only at the captured preferred image base.
    bool preferredBaseOnly = false;
    // nttFile: global open, size, file-info, existence and PC factory in hooks.
    // Direct DAT open/size can bypass those global wrappers. Keep their
    // separate signatures without changing the old five-target contracts.
    std::array<NativeHook, 2> nttDatHooks{};
    std::uint32_t nttDatDeviceVtableRva = 0;
    std::uint32_t nttPcFileVtableRva = 0;
};

// The caller must hash the complete on-disk main executable. Filenames are not
// used as identity: Epic launches its actual engine as Game.exe. Unknown
// builds and the excluded legacy HuCzPL version intentionally fail. Wrapped
// exact builds can use separately verified fixed-base runtime code evidence.
const GameProfile* FindGameProfileBySha256(
    const std::array<std::uint8_t, 32>& sha256) noexcept;

bool NativeHookMatches(const unsigned char* image, std::uint32_t imageSize,
                       const NativeHook& hook, std::uint64_t preferredImageBase,
                       void (*log)(const wchar_t*) = nullptr) noexcept;

} // namespace batman_mods
