#include "asset_selection.h"
#include "memory_guard.h"
#include <windows.h>
#include <MinHook.h>
#include "nufile_redirector.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

static_assert(sizeof(void*) == 8, "These reviewed NuFile adapters require x64.");

namespace batman_mods {
namespace {
// Both descriptors borrow character data for the duration of the factory call.
// Reviewed native constructors copy it into their own inline filename storage.
struct NuString64 {
    std::uint16_t capacity;
    std::uint16_t lengthIncludingNul;
    const char* text;
};
struct SharedString64 {
    const char* text;
    std::uint16_t capacity;
    std::uint16_t lengthIncludingNul;
};
struct SharedOwner {
    void* object;
    void* control;
};
static_assert(sizeof(NuString64) == 16 && offsetof(NuString64, text) == 8);
static_assert(sizeof(SharedString64) == 16 && offsetof(SharedString64, capacity) == 8 &&
              offsetof(SharedString64, lengthIncludingNul) == 10);
static_assert(sizeof(SharedOwner) == 16 && offsetof(SharedOwner, control) == 8);

// Windows x64 has one native calling convention. In particular these detours
// have no synthetic second EDX argument used by the x86 fastcall adapters.
using SmartOpen = void** (*)(void*, void**, const NuString64*, int);
using SmartSize = std::int64_t (*)(void*, const NuString64*);
using SmartOffset = bool (*)(void*, const NuString64*, std::int64_t*);
using SmartFactory = void** (*)(void**, const NuString64*, int);
using CStringOpen = void** (*)(void*, void**, const char*, int);
using CStringSize = std::int64_t (*)(void*, const char*);
using CStringOffset = bool (*)(void*, const char*, std::int64_t*);
using CStringFactory = void** (*)(void**, const char*, int);
using DirectOffset = std::int64_t (*)(void*, const char*);
using SharedOpen = SharedOwner* (*)(void*, SharedOwner*, const SharedString64*, int);
using SharedSize = std::int64_t (*)(void*, const SharedString64*);
using SharedOffset = bool (*)(void*, const SharedString64*, std::int64_t*);
using SharedFactory = SharedOwner* (*)(SharedOwner*, const SharedString64*, int, void*);
using Exists = bool (*)(void*, const SharedString64*);
using ReleaseControl = void (*)(void*);
using FindDevice = void* (*)(std::uint32_t);

SmartOpen g_smartOpen = nullptr;
SmartSize g_smartSize = nullptr;
SmartOffset g_smartOffset = nullptr;
SmartFactory g_smartFactory = nullptr;
CStringOpen g_cstringOpen = nullptr;
CStringSize g_cstringSize = nullptr;
CStringOffset g_cstringOffset = nullptr;
CStringFactory g_cstringFactory = nullptr;
DirectOffset g_directOffset = nullptr;
SharedOpen g_sharedOpen = nullptr;
SharedSize g_sharedSize = nullptr;
SharedOffset g_sharedOffset = nullptr;
SharedFactory g_sharedFactory = nullptr;
Exists g_exists = nullptr;
ReleaseControl g_releaseControl = nullptr;
FindDevice g_findDevice = nullptr;
const void* g_pcVtable = nullptr;
bool g_shared = false;
std::atomic<bool> g_ready{false};
bool g_attempted = false;
LogFn g_log = nullptr;

void Log(const wchar_t* message) noexcept {
    if (g_log) { try { g_log(message); } catch (...) {} }
}

using memory::Readable;

void* PCDevice() {
    if (!g_findDevice) return nullptr;
    // ID 1 is the registered host PC device. The separate current-device global
    // can point at DAT while these detours run and must never supply the owner.
    void* owner = g_findDevice(1);
    if (!Readable(owner, sizeof(void*))) return nullptr;
    return *static_cast<const void* const*>(owner) == g_pcVtable ? owner : nullptr;
}

void LogAssetUse(const ExternalAsset& asset) noexcept {
    if (!g_log) return;
    try {
        const std::wstring message = L"mods open: " + asset.absolutePath +
            L"; bytes=" + std::to_wstring(asset.size);
        Log(message.c_str());
    } catch (...) {}
}

using Selection = AssetSelection<struct NuFile64Selection>;



std::optional<ExternalAsset> Candidate(const char* path) {
    if (!g_ready.load(std::memory_order_acquire) || Selection::Contains(path)) return std::nullopt;
    // Apply the same owner-readiness gate to open, size, existence and offsets.
    // Before PC-device initialization all metadata stays on the original asset.
    if (g_shared && !PCDevice()) return std::nullopt;
    return FindExternalAsset(path);
}

std::optional<ExternalAsset> OpenCandidate(const char* path) noexcept {
    try { return Candidate(path); }
    catch (...) { return std::nullopt; }
}

void** OpenSmart(void* self, void** output, const NuString64* path, int mode) {
    if (mode == 0 && output && path) {
        const auto asset = OpenCandidate(path->text);
        if (asset) {
            Selection selected(path->text, asset->nativePath.c_str());
            try {
                const auto length = static_cast<std::uint16_t>(asset->nativePath.size() + 1);
                const NuString64 native{length, length, asset->nativePath.c_str()};
                void* file = nullptr;
                g_smartFactory(&file, &native, mode);
                if (file) {
                    // The factory acquired exactly one intrusive reference.
                    *output = file;
                    LogAssetUse(*asset);
                    return output;
                }
            } catch (...) { Log(L"mods x64 NuFile smart open failed; original lookup retained"); }
            // Keep suppression active for fallback, including factory errors.
            // Native fallback exceptions propagate normally and are not retried.
            return g_smartOpen(self, output, path, mode);
        }
    }
    return g_smartOpen(self, output, path, mode);
}

std::int64_t SizeSmart(void* self, const NuString64* path) {
    try { const auto asset = Candidate(path ? path->text : nullptr); if (asset) return asset->size; }
    catch (...) {}
    return g_smartSize(self, path);
}

bool OffsetSmart(void* self, const NuString64* path, std::int64_t* output) {
    try { if (Candidate(path ? path->text : nullptr)) return false; } catch (...) {}
    return g_smartOffset(self, path, output);
}

void** OpenCString(void* self, void** output, const char* path, int mode) {
    if (mode == 0 && output) {
        const auto asset = OpenCandidate(path);
        if (asset) {
            Selection selected(path, asset->nativePath.c_str());
            try {
                void* file = nullptr;
                g_cstringFactory(&file, asset->nativePath.c_str(), mode);
                if (file) {
                    *output = file;
                    LogAssetUse(*asset);
                    return output;
                }
            } catch (...) { Log(L"mods x64 NuFile C-string open failed; original lookup retained"); }
            return g_cstringOpen(self, output, path, mode);
        }
    }
    return g_cstringOpen(self, output, path, mode);
}

std::int64_t SizeCString(void* self, const char* path) {
    try { const auto asset = Candidate(path); if (asset) return asset->size; }
    catch (...) {}
    return g_cstringSize(self, path);
}

bool OffsetCString(void* self, const char* path, std::int64_t* output) {
    try {
        if (Candidate(path)) {
            return false;
        }
    } catch (...) {}
    return g_cstringOffset(self, path, output);
}

std::int64_t OffsetDirect(void* self, const char* path) {
    try { if (Candidate(path)) return -1; } catch (...) {}
    return g_directOffset(self, path);
}

std::int64_t SizeShared(void* self, const SharedString64* path) {
    try { const auto asset = Candidate(path ? path->text : nullptr); if (asset) return asset->size; }
    catch (...) {}
    return g_sharedSize(self, path);
}

bool OffsetShared(void* self, const SharedString64* path, std::int64_t* output) {
    try {
        if (Candidate(path ? path->text : nullptr)) {
            // Match the modern native PC device's output contract.
            if (output) *output = -1;
            return false;
        }
    } catch (...) {}
    return g_sharedOffset(self, path, output);
}

bool ExistsShared(void* self, const SharedString64* path) {
    try { if (Candidate(path ? path->text : nullptr)) return true; } catch (...) {}
    return g_exists(self, path);
}

// Factory results may own a control block even when object allocation failed.
// Releasing through the reviewed native helper preserves the engine's exact
// strong/weak policy and native virtual destruction/deallocation conventions.
struct TemporarySharedOwner {
    SharedOwner value{};
    TemporarySharedOwner() = default;
    TemporarySharedOwner(const TemporarySharedOwner&) = delete;
    TemporarySharedOwner& operator=(const TemporarySharedOwner&) = delete;
    ~TemporarySharedOwner() noexcept {
        if (value.control) {
            void* control = value.control;
            value = {};
            if (!memory::SharedControl(control) || !memory::VirtualMethod(control, 1) ||
                !memory::VirtualMethod(control, 2) ||
                !memory::Accessible(reinterpret_cast<void*>(g_releaseControl), 1, memory::Access::Execute)) {
                Log(L"mods invalid shared-owner cleanup target; cleanup skipped"); return;
            }
            try { g_releaseControl(control); }
            catch (...) { Log(L"mods native shared-owner cleanup raised an exception"); }
        }
    }
    SharedOwner Take() noexcept {
        const auto result = value;
        value = {};
        return result;
    }
};

SharedOwner* OpenShared(void* self, SharedOwner* output, const SharedString64* path, int mode) {
    if ((mode == 0 || mode == 3) && output && path) {
        const auto asset = OpenCandidate(path->text);
        if (asset) {
            Selection selected(path->text, asset->nativePath.c_str());
            try {
                void* owner = PCDevice();
                if (owner) {
                    const auto length = static_cast<std::uint16_t>(asset->nativePath.size() + 1);
                    const SharedString64 native{asset->nativePath.c_str(), length, length};
                    TemporarySharedOwner temporary;
                    g_sharedFactory(&temporary.value, &native, mode, owner);
                    if (temporary.value.object && temporary.value.control) {
                        // Native DatOpen constructs a hidden return destination;
                        // it never consumes a previous owner from *output.
                        *output = temporary.Take();
                        LogAssetUse(*asset);
                        return output;
                    }
                    // Destroy the temporary owner before archive fallback.
                }
            } catch (...) { Log(L"mods x64 NuFile shared open failed; original lookup retained"); }
            return g_sharedOpen(self, output, path, mode);
        }
    }
    return g_sharedOpen(self, output, path, mode);
}

struct Hook {
    void* target;
    void* replacement;
    void** original;
    bool created = false;
};
std::array<Hook, 4> g_hooks{};

void Deactivate() noexcept {
    g_ready.store(false, std::memory_order_release);
    for (auto& hook : g_hooks) {
        if (!hook.created) continue;
        // Clear pending enable requests even if a hook is still disabled.
        MH_QueueDisableHook(hook.target);
        MH_DisableHook(hook.target);
    }
    // Already-entered detours may still need their originals. Keep native
    // trampolines for the lifetime of the plugin pinned by bootstrap.
}
}

bool InitializeNuFileRedirector(const std::filesystem::path& gameDirectory,
                               const GameProfile& profile, LogFn log) {
    if (g_attempted) return false;
    g_attempted = true;
    g_log = log;
    const bool smart = profile.backend == RedirectorBackend::NuFileDeviceSmart64;
    const bool cstring = profile.backend == RedirectorBackend::NuFileDeviceSmartCString64;
    const bool shared = profile.backend == RedirectorBackend::NuFileDeviceShared64;
    if (!smart && !cstring && !shared) return false;
    const auto image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image || profile.machine != IMAGE_FILE_MACHINE_AMD64) return false;
    for (const auto& target : profile.hooks)
        if (!target.rva || !NativeHookMatches(image, profile.imageSize, target, profile.preferredImageBase, log)) {
            Log(L"x64 NuFile signature mismatch; native hooks skipped");
            return false;
        }
    if (shared) {
        if (!profile.releaseControl.rva || !profile.pcDeviceLookup.rva ||
            !NativeHookMatches(image, profile.imageSize, profile.releaseControl, profile.preferredImageBase, log) ||
            !NativeHookMatches(image, profile.imageSize, profile.pcDeviceLookup, profile.preferredImageBase, log) ||
            profile.imageSize < sizeof(void*) || !profile.pcDeviceVtableRva ||
            profile.pcDeviceVtableRva > profile.imageSize - sizeof(void*) ||
            !Readable(image + profile.pcDeviceVtableRva, sizeof(void*))) {
            Log(L"x64 shared-owner helper validation failed; native hooks skipped");
            return false;
        }
        g_findDevice = reinterpret_cast<FindDevice>(image + profile.pcDeviceLookup.rva);
        g_pcVtable = image + profile.pcDeviceVtableRva;
        g_releaseControl = reinterpret_cast<ReleaseControl>(image + profile.releaseControl.rva);
        g_sharedFactory = reinterpret_cast<SharedFactory>(image + profile.hooks[2].rva);
    }
    if (!InitializeExternalAssetResolver(gameDirectory, log)) return false;
    g_shared = shared;
    if (smart) {
        g_smartFactory = reinterpret_cast<SmartFactory>(image + profile.hooks[2].rva);
        g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenSmart), reinterpret_cast<void**>(&g_smartOpen)};
        g_hooks[1] = {image + profile.hooks[1].rva, reinterpret_cast<void*>(&SizeSmart), reinterpret_cast<void**>(&g_smartSize)};
        g_hooks[2] = {image + profile.hooks[3].rva, reinterpret_cast<void*>(&OffsetSmart), reinterpret_cast<void**>(&g_smartOffset)};
    } else {
        if (shared) {
            g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenShared), reinterpret_cast<void**>(&g_sharedOpen)};
            g_hooks[1] = {image + profile.hooks[1].rva, reinterpret_cast<void*>(&SizeShared), reinterpret_cast<void**>(&g_sharedSize)};
            g_hooks[2] = {image + profile.hooks[3].rva, reinterpret_cast<void*>(&OffsetShared), reinterpret_cast<void**>(&g_sharedOffset)};
        } else {
            g_cstringFactory = reinterpret_cast<CStringFactory>(image + profile.hooks[2].rva);
            g_hooks[0] = {image + profile.hooks[0].rva, reinterpret_cast<void*>(&OpenCString), reinterpret_cast<void**>(&g_cstringOpen)};
            g_hooks[1] = {image + profile.hooks[1].rva, reinterpret_cast<void*>(&SizeCString), reinterpret_cast<void**>(&g_cstringSize)};
            g_hooks[2] = {image + profile.hooks[3].rva, reinterpret_cast<void*>(&OffsetCString), reinterpret_cast<void**>(&g_cstringOffset)};
        }
    }
    g_hooks[3] = shared
        ? Hook{image + profile.hooks[4].rva, reinterpret_cast<void*>(&ExistsShared), reinterpret_cast<void**>(&g_exists)}
        : Hook{image + profile.hooks[4].rva, reinterpret_cast<void*>(&OffsetDirect), reinterpret_cast<void**>(&g_directOffset)};
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    for (auto& hook : g_hooks) {
        if (MH_CreateHook(hook.target, hook.replacement, hook.original) != MH_OK) {
            // No hook has been enabled, so no detour can hold a trampoline.
            for (auto& staged : g_hooks)
                if (staged.created) MH_RemoveHook(staged.target);
            Log(L"x64 NuFile hook staging failed; original lookup retained");
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
        Log(L"x64 NuFile activation failed; pass-through trampolines retained");
        return false;
    }
    Log(shared ? L"x64 NuFile shared-owner backend enabled" :
        smart ? L"x64 NuFile descriptor backend enabled" : L"x64 NuFile C-string backend enabled");
    return true;
}
}
