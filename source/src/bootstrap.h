#pragma once
#include <windows.h>

namespace loader {
void Attach(HMODULE self) noexcept;
void EnsureInitialized() noexcept;
void Log(const wchar_t* message) noexcept;
}
