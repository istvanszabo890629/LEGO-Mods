#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <optional>
#include <memory>
#include "game_profiles.h"

namespace batman_mods {

using LogFn = void (*)(const wchar_t* message);

namespace detail { struct AssetReadLease; }

struct ExternalAsset {
    std::wstring absolutePath;
    std::string nativePath;
    std::int64_t size;
    // Keep the validated file and mods directory chain stable through the
    // synchronous native open. The game's own handle owns subsequent reads.
    std::shared_ptr<const detail::AssetReadLease> readLease;
};

// Shared path resolver; backends retain their own native file objects and ABIs.
bool InitializeExternalAssetResolver(const std::filesystem::path& gameDirectory, LogFn log);
std::optional<ExternalAsset> FindExternalAsset(const char* requestedPath);
bool SameExternalAssetPath(const char* a, const char* b) noexcept;

// Call only after the host has verified the complete supported EXE hash, and
// outside DllMain/the loader lock. This independently checks all five prologues.
// The override folder defaults to "mods" and is configurable in LegoMods.ini.
// This function creates no files.
bool InitializeRedirector(const std::filesystem::path& gameDirectory,
                          const GameProfile& profile, LogFn log);

#ifdef BATMAN_MODS_TESTING
namespace testing {

using OpenFn = int(__cdecl*)(const char*, int, void*, int);
using ExistsFn = int(__cdecl*)(const char*);
using SizeFn = std::int64_t(__cdecl*)(const char*);
using WholeFileFn = int(__cdecl*)(const char*, void*, int);
using WaveFn = long(__thiscall*)(void*, void*, const char*, void*);

struct NativeTargets {
    OpenFn open = nullptr;
    ExistsFn exists = nullptr;
    SizeFn size = nullptr;
    WholeFileFn wholeFile = nullptr;
    WaveFn wave = nullptr;
};

struct ResolvedPath {
    std::wstring absolutePath;
    std::string nativePath;
    std::int64_t size = 0;
};

// Own-process fixture only: create but never enable one hook, then verify
// staging cleanup versus retained memory after possible activation.
bool CheckRollbackPolicy(OpenFn target, bool activationStarted);

// No code patching: installs fixture functions as native trampolines. A fixture
// must supply all five targets. Reset only after its calls have finished.
bool Configure(const std::filesystem::path& gameDirectory,
               const NativeTargets& targets, LogFn log = nullptr);
void Reset();
bool Resolve(const char* requestedPath, ResolvedPath& result);
int Open(const char* path, int mode, void* archiveContext, int flag);
int Exists(const char* path);
std::int64_t Size(const char* path);
int WholeFile(const char* path, void* destination, int capacity);
long Wave(void* self, void* archiveContext, const char* path, void* formatOut);

}  // namespace testing
#endif

}  // namespace batman_mods
