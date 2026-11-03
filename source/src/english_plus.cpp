#include "memory_guard.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "english_plus.h"
#include "runtime_settings.h"
#include "english_text_targets.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

static_assert(sizeof(void*) == 4, "Build EnglishPlus for the x86 game.");

namespace batman_mods {
namespace {

constexpr std::size_t kMaximumIniBytes = 1024 * 1024;
constexpr std::size_t kMaximumKeyBytes = 96;
std::mutex g_initializationMutex;
bool g_initialized = false;

void Log(LogFn log, const wchar_t* message) noexcept {
    if (log) { try { log(message); } catch (...) {} }
}

void LogConfig(LogFn log, const wchar_t* name, const wchar_t* message) noexcept {
    if (!log) return;
    try { const auto text = std::wstring(name) + message; Log(log, text.c_str()); }
    catch (...) {}
}

void Summary(LogFn log, const EnglishPlusResult& result, const wchar_t* name) noexcept {
    if (!log) return;
    try {
        const auto message = std::wstring(name) + L": keys applied=" +
            std::to_wstring(result.appliedKeys) + L", pointer cells=" +
            std::to_wstring(result.patchedPointers) + L", keys ignored=" +
            std::to_wstring(result.ignoredKeys) + L", invalid lines=" +
            std::to_wstring(result.invalidLines);
        Log(log, message.c_str());
    } catch (...) {}
}

void LogKey(LogFn log, const wchar_t* prefix, const std::string& key) noexcept {
    if (!log) return;
    try {
        const std::wstring wide(key.begin(), key.end()); // Keys are ASCII only.
        const std::wstring message = std::wstring(prefix) + wide;
        Log(log, message.c_str());
    } catch (...) {}
}

void LogConfigKey(LogFn log, const wchar_t* name, const wchar_t* prefix,
                  const std::string& key) noexcept {
    if (!log) return;
    try { const auto text = std::wstring(name) + prefix; LogKey(log, text.c_str(), key); }
    catch (...) {}
}

std::string_view Trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string Lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    return result;
}

bool KeySyntax(std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaximumKeyBytes) return false;
    for (unsigned char c : key) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

// The native settings decoder supports one-, two- and three-byte UTF-8 only.
// Reject overlong encodings, surrogate code points, non-BMP code points and
// control characters instead of passing bytes the decoder cannot consume.
bool BmpUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return false;
            ++i;
        } else if (c >= 0xc2 && c <= 0xdf) {
            if (i + 1 >= text.size()) return false;
            const auto c1 = static_cast<unsigned char>(text[i + 1]);
            if (c1 < 0x80 || c1 > 0xbf) return false;
            const unsigned codePoint = ((c & 0x1f) << 6) | (c1 & 0x3f);
            if (codePoint >= 0x80 && codePoint <= 0x9f) return false;
            i += 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            if (i + 2 >= text.size()) return false;
            const auto c1 = static_cast<unsigned char>(text[i + 1]);
            const auto c2 = static_cast<unsigned char>(text[i + 2]);
            if (c1 < 0x80 || c1 > 0xbf || c2 < 0x80 || c2 > 0xbf ||
                (c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 >= 0xa0)) return false;
            i += 3;
        } else return false;
    }
    return true;
}

bool FormatSyntax(std::string_view text, EnglishTextKind kind) noexcept {
    const char* expected = nullptr;
    std::size_t expectedCount = 0;
    switch (kind) {
    case EnglishTextKind::Plain: break;
    case EnglishTextKind::IntFormat: expected = "d"; expectedCount = 1; break;
    case EnglishTextKind::IntStringFormat: expected = "ds"; expectedCount = 2; break;
    default: return false;
    }
    std::size_t encountered = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        // Deliberately reject %% as well as widths, flags, lengths, positions,
        // %n and all unreviewed types. The caller's native argument order must
        // remain exactly the approved sequence.
        if (encountered >= expectedCount || i + 1 >= text.size() ||
            text[i + 1] != expected[encountered]) return false;
        ++encountered;
        ++i;
    }
    return encountered == expectedCount;
}

struct IniEntry {
    std::string value;
    bool valid = false;
};

struct ParsedIni {
    std::map<std::string, IniEntry> entries;
    std::size_t invalidLines = 0;
    bool fatal = false;
};

ParsedIni Parse(const char* bytes, std::size_t count) {
    ParsedIni parsed;
    if ((!bytes && count) || count > kMaximumIniBytes) { parsed.fatal = true; return parsed; }
    std::string_view text(bytes ? bytes : "", count);
    if (text.find('\0') != std::string_view::npos ||
        (count >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xff &&
                        static_cast<unsigned char>(bytes[1]) == 0xfe) ||
                       (static_cast<unsigned char>(bytes[0]) == 0xfe &&
                        static_cast<unsigned char>(bytes[1]) == 0xff)))) {
        parsed.fatal = true;
        return parsed;
    }
    if (text.size() >= 3 && text.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3))
        text.remove_prefix(3);
    bool active = false;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto end = text.find_first_of("\r\n", cursor);
        auto line = Trim(text.substr(cursor, end == std::string_view::npos ?
                                             text.size() - cursor : end - cursor));
        if (end == std::string_view::npos) cursor = text.size();
        else {
            cursor = end + 1;
            if (text[end] == '\r' && cursor < text.size() && text[cursor] == '\n') ++cursor;
        }
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            active = false;
            const auto close = line.find(']');
            if (close == std::string_view::npos) { ++parsed.invalidLines; continue; }
            const auto tail = Trim(line.substr(close + 1));
            if (!tail.empty() && tail.front() != ';' && tail.front() != '#') {
                ++parsed.invalidLines;
                continue;
            }
            active = Lower(Trim(line.substr(1, close - 1))) == "strings";
            continue;
        }
        if (!active) continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) { ++parsed.invalidLines; continue; }
        const auto key = Trim(line.substr(0, equals));
        if (!KeySyntax(key)) { ++parsed.invalidLines; continue; }
        const auto value = Trim(line.substr(equals + 1));
        const bool valid = !value.empty() && value.size() <= kMaximumEnglishPlusValueBytes && BmpUtf8(value);
        if (!valid && !value.empty()) ++parsed.invalidLines;
        // The last assignment wins even when it disables an earlier value.
        parsed.entries[Lower(key)] = {std::string(value), valid};
    }
    return parsed;
}

using memory::Readable;

struct Patch {
    void* cell;
    const char* original;
    const char* replacement;
};

struct Page {
    void* address;
    DWORD original;
    bool writable = false;
};

bool Writable(Page& page, std::size_t pageSize) noexcept {
    DWORD ignored = 0;
    const DWORD previous = page.original & 0xff;
    const bool executable = previous == PAGE_EXECUTE || previous == PAGE_EXECUTE_READ ||
        previous == PAGE_EXECUTE_READWRITE || previous == PAGE_EXECUTE_WRITECOPY;
    page.writable = VirtualProtect(page.address, pageSize,
        executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &ignored) != FALSE;
    return page.writable;
}

bool RestorePages(std::vector<Page>& pages, std::size_t pageSize) noexcept {
    bool ok = true;
    for (auto& page : pages) if (page.writable) {
        DWORD ignored = 0;
        if (VirtualProtect(page.address, pageSize, page.original, &ignored)) page.writable = false;
        else ok = false;
    }
    return ok;
}

const char* CompareExchange(const Patch& patch, const char* expected, const char* replacement) noexcept {
    return static_cast<const char*>(InterlockedCompareExchangePointer(
        static_cast<PVOID volatile*>(patch.cell), const_cast<char*>(replacement),
        const_cast<char*>(expected)));
}

EnglishPlusResult Apply(const char* bytes, std::size_t count, void* imageBase,
                        std::size_t imageSize, EnglishTextTargetView targets, LogFn log,
                        const wchar_t* configName = L"ENGLISH_PLUS_LB1.ini") {
    EnglishPlusResult result;
    auto parsed = Parse(bytes, count);
    result.invalidLines = parsed.invalidLines;
    if (parsed.fatal) {
        LogConfig(log, configName, L" rejected: unsupported encoding, embedded NUL or excessive file size.");
        return result;
    }
    if (!imageBase || !imageSize || !targets.data || !targets.size) return result;
    const auto base = reinterpret_cast<std::uintptr_t>(imageBase);
    if (imageSize > std::numeric_limits<std::uintptr_t>::max() - base) return result;
    const auto imageEnd = base + imageSize;
    std::map<std::string, const char*> replacements;
    std::vector<std::unique_ptr<char[]>> allocations;
    std::vector<Patch> patches;
    std::set<std::uintptr_t> usedCells;
    allocations.reserve(parsed.entries.size());
    patches.reserve(targets.size);
    for (const auto& entry : parsed.entries) {
        bool known = false;
        std::size_t maximumBytes = kMaximumEnglishPlusValueBytes;
        bool validFormat = true;
        for (std::size_t i = 0; i < targets.size; ++i) {
            if (targets.data[i].key && Lower(targets.data[i].key) == entry.first) {
                known = true;
                maximumBytes = std::min(maximumBytes, targets.data[i].maxValueBytes);
                validFormat = validFormat && FormatSyntax(entry.second.value, targets.data[i].kind);
            }
        }
        if (!known || !entry.second.valid) {
            ++result.ignoredKeys;
            LogConfigKey(log, configName, known ? L" ignored empty/invalid key: " :
                               L" ignored unknown key: ", entry.first);
            continue;
        }
        if (entry.second.value.size() > maximumBytes) {
            ++result.ignoredKeys;
            ++result.invalidLines;
            LogConfigKey(log, configName, L" ignored key exceeding its native target byte limit: ", entry.first);
            continue;
        }
        if (!validFormat) {
            ++result.ignoredKeys;
            ++result.invalidLines;
            LogConfigKey(log, configName, L" ignored key with an invalid native format sequence: ", entry.first);
            continue;
        }
        auto allocation = std::make_unique<char[]>(entry.second.value.size() + 1);
        std::memcpy(allocation.get(), entry.second.value.c_str(), entry.second.value.size() + 1);
        replacements.emplace(entry.first, allocation.get());
        allocations.push_back(std::move(allocation));
    }
    if (replacements.empty()) { result.status = EnglishPlusStatus::NoOverrides; Summary(log, result, configName); return result; }
    for (std::size_t i = 0; i < targets.size; ++i) {
        const auto& target = targets.data[i];
        if (!target.key || !target.expected) return result;
        const auto found = replacements.find(Lower(target.key));
        if (found == replacements.end()) continue;
        const std::size_t expectedLength = strnlen_s(target.expected, 512);
        if (!expectedLength || expectedLength >= 512 || !FormatSyntax(target.expected, target.kind) ||
            target.pointerRva > imageSize || sizeof(void*) > imageSize - target.pointerRva) return result;
        const auto address = base + target.pointerRva;
        if (address % alignof(void*) || !usedCells.insert(address).second ||
            !Readable(reinterpret_cast<void*>(address), sizeof(void*))) return result;
        const char* original = nullptr;
        std::memcpy(&original, reinterpret_cast<void*>(address), sizeof(original));
        const auto originalAddress = reinterpret_cast<std::uintptr_t>(original);
        if (originalAddress < base || originalAddress >= imageEnd ||
            expectedLength + 1 > imageEnd - originalAddress ||
            !Readable(original, expectedLength + 1) ||
            std::memcmp(original, target.expected, expectedLength + 1)) {
            LogConfigKey(log, configName, L" original-pointer validation failed for key: ", Lower(target.key));
            return result;
        }
        patches.push_back({reinterpret_cast<void*>(address), original, found->second});
    }
    if (patches.empty()) { result.status = EnglishPlusStatus::NoOverrides; Summary(log, result, configName); return result; }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::size_t pageSize = system.dwPageSize;
    if (!pageSize || (pageSize & (pageSize - 1))) return result;
    std::vector<Page> pages;
    pages.reserve(patches.size());
    for (const auto& patch : patches) {
        const auto address = reinterpret_cast<std::uintptr_t>(patch.cell) & ~(pageSize - 1);
        if (std::any_of(pages.begin(), pages.end(), [address](const Page& p) {
            return reinterpret_cast<std::uintptr_t>(p.address) == address;
        })) continue;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return result;
        pages.push_back({reinterpret_cast<void*>(address), info.Protect});
    }
    for (auto& page : pages) if (!Writable(page, pageSize)) {
        RestorePages(pages, pageSize);
        LogConfig(log, configName, L" could not prepare writable pointer pages; no labels changed.");
        return result;
    }
    std::size_t changed = 0;
    for (const auto& patch : patches) {
        if (CompareExchange(patch, patch.original, patch.replacement) != patch.original) break;
        ++changed;
    }
    const bool complete = changed == patches.size();
    bool protectionRestored = false;
    if (complete) protectionRestored = RestorePages(pages, pageSize);
    if (!complete || !protectionRestored) {
        // Retry writable protection before rolling back any pages already
        // restored. A failed cleanup must never leave dangling text pointers.
        for (auto& page : pages) if (!page.writable) Writable(page, pageSize);
        for (std::size_t i = 0; i < changed; ++i) {
            const auto& patch = patches[i];
            const auto address = reinterpret_cast<std::uintptr_t>(patch.cell) & ~(pageSize - 1);
            const auto page = std::find_if(pages.begin(), pages.end(), [address](const Page& p) {
                return reinterpret_cast<std::uintptr_t>(p.address) == address;
            });
            if (page != pages.end() && page->writable)
                CompareExchange(patch, patch.replacement, patch.original);
        }
        RestorePages(pages, pageSize);
        // Readers may already have copied a replacement pointer even when all
        // table cells were restored. Retain allocations after any table write.
        if (changed) for (auto& allocation : allocations) allocation.release();
        LogConfig(log, configName, L" patch transaction failed; rollback attempted, replacement storage retained safely.");
        return result;
    }
    for (auto& allocation : allocations) allocation.release();
    result.appliedKeys = replacements.size();
    result.patchedPointers = patches.size();
    result.status = EnglishPlusStatus::Applied;
    Summary(log, result, configName);
    return result;
}

}  // namespace

EnglishPlusResult InitializeEnglishPlus(const std::filesystem::path& gameDirectory,
                                       const GameProfile& profile, LogFn log) {
    std::lock_guard<std::mutex> lock(g_initializationMutex);
    EnglishPlusResult result;
    const auto configName = GetEnglishTextConfig(profile.id);
    if (!configName || !profile.textConfigFile || std::wcscmp(configName, profile.textConfigFile) ||
        !GetEnglishTextTargets(profile.id).size) {
        result.status = EnglishPlusStatus::NoOverrides;
        return result;
    }
    if (g_initialized) { result.status = EnglishPlusStatus::AlreadyInitialized; return result; }
    g_initialized = true;
    try {
        const auto path = GetOverrideDirectory(gameDirectory, log) / profile.textConfigFile;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                result.status = EnglishPlusStatus::NoFile;
                LogConfig(log, configName, L" not present; original EXE labels retained.");
            } else LogConfig(log, configName, L" could not be opened; original EXE labels retained.");
            return result;
        }
        struct CloseFile { HANDLE file; ~CloseFile() { CloseHandle(file); } } close{file};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
            size.QuadPart > static_cast<LONGLONG>(kMaximumIniBytes)) {
            LogConfig(log, configName, L" rejected: invalid or excessive file size.");
            return result;
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!bytes.empty() && (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                              read != bytes.size())) {
            LogConfig(log, configName, L" read failed; original EXE labels retained.");
            return result;
        }
        const auto targets = GetEnglishTextTargets(profile.id);
        result = Apply(bytes.data(), bytes.size(), GetModuleHandleW(nullptr), profile.imageSize, targets, log, configName);
        return result;
    } catch (...) {
        LogConfig(log, configName, L" initialization exception; overrides were not completed.");
        return result;
    }
}

#ifdef BATMAN_MODS_TESTING
namespace english_plus_testing {

EnglishPlusResult Apply(const char* bytes, std::size_t count, void* base,
                        std::size_t imageSize, EnglishTextTargetView targets, LogFn log) {
    try { return batman_mods::Apply(bytes, count, base, imageSize, targets, log); }
    catch (...) { return {}; }
}

}  // namespace english_plus_testing
#endif

}  // namespace batman_mods
