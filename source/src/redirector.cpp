#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_M_IX86)
#include <MinHook.h>
#endif

#include "redirector.h"
#include "runtime_settings.h"
#include "win_handle.h"
#include "image_headers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace batman_mods {
namespace detail {
struct AssetReadLease {
    std::vector<UniqueHandle> directories;
    UniqueHandle file;
};
}
namespace {

#if defined(_M_IX86)
// These ABIs were checked in the installed EXE, including both WAVE callers.
using OpenFn = int(__cdecl*)(const char*, int, void*, int);
using ExistsFn = int(__cdecl*)(const char*);
using SizeFn = std::int64_t(__cdecl*)(const char*);
using WholeFileFn = int(__cdecl*)(const char*, void*, int);
using WaveFn = HRESULT(__thiscall*)(void*, void*, const char*, void*);

OpenFn g_open = nullptr;
ExistsFn g_exists = nullptr;
SizeFn g_size = nullptr;
WholeFileFn g_wholeFile = nullptr;
WaveFn g_wave = nullptr;
#endif

struct Configuration {
    std::wstring gameDirectory;
    std::wstring modsDirectory;
    LogFn log = nullptr;
};

using Override = ExternalAsset;

std::shared_ptr<const Configuration> g_configuration;
std::mutex g_lifecycleMutex;
#if defined(_M_IX86)
struct CreatedHook {
    void* target;
    void** original;
    bool disabled = false;
};
std::vector<CreatedHook> g_createdHooks;
bool g_ownsMinHook = false;
bool g_cleanupFailed = false;
bool g_modulePinned = false;
// After ApplyQueued starts, even a failed call may have exposed a trampoline
// to a game thread. Such code and original pointers must live until exit.
bool g_activationAttempted = false;
#endif

struct PathView {
    const char* data = nullptr;
    std::size_t length = 0;

    explicit PathView(const char* path = nullptr) noexcept : data(path) {
        if (path) {
            length = strnlen_s(path, 256);
            if (!length || length >= 256) data = nullptr;
        }
    }
};

bool IsSeparator(char character) noexcept { return character == '\\' || character == '/'; }

// Component iteration borrows the existing bounded strings. Neither the linked
// selection stack nor path comparison allocates, including during native pumps.
bool NextComponent(const PathView& path, std::size_t& cursor,
                   const char*& component, std::size_t& length) noexcept {
    while (cursor < path.length) {
        while (cursor < path.length && IsSeparator(path.data[cursor])) ++cursor;
        const std::size_t begin = cursor;
        while (cursor < path.length && !IsSeparator(path.data[cursor])) ++cursor;
        length = cursor - begin;
        if (length && !(length == 1 && path.data[begin] == '.')) {
            component = path.data + begin;
            return true;
        }
    }
    return false;
}

unsigned char FoldPathByte(unsigned char character) noexcept {
    return character >= 'A' && character <= 'Z'
        ? static_cast<unsigned char>(character + ('a' - 'A')) : character;
}

bool SameSelectedPath(const PathView& a, const PathView& b) noexcept {
    if (!a.data || !b.data) return false;
    // Do not make a root-relative request equivalent to a relative asset name.
    if (IsSeparator(a.data[0]) != IsSeparator(b.data[0])) return false;
    std::size_t aCursor = 0, bCursor = 0;
    for (;;) {
        const char* aComponent = nullptr;
        const char* bComponent = nullptr;
        std::size_t aLength = 0, bLength = 0;
        const bool haveA = NextComponent(a, aCursor, aComponent, aLength);
        const bool haveB = NextComponent(b, bCursor, bComponent, bLength);
        if (haveA != haveB) return false;
        if (!haveA) return true;
        if (aLength != bLength) return false;
        // Packed asset names use ASCII. Preserve non-ASCII bytes exactly rather
        // than applying a different code page to UTF-8 versus ACP request views.
        for (std::size_t index = 0; index < aLength; ++index)
            if (FoldPathByte(static_cast<unsigned char>(aComponent[index])) !=
                FoldPathByte(static_cast<unsigned char>(bComponent[index]))) return false;
    }
}

#if defined(_M_IX86)
struct SelectionScope;
thread_local const SelectionScope* g_selectedPaths = nullptr;

// A native wait may pump an unrelated asset request. Suppress only the original
// and resolved names of active selections, not every request on this thread.
struct SelectionScope {
    PathView original;
    PathView resolved;
    const SelectionScope* previous;

    SelectionScope(const char* requested, const char* selected) noexcept
        : original(requested), resolved(selected), previous(g_selectedPaths) {
        g_selectedPaths = this;
    }
    ~SelectionScope() { g_selectedPaths = previous; }
    SelectionScope(const SelectionScope&) = delete;
    SelectionScope& operator=(const SelectionScope&) = delete;
};

bool IsSelectedPath(const char* path) noexcept {
    const PathView requested(path);
    for (const SelectionScope* scope = g_selectedPaths; scope; scope = scope->previous)
        if (SameSelectedPath(requested, scope->original) ||
            SameSelectedPath(requested, scope->resolved)) return true;
    return false;
}
#endif

#if defined(_M_IX86)
void Log(const Configuration& configuration, const wchar_t* message) noexcept {
    if (configuration.log) {
        try { configuration.log(message); } catch (...) {}
    }
}

void LogRedirect(const Configuration& configuration, const wchar_t* operation,
                 const Override& redirected) noexcept {
    if (!configuration.log) return;
    try {
        const std::wstring message = std::wstring(L"mods ") + operation + L": " +
                                     redirected.absolutePath;
        Log(configuration, message.c_str());
    } catch (...) {}
}
#endif

bool EqualInsensitive(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
        CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                             static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool IsBelow(const std::wstring& path, const std::wstring& directory) {
    return path.size() > directory.size() && path[directory.size()] == L'\\' &&
        CompareStringOrdinal(path.c_str(), static_cast<int>(directory.size()),
                             directory.c_str(), static_cast<int>(directory.size()),
                             TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> FullPath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!required || required > 32768) return std::nullopt;
    std::wstring result(required, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), required, result.data(), nullptr);
    if (!length || length >= required) return std::nullopt;
    result.resize(length);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

std::optional<std::wstring> FinalPath(HANDLE file) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(file, nullptr, 0, flags);
    if (!required || required > 32768) return std::nullopt;
    std::wstring result(required + 1, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, result.data(),
                                                  static_cast<DWORD>(result.size()), flags);
    if (!length || length >= result.size()) return std::nullopt;
    result.resize(length);
    while (result.size() > 7 && result.back() == L'\\') result.pop_back();
    return result;
}

std::optional<std::wstring> Decode(const char* text, int length, UINT codePage) {
    const int required = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
                                             text, length, nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring result(required, L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text, length,
                            result.data(), required) != required) return std::nullopt;
    return result;
}

// The native engine ends in CreateFileA and copies paths into 0x100-byte arrays.
// An ACP round trip is mandatory; UTF-8 bytes cannot be passed to CreateFileA on
// a machine whose active code page is something else.
std::optional<std::string> NativePath(const std::wstring& path) {
    const UINT codePage = GetACP();
    const bool utf8Acp = codePage == CP_UTF8;
    BOOL substituted = FALSE;
    const DWORD flags = utf8Acp ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL* usedDefault = utf8Acp ? nullptr : &substituted;
    const int required = WideCharToMultiByte(codePage, flags, path.data(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, usedDefault);
    if (required <= 0 || required >= 256 || substituted) return std::nullopt;
    std::string result(required, '\0');
    substituted = FALSE;
    if (WideCharToMultiByte(codePage, flags, path.data(), static_cast<int>(path.size()),
        result.data(), required, nullptr, usedDefault) != required || substituted)
        return std::nullopt;
    const auto roundTrip = Decode(result.data(), required, codePage);
    if (!roundTrip || *roundTrip != path) return std::nullopt;
    return result;
}

bool IsDriveAbsolute(const std::wstring& path) {
    return path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
}

bool ReservedComponent(const std::wstring& component) {
    const auto dot = component.find(L'.');
    const std::wstring base = component.substr(0, dot);
    for (const wchar_t* reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (EqualInsensitive(base, reserved)) return true;
    if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9')
        if (EqualInsensitive(base.substr(0, 3), L"COM") ||
            EqualInsensitive(base.substr(0, 3), L"LPT")) return true;
    return false;
}

std::optional<std::wstring> RelativeRequest(const Configuration& configuration,
                                           std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (IsDriveAbsolute(path)) {
        // An absolute request may name an asset under this game, never a path
        // outside it. UNC, device, root-relative and drive-relative forms fail.
        if (!IsBelow(path, configuration.gameDirectory)) return std::nullopt;
        path.erase(0, configuration.gameDirectory.size() + 1);
    } else if (path.empty() || path.front() == L'\\' || path.find(L':') != std::wstring::npos) {
        return std::nullopt;
    }
    std::wstring relative;
    for (std::size_t begin = 0; begin <= path.size();) {
        const auto end = path.find(L'\\', begin);
        const auto count = end == std::wstring::npos ? path.size() - begin : end - begin;
        const std::wstring component = path.substr(begin, count);
        if (component == L"..") return std::nullopt;
        if (!component.empty() && component != L".") {
            if (component.back() == L'.' || component.back() == L' ' ||
                ReservedComponent(component)) return std::nullopt;
            for (const wchar_t character : component)
                if (character < 32 || std::wstring_view(L"<>:\"|?*").find(character) != std::wstring_view::npos)
                    return std::nullopt;
            if (!relative.empty()) relative += L'\\';
            relative += component;
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return relative.empty() ? std::nullopt : std::optional<std::wstring>(std::move(relative));
}

std::optional<Override> ResolveDecoded(const Configuration& configuration,
                                      const std::wstring& decoded) {
    const auto relative = RelativeRequest(configuration, decoded);
    if (!relative) return std::nullopt;
    const std::wstring candidate = configuration.modsDirectory + L'\\' + *relative;
    // A negative hint only: hits still undergo every handle/containment check.
    // Do not cache absence; a newly added resource must be visible next time.
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return std::nullopt;
    const auto native = NativePath(candidate);
    if (!native) return std::nullopt;

    // Retain handles, denying write/delete sharing, from the mods root through
    // every child directory and the file. An attribute-only path check followed
    // by closing the file left a replacement window before the native reopen.
    const auto lease = std::make_shared<detail::AssetReadLease>();
    lease->directories.reserve(1 + std::count(relative->begin(), relative->end(), L'\\'));
    const auto lockDirectory = [&lease](const std::wstring& directory) {
        // Attribute-only opens do not participate in Windows share locking.
        UniqueHandle handle(CreateFileW(directory.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        lease->directories.push_back(std::move(handle));
        return true;
    };
    std::wstring checked = configuration.modsDirectory;
    if (!lockDirectory(checked)) return std::nullopt;
    for (std::size_t begin = 0; begin < relative->size();) {
        const auto end = relative->find(L'\\', begin);
        if (end == std::wstring::npos) break;
        checked += L'\\';
        checked.append(*relative, begin, end - begin);
        if (!lockDirectory(checked)) return std::nullopt;
        begin = end + 1;
    }

    lease->file = UniqueHandle(CreateFileW(candidate.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!lease->file || GetFileType(lease->file.get()) != FILE_TYPE_DISK ||
        !GetFileInformationByHandle(lease->file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return std::nullopt;
    const auto rootPath = FinalPath(lease->directories.front().get());
    const auto filePath = FinalPath(lease->file.get());
    if (!rootPath || !filePath || !IsBelow(*filePath, *rootPath)) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(lease->file.get(), &size) || size.QuadPart < 0 ||
        size.QuadPart > std::numeric_limits<int>::max()) return std::nullopt;
    return Override{candidate, *native, size.QuadPart, lease};
}

std::optional<Override> ResolveOverride(const Configuration& configuration, const char* request) {
    if (!request) return std::nullopt;
    const std::size_t length = strnlen_s(request, 256);
    if (!length || length >= 256) return std::nullopt;
    // Native requests end in CreateFileA: preserve their ACP interpretation
    // first. Strict UTF-8 is an additional fallback only when that file is absent.
    const auto ansi = Decode(request, static_cast<int>(length), GetACP());
    if (ansi) {
        auto resolved = ResolveDecoded(configuration, *ansi);
        if (resolved) return resolved;
    }
    if (GetACP() != CP_UTF8) {
        const auto utf8 = Decode(request, static_cast<int>(length), CP_UTF8);
        if (utf8 && (!ansi || *ansi != *utf8)) return ResolveDecoded(configuration, *utf8);
    }
    return std::nullopt;
}

std::shared_ptr<const Configuration> MakeConfiguration(const std::filesystem::path& gameDirectory,
                                                       LogFn log) {
    const auto absolute = FullPath(gameDirectory.wstring());
    if (!absolute || !IsDriveAbsolute(*absolute)) return {};
    auto configuration = std::make_shared<Configuration>();
    configuration->gameDirectory = *absolute;
    configuration->modsDirectory = GetOverrideDirectory(std::filesystem::path(*absolute), log).wstring();
    configuration->log = log;
    return configuration;
}

#if defined(_M_IX86)
std::optional<Override> TryResolveOverride(const Configuration& configuration, const char* path) noexcept {
    try { return ResolveOverride(configuration, path); }
    catch (...) { return std::nullopt; }
}

int __cdecl HookOpen(const char* path, int mode, void* archiveContext, int flag) {
    if (!g_open) return 0;
    if (mode == 0 && !IsSelectedPath(path)) {
        const auto configuration = std::atomic_load(&g_configuration);
        if (configuration) {
            if (const auto redirected = TryResolveOverride(*configuration, path)) {
                SelectionScope scope(path, redirected->nativePath.c_str());
                try {
                    const int result = g_open(redirected->nativePath.c_str(), mode, nullptr, flag);
                    if (result) { LogRedirect(*configuration, L"open", *redirected); return result; }
                } catch (...) {}
                Log(*configuration, L"mods open failed; using the original asset");
                // Outside the override handler: never catch/retry a native
                // fallback exception. Retain selection suppression throughout.
                return g_open(path, mode, archiveContext, flag);
            }
        }
    }
    return g_open(path, mode, archiveContext, flag);
}

int __cdecl HookExists(const char* path) {
    if (!g_exists) return 0;
    if (!IsSelectedPath(path)) {
        try {
            const auto configuration = std::atomic_load(&g_configuration);
            if (configuration) {
                const auto redirected = ResolveOverride(*configuration, path);
                // Archive hits return the entry's size; native disk hits return
                // true. Preserve useful size values and keep empty disk files true.
                if (redirected) return redirected->size ? static_cast<int>(redirected->size) : 1;
            }
        } catch (...) {}
    }
    return g_exists(path);
}

std::int64_t __cdecl HookSize(const char* path) {
    if (!g_size) return -1;
    if (!IsSelectedPath(path)) {
        try {
            const auto configuration = std::atomic_load(&g_configuration);
            if (configuration) {
                const auto redirected = ResolveOverride(*configuration, path);
                if (redirected) return redirected->size;
            }
        } catch (...) {}
    }
    return g_size(path);
}

int __cdecl HookWholeFile(const char* path, void* destination, int capacity) {
    if (!g_wholeFile) return 0;
    if (!IsSelectedPath(path)) {
        const auto configuration = std::atomic_load(&g_configuration);
        if (configuration) {
            if (const auto redirected = TryResolveOverride(*configuration, path)) {
                SelectionScope scope(path, redirected->nativePath.c_str());
                try {
                    const int result = g_wholeFile(redirected->nativePath.c_str(), destination, capacity);
                    if (result) { LogRedirect(*configuration, L"whole-file", *redirected); return result; }
                } catch (...) {}
                Log(*configuration, L"mods whole-file load failed; using the original asset");
                return g_wholeFile(path, destination, capacity);
            }
        }
    }
    return g_wholeFile(path, destination, capacity);
}

HRESULT __fastcall HookWave(void* self, void*, void* archiveContext,
                            const char* path, void* formatOut) {
    if (!g_wave) return E_FAIL;
    if (!IsSelectedPath(path)) {
        const auto configuration = std::atomic_load(&g_configuration);
        if (configuration) {
            if (const auto redirected = TryResolveOverride(*configuration, path)) {
                SelectionScope scope(path, redirected->nativePath.c_str());
                try {
                    const HRESULT result = g_wave(self, nullptr, redirected->nativePath.c_str(), formatOut);
                    if (SUCCEEDED(result)) { LogRedirect(*configuration, L"WAVE stream", *redirected); return result; }
                } catch (...) {}
                Log(*configuration, L"mods WAVE load failed; using the original asset");
                return g_wave(self, archiveContext, path, formatOut);
            }
        }
    }
    return g_wave(self, archiveContext, path, formatOut);
}

struct HookSpecification {
    std::uintptr_t rva;
    void* detour;
    void** original;
};

std::array<HookSpecification, 5> Specifications(const GameProfile& profile) {
    return {{
        {profile.hooks[0].rva,
         reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void**>(&g_open)},
        {profile.hooks[1].rva,
         reinterpret_cast<void*>(&HookExists), reinterpret_cast<void**>(&g_exists)},
        {profile.hooks[2].rva,
         reinterpret_cast<void*>(&HookSize), reinterpret_cast<void**>(&g_size)},
        {profile.hooks[3].rva,
         reinterpret_cast<void*>(&HookWholeFile), reinterpret_cast<void**>(&g_wholeFile)},
        {profile.hooks[4].rva,
         reinterpret_cast<void*>(&HookWave), reinterpret_cast<void**>(&g_wave)}
    }};
}

void ClearTargets() noexcept {
    g_open = nullptr;
    g_exists = nullptr;
    g_size = nullptr;
    g_wholeFile = nullptr;
    g_wave = nullptr;
}

void PinModule() noexcept {
    if (g_modulePinned) return;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&HookOpen), &module)) g_modulePinned = true;
}

bool RemoveCreatedHooks(const Configuration* fallbackLog = nullptr) noexcept {
    // Publish pass-through before touching hooks. Once activation has started,
    // retain all trampolines. Only never-exposed staging memory may be freed.
    const auto configuration = std::atomic_exchange(&g_configuration,
        std::shared_ptr<const Configuration>{});
    const Configuration* logger = configuration ? configuration.get() : fallbackLog;
    for (auto& hook : g_createdHooks) {
        MH_QueueDisableHook(hook.target);
        const MH_STATUS status = MH_DisableHook(hook.target);
        hook.disabled = status == MH_OK || status == MH_ERROR_DISABLED;
    }
    if (g_activationAttempted) {
        // Disabling patches cannot prove that previously entered detours have
        // returned. Keep ALL original pointers/trampolines, including when
        // disable failed; the cleared resolver makes reachable hooks pass through.
        g_cleanupFailed = true;
        PinModule();
        if (logger) Log(*logger, L"VFS activation aborted; pass-through enabled, all live trampolines retained until exit");
        return false;
    }
    for (std::size_t index = 0; index < g_createdHooks.size();) {
        auto& hook = g_createdHooks[index];
        if (hook.disabled && MH_RemoveHook(hook.target) == MH_OK) {
            *hook.original = nullptr;
            g_createdHooks.erase(g_createdHooks.begin() + index);
        } else {
            ++index;
        }
    }
    bool complete = g_createdHooks.empty();
    if (complete && g_ownsMinHook) {
        const MH_STATUS status = MH_Uninitialize();
        if (status == MH_OK || status == MH_ERROR_NOT_INITIALIZED) g_ownsMinHook = false;
        else complete = false;
    }
    g_cleanupFailed = !complete;
    if (!complete) {
        PinModule();
        if (logger) Log(*logger, g_modulePinned
            ? L"VFS rollback incomplete; pass-through enabled, live trampolines retained, module pinned"
            : L"VFS rollback incomplete; pass-through enabled, live trampolines retained; module must remain loaded");
        return false;
    }
    ClearTargets();
    return true;
}
#endif

}  // namespace

bool InitializeExternalAssetResolver(const std::filesystem::path& gameDirectory, LogFn log) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (std::atomic_load(&g_configuration)) return false;
#if defined(_M_IX86)
    if (!g_createdHooks.empty() || g_cleanupFailed || g_ownsMinHook) return false;
#endif
    const auto configuration = MakeConfiguration(gameDirectory, log);
    if (!configuration) return false;
    std::atomic_store(&g_configuration, configuration);
    return true;
}

std::optional<ExternalAsset> FindExternalAsset(const char* requestedPath) {
    const auto configuration = std::atomic_load(&g_configuration);
    if (!configuration) return std::nullopt;
    return ResolveOverride(*configuration, requestedPath);
}

bool SameExternalAssetPath(const char* a, const char* b) noexcept {
    return SameSelectedPath(PathView(a), PathView(b));
}

#if defined(_M_IX86)
bool InitializeRedirector(const std::filesystem::path& gameDirectory,
                          const GameProfile& profile, LogFn log) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    const bool flat = profile.backend == RedirectorBackend::FlatVfs;
    if (!flat && profile.backend != RedirectorBackend::BatmanVfs) return false;
    if (flat && profile.hooks[4].rva) return false;
    const std::size_t hookCount = flat ? 4 : 5;
    if (std::atomic_load(&g_configuration) || !g_createdHooks.empty() ||
        g_cleanupFailed || g_ownsMinHook) return false;
    try {
        const auto configuration = MakeConfiguration(gameDirectory, log);
        if (!configuration) return false;
        const auto module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!NativeImageHeadersMatch(module, profile.imageSize)) return false;
        const auto specifications = Specifications(profile);
        for (std::size_t index = 0; index < hookCount; ++index) {
            const auto& specification = specifications[index];
            if (!specification.rva) return false;
            if (!NativeHookMatches(module, profile.imageSize, profile.hooks[index], profile.preferredImageBase, log)) {
                Log(*configuration, L"VFS signature mismatch; no hooks installed");
                return false;
            }
        }
        // Reserve before taking ownership of any MinHook state: allocation
        // failure must not strand an initialized library without hook records.
        g_createdHooks.reserve(hookCount);
        const MH_STATUS initialized = MH_Initialize();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
            Log(*configuration, L"MinHook initialization failed");
            return false;
        }
        g_ownsMinHook = initialized == MH_OK;
        for (std::size_t index = 0; index < hookCount; ++index) {
            const auto& specification = specifications[index];
            void* target = module + specification.rva;
            if (MH_CreateHook(target, specification.detour, specification.original) != MH_OK) {
                Log(*configuration, L"VFS hook staging failed; rolling back");
                RemoveCreatedHooks(configuration.get());
                return false;
            }
            g_createdHooks.push_back({target, specification.original, false});
        }
        // Publish immutable resolver state before any detour can execute.
        std::atomic_store(&g_configuration, configuration);
        for (const auto& hook : g_createdHooks) {
            if (MH_QueueEnableHook(hook.target) != MH_OK) {
                Log(*configuration, L"VFS hook activation staging failed; rolling back");
                RemoveCreatedHooks(configuration.get());
                return false;
            }
        }
        g_activationAttempted = true;
        if (MH_ApplyQueued() != MH_OK) {
            Log(*configuration, L"VFS hook activation failed; rolling back");
            RemoveCreatedHooks(configuration.get());
            return false;
        }
        Log(*configuration, flat
            ? L"mods redirector active: open, existence, size and whole-file"
            : L"mods redirector active: open, existence, size, whole-file and WAVE");
        return true;
    } catch (...) {
        RemoveCreatedHooks();
        return false;
    }
}

#ifdef BATMAN_MODS_TESTING
namespace testing {

bool CheckRollbackPolicy(OpenFn target, bool activationStarted) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!target || std::atomic_load(&g_configuration) || !g_createdHooks.empty() ||
        g_ownsMinHook || g_cleanupFailed || g_activationAttempted) return false;
    g_createdHooks.reserve(1);
    if (MH_Initialize() != MH_OK) return false;
    g_ownsMinHook = true;
    void* address = reinterpret_cast<void*>(target);
    if (MH_CreateHook(address, reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void**>(&g_open)) != MH_OK) {
        MH_Uninitialize(); g_ownsMinHook = false; return false;
    }
    const auto original = g_open;
    g_createdHooks.push_back({address, reinterpret_cast<void**>(&g_open), false});
    // The fixture never enables this hook, so its final explicit teardown is
    // quiescent. Marking it exposed exercises the real conservative rollback.
    g_activationAttempted = activationStarted;
    const bool removed = RemoveCreatedHooks();
    const bool okay = activationStarted
        ? !removed && g_createdHooks.size() == 1 && g_open == original
        : removed && g_createdHooks.empty() && !g_open;
    if (activationStarted) {
        if (MH_RemoveHook(address) != MH_OK || MH_Uninitialize() != MH_OK) return false;
        g_createdHooks.clear(); ClearTargets();
    }
    g_activationAttempted = false; g_cleanupFailed = false; g_ownsMinHook = false;
    return okay;
}

bool Configure(const std::filesystem::path& gameDirectory, const NativeTargets& targets, LogFn log) {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!g_createdHooks.empty() || g_cleanupFailed || g_ownsMinHook ||
        !targets.open || !targets.exists || !targets.size ||
        !targets.wholeFile || !targets.wave) return false;
    try {
        const auto configuration = MakeConfiguration(gameDirectory, log);
        if (!configuration) return false;
        g_open = targets.open;
        g_exists = targets.exists;
        g_size = targets.size;
        g_wholeFile = targets.wholeFile;
        g_wave = targets.wave;
        std::atomic_store(&g_configuration, configuration);
        return true;
    } catch (...) { return false; }
}

void Reset() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!g_createdHooks.empty() || g_cleanupFailed || g_ownsMinHook) return;
    std::atomic_store(&g_configuration, std::shared_ptr<const Configuration>{});
    ClearTargets();
}

bool Resolve(const char* requestedPath, ResolvedPath& result) {
    result = {};
    try {
        const auto configuration = std::atomic_load(&g_configuration);
        if (!configuration) return false;
        const auto resolved = ResolveOverride(*configuration, requestedPath);
        if (!resolved) return false;
        result = {resolved->absolutePath, resolved->nativePath, resolved->size};
        return true;
    } catch (...) { return false; }
}

int Open(const char* path, int mode, void* context, int flag) { return HookOpen(path, mode, context, flag); }
int Exists(const char* path) { return HookExists(path); }
std::int64_t Size(const char* path) { return HookSize(path); }
int WholeFile(const char* path, void* destination, int capacity) { return HookWholeFile(path, destination, capacity); }
long Wave(void* self, void* context, const char* path, void* formatOut) {
    return HookWave(self, nullptr, context, path, formatOut);
}

}  // namespace testing
#endif
#else
// The original Batman VFS and its integer handles are verified only for x86.
// x64 backends share the resolver above and install their own native hooks.
bool InitializeRedirector(const std::filesystem::path&, const GameProfile&, LogFn) {
    return false;
}

#endif

}  // namespace batman_mods
