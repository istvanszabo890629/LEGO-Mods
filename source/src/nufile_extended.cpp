#include "asset_selection.h"
#include "memory_guard.h"
#include <windows.h>
#include <MinHook.h>
#include "nufile_redirector.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Only the newly reviewed families use these adapters. The previous native
// adapters remain unchanged. x86 methods have an ECX receiver and a callee-
// cleaned stack; Win64 has one calling convention and no dummy EDX argument.
#if defined(_M_IX86)
#define EXT_METHOD __thiscall
#define EXT_DETOUR __fastcall
#define EXT_SELF void* self, void*
#else
#define EXT_METHOD
#define EXT_DETOUR
#define EXT_SELF void* self
#endif

namespace batman_mods {
namespace {
struct FrontString {
    const char* text;
    std::uint16_t capacity;
    std::uint16_t lengthIncludingNul;
};
struct SharedOwner { void* object; void* control; };
static_assert(offsetof(FrontString, text) == 0);
static_assert(offsetof(FrontString, capacity) == sizeof(void*));
static_assert(offsetof(FrontString, lengthIncludingNul) == sizeof(void*) + 2);
static_assert(sizeof(FrontString) == (sizeof(void*) == 8 ? 16 : 8));
static_assert(sizeof(SharedOwner) == 2 * sizeof(void*));

using RawOpen = void* (EXT_METHOD*)(void*, const char*, int);
using RawFactory = void* (__cdecl*)(const char*, int);
using IntrusiveOpen = void** (EXT_METHOD*)(void*, void**, const FrontString*, int);
using IntrusiveFactory = void** (__cdecl*)(void**, const FrontString*, int);
using SharedOpen = SharedOwner* (EXT_METHOD*)(void*, SharedOwner*, const FrontString*, int);
using SharedFactory = SharedOwner* (__cdecl*)(SharedOwner*, const FrontString*, int, void*);
// Both pointer representations have the same native pointer ABI. Name() is
// the only place interpreting them; originals always receive the same value.
using Size = std::int64_t (EXT_METHOD*)(void*, const void*);
using Offset = bool (EXT_METHOD*)(void*, const void*, std::int64_t*);
using Exists = bool (EXT_METHOD*)(void*, const void*);
using FindDevice = void* (__cdecl*)(std::uint32_t);
using NativeCleanup = void (EXT_METHOD*)(void*);

RawOpen g_rawOpen = nullptr;
RawFactory g_rawFactory = nullptr;
IntrusiveOpen g_intrusiveOpen = nullptr;
IntrusiveFactory g_intrusiveFactory = nullptr;
SharedOpen g_sharedOpen = nullptr;
SharedFactory g_sharedFactory = nullptr;
Size g_size = nullptr;
Offset g_offset = nullptr;
Exists g_exists = nullptr;
FindDevice g_findDevice = nullptr;
const void* g_pcVtable = nullptr;
std::uint32_t g_pcId = 0;
bool g_raw = false, g_shared = false, g_attempted = false;
std::atomic<bool> g_ready{false};
LogFn g_log = nullptr;

void Log(const wchar_t* text) noexcept {
    if (g_log) { try { g_log(text); } catch (...) {} }
}
using memory::Readable;
void* PCDevice() {
    void* owner = g_findDevice ? g_findDevice(g_pcId) : nullptr;
    return Readable(owner, sizeof(void*)) && *static_cast<const void* const*>(owner) == g_pcVtable ? owner : nullptr;
}
const char* Name(const void* path) noexcept {
    return g_raw ? static_cast<const char*>(path) : path ? static_cast<const FrontString*>(path)->text : nullptr;
}
using Selection = AssetSelection<struct ExtendedSelection>;
std::optional<ExternalAsset> Candidate(const char* path) noexcept {
    try {
        if (!g_ready.load(std::memory_order_acquire)) return std::nullopt;
        if (Selection::Contains(path)) return std::nullopt;
        // Keep all metadata on the same asset before the host device exists.
        if (g_shared && !PCDevice()) return std::nullopt;
        return FindExternalAsset(path);
    } catch (...) { return std::nullopt; }
}
void LogUse(const ExternalAsset& asset) noexcept {
    if (!g_log) return;
    try { const auto message=L"mods open: "+asset.absolutePath+L"; bytes="+std::to_wstring(asset.size); Log(message.c_str()); }
    catch (...) {}
}

struct TemporaryIntrusive {
    void* object = nullptr;
    TemporaryIntrusive() = default;
    TemporaryIntrusive(const TemporaryIntrusive&) = delete;
    TemporaryIntrusive& operator=(const TemporaryIntrusive&) = delete;
    ~TemporaryIntrusive() noexcept {
        if (object) {
            void* cleanup = memory::VirtualMethod(object, 5);
            if (!cleanup) { Log(L"mods invalid intrusive cleanup target; cleanup skipped"); return; }
            try { reinterpret_cast<NativeCleanup>(cleanup)(object); }
            catch (...) { Log(L"mods native intrusive cleanup raised an exception"); }
        }
    }
    void* Take() noexcept { auto result=object; object=nullptr; return result; }
};
struct TemporaryShared {
    SharedOwner value{};
    TemporaryShared() = default;
    TemporaryShared(const TemporaryShared&) = delete;
    TemporaryShared& operator=(const TemporaryShared&) = delete;
    ~TemporaryShared() noexcept {
        if (!value.control) return;
        // This is the exact Ninjago native protocol observed in PC open:
        // strong is DWORD at control+pointerSize, weak at pointerSize+4.
        // Both virtual cleanup methods belong to the game's control block.
        void* control=value.control;
        value = {};
        void* destroy = memory::VirtualMethod(control, 1);
        if (!memory::SharedControl(control) || !destroy || !memory::VirtualMethod(control, 2)) {
            Log(L"mods invalid shared control block; cleanup skipped"); return;
        }
        try {
            auto counts=reinterpret_cast<volatile LONG*>(static_cast<unsigned char*>(control)+sizeof(void*));
            if (InterlockedDecrement(counts) > 0) { InterlockedDecrement(counts+1); return; }
            reinterpret_cast<NativeCleanup>(destroy)(control);
            if (InterlockedDecrement(counts+1) == 0) {
                void* deallocate = memory::VirtualMethod(control, 2);
                if (deallocate) reinterpret_cast<NativeCleanup>(deallocate)(control);
                else Log(L"mods invalid shared deallocation target; cleanup skipped");
            }
        } catch (...) { Log(L"mods native shared cleanup raised an exception"); }
    }
    SharedOwner Take() noexcept { auto result=value; value={}; return result; }
};

void* EXT_DETOUR OpenRaw(EXT_SELF, const char* path, int mode) {
    if (mode == 0) {
        if (const auto asset=Candidate(path)) {
            Selection selected(path, asset->nativePath.c_str());
            try { if (void* file=g_rawFactory(asset->nativePath.c_str(), mode)) { LogUse(*asset); return file; } }
            catch (...) { Log(L"mods extended raw open failed; archive fallback retained"); }
            return g_rawOpen(self,path,mode);
        }
    }
    return g_rawOpen(self,path,mode);
}
void** EXT_DETOUR OpenIntrusive(EXT_SELF, void** output, const FrontString* path, int mode) {
    if (mode == 0 && output && path) {
        if (const auto asset=Candidate(path->text)) {
            Selection selected(path->text,asset->nativePath.c_str());
            try {
                const auto length=static_cast<std::uint16_t>(asset->nativePath.size()+1);
                const FrontString native{asset->nativePath.c_str(),length,length};
                TemporaryIntrusive temporary;
                g_intrusiveFactory(&temporary.object,&native,mode);
                if (temporary.object) { *output=temporary.Take(); LogUse(*asset); return output; }
            } catch (...) { Log(L"mods extended intrusive open failed; archive fallback retained"); }
            return g_intrusiveOpen(self,output,path,mode);
        }
    }
    return g_intrusiveOpen(self,output,path,mode);
}
SharedOwner* EXT_DETOUR OpenShared(EXT_SELF, SharedOwner* output, const FrontString* path, int mode) {
    if ((mode == 0 || mode == 3) && output && path) {
        if (const auto asset=Candidate(path->text)) {
            Selection selected(path->text,asset->nativePath.c_str());
            try {
                if (void* owner=PCDevice()) {
                    const auto length=static_cast<std::uint16_t>(asset->nativePath.size()+1);
                    const FrontString native{asset->nativePath.c_str(),length,length};
                    TemporaryShared temporary;
                    g_sharedFactory(&temporary.value,&native,mode,owner);
                    if (temporary.value.object && temporary.value.control) { *output=temporary.Take(); LogUse(*asset); return output; }
                }
            } catch (...) { Log(L"mods extended shared open failed; archive fallback retained"); }
            return g_sharedOpen(self,output,path,mode);
        }
    }
    return g_sharedOpen(self,output,path,mode);
}
std::int64_t EXT_DETOUR FileSize(EXT_SELF, const void* path) {
    if (const auto asset=Candidate(Name(path))) return asset->size;
    return g_size(self,path);
}
bool EXT_DETOUR FileOffset(EXT_SELF, const void* path, std::int64_t* output) {
    if (Candidate(Name(path))) {
        if (g_shared && output) *output=-1;
        return false;
    }
    return g_offset(self,path,output);
}
bool EXT_DETOUR FileExists(EXT_SELF, const void* path) {
    if (Candidate(Name(path))) return true;
    return g_exists(self,path);
}
struct Hook { void* target{}; void* replacement{}; void** original{}; bool created=false; };
std::array<Hook,4> g_hooks{};
void Deactivate() noexcept {
    g_ready.store(false,std::memory_order_release);
    for (auto& hook:g_hooks) if (hook.created) { MH_QueueDisableHook(hook.target); MH_DisableHook(hook.target); }
}
}

bool InitializeExtendedNuFileRedirector(const std::filesystem::path& gameDirectory,
                                       const GameProfile& profile, LogFn log) {
    if (g_attempted) return false;
    g_attempted=true; g_log=log;
    g_raw=profile.backend == RedirectorBackend::NuFileExtendedRaw;
    g_shared=profile.backend == RedirectorBackend::NuFileExtendedShared;
    if (!g_raw && !g_shared && profile.backend != RedirectorBackend::NuFileExtendedIntrusive) return false;
    const auto image=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image || profile.machine != (sizeof(void*) == 8 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386)) return false;
    for (std::size_t index=0;index<profile.hooks.size();++index) {
        const auto& target=profile.hooks[index];
        if ((!target.rva && index<3) || (target.rva && !NativeHookMatches(image,profile.imageSize,target,profile.preferredImageBase,log))) {
            Log(L"Extended NuFile signature mismatch; native hooks skipped"); return false;
        }
    }
    if (g_shared) {
        if (!profile.pcDeviceLookup.rva || !profile.pcDeviceId || profile.pcDeviceId == 0xffff ||
            !NativeHookMatches(image,profile.imageSize,profile.pcDeviceLookup,profile.preferredImageBase,log) ||
            profile.imageSize < sizeof(void*) || !profile.pcDeviceVtableRva || profile.pcDeviceVtableRva > profile.imageSize-sizeof(void*) ||
            !Readable(image+profile.pcDeviceVtableRva,sizeof(void*))) return false;
        g_findDevice=reinterpret_cast<FindDevice>(image+profile.pcDeviceLookup.rva);
        g_pcVtable=image+profile.pcDeviceVtableRva; g_pcId=profile.pcDeviceId;
    }
    if (!InitializeExternalAssetResolver(gameDirectory,log)) return false;
    if (g_raw) {
        g_rawFactory=reinterpret_cast<RawFactory>(image+profile.hooks[2].rva);
        g_hooks[0]={image+profile.hooks[0].rva,reinterpret_cast<void*>(&OpenRaw),reinterpret_cast<void**>(&g_rawOpen)};
    } else if (g_shared) {
        g_sharedFactory=reinterpret_cast<SharedFactory>(image+profile.hooks[2].rva);
        g_hooks[0]={image+profile.hooks[0].rva,reinterpret_cast<void*>(&OpenShared),reinterpret_cast<void**>(&g_sharedOpen)};
    } else {
        g_intrusiveFactory=reinterpret_cast<IntrusiveFactory>(image+profile.hooks[2].rva);
        g_hooks[0]={image+profile.hooks[0].rva,reinterpret_cast<void*>(&OpenIntrusive),reinterpret_cast<void**>(&g_intrusiveOpen)};
    }
    g_hooks[1]={image+profile.hooks[1].rva,reinterpret_cast<void*>(&FileSize),reinterpret_cast<void**>(&g_size)};
    if (profile.hooks[3].rva) g_hooks[2]={image+profile.hooks[3].rva,reinterpret_cast<void*>(&FileOffset),reinterpret_cast<void**>(&g_offset)};
    if (profile.hooks[4].rva) g_hooks[3]={image+profile.hooks[4].rva,reinterpret_cast<void*>(&FileExists),reinterpret_cast<void**>(&g_exists)};
    const auto initialized=MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    for (auto& hook:g_hooks) {
        if (!hook.target) continue;
        if (MH_CreateHook(hook.target,hook.replacement,hook.original) != MH_OK) {
            for (auto& staged:g_hooks) if (staged.created) MH_RemoveHook(staged.target);
            Log(L"Extended NuFile staging failed; original lookup retained"); return false;
        }
        hook.created=true;
    }
    g_ready.store(true,std::memory_order_release);
    for (const auto& hook:g_hooks) if (hook.created && MH_QueueEnableHook(hook.target) != MH_OK) { Deactivate(); return false; }
    if (MH_ApplyQueued() != MH_OK) { Deactivate(); Log(L"Extended NuFile activation failed; pass-through retained"); return false; }
    Log(g_raw ? L"Extended NuFile raw backend enabled" : g_shared ? L"Extended NuFile shared-owner backend enabled" : L"Extended NuFile front-string intrusive backend enabled");
    return true;
}
}
