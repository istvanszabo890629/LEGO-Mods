#include "win_handle.h"
#include "image_headers.h"
#include "bootstrap.h"
#include "redirector.h"
#include "game_profiles.h"
#if defined(_M_IX86)
#include "english_plus.h"
#endif
#include "nufile_redirector.h"
#include "runtime_settings.h"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if !defined(_M_IX86) && !defined(_M_X64)
#error The verified backends support x86 and x64 Windows processes.
#endif

namespace {
HMODULE gSelf = nullptr;
INIT_ONCE gInit = INIT_ONCE_STATIC_INIT;
HANDLE gLog = INVALID_HANDLE_VALUE;
std::atomic<bool> gLoggingEnabled{false};

std::filesystem::path ModulePath(HMODULE module) {
  std::wstring buffer(32768, L'\0');
  DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!length || length >= buffer.size()) return {};
  buffer.resize(length);
  return buffer;
}

bool ReadExecutableIdentity(const std::filesystem::path& file, std::uint64_t& fileSize,
                            std::array<std::uint8_t, 32>& digest,
                            batman_mods::UniqueHandle& readLease) {
  batman_mods::UniqueHandle input(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!input) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(input.get(), &size) || size.QuadPart <= 0) { return false; }
  fileSize = static_cast<std::uint64_t>(size.QuadPart);
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
  if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<BYTE, 65536> buffer{};
  DWORD read = 0;
  std::uint64_t totalRead = 0;
  while (ok) {
    if (!ReadFile(input.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    totalRead += read;
    ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
  }
  ok = ok && totalRead == fileSize;
  if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  if (ok) readLease = std::move(input);
  return ok;
}

bool VerifyRunningImage(const batman_mods::GameProfile& profile) noexcept {
  const auto module = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
  if (!batman_mods::NativeImageHeadersMatch(module, profile.imageSize)) return false;
#if defined(_M_X64)
  constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
  constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#endif
  return profile.machine == kMachine &&
      (!profile.preferredBaseOnly || reinterpret_cast<std::uintptr_t>(module) == profile.preferredImageBase);
}

BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*) {
  try {
    auto selfPath = ModulePath(gSelf);
    const auto executable = ModulePath(nullptr);
    const bool logging = !executable.empty() &&
        batman_mods::ModuleLoggingEnabled(executable.parent_path());
    gLoggingEnabled.store(logging, std::memory_order_release);
    if (logging && !selfPath.empty()) {
      auto logPath = selfPath.parent_path() / L"LegoMods.log";
      gLog = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    loader::Log(L"LEGO Mods 0.5.16; game-specific verified backends");
    std::uint64_t fileSize = 0;
    std::array<std::uint8_t, 32> digest{};
    batman_mods::UniqueHandle executableReadLease;
    if (executable.empty() || !ReadExecutableIdentity(executable, fileSize, digest, executableReadLease)) {
      loader::Log(L"Could not verify the main executable; game modifications skipped.");
      return TRUE;
    }
    constexpr wchar_t hex[] = L"0123456789abcdef";
    wchar_t hashText[65]{}, identity[256]{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
      hashText[2*i] = hex[digest[i] >> 4];
      hashText[2*i+1] = hex[digest[i] & 15];
    }
    swprintf_s(identity, L"Host image: bytes=%llu SHA256=%ls mapped=%p",
        static_cast<unsigned long long>(fileSize), hashText,
        reinterpret_cast<void*>(GetModuleHandleW(nullptr)));
    loader::Log(identity);
    const auto profile = batman_mods::FindGameProfileBySha256(digest);
    if (!profile || profile->fileSize != fileSize || !VerifyRunningImage(*profile)) {
      loader::Log(L"Unknown/unsupported executable or image layout; game modifications skipped.");
      return TRUE;
    }
    loader::Log(profile->label);
    swprintf_s(identity, L"Profile image: machine=0x%04X preferred=0x%llX size=0x%08X",
        profile->machine, static_cast<unsigned long long>(profile->preferredImageBase), profile->imageSize);
    loader::Log(identity);
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&gSelf), &pinned)) {
      loader::Log(L"Could not pin plugin; game hooks skipped.");
      return TRUE;
    }
    const batman_mods::LogFn runtimeLog = logging ? loader::Log : nullptr;
    const bool extended = profile->backend == batman_mods::RedirectorBackend::NuFileExtendedRaw ||
        profile->backend == batman_mods::RedirectorBackend::NuFileExtendedIntrusive ||
        profile->backend == batman_mods::RedirectorBackend::NuFileExtendedShared;
    const bool flatVfs = profile->backend == batman_mods::RedirectorBackend::BatmanVfs ||
        profile->backend == batman_mods::RedirectorBackend::FlatVfs;
    const bool redirected =
#if defined(_M_X64)
        profile->backend == batman_mods::RedirectorBackend::NttFile64
        ? batman_mods::InitializeNttFileRedirector(executable.parent_path(), *profile, runtimeLog)
        :
#endif
        flatVfs
        ? batman_mods::InitializeRedirector(executable.parent_path(), *profile, runtimeLog)
        : extended ? batman_mods::InitializeExtendedNuFileRedirector(executable.parent_path(), *profile, runtimeLog)
                   : batman_mods::InitializeNuFileRedirector(executable.parent_path(), *profile, runtimeLog);
    if (redirected)
      loader::Log(L"Native mods redirection enabled. Missing overrides use original lookup.");
    else
      loader::Log(L"Native redirection initialization failed; original lookup retained.");
    // Validated text dictionaries are independent of archive dispatch hooks.
    // The INI module validates every target pointer and expected original text.
#if defined(_M_IX86)
    if (profile->textConfigFile) {
      const auto textResult = batman_mods::InitializeEnglishPlus(executable.parent_path(), *profile, runtimeLog);
      if (textResult.status == batman_mods::EnglishPlusStatus::Error)
        loader::Log(L"Text configuration could not be applied; see preceding diagnostics.");
    }
#endif
  } catch (...) {
    loader::Log(L"Initialization exception; original lookup retained.");
  }
  return TRUE;
}
}

namespace loader {
void Attach(HMODULE self) noexcept { gSelf = self; }
void EnsureInitialized() noexcept { InitOnceExecuteOnce(&gInit, Initialize, nullptr, nullptr); }
void Log(const wchar_t* message) noexcept {
  if (!gLoggingEnabled.load(std::memory_order_acquire) || !message) return;
  OutputDebugStringW(message);
  OutputDebugStringW(L"\n");
  if (gLog == INVALID_HANDLE_VALUE) return;
  char line[4096]{};
  SYSTEMTIME now{};
  GetLocalTime(&now);
  const int prefix = sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u] ",
      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  if (prefix < 0) return;
  int bytes = WideCharToMultiByte(CP_UTF8, 0, message, -1, line + prefix,
                                 static_cast<int>(sizeof(line)) - prefix - 3, nullptr, nullptr);
  if (!bytes) return;
  const int end = prefix + bytes - 1;
  line[end] = '\r'; line[end + 1] = '\n';
  DWORD written = 0;
  WriteFile(gLog, line, static_cast<DWORD>(end + 2), &written, nullptr);
}
}
