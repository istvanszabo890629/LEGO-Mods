#pragma once
#include "memory_guard.h"
#include <cstring>

namespace batman_mods {

// Snapshot only committed/readable header ranges. This is a precondition
// check, not synchronization with another module changing page protection.
inline bool NativeImageHeadersMatch(const unsigned char* image, std::uint32_t imageSize) noexcept {
    // The identity fields end at SizeOfImage; data directories are not needed.
    constexpr std::size_t identityBytes = offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
        offsetof(IMAGE_OPTIONAL_HEADER, SizeOfImage) + sizeof(DWORD);
    static_assert(identityBytes <= sizeof(IMAGE_NT_HEADERS));
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (!image || imageSize < sizeof(IMAGE_DOS_HEADER) ||
        imageSize > std::numeric_limits<std::uintptr_t>::max() - base ||
        !memory::Readable(image, sizeof(IMAGE_DOS_HEADER))) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096) return false;
    const auto offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > imageSize || identityBytes > imageSize - offset ||
        !memory::Readable(image + offset, identityBytes)) return false;
    IMAGE_NT_HEADERS headers{};
    std::memcpy(&headers, image + offset, identityBytes);
#if defined(_M_X64)
    constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
    constexpr WORD magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
    constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
    constexpr WORD magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif
    return headers.Signature == IMAGE_NT_SIGNATURE && headers.FileHeader.Machine == machine &&
        headers.OptionalHeader.Magic == magic && headers.OptionalHeader.SizeOfImage == imageSize;
}

} // namespace batman_mods
