#include "asset_selection.h"
#include <windows.h>
#include <MinHook.h>
#include "nufile_redirector.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

static_assert(sizeof(void*) == 4, "The reviewed NuFile adapters are x86.");

namespace batman_mods {
namespace {
// The native constructors copy this borrowed name into the returned object's
// own inline filename storage. Neither descriptor nor character data is owned.
struct NuString8 {
    std::uint16_t capacity;
    std::uint16_t lengthIncludingNul;
    const char* text;
};
static_assert(sizeof(NuString8) == 8 && offsetof(NuString8, text) == 4);

using RawOpen = void* (__thiscall*)(void*, const char*, int);
using RawSize = std::int64_t (__thiscall*)(void*, const char*);
using RawOffset = bool (__thiscall*)(void*, const char*, std::int64_t*);
using RawFactory = void* (__cdecl*)(const char*, int);
using SmartOpen = void** (__thiscall*)(void*, void**, const NuString8*, int);
using SmartSize = std::int64_t (__thiscall*)(void*, const NuString8*);
using SmartOffset = bool (__thiscall*)(void*, const NuString8*, std::int64_t*);
using SmartFactory = void** (__cdecl*)(void**, const NuString8*, int);
using SmartCStringOpen = void** (__thiscall*)(void*, void**, const char*, int);
using SmartCStringFactory = void** (__cdecl*)(void**, const char*, int);
using DirectOffset = std::int64_t (__thiscall*)(void*, const char*);

RawOpen g_rawOpen = nullptr;
RawSize g_rawSize = nullptr;
RawOffset g_rawOffset = nullptr;
RawFactory g_rawFactory = nullptr;
SmartOpen g_smartOpen = nullptr;
SmartSize g_smartSize = nullptr;
SmartOffset g_smartOffset = nullptr;
SmartFactory g_smartFactory = nullptr;
SmartCStringOpen g_smartCStringOpen = nullptr;
SmartCStringFactory g_smartCStringFactory = nullptr;
DirectOffset g_directOffset = nullptr;
std::atomic<bool> g_ready{false};
bool g_attempted = false;
LogFn g_log = nullptr;

void Log(const wchar_t* message) noexcept {
    if (g_log) { try { g_log(message); } catch (...) {} }
}

void LogAssetUse(const ExternalAsset& asset) noexcept {
    if (!g_log) return;
    try {
        const std::wstring message = L"mods open: " + asset.absolutePath +
            L"; bytes=" + std::to_wstring(asset.size);
        Log(message.c_str());
    } catch (...) {}
}

using Selection = AssetSelection<struct NuFile32Selection>;



std::optional<ExternalAsset> Candidate(const char* path) {
    if (!g_ready.load(std::memory_order_acquire) || Selection::Contains(path)) return std::nullopt;
    return FindExternalAsset(path);
}

std::optional<ExternalAsset> OpenCandidate(const char* path) noexcept {
    try { return Candidate(path); }
    catch (...) { return std::nullopt; }
}

void* __fastcall OpenRaw(void* self, void*, const char* path, int mode) {
    if (mode == 0) {
        if (const auto asset = OpenCandidate(path)) {
            Selection selected(path, asset->nativePath.c_str());
            try {
                if (void* file = g_rawFactory(asset->nativePath.c_str(), mode)) {
                    LogAssetUse(*asset); return file;
                }
            } catch (...) { Log(L"mods NuFile open failed; original lookup retained"); }
            return g_rawOpen(self, path, mode);
        }
    }
    return g_rawOpen(self, path, mode);
}

std::int64_t __fastcall SizeRaw(void* self, void*, const char* path) {
    try { const auto asset = Candidate(path); if (asset) return asset->size; }
    catch (...) {}
    return g_rawSize(self, path);
}

bool __fastcall OffsetRaw(void* self, void*, const char* path, std::int64_t* output) {
    try { if (Candidate(path)) return false; } catch (...) {}
    return g_rawOffset(self, path, output);
}

void** __fastcall OpenSmart(void* self, void*, void** output, const NuString8* path, int mode) {
    if (mode == 0 && output && path) {
        if (const auto asset = OpenCandidate(path->text)) {
            Selection selected(path->text, asset->nativePath.c_str());
            try {
                const auto length = static_cast<std::uint16_t>(asset->nativePath.size() + 1);
                const NuString8 native{length, length, asset->nativePath.c_str()};
                void* file = nullptr;
                g_smartFactory(&file, &native, mode);
                if (file) { *output = file; LogAssetUse(*asset); return output; }
            } catch (...) { Log(L"mods NuFile smart open failed; original lookup retained"); }
            return g_smartOpen(self, output, path, mode);
        }
    }
    return g_smartOpen(self, output, path, mode);
}

std::int64_t __fastcall SizeSmart(void* self, void*, const NuString8* path) {
    try { const auto asset = Candidate(path ? path->text : nullptr); if (asset) return asset->size; }
    catch (...) {}
    return g_smartSize(self, path);
}

void** __fastcall OpenSmartCString(void* self, void*, void** output, const char* path, int mode) {
    if (mode == 0 && output) {
        if (const auto asset = OpenCandidate(path)) {
            Selection selected(path, asset->nativePath.c_str());
            try {
                void* file = nullptr;
                g_smartCStringFactory(&file, asset->nativePath.c_str(), mode);
                if (file) { *output = file; LogAssetUse(*asset); return output; }
            } catch (...) { Log(L"mods NuFile smart C-string open failed; original lookup retained"); }
            return g_smartCStringOpen(self, output, path, mode);
        }
    }
    return g_smartCStringOpen(self, output, path, mode);
}

bool __fastcall OffsetSmart(void* self, void*, const NuString8* path, std::int64_t* output) {
    try { if (Candidate(path ? path->text : nullptr)) return false; } catch (...) {}
    return g_smartOffset(self, path, output);
}

std::int64_t __fastcall OffsetDirect(void* self, void*, const char* path) {
    try { if (Candidate(path)) return -1; } catch (...) {}
    return g_directOffset(self, path);
}

struct Hook {
    void* target;
    void* replacement;
    void** original;
    bool created = false;
};
std::array<Hook, 4> g_hooks{};

// On activation failure keep trampolines for the process lifetime: an already
// entered detour may still need its original. Hooks then act as pass-through.
void Deactivate() noexcept {
    g_ready.store(false, std::memory_order_release);
    for (auto& hook : g_hooks)
        if (hook.created) {
            MH_QueueDisableHook(hook.target);
            MH_DisableHook(hook.target);
        }
}
}

bool InitializeNuFileRedirector(const std::filesystem::path& gameDirectory,
                               const GameProfile& profile, LogFn log) {
    if (g_attempted) return false;
    g_attempted = true;
    g_log = log;
    const bool smart = profile.backend == RedirectorBackend::NuFileDeviceSmart;
    const bool smartCString = profile.backend == RedirectorBackend::NuFileDeviceSmartCString;
    if (!smart && !smartCString && profile.backend != RedirectorBackend::NuFileDevice) return false;
    const auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return false;
    for (const auto& target : profile.hooks)
        if (!target.rva || !NativeHookMatches(image, profile.imageSize, target, profile.preferredImageBase, log)) {
            Log(L"NuFile signature mismatch; native hooks skipped");
            return false;
        }
    if (!InitializeExternalAssetResolver(gameDirectory, log)) return false;
    if (smart) {
        g_smartFactory = reinterpret_cast<SmartFactory>(image + profile.hooks[2].rva);
        g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenSmart), reinterpret_cast<void**>(&g_smartOpen)};
        g_hooks[1] = {image + profile.hooks[1].rva, reinterpret_cast<void*>(&SizeSmart), reinterpret_cast<void**>(&g_smartSize)};
        g_hooks[2] = {image + profile.hooks[3].rva, reinterpret_cast<void*>(&OffsetSmart), reinterpret_cast<void**>(&g_smartOffset)};
    } else {
        if (smartCString) {
            g_smartCStringFactory = reinterpret_cast<SmartCStringFactory>(image + profile.hooks[2].rva);
            g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenSmartCString), reinterpret_cast<void**>(&g_smartCStringOpen)};
        } else {
            g_rawFactory = reinterpret_cast<RawFactory>(image + profile.hooks[2].rva);
            g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenRaw), reinterpret_cast<void**>(&g_rawOpen)};
        }
        g_hooks[1] = {image + profile.hooks[1].rva, reinterpret_cast<void*>(&SizeRaw), reinterpret_cast<void**>(&g_rawSize)};
        g_hooks[2] = {image + profile.hooks[3].rva, reinterpret_cast<void*>(&OffsetRaw), reinterpret_cast<void**>(&g_rawOffset)};
    }
    g_hooks[3] = {image + profile.hooks[4].rva, reinterpret_cast<void*>(&OffsetDirect), reinterpret_cast<void**>(&g_directOffset)};
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    for (auto& hook : g_hooks) {
        if (MH_CreateHook(hook.target, hook.replacement, hook.original) != MH_OK) {
            // None has been enabled yet, so these trampolines have no callers.
            for (auto& staged : g_hooks)
                if (staged.created) MH_RemoveHook(staged.target);
            Log(L"NuFile hook staging failed; original functions retained");
            return false;
        }
        hook.created = true;
    }
    g_ready.store(true, std::memory_order_release);
    for (const auto& hook : g_hooks)
        if (MH_QueueEnableHook(hook.target) != MH_OK) {
            Deactivate();
            return false;
        }
    if (MH_ApplyQueued() != MH_OK) {
        Deactivate();
        Log(L"NuFile hook activation failed; pass-through trampolines retained");
        return false;
    }
    Log(smart ? L"NuFile smart-pointer backend enabled" :
        smartCString ? L"NuFile smart C-string backend enabled" : L"NuFile raw-pointer backend enabled");
    return true;
}
}
