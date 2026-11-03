#pragma once
#include <filesystem>
#include <string_view>

namespace batman_mods {

// One ordinary child folder of the executable directory. Asset subdirectories
// remain unrestricted by this option and retain the resolver's existing checks.
constexpr bool ValidModsFolderName(std::wstring_view name) noexcept {
    if (name.empty() || name.size() > 128 || name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t c : name) {
        if (c < 32 || c == 127 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') return false;
    }
    auto stem = name.substr(0, name.find(L'.'));
    auto upper = [](wchar_t c) constexpr { return c >= L'a' && c <= L'z' ? wchar_t(c - L'a' + L'A') : c; };
    auto equal = [&](std::wstring_view word) constexpr {
        if (stem.size() != word.size()) return false;
        for (std::size_t i = 0; i < stem.size(); ++i) if (upper(stem[i]) != word[i]) return false;
        return true;
    };
    if (equal(L"CON") || equal(L"PRN") || equal(L"AUX") || equal(L"NUL") ||
        equal(L"CONIN$") || equal(L"CONOUT$")) return false;
    if (stem.size() == 4 && stem[3] >= L'1' && stem[3] <= L'9' &&
        ((upper(stem[0]) == L'C' && upper(stem[1]) == L'O' && upper(stem[2]) == L'M') ||
         (upper(stem[0]) == L'L' && upper(stem[1]) == L'P' && upper(stem[2]) == L'T'))) return false;
    return true;
}

namespace settings_detail {
constexpr std::wstring_view Trim(std::wstring_view value) noexcept {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.remove_suffix(1);
    return value;
}
constexpr bool Equal(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto fold = [](wchar_t c) constexpr { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; };
        if (fold(a[i]) != fold(b[i])) return false;
    }
    return true;
}
struct FolderSetting { bool valid; std::wstring_view name; bool loggingEnabled = false; };
constexpr FolderSetting Parse(std::wstring_view text) noexcept {
    for (wchar_t c : text) if ((c < 32 && c != L'\t' && c != L'\r' && c != L'\n') || c == 127) return {false, {}};
    bool files = false, logging = false, found = false, loggingFound = false, loggingEnabled = false, loggingValid = true;
    std::wstring_view folder(L"mods");
    auto remaining = text;
    while (!remaining.empty()) {
        const auto end = remaining.find_first_of(L"\r\n");
        const auto line = Trim(remaining.substr(0, end));
        if (end == std::wstring_view::npos) remaining = {};
        else remaining.remove_prefix(end + 1);
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.front() == L'[') {
            if (line.back() != L']') return {false, {}};
            files = Equal(Trim(line.substr(1, line.size() - 2)), L"Files");
            logging = Equal(Trim(line.substr(1, line.size() - 2)), L"Logging");
            continue;
        }
        if (!files && !logging) continue;
        const auto equals = line.find(L'=');
        if (equals == std::wstring_view::npos) return {false, {}};
        const auto key = Trim(line.substr(0, equals));
        if (files && !Equal(key, L"ModsFolder")) continue;
        if (logging && !Equal(key, L"Enabled")) continue;
        auto value = Trim(line.substr(equals + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
        if (logging) {
            // A bad logging value must not change a valid asset folder.
            // Ambiguous or unsupported values conservatively keep logging off.
            if (loggingFound || (value != L"0" && value != L"1")) loggingValid = false;
            else loggingEnabled = value == L"1";
            loggingFound = true;
            continue;
        }
        if (found) return {false, {}};
        if (!ValidModsFolderName(value)) return {false, {}};
        folder = value; found = true;
    }
    return {true, folder, loggingEnabled && loggingValid};
}
} // namespace settings_detail

// Read once per executable directory, outside DllMain. The optional UTF-8
// LegoMods.ini lives beside the EXE; missing/invalid settings keep "mods".
std::filesystem::path GetOverrideDirectory(const std::filesystem::path& gameDirectory,
                                         void (*log)(const wchar_t*) = nullptr);

// Same cached startup settings as the asset folder. Missing or invalid settings
// keep logging off. This query performs no logging and creates no output file.
bool ModuleLoggingEnabled(const std::filesystem::path& gameDirectory);

} // namespace batman_mods
