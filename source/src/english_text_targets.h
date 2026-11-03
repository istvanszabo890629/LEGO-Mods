#pragma once

#include "english_plus.h"
#include "english_input_targets.h"
#include "game_profiles.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace batman_mods {

// Verified native engine settings dictionary, independent of english.txt's
// numeric text table and trc.csv's named strings. The engine copies one of the
// two English dictionaries below into its lazy active settings lookup table.
// Each key is a plain label (no printf directives). Only verified pointer cells
// are replaced, never arbitrary literal matches or a global byte search.
struct EnglishSettingsLabel {
    const char* key;
    const char* expected;
};

inline constexpr std::array<EnglishSettingsLabel, 40> kEnglishSettingsLabels{{
    {"Quit", "Quit"},
    {"QuitToWindows", "Quit to Windows"},
    {"QuitToDemosMenu", "Quit to Demos Menu"},
    {"ReturnToGame", "Return to game"},
    {"Low", "Low"},
    {"Medium", "Medium"},
    {"High", "High"},
    {"Resume", "Resume"},
    {"Accept", "Accept"},
    {"Decline", "Decline"},
    {"Back", "Back"},
    {"Yes", "Yes"},
    {"No", "No"},
    {"OK", "OK"},
    {"Cancel", "Cancel"},
    {"On", "On"},
    {"Off", "Off"},
    {"ResetToDefaults", "Reset To Defaults"},
    {"VideoSettings", "Video Settings"},
    {"ScreenResolution", "Screen Resolution"},
    {"RefreshRate", "Refresh Rate"},
    {"VerticalSync", "Vertical Sync"},
    {"AspectRatio", "Aspect Ratio"},
    {"EnhancedGraphics", "Enhanced Graphics"},
    {"FromScreenResolution", "From Screen-Res"},
    {"UseCurrentSettings", "Use current settings"},
    {"TextureQuality", "Texture Quality"},
    {"Revert", "Revert"},
    {"RestartRequired", "You must restart the game for these changes to take effect!"},
    {"EffectsSettings", "Effects Settings"},
    {"FSAA", "FSAA"},
    {"ExternallySet", "Externally Set"},
    {"Shadows", "Shadows"},
    {"Bloom", "Bloom"},
    {"DepthOfField", "Depth of Field"},
    {"SpeedBlur", "Speed Blur"},
    {"DynamicLightQuality", "Dynamic Light Quality"},
    {"EdgeAA", "Edge AA"},
    {"AmbientOcclusion", "Ambient Occlusion"},
    {"ShowGameControls", "Show Game Controls"},
}};

template<std::size_t Count>
constexpr std::array<EnglishTextTarget, Count * 2> MakeEnglishSettingsTargetsN(
    std::uint32_t firstTableRva, std::uint32_t secondTableRva,
    std::size_t maximumBytes = kMaximumEnglishPlusValueBytes) {
    static_assert(Count > 10 && Count <= kEnglishSettingsLabels.size());
    std::array<EnglishTextTarget, Count * 2> targets{};
    for (std::size_t id = 0; id < Count; ++id) {
        // The first dictionary stores rows 9 and 10 in opposite order. The IDs
        // themselves are explicit; preserving the row order matters for cells.
        const std::size_t firstRow = id == 9 ? 10 : id == 10 ? 9 : id;
        const auto& label = kEnglishSettingsLabels[id];
        targets[id * 2] = {label.key, label.expected,
            firstTableRva + static_cast<std::uint32_t>(firstRow * 8 + 4), maximumBytes};
        targets[id * 2 + 1] = {label.key, label.expected,
            secondTableRva + static_cast<std::uint32_t>(id * 8 + 4), maximumBytes};
    }
    return targets;
}

constexpr std::array<EnglishTextTarget, 80> MakeEnglishSettingsTargets(
    std::uint32_t firstTableRva, std::uint32_t secondTableRva,
    std::size_t maximumBytes = kMaximumEnglishPlusValueBytes) {
    return MakeEnglishSettingsTargetsN<40>(firstTableRva, secondTableRva, maximumBytes);
}

inline constexpr auto kSteamEnglishTextTargets =
    MakeEnglishSettingsTargets(0x54D5B0, 0x54D6F8);
inline constexpr auto kGogEnglishTextTargets =
    MakeEnglishSettingsTargets(0x54C5B0, 0x54C6F8);
inline constexpr auto kEpicEnglishTextTargets =
    MakeEnglishSettingsTargets(0x54C5B0, 0x54C6F8);
// IJ2 shares the 40 explicit settings IDs, not LB1's input dictionary.
// The exact Steam and GOG builds have the same verified English cell RVAs.
// Its native 256-byte formatter can combine two labels. Limit each to 64 bytes.
inline constexpr auto kIndy2EnglishTextTargets =
    MakeEnglishSettingsTargets(0xAB0B58, 0xAB0CA0, 64);
// IJ1 has only IDs 0..37. Its row 38 is the terminator, not a text target.
// Both exact Steam and GOG builds use these cells; other builds remain gated.
inline constexpr auto kIndy1EnglishTextTargets =
    MakeEnglishSettingsTargetsN<38>(0x521D10, 0x521E48, 64);

// TCS has 39 rows: IDs 0..37 match the shared labels; ID 38 is Show Game
// Controls, without the Ambient Occlusion row found in the later IJ2 build.
constexpr std::array<EnglishTextTarget, 78> MakeTcsEnglishTextTargets() {
    const auto common = MakeEnglishSettingsTargetsN<38>(0x427800, 0x427940, 64);
    std::array<EnglishTextTarget, 78> result{};
    for (std::size_t i = 0; i < common.size(); ++i) result[i] = common[i];
    result[76] = {"ShowGameControls", "Show Game Controls", 0x427934, 64};
    result[77] = {"ShowGameControls", "Show Game Controls", 0x427A74, 64};
    return result;
}
inline constexpr auto kTcsEnglishTextTargets = MakeTcsEnglishTextTargets();

template<std::size_t N, std::size_t M>
constexpr std::array<EnglishTextTarget, N + M> CombineEnglishTargets(
    const std::array<EnglishTextTarget, N>& settings,
    const std::array<EnglishTextTarget, M>& input) {
    std::array<EnglishTextTarget, N + M> result{};
    for (std::size_t i = 0; i < N; ++i) result[i] = settings[i];
    for (std::size_t i = 0; i < M; ++i) result[N + i] = input[i];
    return result;
}

inline constexpr auto kSteamAllEnglishTextTargets =
    CombineEnglishTargets(kSteamEnglishTextTargets, kSteamEnglishInputTargets);
inline constexpr auto kGogAllEnglishTextTargets =
    CombineEnglishTargets(kGogEnglishTextTargets, kGogEnglishInputTargets);
inline constexpr auto kEpicAllEnglishTextTargets =
    CombineEnglishTargets(kEpicEnglishTextTargets, kEpicEnglishInputTargets);

inline EnglishTextTargetView GetEnglishTextTargets(BuildId build) noexcept {
    switch (build) {
    case BuildId::None:
        return {};
    case BuildId::SteamEnglish:
        return {kSteamAllEnglishTextTargets.data(), kSteamAllEnglishTextTargets.size()};
    case BuildId::GogEnglish:
        return {kGogAllEnglishTextTargets.data(), kGogAllEnglishTextTargets.size()};
    case BuildId::EpicEnglish:
        return {kEpicAllEnglishTextTargets.data(), kEpicAllEnglishTextTargets.size()};
    case BuildId::Indy2SteamEnglish:
    case BuildId::Indy2GogEnglish:
        return {kIndy2EnglishTextTargets.data(), kIndy2EnglishTextTargets.size()};
    case BuildId::Indy1SteamEnglish:
    case BuildId::Indy1GogEnglish:
        return {kIndy1EnglishTextTargets.data(), kIndy1EnglishTextTargets.size()};
    case BuildId::TcsGogEnglish:
        return {kTcsEnglishTextTargets.data(), kTcsEnglishTextTargets.size()};
    }
    return {};
}

inline const wchar_t* GetEnglishTextConfig(BuildId build) noexcept {
    switch (build) {
    case BuildId::SteamEnglish:
    case BuildId::GogEnglish:
    case BuildId::EpicEnglish: return L"ENGLISH_PLUS_LB1.ini";
    case BuildId::Indy2SteamEnglish:
    case BuildId::Indy2GogEnglish: return L"ENGLISH_PLUS_IJ2.ini";
    case BuildId::Indy1SteamEnglish:
    case BuildId::Indy1GogEnglish: return L"ENGLISH_PLUS_IJ1.ini";
    case BuildId::TcsGogEnglish: return L"ENGLISH_PLUS_TCS.ini";
    default: return nullptr;
    }
}

}  // namespace batman_mods
