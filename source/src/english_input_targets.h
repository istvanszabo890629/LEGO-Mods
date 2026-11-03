#pragma once

#include "english_plus.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace batman_mods {

// Native input-name display dictionary. Language initialization points the
// active lookup pointers directly at this table, rather than copying labels.
// Native consumers include 32-byte buffers: the proven per-key limits below
// bound complete formatted names, including numeric values, low/high labels,
// optional two-byte wrappers and the final NUL. The two format targets permit
// only their explicitly declared argument types; they are not plain text.
struct EnglishInputLabel {
    const char* key;
    const char* expected;
    std::uint32_t index;
    std::size_t maxValueBytes;
    EnglishTextKind kind;
};

inline constexpr std::array<EnglishInputLabel, 56> kEnglishInputLabels{{
    {"InputHatUp", "Hat Up", 0, 29, EnglishTextKind::Plain},
    {"InputHatRight", "Hat Right", 1, 29, EnglishTextKind::Plain},
    {"InputHatDown", "Hat Down", 2, 29, EnglishTextKind::Plain},
    {"InputHatLeft", "Hat Left", 3, 29, EnglishTextKind::Plain},
    {"InputButton", "Button", 4, 22, EnglishTextKind::Plain},
    {"InputLow", "Low", 5, 10, EnglishTextKind::Plain},
    {"InputHigh", "High", 6, 10, EnglishTextKind::Plain},
    {"InputAnalogueFormat", "Analogue %d (%s)", 7, 16, EnglishTextKind::IntStringFormat},
    {"InputAxisX", "X Axis", 8, 18, EnglishTextKind::Plain},
    {"InputAxisY", "Y Axis", 9, 18, EnglishTextKind::Plain},
    {"InputAxisZ", "Z Axis", 10, 18, EnglishTextKind::Plain},
    {"InputRotationX", "X Rotation", 11, 18, EnglishTextKind::Plain},
    {"InputRotationY", "Y Rotation", 12, 18, EnglishTextKind::Plain},
    {"InputRotationZ", "Z Rotation", 13, 18, EnglishTextKind::Plain},
    {"InputLeftStickLeft", "Left Stick Left", 14, 29, EnglishTextKind::Plain},
    {"InputLeftStickRight", "Left Stick Right", 15, 29, EnglishTextKind::Plain},
    {"InputLeftStickUp", "Left Stick Up", 16, 29, EnglishTextKind::Plain},
    {"InputLeftStickDown", "Left Stick Down", 17, 29, EnglishTextKind::Plain},
    {"InputRightStickLeft", "Right Stick Left", 18, 29, EnglishTextKind::Plain},
    {"InputRightStickRight", "Right Stick Right", 19, 29, EnglishTextKind::Plain},
    {"InputRightStickUp", "Right Stick Up", 20, 29, EnglishTextKind::Plain},
    {"InputRightStickDown", "Right Stick Down", 21, 29, EnglishTextKind::Plain},
    {"InputButtonY", "Button Y", 22, 29, EnglishTextKind::Plain},
    {"InputButtonB", "Button B", 23, 29, EnglishTextKind::Plain},
    {"InputButtonA", "Button A", 24, 29, EnglishTextKind::Plain},
    {"InputButtonX", "Button X", 25, 29, EnglishTextKind::Plain},
    {"InputLeftShoulder", "Left Shoulder", 26, 29, EnglishTextKind::Plain},
    {"InputRightShoulder", "Right Shoulder", 27, 29, EnglishTextKind::Plain},
    {"InputLeftTrigger", "Left Trigger", 28, 29, EnglishTextKind::Plain},
    {"InputRightTrigger", "Right Trigger", 29, 29, EnglishTextKind::Plain},
    {"InputLeftThumbStick", "Left Thumb Stick", 30, 29, EnglishTextKind::Plain},
    {"InputRightThumbStick", "Right Thumb Stick", 31, 29, EnglishTextKind::Plain},
    {"InputStart", "Start", 32, 29, EnglishTextKind::Plain},
    {"InputBack", "Back", 33, 29, EnglishTextKind::Plain},
    {"InputMouseX", "Mouse X", 34, 29, EnglishTextKind::Plain},
    {"InputMouseY", "Mouse Y", 35, 29, EnglishTextKind::Plain},
    {"InputMouseZ", "Mouse Z", 36, 29, EnglishTextKind::Plain},
    {"InputMouseLeft", "Mouse Left", 37, 29, EnglishTextKind::Plain},
    {"InputMouseUp", "Mouse Up", 38, 29, EnglishTextKind::Plain},
    {"InputMouseWheelDown", "Mouse Wheel Down", 39, 29, EnglishTextKind::Plain},
    {"InputMouseRight", "Mouse Right", 40, 29, EnglishTextKind::Plain},
    {"InputMouseDown", "Mouse Down", 41, 29, EnglishTextKind::Plain},
    {"InputMouseWheelUp", "Mouse Wheel Up", 42, 29, EnglishTextKind::Plain},
    {"InputMouseLeftButton", "Mouse Left Button", 43, 29, EnglishTextKind::Plain},
    {"InputMouseRightButton", "Mouse Right Button", 44, 29, EnglishTextKind::Plain},
    {"InputMouseMiddleButton", "Mouse Middle Button", 45, 29, EnglishTextKind::Plain},
    {"InputMouseButtonFormat", "Mouse Button %d", 46, 23, EnglishTextKind::IntFormat},
    {"InputNoInput", "No Input", 47, 29, EnglishTextKind::Plain},
    {"InputControllerNotSupported", "Controller Not Supported", 48, 29, EnglishTextKind::Plain},
    {"InputNotEnoughButtons", "Not Enough Buttons", 49, 29, EnglishTextKind::Plain},
    {"InputConfigureTheseItems", "You must configure these items", 50, 30, EnglishTextKind::Plain},
    {"InputConfigureAllItems", "You must configure all items", 51, 29, EnglishTextKind::Plain},
    {"InputPovUp", "POV Up", 52, 29, EnglishTextKind::Plain},
    {"InputPovRight", "POV Right", 53, 29, EnglishTextKind::Plain},
    {"InputPovDown", "POV Down", 54, 29, EnglishTextKind::Plain},
    {"InputPovLeft", "POV Left", 55, 29, EnglishTextKind::Plain},
}};

constexpr std::array<EnglishTextTarget, 56> MakeEnglishInputTargets(
    std::uint32_t tableRva) {
    std::array<EnglishTextTarget, 56> targets{};
    for (std::size_t i = 0; i < kEnglishInputLabels.size(); ++i) {
        const auto& label = kEnglishInputLabels[i];
        targets[i] = {label.key, label.expected, tableRva + label.index * 4,
                      label.maxValueBytes, label.kind};
    }
    return targets;
}

inline constexpr auto kSteamEnglishInputTargets = MakeEnglishInputTargets(0x545F80);
inline constexpr auto kGogEnglishInputTargets = MakeEnglishInputTargets(0x544F80);
inline constexpr auto kEpicEnglishInputTargets = MakeEnglishInputTargets(0x544F80);

} // namespace batman_mods
