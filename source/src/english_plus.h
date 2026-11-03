#pragma once

#include "game_profiles.h"
#include "redirector.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace batman_mods {

// The settings formatter uses fixed native buffers. Individual target families
// may impose a smaller proven limit through EnglishTextTarget::maxValueBytes.
inline constexpr std::size_t kMaximumEnglishPlusValueBytes = 96;

enum class EnglishTextKind {
    Plain,
    IntFormat,
    IntStringFormat
};

// Each entry describes a verified pointer cell, not an arbitrary string scan.
// Multiple pointer cells may share one key. Format kinds describe the exact
// argument sequence of separately reviewed, bounded native callers.
struct EnglishTextTarget {
    const char* key;
    const char* expected;
    std::uint32_t pointerRva;
    std::size_t maxValueBytes = kMaximumEnglishPlusValueBytes;
    EnglishTextKind kind = EnglishTextKind::Plain;
};

struct EnglishTextTargetView {
    const EnglishTextTarget* data = nullptr;
    std::size_t size = 0;
};

enum class EnglishPlusStatus {
    NoFile,
    NoOverrides,
    Applied,
    AlreadyInitialized,
    Error
};

struct EnglishPlusResult {
    EnglishPlusStatus status = EnglishPlusStatus::Error;
    std::size_t appliedKeys = 0;
    std::size_t patchedPointers = 0;
    std::size_t ignoredKeys = 0;
    std::size_t invalidLines = 0;
};

// Startup-only. Call after validating the full supported executable hash and
// outside DllMain. Reads the verified build's separate textConfigFile once.
// LB1, IJ1 and IJ2 dictionaries are independently selected; [Strings] and
// ASCII keys are case-insensitive; duplicate keys use the last assignment.
// Values are literal UTF-8 (no escape syntax), trimmed at their edges. Values
// over the global or target-specific byte limit are rejected. Plain labels
// cannot contain '%'; reviewed format labels must retain their exact ordered
// %d or %d/%s tokens, with no other percent syntax.
// Missing/empty/invalid values keep
// the original label. Only valid BMP UTF-8 is accepted by this game's decoder.
// Successfully installed strings remain allocated for the process lifetime.
EnglishPlusResult InitializeEnglishPlus(const std::filesystem::path& gameDirectory,
                                       const GameProfile& profile, LogFn log);

#ifdef BATMAN_MODS_TESTING
namespace english_plus_testing {

// Fixture-only API. It does not inspect, modify or execute another process.
// The fixture image must contain both original C strings and pointer cells.
EnglishPlusResult Apply(const char* iniBytes, std::size_t byteCount,
                        void* imageBase, std::size_t imageSize,
                        EnglishTextTargetView targets, LogFn log = nullptr);

}  // namespace english_plus_testing
#endif

}  // namespace batman_mods
