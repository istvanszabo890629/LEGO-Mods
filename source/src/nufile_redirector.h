#pragma once
#include "redirector.h"

namespace batman_mods {
// Reviewed x86 NuFileDevice families. Hook order in GameProfile:
// DAT open, DAT size, native PC factory, virtual archive offset, direct offset.
bool InitializeNuFileRedirector(const std::filesystem::path& gameDirectory,
                               const GameProfile& profile, LogFn log);
bool InitializeExtendedNuFileRedirector(const std::filesystem::path& gameDirectory,
                                       const GameProfile& profile, LogFn log);
#if defined(_M_X64)
bool InitializeNttFileRedirector(const std::filesystem::path& gameDirectory,
                               const GameProfile& profile, LogFn log);
#endif
}
