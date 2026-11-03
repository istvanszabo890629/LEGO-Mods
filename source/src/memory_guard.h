#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace batman_mods::memory {

enum class Access { Read, Write, Execute };

// These are precondition checks, not exception recovery or a security boundary
// against another thread changing memory. Native ownership must remain valid.
inline bool Accessible(const void* pointer, std::size_t length, Access access) noexcept {
    auto cursor = reinterpret_cast<std::uintptr_t>(pointer);
    if (!cursor || !length || length > std::numeric_limits<std::uintptr_t>::max() - cursor) return false;
    const auto end = cursor + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = region.Protect & 0xff;
        const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool executable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool readable = writable || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
        if ((access == Access::Read && !readable) || (access == Access::Write && !writable) ||
            (access == Access::Execute && !executable)) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (cursor < begin || region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - begin) return false;
        const auto next = begin + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

inline bool Readable(const void* pointer, std::size_t length) noexcept {
    return Accessible(pointer, length, Access::Read);
}

inline void* VirtualMethod(void* object, std::size_t slot) noexcept {
    if (!Readable(object, sizeof(void*)) || slot >= 64) return nullptr;
    auto table = *static_cast<void***>(object);
    if (!Readable(table, (slot + 1) * sizeof(void*))) return nullptr;
    void* method = table[slot];
    return Accessible(method, 1, Access::Execute) ? method : nullptr;
}

// Shared-owner adapters use reviewed DWORD counters immediately after a
// pointer-sized vtable field. Do not dereference/decrement an invalid block.
inline bool SharedControl(void* control) noexcept {
    if (!Readable(control, sizeof(void*) + 2 * sizeof(LONG))) return false;
    auto address = static_cast<unsigned char*>(control) + sizeof(void*);
    if (reinterpret_cast<std::uintptr_t>(address) % alignof(LONG) != 0 ||
        !Accessible(address, 2 * sizeof(LONG), Access::Write)) return false;
    auto counts = reinterpret_cast<volatile LONG*>(address);
    // A temporary that owns one native strong reference must keep both
    // counters positive. Read atomically without changing either counter.
    return InterlockedCompareExchange(counts, 0, 0) > 0 &&
        InterlockedCompareExchange(counts + 1, 0, 0) > 0;
}

} // namespace batman_mods::memory
