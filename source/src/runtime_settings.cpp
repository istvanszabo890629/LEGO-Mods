#include <windows.h>
#include "runtime_settings.h"
#include <mutex>
#include <optional>
#include <string>

namespace batman_mods {
namespace {
std::mutex g_mutex;
std::filesystem::path g_gameDirectory, g_overrideDirectory;
bool g_loaded = false, g_loggingEnabled = false, g_folderReported = false;
struct Settings { std::wstring folder = L"mods"; bool loggingEnabled = false; };
constexpr std::size_t kMaximumSettingsBytes = 16384;

static_assert(ValidModsFolderName(L"mods") && ValidModsFolderName(L"magyaritas") &&
              ValidModsFolderName(L"Sajat forditas") && ValidModsFolderName(L"módosítás"));
static_assert(!ValidModsFolderName(L"") && !ValidModsFolderName(L".") && !ValidModsFolderName(L"..") &&
              !ValidModsFolderName(L"..\\outside") && !ValidModsFolderName(L"C:\\mods") &&
              !ValidModsFolderName(L"\\server") && !ValidModsFolderName(L"mods/hu") &&
              !ValidModsFolderName(L"mods:stream") && !ValidModsFolderName(L"CON.txt") &&
              !ValidModsFolderName(L"com1") && !ValidModsFolderName(L"mods."));
static_assert(settings_detail::Parse(L"").valid && settings_detail::Parse(L"").name == L"mods");
static_assert(settings_detail::Parse(L"[Files]\nModsFolder=magyaritas\n").name == L"magyaritas");
static_assert(settings_detail::Parse(L"; comment\r\n[files]\r\nmodsfolder = \"Sajat forditas\"\r\n").name == L"Sajat forditas");
static_assert(settings_detail::Parse(L"[Files]\nModsFolder=módosítás\n").name == L"módosítás");
static_assert(settings_detail::Parse(L"[Other]\nModsFolder=other\n").name == L"mods");
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=..\\outside\n").valid);
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=C:\\outside\n").valid);
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=CON.txt\n").valid);
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=\n").valid);
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=mods\nModsFolder=other\n").valid);
static_assert(!settings_detail::Parse(L"[Files\nModsFolder=mods\n").valid);
static_assert(!settings_detail::Parse(L"").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Files]\nModsFolder=magyaritas\n").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Logging]\nEnabled=0\n").loggingEnabled);
static_assert(settings_detail::Parse(L"[Logging]\nEnabled=1\n").valid &&
              settings_detail::Parse(L"[Logging]\nEnabled=1\n").loggingEnabled);
static_assert(settings_detail::Parse(L"[logging]\r\nenabled = \"1\"\r\n[Files]\r\nModsFolder=magyaritas\r\n").loggingEnabled &&
              settings_detail::Parse(L"[logging]\r\nenabled = \"1\"\r\n[Files]\r\nModsFolder=magyaritas\r\n").name == L"magyaritas");
static_assert(settings_detail::Parse(L"[Files]\nModsFolder=magyaritas\n[Logging]\nEnabled=1\n").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Other]\nEnabled=1\n").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Logging]\nEnabled=2\n").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Logging]\nEnabled=\n").loggingEnabled);
static_assert(!settings_detail::Parse(L"[Logging]\nEnabled=1\nEnabled=0\n").loggingEnabled);
static_assert(settings_detail::Parse(L"[Files]\nModsFolder=magyaritas\n[Logging]\nEnabled=2\n").valid &&
              settings_detail::Parse(L"[Files]\nModsFolder=magyaritas\n[Logging]\nEnabled=2\n").name == L"magyaritas");

void Log(void (*log)(const wchar_t*), const wchar_t* message) noexcept {
    if (log) { try { log(message); } catch (...) {} }
}
std::optional<Settings> Parse(const std::string& bytes) {
    std::string_view utf8(bytes);
    if (utf8.substr(0, 3) == "\xEF\xBB\xBF") utf8.remove_prefix(3);
    if (utf8.empty()) return Settings{};
    if (utf8.find('\0') != std::string_view::npos) return std::nullopt;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring text(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), text.data(), count) != count) return std::nullopt;
    const auto setting = settings_detail::Parse(text);
    if (!setting.valid) return std::nullopt;
    return Settings{std::wstring(setting.name), setting.loggingEnabled};
}
Settings Read(const std::filesystem::path& path, void (*log)(const wchar_t*)) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            Log(log, L"LegoMods.ini could not be opened; using mods.");
        return {};
    }
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(kMaximumSettingsBytes)) {
        Log(log, L"LegoMods.ini has an invalid size; using defaults."); return {};
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!bytes.empty() && (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())) {
        Log(log, L"LegoMods.ini read failed; using defaults."); return {};
    }
    const auto parsed = Parse(bytes);
    if (!parsed) { Log(log, L"LegoMods.ini is invalid; using mods with logging disabled."); return {}; }
    return *parsed;
}
void Load(const std::filesystem::path& gameDirectory, void (*log)(const wchar_t*)) {
    if (g_loaded && g_gameDirectory == gameDirectory) return;
    const auto settings = Read(gameDirectory / L"LegoMods.ini", log);
    g_gameDirectory = gameDirectory;
    g_overrideDirectory = gameDirectory / settings.folder;
    g_loggingEnabled = settings.loggingEnabled;
    g_folderReported = false;
    g_loaded = true;
}
}

std::filesystem::path GetOverrideDirectory(const std::filesystem::path& gameDirectory,
                                         void (*log)(const wchar_t*)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Load(gameDirectory, log);
    if (!g_folderReported && log) {
        const auto message = L"Override folder: " + g_overrideDirectory.wstring();
        Log(log, message.c_str());
        g_folderReported = true;
    }
    return g_overrideDirectory;
}
bool ModuleLoggingEnabled(const std::filesystem::path& gameDirectory) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Load(gameDirectory, nullptr);
    return g_loggingEnabled;
}
} // namespace batman_mods
