#include "../src/memory_guard.h"
#include "../src/asset_selection.h"
#include "../src/win_handle.h"
#include "../src/image_headers.h"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value) { if (!value) throw std::runtime_error("Hardening fixture failed"); }
void NativeMethod(void*) {}

constexpr batman_mods::NativeHook RelocationHook(std::uint8_t offset, std::uint8_t count = 1) {
    batman_mods::NativeHook hook{};
    hook.relocationCount = count;
    hook.relocations[0] = offset;
    return hook;
}
static_assert(batman_mods::NativeHookReadSize(batman_mods::NativeHook{}) == 16);
static_assert(batman_mods::NativeHookReadSize(RelocationHook(0)) == 16);
static_assert(batman_mods::NativeHookReadSize(RelocationHook(15)) == 15 + sizeof(std::uintptr_t));
static_assert(batman_mods::NativeHookReadSize(RelocationHook(16)) == 0);
static_assert(batman_mods::NativeHookReadSize(RelocationHook(0, 5)) == 0);

void ImageReadFixtures() {
    using namespace batman_mods;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto page = static_cast<std::size_t>(system.dwPageSize);
    const auto size = static_cast<std::uint32_t>(page * 3);
    auto image = static_cast<unsigned char*>(VirtualAlloc(nullptr, size,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(image != nullptr);
    try {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 128;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
#if defined(_M_X64)
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#else
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#endif
        nt->OptionalHeader.SizeOfImage = size;
        NativeHook hook{};
        hook.rva = static_cast<std::uint32_t>(2 * page - hook.prologue.size());
        hook.prologue.fill(0x90);
        std::memcpy(image + hook.rva, hook.prologue.data(), hook.prologue.size());
        DWORD old = 0;
        Check(VirtualProtect(image + 2 * page, page, PAGE_NOACCESS, &old) != FALSE);
        const auto base = reinterpret_cast<std::uintptr_t>(image);
        Check(NativeHookMatches(image, size, hook, base, nullptr)); // 16 readable bytes, no extra page read
        auto outside = hook;
        outside.rva = size - 15;
        Check(!NativeHookMatches(image, size, outside, base, nullptr));
        hook.relocationCount = 1;
        hook.relocations[0] = 15;
        Check(!NativeHookMatches(image, size, hook, base, nullptr)); // required relocation tail is unreadable
        hook.relocations[0] = 16;
        Check(!NativeHookMatches(image, size, hook, base, nullptr)); // invalid metadata
        hook.relocationCount = 0;
        Check(NativeImageHeadersMatch(image, size));
        Check(VirtualProtect(image, page, PAGE_NOACCESS, &old) != FALSE);
        Check(!NativeImageHeadersMatch(image, size));
        Check(!NativeHookMatches(image, size, hook, base, nullptr));
        Check(VirtualProtect(image, page, PAGE_READWRITE, &old) != FALSE);
        Check(VirtualProtect(image + page, page, PAGE_NOACCESS, &old) != FALSE);
        constexpr auto prefix = offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
            offsetof(IMAGE_OPTIONAL_HEADER, SizeOfImage) + sizeof(DWORD);
        dos->e_lfanew = static_cast<LONG>(page - prefix);
        std::memcpy(image + dos->e_lfanew, nt, prefix);
        Check(NativeImageHeadersMatch(image, size)); // unused NT suffix may be on the protected next page
        dos->e_lfanew = static_cast<LONG>(page - 16);
        Check(!NativeImageHeadersMatch(image, size)); // required NT fields cross into an unreadable page
        dos->e_lfanew = 128;
        Check(VirtualProtect(image + page, page, PAGE_READWRITE, &old) != FALSE);
        Check(VirtualProtect(image + page, page, PAGE_READWRITE | PAGE_GUARD, &old) != FALSE);
        Check(!NativeHookMatches(image, size, hook, base, nullptr));
        MEMORY_BASIC_INFORMATION guarded{};
        Check(VirtualQuery(image + page, &guarded, sizeof(guarded)) != 0 && (guarded.Protect & PAGE_GUARD));
        Check(VirtualProtect(image + page, page, PAGE_READWRITE, &old) != FALSE);
        hook.relocationCount = 1;
        hook.relocations[0] = 0;
        for (const auto preferred : {base - 0x10000, base + 0x10000}) {
            const std::uintptr_t expected = preferred + 0x1234;
            const std::uintptr_t mapped = base + 0x1234;
            std::memcpy(hook.prologue.data(), &expected, sizeof(expected));
            std::memcpy(image + hook.rva, &mapped, sizeof(mapped));
            Check(NativeHookMatches(image, size, hook, preferred, nullptr));
        }
        Check(!NativeImageHeadersMatch(nullptr, size));
        Check(!NativeImageHeadersMatch(image, sizeof(IMAGE_DOS_HEADER)));
        Check(!NativeImageHeadersMatch(reinterpret_cast<const unsigned char*>(
            std::numeric_limits<std::uintptr_t>::max() - 1), size));
        Check(!NativeHookMatches(image, size, hook, 0, nullptr));
    } catch (...) { VirtualFree(image, 0, MEM_RELEASE); throw; }
    VirtualFree(image, 0, MEM_RELEASE);
}
}

// Compiled for x86 and x64. Execution remains explicitly opt-in via -Test.
void RunMemoryGuardFixtures() {
    using namespace batman_mods;
    ImageReadFixtures();
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto page = static_cast<std::size_t>(info.dwPageSize);
    auto allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(allocation != nullptr);
    try {
        DWORD old = 0;
        Check(VirtualProtect(allocation + page, page, PAGE_READONLY, &old) != FALSE);
        Check(memory::Readable(allocation + page - 1, 2));
        Check(!memory::Accessible(allocation + page - 1, 2, memory::Access::Write));
        Check(VirtualProtect(allocation + page, page, PAGE_NOACCESS, &old) != FALSE);
        Check(!memory::Readable(allocation + page - 1, 2));
        Check(!memory::Readable(nullptr, 1));
        Check(!memory::Readable(allocation, 0));
        Check(!memory::Readable(reinterpret_cast<void*>(std::numeric_limits<std::uintptr_t>::max() - 1), 4));
        Check(memory::VirtualMethod(allocation + page, 5) == nullptr);
        void* table[6]{};
        struct Object { void** table; } object{table};
        table[5] = allocation;
        Check(memory::VirtualMethod(&object, 5) == nullptr); // readable data is not callable
        table[5] = reinterpret_cast<void*>(&NativeMethod);
        Check(memory::VirtualMethod(&object, 5) == table[5]);
        Check(memory::VirtualMethod(&object, 64) == nullptr);
        struct Control { void** table; LONG strong; LONG weak; } control{table, 1, 1};
        Check(memory::SharedControl(&control));
        control.strong = 0;
        Check(!memory::SharedControl(&control));
        control.strong = 1;
        control.weak = 0;
        Check(!memory::SharedControl(&control));
        Check(control.strong == 1 && control.weak == 0);
        Check(!memory::SharedControl(allocation + page - sizeof(void*)));
        Check(!memory::SharedControl(allocation + 1));
    } catch (...) { VirtualFree(allocation, 0, MEM_RELEASE); throw; }
    VirtualFree(allocation, 0, MEM_RELEASE);

    using Selected = AssetSelection<struct FirstBackend>;
    using Other = AssetSelection<struct SecondBackend>;
    Check(!Selected::Contains("stuff/text/a.txt"));
    {
        Selected outer("stuff/text/a.txt", "C:/game/mods/stuff/text/a.txt");
        Check(Selected::Contains("STUFF\\TEXT\\A.TXT"));
        Check(!Other::Contains("stuff/text/a.txt"));
        try {
            Selected inner("other.bin", "C:/game/mods/other.bin");
            Check(Selected::Contains("other.bin"));
            throw 1;
        } catch (int) {}
        Check(!Selected::Contains("other.bin"));
        Check(Selected::Contains("stuff/text/a.txt"));
    }
    Check(!Selected::Contains("stuff/text/a.txt"));
}
