#include "asset_selection.h"
#include "memory_guard.h"
#include <windows.h>
#include <MinHook.h>
#include "nufile_redirector.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

static_assert(sizeof(void*) == 8, "The reviewed nttFile backend is x64-only.");

namespace batman_mods {
namespace {
struct FrontString {
    const char* text;
    std::uint16_t capacity;
    std::uint16_t lengthIncludingNul;
};
static_assert(sizeof(FrontString) == 16 && offsetof(FrontString, capacity) == 8 &&
              offsetof(FrontString, lengthIncludingNul) == 10);
constexpr bool ReadMode(int mode) noexcept { return mode == 0 || mode == 3 || mode == 4; }
static_assert(ReadMode(0) && ReadMode(3) && ReadMode(4));
static_assert(!ReadMode(-1) && !ReadMode(1) && !ReadMode(2) && !ReadMode(5));

using Open = void** (*)(void**, const FrontString*, int);
using Size = std::int64_t (*)(const FrontString*);
using FileInfo = bool (*)(const FrontString*, void*);
using Exists = bool (*)(const FrontString*);
using DatOpen = void** (*)(void*, void**, const FrontString*, int);
using DatSize = std::int64_t (*)(void*, const FrontString*);
using FindDevice = void** (*)(void**, const FrontString*);
using ResolvePath = bool (*)(void*, char*, const FrontString*, int, bool, bool);
using Cleanup = void (*)(void*);
using FileSize = std::int64_t (*)(void*);

Open g_open = nullptr;
Size g_size = nullptr;
FileInfo g_info = nullptr;
Exists g_exists = nullptr;
DatOpen g_datOpen = nullptr;
DatSize g_datSize = nullptr;
FindDevice g_findDevice = nullptr;
const void* g_pcDeviceVtable = nullptr;
const void* g_datDeviceVtable = nullptr;
const void* g_pcFileVtable = nullptr;
std::atomic<bool> g_ready{false};
bool g_attempted = false;
LogFn g_log = nullptr;

void Log(const wchar_t* text) noexcept {
    if (g_log) { try { g_log(text); } catch (...) {} }
}
using memory::Readable;
struct NativeOwner {
    void* object = nullptr;
    NativeOwner() = default;
    NativeOwner(const NativeOwner&) = delete;
    NativeOwner& operator=(const NativeOwner&) = delete;
    ~NativeOwner() noexcept {
        if (object) {
            void* method = memory::VirtualMethod(object, 5);
            if (!method) { Log(L"nttFile invalid native release target; cleanup skipped"); return; }
            try {
                reinterpret_cast<Cleanup>(method)(object);
            } catch (...) { Log(L"nttFile native release raised an exception"); }
        }
    }
    void* Take() noexcept { auto result = object; object = nullptr; return result; }
};
thread_local bool g_normalizing = false;
struct NormalizeScope {
    NormalizeScope() noexcept { g_normalizing = true; }
    ~NormalizeScope() { g_normalizing = false; }
};
using Selection = AssetSelection<struct NttSelection>;
std::optional<ExternalAsset> Candidate(const FrontString* path) noexcept {
    try {
        if (!g_ready.load(std::memory_order_acquire) || g_normalizing || !path || !path->text) return std::nullopt;
        if (Selection::Contains(path->text)) return std::nullopt;
        if (auto result = FindExternalAsset(path->text)) return result;
        // A device-qualified path is normalized by the exact native device,
        // including its mount/root. Unknown device families retain their path.
        const auto length = strnlen_s(path->text, 256);
        if (!length || length >= 256) return std::nullopt;
        bool qualified = false;
        for (std::size_t i = 0; i < length && i < 8; ++i)
            if (path->text[i] == ':') { qualified = true; break; }
        if (!qualified) return std::nullopt;
        NormalizeScope normalizing;
        NativeOwner device;
        g_findDevice(&device.object, path);
        if (!Readable(device.object, sizeof(void*))) return std::nullopt;
        auto table = *static_cast<void***>(device.object);
        if (table != g_pcDeviceVtable && table != g_datDeviceVtable) return std::nullopt;
        void* normalize = memory::VirtualMethod(device.object, 22);
        if (!normalize) return std::nullopt;
        std::array<char, 1024> normalized{};
        if (!reinterpret_cast<ResolvePath>(normalize)(device.object, normalized.data(), path,
                static_cast<int>(normalized.size()), true, false)) return std::nullopt;
        return FindExternalAsset(normalized.data());
    } catch (...) { return std::nullopt; }
}
FrontString NativeName(const ExternalAsset& asset) noexcept {
    const auto length = static_cast<std::uint16_t>(asset.nativePath.size()+1);
    return {asset.nativePath.c_str(), length, length};
}
bool TryOpen(void** output, const FrontString* path, int mode) noexcept {
    try {
        if (!output || !ReadMode(mode)) return false;
        const auto asset = Candidate(path);
        if (!asset) return false;
        Selection selected(path->text, asset->nativePath.c_str());
        const auto native = NativeName(*asset);
        NativeOwner file;
        // Re-enter the original global dispatcher with an absolute PC path.
        // It supplies the real device owner to the native PC factory, retains
        // it, and keeps normal read/background-read modes and file lifetime.
        g_open(&file.object, &native, mode);
        if (!Readable(file.object, sizeof(void*))) return false;
        auto table = *static_cast<void***>(file.object);
        if (table != g_pcFileVtable) return false;
        void* sizeMethod = memory::VirtualMethod(file.object, 15);
        if (!sizeMethod || reinterpret_cast<FileSize>(sizeMethod)(file.object) != asset->size) return false;
        *output = file.Take();
        if (g_log) try {
            const auto message = L"mods open: " + asset->absolutePath +
                L"; bytes=" + std::to_wstring(asset->size) + L"; nttFile mode=" + std::to_wstring(mode);
            Log(message.c_str());
        } catch (...) {}
        return true;
    } catch (...) { Log(L"nttFile override open failed; original lookup retained"); return false; }
}
std::optional<std::int64_t> TrySize(const FrontString* path) noexcept {
    try {
        const auto asset = Candidate(path);
        if (!asset) return std::nullopt;
        Selection selected(path->text, asset->nativePath.c_str());
        const auto native = NativeName(*asset);
        const auto size = g_size(&native);
        if (size == asset->size) return size;
    } catch (...) { Log(L"nttFile override size failed; original lookup retained"); }
    return std::nullopt;
}
void** OpenFile(void** output, const FrontString* path, int mode) {
    if (TryOpen(output, path, mode)) return output;
    return g_open(output, path, mode);
}
void** OpenDat(void* self, void** output, const FrontString* path, int mode) {
    if (TryOpen(output, path, mode)) return output;
    return g_datOpen(self, output, path, mode);
}
std::int64_t SizeFile(const FrontString* path) {
    if (const auto size = TrySize(path)) return *size;
    return g_size(path);
}
std::int64_t SizeDat(void* self, const FrontString* path) {
    if (const auto size = TrySize(path)) return *size;
    return g_datSize(self, path);
}
bool InfoFile(const FrontString* path, void* output) {
    try {
        if (output) {
            if (const auto asset = Candidate(path)) {
                Selection selected(path->text, asset->nativePath.c_str());
                const auto native = NativeName(*asset);
                if (g_info(&native, output)) return true;
            }
        }
    } catch (...) { Log(L"nttFile override metadata failed; original lookup retained"); }
    return g_info(path, output);
}
bool ExistsFile(const FrontString* path) {
    try {
        if (const auto asset = Candidate(path)) {
            Selection selected(path->text, asset->nativePath.c_str());
            const auto native = NativeName(*asset);
            if (g_exists(&native)) return true;
        }
    } catch (...) { Log(L"nttFile override existence failed; original lookup retained"); }
    return g_exists(path);
}
struct Hook { void* target{}; void* replacement{}; void** original{}; bool created = false; };
std::array<Hook, 6> g_hooks{};
void Deactivate() noexcept {
    g_ready.store(false, std::memory_order_release);
    for (auto& hook : g_hooks) if (hook.created) { MH_QueueDisableHook(hook.target); MH_DisableHook(hook.target); }
}
}

bool InitializeNttFileRedirector(const std::filesystem::path& gameDirectory,
                               const GameProfile& profile, LogFn log) {
    if (g_attempted) return false;
    g_attempted = true; g_log = log;
    if (profile.backend != RedirectorBackend::NttFile64 || profile.machine != IMAGE_FILE_MACHINE_AMD64) return false;
    const auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    for (const auto& hook : profile.hooks)
        if (!hook.rva || !NativeHookMatches(image, profile.imageSize, hook, profile.preferredImageBase, log)) return false;
    for (const auto& hook : profile.nttDatHooks)
        if (!hook.rva || !NativeHookMatches(image, profile.imageSize, hook, profile.preferredImageBase, log)) return false;
    if (!profile.pcDeviceLookup.rva || !NativeHookMatches(image, profile.imageSize,
            profile.pcDeviceLookup, profile.preferredImageBase, log)) return false;
    for (const auto rva : {profile.pcDeviceVtableRva, profile.nttDatDeviceVtableRva, profile.nttPcFileVtableRva})
        if (!rva || profile.imageSize < 40*sizeof(void*) || rva > profile.imageSize-40*sizeof(void*) ||
            !Readable(image+rva, 40*sizeof(void*))) return false;
    g_pcDeviceVtable = image+profile.pcDeviceVtableRva;
    g_datDeviceVtable = image+profile.nttDatDeviceVtableRva;
    g_pcFileVtable = image+profile.nttPcFileVtableRva;
    g_findDevice = reinterpret_cast<FindDevice>(image+profile.pcDeviceLookup.rva);
    if (!InitializeExternalAssetResolver(gameDirectory, log)) return false;
    g_hooks = {{
        {image+profile.hooks[0].rva, reinterpret_cast<void*>(&OpenFile), reinterpret_cast<void**>(&g_open)},
        {image+profile.hooks[1].rva, reinterpret_cast<void*>(&SizeFile), reinterpret_cast<void**>(&g_size)},
        {image+profile.hooks[2].rva, reinterpret_cast<void*>(&InfoFile), reinterpret_cast<void**>(&g_info)},
        {image+profile.hooks[3].rva, reinterpret_cast<void*>(&ExistsFile), reinterpret_cast<void**>(&g_exists)},
        {image+profile.nttDatHooks[0].rva, reinterpret_cast<void*>(&OpenDat), reinterpret_cast<void**>(&g_datOpen)},
        {image+profile.nttDatHooks[1].rva, reinterpret_cast<void*>(&SizeDat), reinterpret_cast<void**>(&g_datSize)}
    }};
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    for (auto& hook : g_hooks) {
        if (MH_CreateHook(hook.target, hook.replacement, hook.original) != MH_OK) {
            for (auto& staged : g_hooks) if (staged.created) MH_RemoveHook(staged.target);
            Log(L"nttFile hook staging failed; original lookup retained"); return false;
        }
        hook.created = true;
    }
    g_ready.store(true, std::memory_order_release);
    for (const auto& hook : g_hooks)
        if (MH_QueueEnableHook(hook.target) != MH_OK) { Deactivate(); return false; }
    if (MH_ApplyQueued() != MH_OK) { Deactivate(); Log(L"nttFile activation failed; pass-through retained"); return false; }
    Log(L"nttFile x64 backend enabled: global open/size/metadata/existence and direct DAT open/size; native PC ownership retained");
    return true;
}
}
