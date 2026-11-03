// EnglishPlus tests modify only their own allocated fixture memory.
// No game, external executable or native game code is loaded or executed.
#ifndef BATMAN_MODS_TESTING
#define BATMAN_MODS_TESTING
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../src/english_plus.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

static_assert(sizeof(void*) == 4, "Build the EnglishPlus fixture for x86.");

namespace {
using namespace batman_mods;
int g_checks = 0;

void Check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) throw std::runtime_error(description);
}

struct Fixture {
    static constexpr std::size_t kBytes = 4096;
    unsigned char* base = nullptr;
    std::array<EnglishTextTarget, 4> targets{{
        {"ScreenResolution", "Screen Resolution", 0x100},
        {"ScreenResolution", "Screen Resolution", 0x108},
        {"ResetToDefaults", "Reset To Defaults", 0x110},
        {"ResetToDefaults", "Reset To Defaults", 0x118},
    }};

    Fixture() {
        base = static_cast<unsigned char*>(VirtualAlloc(nullptr, kBytes,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!base) throw std::runtime_error("fixture allocation failed");
        std::memcpy(base + 0x400, "Screen Resolution", sizeof("Screen Resolution"));
        std::memcpy(base + 0x480, "Reset To Defaults", sizeof("Reset To Defaults"));
        Set(0x100, reinterpret_cast<const char*>(base + 0x400));
        Set(0x108, reinterpret_cast<const char*>(base + 0x400));
        Set(0x110, reinterpret_cast<const char*>(base + 0x480));
        Set(0x118, reinterpret_cast<const char*>(base + 0x480));
        ReadOnly();
    }
    ~Fixture() { if (base) VirtualFree(base, 0, MEM_RELEASE); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void ReadOnly() {
        DWORD old = 0;
        if (!VirtualProtect(base, kBytes, PAGE_READONLY, &old))
            throw std::runtime_error("fixture page protection failed");
    }
    void Writeable() {
        DWORD old = 0;
        if (!VirtualProtect(base, kBytes, PAGE_READWRITE, &old))
            throw std::runtime_error("fixture writable preparation failed");
    }
    void Set(std::size_t rva, const char* pointer) {
        std::memcpy(base + rva, &pointer, sizeof(pointer));
    }
    const char* Get(std::size_t rva) const {
        const char* pointer = nullptr;
        std::memcpy(&pointer, base + rva, sizeof(pointer));
        return pointer;
    }
    EnglishPlusResult Apply(const std::string& ini) {
        return english_plus_testing::Apply(ini.data(), ini.size(), base, kBytes,
                                           {targets.data(), targets.size()});
    }
    bool OriginalScreen() const {
        return Get(0x100) == reinterpret_cast<const char*>(base + 0x400) &&
               Get(0x108) == reinterpret_cast<const char*>(base + 0x400);
    }
    bool OriginalReset() const {
        return Get(0x110) == reinterpret_cast<const char*>(base + 0x480) &&
               Get(0x118) == reinterpret_cast<const char*>(base + 0x480);
    }
    bool IsReadOnly() const {
        MEMORY_BASIC_INFORMATION info{};
        return VirtualQuery(base, &info, sizeof(info)) && info.Protect == PAGE_READONLY;
    }
};

struct FormatFixture : Fixture {
    FormatFixture() {
        Writeable();
        std::memcpy(base + 0x400, "Analogue %d (%s)", sizeof("Analogue %d (%s)"));
        std::memcpy(base + 0x480, "Mouse Button %d", sizeof("Mouse Button %d"));
        targets = {{
            {"InputAnalogueFormat", "Analogue %d (%s)", 0x100, 16, EnglishTextKind::IntStringFormat},
            {"InputAnalogueFormat", "Analogue %d (%s)", 0x108, 16, EnglishTextKind::IntStringFormat},
            {"InputMouseButtonFormat", "Mouse Button %d", 0x110, 23, EnglishTextKind::IntFormat},
            {"InputMouseButtonFormat", "Mouse Button %d", 0x118, 23, EnglishTextKind::IntFormat},
        }};
        ReadOnly();
    }
};

void SuccessfulPatch() {
    Fixture fixture;
    const std::string value = u8"Képernyőfelbontás";
    const std::string ini = std::string("\xef\xbb\xbf") +
        "; comment\r\n[Other]\r\nScreenResolution=IGNORED\r\n"
        "[sTrInGs] ; section comment\r\nScreenResolution=First\r\n"
        "screenresolution= " + value + " \r\nResetToDefaults=A=B ;literal #data\r\n";
    const auto result = fixture.Apply(ini);
    Check(result.status == EnglishPlusStatus::Applied, "valid INI applies");
    Check(result.appliedKeys == 2 && result.patchedPointers == 4, "two keys patch four verified cells");
    Check(std::strcmp(fixture.Get(0x100), value.c_str()) == 0, "BMP UTF-8 retained without ACP loss");
    Check(fixture.Get(0x100) == fixture.Get(0x108), "duplicate target keys share persistent storage");
    Check(std::strcmp(fixture.Get(0x110), "A=B ;literal #data") == 0, "first equals splits and inline punctuation is literal");
    Check(fixture.IsReadOnly(), "original page protection restored");
}

void LiteralBackslashesAndMissingKeys() {
    Fixture fixture;
    const auto result = fixture.Apply("[Strings]\nResetToDefaults=A\\B\\nC\n");
    Check(result.status == EnglishPlusStatus::Applied && result.appliedKeys == 1,
          "one-key partial override applies");
    Check(fixture.OriginalScreen(), "missing key preserves original pointer");
    Check(std::strcmp(fixture.Get(0x110), "A\\B\\nC") == 0, "backslash sequences are not interpreted");
}

void RejectedValue(const std::string& value, const char* description) {
    Fixture fixture;
    const auto result = fixture.Apply("[Strings]\nScreenResolution=" + value + "\n");
    Check(result.status == EnglishPlusStatus::NoOverrides, description);
    Check(fixture.OriginalScreen() && fixture.OriginalReset(), "invalid value leaves all pointers original");
    Check(fixture.IsReadOnly(), "invalid value never changes page protection");
}

void InvalidValues() {
    RejectedValue("", "empty value retains original");
    RejectedValue(std::string(kMaximumEnglishPlusValueBytes + 1, 'x'), "excessive byte count rejected");
    RejectedValue("100%", "format metacharacter rejected");
    RejectedValue(std::string("\xc0\xaf", 2), "overlong UTF-8 rejected");
    RejectedValue(std::string("\xe2\x82", 2), "truncated UTF-8 rejected");
    RejectedValue(std::string("\xed\xa0\x80", 3), "UTF-8 surrogate rejected");
    RejectedValue(std::string("\xf0\x9f\x98\x80", 4), "non-BMP UTF-8 rejected for native decoder");
    RejectedValue(std::string("a\x01" "b", 3), "embedded control rejected");
    RejectedValue(std::string("\xc2\x85", 2), "C1 control rejected");
    Fixture maximum;
    const auto ok = maximum.Apply("[Strings]\nScreenResolution=" +
                                  std::string(kMaximumEnglishPlusValueBytes, 'x') + "\n");
    Check(ok.status == EnglishPlusStatus::Applied, "maximum safe byte count accepted");
}

void DuplicateAndUnknownKeys() {
    Fixture fixture;
    const auto result = fixture.Apply("[Strings]\nScreenResolution=First\nSCREENRESOLUTION=\n"
                                      "Unrecognized=Value\nMalformed line\n");
    Check(result.status == EnglishPlusStatus::NoOverrides, "last empty assignment disables earlier valid value");
    Check(result.ignoredKeys == 2 && result.invalidLines == 1, "unknown and invalid entries counted");
    Check(fixture.OriginalScreen(), "last empty duplicate does not retain earlier translation");
}

void PerTargetLimits() {
    Fixture fixture;
    // A key used by multiple native consumers must obey the smallest limit.
    fixture.targets[0].maxValueBytes = 8;
    fixture.targets[1].maxValueBytes = 64;
    const auto result = fixture.Apply("[Strings]\nScreenResolution=123456789\nResetToDefaults=Short\n");
    Check(result.status == EnglishPlusStatus::Applied && result.appliedKeys == 1 &&
          result.patchedPointers == 2, "safe key applies while a lower-cap key is rejected");
    Check(result.ignoredKeys == 1 && result.invalidLines == 1, "per-target excessive length is counted");
    Check(fixture.OriginalScreen(), "minimum cap across duplicate targets preserves both original cells");
    Check(std::strcmp(fixture.Get(0x110), "Short") == 0 && fixture.IsReadOnly(),
          "independent permitted key patches with restored page protection");
    Fixture boundary;
    boundary.targets[0].maxValueBytes = 8;
    const auto exact = boundary.Apply("[Strings]\nScreenResolution=12345678\n");
    Check(exact.status == EnglishPlusStatus::Applied && exact.patchedPointers == 2,
          "exact smallest target byte limit is accepted");
}

void TypedFormatLabels() {
    FormatFixture fixture;
    const auto result = fixture.Apply(u8"[Strings]\nInputAnalogueFormat=Kar %d (%s)\nInputMouseButtonFormat=Egérgomb %d\n");
    Check(result.status == EnglishPlusStatus::Applied && result.appliedKeys == 2 &&
          result.patchedPointers == 4, "reviewed typed format strings apply");
    Check(std::strcmp(fixture.Get(0x100), "Kar %d (%s)") == 0,
          "integer/string format preserves approved argument order");
    Check(std::strcmp(fixture.Get(0x110), u8"Egérgomb %d") == 0,
          "integer format preserves UTF-8 literal bytes");
    char output[32]{};
    const int analogueLength = std::snprintf(output, sizeof(output), fixture.Get(0x100), -32768, "1234567890");
    Check(analogueLength >= 0 && analogueLength + 2 < static_cast<int>(sizeof(output)),
          "approved integer/string fixture fits native-size buffer including wrapper");
    const int mouseLength = std::snprintf(output, sizeof(output), fixture.Get(0x110), -32768);
    Check(mouseLength >= 0 && mouseLength + 2 < static_cast<int>(sizeof(output)),
          "approved integer fixture fits native-size buffer including wrapper");
    Check(fixture.IsReadOnly(), "typed format patch restores original protection");
}

void RejectedFormat(const char* key, const std::string& value, const char* description) {
    FormatFixture fixture;
    const auto result = fixture.Apply(std::string("[Strings]\n") + key + '=' + value + '\n');
    Check(result.status == EnglishPlusStatus::NoOverrides && result.ignoredKeys == 1 &&
          result.invalidLines == 1, description);
    Check(fixture.OriginalScreen() && fixture.OriginalReset(), "invalid format retains every original pointer");
}

void InvalidTypedFormats() {
    RejectedFormat("InputAnalogueFormat", "%s %d", "changed argument order rejected");
    RejectedFormat("InputAnalogueFormat", "%d %n", "write-count token rejected");
    RejectedFormat("InputAnalogueFormat", "%d %x", "unreviewed conversion type rejected");
    RejectedFormat("InputAnalogueFormat", "%d %% %s", "escaped percent syntax rejected");
    RejectedFormat("InputAnalogueFormat", "%d %s %s", "duplicate format token rejected");
    RejectedFormat("InputAnalogueFormat", "%d", "missing required string argument rejected");
    RejectedFormat("InputAnalogueFormat", "%d %10s", "string field width rejected");
    RejectedFormat("InputAnalogueFormat", "%ld %s", "integer length modifier rejected");
    RejectedFormat("InputAnalogueFormat", "%1$d %s", "positional argument syntax rejected");
    RejectedFormat("InputMouseButtonFormat", "%02d", "integer width/flag syntax rejected");
    RejectedFormat("InputMouseButtonFormat", "%d %d", "duplicate integer token rejected");
    RejectedFormat("InputMouseButtonFormat", "Button %", "trailing percent rejected");
    RejectedFormat("InputMouseButtonFormat", "Button %s", "wrong native argument type rejected");
    RejectedFormat("InputMouseButtonFormat", "%d %%", "escaped percent after approved token rejected");

    FormatFixture unknown;
    const auto ignored = unknown.Apply("[Strings]\nUnknownFormat=%n\n");
    Check(ignored.status == EnglishPlusStatus::NoOverrides && ignored.ignoredKeys == 1 &&
          ignored.invalidLines == 0, "unknown key is ignored without applying any format interpretation");

    FormatFixture wrongExpected;
    wrongExpected.targets[0].kind = EnglishTextKind::IntFormat;
    wrongExpected.targets[1].kind = EnglishTextKind::IntFormat;
    const auto invalidTarget = wrongExpected.Apply("[Strings]\nInputAnalogueFormat=%d\n");
    Check(invalidTarget.status == EnglishPlusStatus::Error && wrongExpected.OriginalScreen(),
          "target expected original must satisfy the same reviewed format kind");
}

void FileEncodingAndStructure() {
    Fixture nul;
    const std::string bytes("[Strings]\nScreenResolution=A\0B\n", 31);
    const auto invalidNul = nul.Apply(bytes);
    Check(invalidNul.status == EnglishPlusStatus::Error && nul.OriginalScreen(), "NUL in file rejected atomically");
    Fixture utf16;
    const auto wrong = utf16.Apply(std::string("\xff\xfe[\0S\0", 6));
    Check(wrong.status == EnglishPlusStatus::Error && utf16.OriginalScreen(), "UTF-16 file rejected");
    Fixture sections;
    const auto noSection = sections.Apply("ScreenResolution=Ignored\n[Malformed\nResetToDefaults=Ignored\n");
    Check(noSection.status == EnglishPlusStatus::NoOverrides && sections.OriginalScreen() &&
          sections.OriginalReset(), "keys outside valid section ignored");
    Fixture cr;
    const auto loneCr = cr.Apply("[Strings]\rScreenResolution=CR-only\r");
    Check(loneCr.status == EnglishPlusStatus::Applied, "lone CR line endings accepted");
}

void PrevalidationIsAtomic() {
    Fixture fixture;
    fixture.Writeable();
    // Make the final selected cell disagree with its verified original label.
    fixture.Set(0x118, reinterpret_cast<const char*>(fixture.base + 0x400));
    fixture.ReadOnly();
    const auto result = fixture.Apply("[Strings]\nScreenResolution=Replacement\nResetToDefaults=Reset\n");
    Check(result.status == EnglishPlusStatus::Error, "mismatched original aborts transaction");
    Check(fixture.OriginalScreen(), "earlier validated cells remain unchanged on later mismatch");
    Check(fixture.IsReadOnly(), "validation failure preserves original protection");
    Fixture outside;
    outside.targets[0].pointerRva = static_cast<std::uint32_t>(Fixture::kBytes + 4);
    const auto rejected = outside.Apply("[Strings]\nScreenResolution=Replacement\n");
    Check(rejected.status == EnglishPlusStatus::Error && outside.OriginalScreen(), "out-of-image target rejected");
    Fixture duplicate;
    duplicate.targets[1].pointerRva = duplicate.targets[0].pointerRva;
    const auto duplicateResult = duplicate.Apply("[Strings]\nScreenResolution=Replacement\n");
    Check(duplicateResult.status == EnglishPlusStatus::Error && duplicate.OriginalScreen(), "duplicate pointer cells rejected");
}
}

int main() {
    try {
        SuccessfulPatch();
        LiteralBackslashesAndMissingKeys();
        InvalidValues();
        DuplicateAndUnknownKeys();
        PerTargetLimits();
        TypedFormatLabels();
        InvalidTypedFormats();
        FileEncodingAndStructure();
        PrevalidationIsAtomic();
        std::cout << "PASS: " << g_checks << " EnglishPlus fixture checks.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
