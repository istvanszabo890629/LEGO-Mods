// x86 integration tests. MinHook patches only functions defined in this helper.
#ifndef BATMAN_MODS_TESTING
#define BATMAN_MODS_TESTING
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <MinHook.h>
#include "../src/redirector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 4, "Build redirector_tests for x86/Win32.");
namespace fs = std::filesystem;
namespace test = batman_mods::testing;
void RunMemoryGuardFixtures();

namespace {

constexpr int kOriginalOpen = 701;
constexpr int kModOpen = 902;
constexpr int kOriginalExists = 333;
constexpr std::int64_t kOriginalSize = 0x123456789ABLL;
constexpr long kOriginalWave = 0x2345;
constexpr int kWholeReturnBase = 12000;
const std::string kOriginalBytes = "base";
const std::string kReplacement = "replacement data is longer than the original";
const std::string kNestedReplacement = "different nested asset replacement";
const char* kAsset = "STUFF\\TEXT\\ENGLISH.TXT";
fs::path g_gameRoot;
std::string g_modsPrefix;
bool g_failModOpen = false;
bool g_failModWhole = false;
bool g_failModWave = false;
bool g_throwOriginalOpen = false, g_throwOriginalWhole = false, g_throwOriginalWave = false;
int g_checks = 0;
int g_nestedExists = 0;
int g_nestedDifferentExists = 0;
int g_nestedDifferentOpen = 0;
std::int64_t g_nestedDifferentSize = -1;
std::string g_nestedDifferentBytes;
int g_fallbackNestedExists = 0;
int g_fallbackNestedOpen = 0;
std::int64_t g_fallbackNestedSize = -1;

struct OpenCall { std::string path; int mode; void* context; int flag; };
struct WholeCall { std::string path; void* destination; int capacity; };
struct WaveCall { void* self; void* context; std::string path; void* formatOut; };
std::vector<OpenCall> g_openCalls;
std::vector<std::string> g_existsCalls;
std::vector<std::string> g_sizeCalls;
std::vector<WholeCall> g_wholeCalls;
std::vector<WaveCall> g_waveCalls;

void Check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) throw std::runtime_error(description);
}

std::string Normalize(std::string value) {
    for (char& c : value) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool IsMod(const char* path) {
    return path && Normalize(path).compare(0, g_modsPrefix.size(), g_modsPrefix) == 0;
}

std::string SafePath(const char* path) { return path ? std::string(path) : std::string("<null>"); }

void ClearCalls() {
    g_openCalls.clear(); g_existsCalls.clear(); g_sizeCalls.clear();
    g_wholeCalls.clear(); g_waveCalls.clear(); g_nestedExists = 0;
    g_nestedDifferentExists = 0; g_nestedDifferentOpen = 0; g_nestedDifferentSize = -1;
    g_nestedDifferentBytes.clear();
    g_fallbackNestedExists = 0; g_fallbackNestedOpen = 0; g_fallbackNestedSize = -1;
}

void WriteFile(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot create a test fixture file");
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("Cannot write a test fixture file");
}

std::string ReadFile(const char* path) {
    std::ifstream stream(fs::path(path), std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

__declspec(noinline) int __cdecl FakeOpen(const char* path, int mode, void* context, int flag) {
    g_openCalls.push_back({SafePath(path), mode, context, flag});
    if (!IsMod(path) && g_throwOriginalOpen) throw std::runtime_error("native open fixture");
    return IsMod(path) ? (g_failModOpen ? 0 : kModOpen) : kOriginalOpen;
}

__declspec(noinline) int __cdecl FakeExists(const char* path) {
    g_existsCalls.push_back(SafePath(path));
    return kOriginalExists;
}

__declspec(noinline) std::int64_t __cdecl FakeSize(const char* path) {
    g_sizeCalls.push_back(SafePath(path));
    if (IsMod(path)) return static_cast<std::int64_t>(fs::file_size(fs::path(path)));
    return kOriginalSize;
}

// These volatile call sites ensure optimized builds enter the patched fixture
// functions rather than bypassing the detour via compiler inference.
int EnterOpen(const char* path, int mode, void* context, int flag) {
    test::OpenFn volatile entry = &FakeOpen;
    return entry(path, mode, context, flag);
}
int EnterExists(const char* path) {
    test::ExistsFn volatile entry = &FakeExists;
    return entry(path);
}
std::int64_t EnterSize(const char* path) {
    test::SizeFn volatile entry = &FakeSize;
    return entry(path);
}

__declspec(noinline) int __cdecl FakeWholeFile(const char* path, void* destination, int capacity) {
    g_wholeCalls.push_back({SafePath(path), destination, capacity});
    const bool mod = IsMod(path);
    if (!mod && g_throwOriginalWhole) throw std::runtime_error("native whole-file fixture");
    if (mod && g_failModWhole) return 0;
    if (mod) {
        // Nested calls for the selected original/resolved asset stay native.
        // A parser's dependency on a different asset must still be redirected.
        g_nestedExists = EnterExists(kAsset);
        EnterOpen(path, 0, reinterpret_cast<void*>(0x12345678), 0x5647);
        EnterSize(path);
        g_nestedDifferentExists = EnterExists("new_asset.bin");
        g_nestedDifferentSize = EnterSize("new_asset.bin");
        g_nestedDifferentOpen = EnterOpen("new_asset.bin", 0, reinterpret_cast<void*>(0x34567812), 0x7832);
        if (!g_openCalls.empty()) g_nestedDifferentBytes = ReadFile(g_openCalls.back().path.c_str());
    } else if (path && std::strcmp(path, kAsset) == 0 && g_failModWhole) {
        // Once this mod attempt has failed, the original parser's nested calls
        // must not rediscover the same mod or use its conflicting metadata.
        g_fallbackNestedExists = EnterExists(path);
        g_fallbackNestedSize = EnterSize(path);
        g_fallbackNestedOpen = EnterOpen(path, 0, reinterpret_cast<void*>(0x23456781), 0x6758);
    }
    const std::string bytes = mod ? ReadFile(path) : kOriginalBytes;
    if (!destination || capacity < 0 || static_cast<std::size_t>(capacity) < bytes.size()) return 0;
    if (!bytes.empty()) std::memcpy(destination, bytes.data(), bytes.size());
    return kWholeReturnBase + static_cast<int>(bytes.size());
}

__declspec(noinline) long __fastcall FakeWave(void* self, void*, void* context,
                                             const char* path, void* formatOut) {
    g_waveCalls.push_back({self, context, SafePath(path), formatOut});
    const bool mod = IsMod(path);
    if (!mod && g_throwOriginalWave) throw std::runtime_error("native WAVE fixture");
    if (mod && g_failModWave) return E_FAIL;
    if (formatOut) *static_cast<unsigned*>(formatOut) = mod ? 0x4D4F4453U : 0x4E415449U;
    return mod ? S_OK : kOriginalWave;
}

long __fastcall WaveDetour(void* self, void*, void* context, const char* path, void* formatOut) {
    return test::Wave(self, context, path, formatOut);
}

int EnterWhole(const char* path, void* destination, int capacity) {
    test::WholeFileFn volatile entry = &FakeWholeFile;
    return entry(path, destination, capacity);
}
long EnterWave(void* self, void* context, const char* path, void* formatOut) {
    test::WaveFn volatile entry = reinterpret_cast<test::WaveFn>(&FakeWave);
    return entry(self, context, path, formatOut);
}

class HookFixture {
public:
    HookFixture() {
        Check(MH_Initialize() == MH_OK, "MinHook initializes in our helper process");
        initialized_ = true;
    }
    ~HookFixture() {
        for (void* target : hooks_) MH_DisableHook(target);
        test::Reset();
        for (void* target : hooks_) MH_RemoveHook(target);
        if (initialized_) MH_Uninitialize();
    }
    void Configure() {
        test::NativeTargets targets;
        Add(reinterpret_cast<void*>(&FakeOpen), reinterpret_cast<void*>(&test::Open), reinterpret_cast<void**>(&targets.open));
        Add(reinterpret_cast<void*>(&FakeExists), reinterpret_cast<void*>(&test::Exists), reinterpret_cast<void**>(&targets.exists));
        Add(reinterpret_cast<void*>(&FakeSize), reinterpret_cast<void*>(&test::Size), reinterpret_cast<void**>(&targets.size));
        Add(reinterpret_cast<void*>(&FakeWholeFile), reinterpret_cast<void*>(&test::WholeFile), reinterpret_cast<void**>(&targets.wholeFile));
        Add(reinterpret_cast<void*>(&FakeWave), reinterpret_cast<void*>(&WaveDetour), reinterpret_cast<void**>(&targets.wave));
        Check(test::Configure(g_gameRoot, targets), "Redirector accepts all five native trampolines");
        for (void* target : hooks_) Check(MH_QueueEnableHook(target) == MH_OK, "Own fixture detour queues successfully");
        Check(MH_ApplyQueued() == MH_OK, "Five own-process fixture detours activate together");
    }
private:
    void Add(void* target, void* detour, void** original) {
        Check(MH_CreateHook(target, detour, original) == MH_OK, "Own fixture function supports an x86 MinHook trampoline");
        hooks_.push_back(target);
    }
    bool initialized_ = false;
    std::vector<void*> hooks_;
};

void TestResolution() {
    test::ResolvedPath resolved;
    for (const char* spelling : {kAsset, "stuff/text/english.txt", ".\\stuff//text/./english.txt"}) {
        Check(test::Resolve(spelling, resolved), "Relative asset resolves regardless of slash/case spelling");
        Check(resolved.size == static_cast<std::int64_t>(kReplacement.size()), "Resolver returns replacement's own size");
        Check(Normalize(resolved.nativePath) == Normalize((g_gameRoot / L"mods/STUFF/TEXT/ENGLISH.TXT").string()), "Resolved native path mirrors internal folders below mods");
    }
    const std::string inGameAbsolute = (g_gameRoot / L"STUFF/TEXT/ENGLISH.TXT").string();
    Check(test::Resolve(inGameAbsolute.c_str(), resolved), "An absolute request under the configured game root is supported");
    Check(test::Resolve("new_asset.bin", resolved), "A mod asset needs no original loose file");
    Check(!fs::exists(g_gameRoot / L"new_asset.bin"), "The new-asset fixture has no loose original");
    Check(test::Resolve("textures/asset.dds", resolved), "Arbitrary file extensions use the same resolver");
    Check(!test::Resolve("STUFF/TEXT", resolved), "A directory cannot be treated as a replacement file");
    Check(!test::Resolve("missing.file", resolved), "A missing mod file does not resolve");
    Check(resolved.absolutePath.empty() && resolved.nativePath.empty() && resolved.size == 0, "Failed resolve clears output state");
    WriteFile(g_gameRoot/L"mods/missing.file", "added after the first miss");
    Check(test::Resolve("missing.file", resolved), "An earlier miss must not hide a newly created mod file");
    fs::remove(g_gameRoot/L"mods/missing.file");

    const char* invalid[] = {nullptr, "", ".", "..\\outside.txt", "STUFF\\..\\TEXT\\ENGLISH.TXT",
        "C:\\outside\\ENGLISH.TXT", "C:STUFF\\TEXT\\ENGLISH.TXT", "\\STUFF\\TEXT\\ENGLISH.TXT",
        "\\\\server\\share\\ENGLISH.TXT", "\\\\?\\C:\\ENGLISH.TXT", "\\\\.\\NUL", "NUL.txt", "COM1.bin",
        "STUFF\\TEXT\\ENGLISH.TXT:stream", "STUFF\\TEXT\\ENGLISH.TXT.", "STUFF\\TEXT\\ENGLISH.TXT ",
        "STUFF\\TEXT\\*.TXT", "STUFF\\TEXT\\?.TXT", "STUFF\\TEXT\\<bad>.TXT", "STUFF\\TEXT\\bad\x01.TXT"};
    for (const char* path : invalid) Check(!test::Resolve(path, resolved), "Unsafe/non-asset request is not redirected");
    const std::string existingOutside = (g_gameRoot.parent_path() / L"outside.bin").string();
    Check(fs::exists(fs::path(existingOutside)) && !test::Resolve(existingOutside.c_str(), resolved),
          "An existing absolute file outside the game cannot be redirected");
    const std::string oversized(256, 'a');
    Check(!test::Resolve(oversized.c_str(), resolved), "Overlong engine request is not redirected");
    std::cout << "PASS: Path resolution, independent file types and invalid paths.\n";
}

void TestOpen() {
    int contextMarker = 91;
    void* context = &contextMarker;
    constexpr int flag = 0x76543210;
    ClearCalls();
    Check(EnterOpen(kAsset, 0, context, flag) == kModOpen, "Read-open returns native result for selected override");
    Check(g_openCalls.size() == 1 && IsMod(g_openCalls[0].path.c_str()), "Read-open passes a mods path to native trampoline");
    Check(g_openCalls[0].mode == 0 && !g_openCalls[0].context && g_openCalls[0].flag == flag, "Redirected read clears archive context and preserves mode/flag");

    ClearCalls();
    Check(EnterOpen("missing.file", 0, context, flag) == kOriginalOpen, "Missing override falls back to original open");
    Check(g_openCalls.size() == 1 && g_openCalls[0].path == "missing.file" && g_openCalls[0].context == context && g_openCalls[0].flag == flag, "Fallback preserves original path/context/flag exactly");

    for (int mode : {1, 2, 3, -1, 0x100}) {
        ClearCalls();
        Check(EnterOpen(kAsset, mode, context, flag) == kOriginalOpen, "Non-read mode stays on original asset");
        Check(g_openCalls.size() == 1 && g_openCalls[0].path == kAsset && g_openCalls[0].mode == mode &&
              g_openCalls[0].context == context && g_openCalls[0].flag == flag, "Write/other modes preserve all native arguments");
    }

    ClearCalls(); g_failModOpen = true;
    Check(EnterOpen(kAsset, 0, context, flag) == kOriginalOpen, "Failed mod open retries the original asset");
    Check(g_openCalls.size() == 2 && IsMod(g_openCalls[0].path.c_str()) && g_openCalls[1].path == kAsset &&
          g_openCalls[1].context == context && g_openCalls[1].flag == flag, "Failed mod-open fallback retains original arguments");
    g_failModOpen = false;
    std::cout << "PASS: Actual x86 open detour, read selection, modes and fallback arguments.\n";
}

void TestMetadata() {
    ClearCalls();
    Check(EnterExists(kAsset) == static_cast<int>(kReplacement.size()), "Exists reports nonempty replacement size");
    Check(EnterSize(kAsset) == static_cast<std::int64_t>(kReplacement.size()), "Size reports longer replacement size");
    Check(g_existsCalls.empty() && g_sizeCalls.empty(), "Replacement metadata avoids original archive callbacks");
    Check(EnterExists("empty.bin") != 0, "Empty mod file still exists");
    Check(EnterSize("empty.bin") == 0, "Empty mod file has zero size");
    Check(EnterExists("missing.file") == kOriginalExists, "Missing override preserves native existence result");
    Check(EnterSize("missing.file") == kOriginalSize, "Missing override preserves full 64-bit native size result");
    Check(g_existsCalls.size() == 1 && g_existsCalls[0] == "missing.file" &&
          g_sizeCalls.size() == 1 && g_sizeCalls[0] == "missing.file", "Metadata fallback passes unchanged original paths");
    std::cout << "PASS: Actual exists/64-bit-size detours, changed sizes and empty files.\n";
}

void TestWholeFile() {
    std::array<char, 128> output;
    output.fill('#'); ClearCalls();
    Check(EnterWhole(kAsset, output.data(), static_cast<int>(output.size())) == kWholeReturnBase + static_cast<int>(kReplacement.size()), "Whole-file hook preserves native successful return value");
    Check(std::string(output.data(), kReplacement.size()) == kReplacement && output[kReplacement.size()] == '#', "Whole-file reads all replacement bytes without overrunning content");
    Check(g_wholeCalls.size() == 1 && IsMod(g_wholeCalls[0].path.c_str()) && g_wholeCalls[0].destination == output.data() &&
          g_wholeCalls[0].capacity == static_cast<int>(output.size()), "Whole-file native call preserves destination and capacity");
    Check(g_nestedExists == kOriginalExists && g_existsCalls.size() == 1, "Nested same-original asset lookup stays native");
    Check(g_openCalls.size() == 2 && g_openCalls[0].context == reinterpret_cast<void*>(0x12345678) &&
          g_openCalls[0].flag == 0x5647, "Nested same-resolved asset open keeps native context and flag");
    Check(g_nestedDifferentExists == static_cast<int>(kNestedReplacement.size()) &&
          g_nestedDifferentSize == static_cast<std::int64_t>(kNestedReplacement.size()),
          "A different nested asset still receives replacement metadata");
    Check(g_nestedDifferentOpen == kModOpen && g_openCalls[1].context == nullptr && g_openCalls[1].flag == 0x7832 &&
          IsMod(g_openCalls[1].path.c_str()) && g_nestedDifferentBytes == kNestedReplacement,
          "A different nested asset opens and reads its own replacement while the outer native parser is active");

    output.fill('#'); ClearCalls();
    Check(EnterWhole(kAsset, output.data(), 1) == 0, "Insufficient capacity preserves native failure");
    Check(output[0] == '#' && g_wholeCalls.size() == 2 && g_wholeCalls[0].capacity == 1 && g_wholeCalls[1].capacity == 1,
          "Insufficient buffers remain untouched through mod and original attempts");
    Check(g_wholeCalls[1].path == kAsset && g_wholeCalls[1].destination == output.data(), "Whole-file capacity fallback retains original path and buffer");

    output.fill('#'); ClearCalls();
    Check(EnterWhole(kAsset, output.data(), -7) == 0 && output[0] == '#' && g_wholeCalls.size() == 2 &&
          g_wholeCalls[0].capacity == -7 && g_wholeCalls[1].capacity == -7,
          "Negative capacity is forwarded unchanged and leaves the buffer untouched");

    output.fill('#'); ClearCalls(); g_failModWhole = true;
    Check(EnterWhole(kAsset, output.data(), static_cast<int>(output.size())) == kWholeReturnBase + static_cast<int>(kOriginalBytes.size()), "Failed mod whole-file parse falls back to native original");
    Check(g_wholeCalls.size() == 2 && g_wholeCalls[1].path == kAsset && std::string(output.data(), kOriginalBytes.size()) == kOriginalBytes,
          "Whole-file fallback returns original content");
    Check(g_fallbackNestedExists == kOriginalExists && g_fallbackNestedSize == kOriginalSize &&
          g_fallbackNestedOpen == kOriginalOpen && g_openCalls.size() == 1 &&
          g_openCalls[0].path == kAsset && g_openCalls[0].context == reinterpret_cast<void*>(0x23456781) &&
          g_openCalls[0].flag == 0x6758,
          "Failed override fallback keeps same-asset nested open and metadata native");
    g_failModWhole = false;

    ClearCalls();
    Check(EnterWhole(kAsset, nullptr, 128) == 0 && g_wholeCalls.size() == 2 && !g_wholeCalls[1].destination,
          "Null whole-file destination is passed through unchanged");
    output.fill('#'); ClearCalls();
    Check(EnterWhole("empty.bin", output.data(), 0) == kWholeReturnBase && output[0] == '#', "Empty replacement supports zero-capacity native load");
    std::cout << "PASS: Actual whole-file detour, buffers/capacity and nested native calls.\n";
}

void TestWave() {
    int objectMarker = 17, contextMarker = 23;
    unsigned format = 0;
    ClearCalls();
    Check(EnterWave(&objectMarker, &contextMarker, "audio/test.wav", &format) == S_OK, "WAVE detour accepts zero HRESULT success");
    Check(g_waveCalls.size() == 1 && g_waveCalls[0].self == &objectMarker && !g_waveCalls[0].context &&
          IsMod(g_waveCalls[0].path.c_str()) && g_waveCalls[0].formatOut == &format && format == 0x4D4F4453U,
          "WAVE thiscall/fastcall bridge preserves this/output and clears redirected archive context");

    ClearCalls(); format = 0;
    Check(EnterWave(&objectMarker, &contextMarker, "audio/missing.wav", &format) == kOriginalWave,
          "Missing WAVE override preserves original HRESULT");
    Check(g_waveCalls.size() == 1 && g_waveCalls[0].self == &objectMarker && g_waveCalls[0].context == &contextMarker &&
          g_waveCalls[0].path == "audio/missing.wav" && g_waveCalls[0].formatOut == &format && format == 0x4E415449U,
          "Missing WAVE fallback preserves all native arguments");

    ClearCalls(); format = 0; g_failModWave = true;
    Check(EnterWave(&objectMarker, &contextMarker, "audio/test.wav", &format) == kOriginalWave, "Failed mod WAVE parser retries original stream");
    Check(g_waveCalls.size() == 2 && !g_waveCalls[0].context && g_waveCalls[1].self == &objectMarker &&
          g_waveCalls[1].context == &contextMarker && g_waveCalls[1].path == "audio/test.wav" &&
          g_waveCalls[1].formatOut == &format && format == 0x4E415449U,
          "Failed WAVE override restores context/path/output arguments");
    g_failModWave = false;
    std::cout << "PASS: Actual x86 WAVE detour and thiscall/fastcall argument preservation.\n";
}

fs::path FixtureDirectory() {
    std::wstring module(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) throw std::runtime_error("Cannot locate our helper executable");
    module.resize(length);
    const fs::path base = fs::path(module).parent_path();
    return base / (L"redirector-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
}

void TestFallbackExceptions() {
    ClearCalls(); g_failModOpen = true; g_throwOriginalOpen = true;
    bool caught = false;
    try { EnterOpen(kAsset, 0, nullptr, 1); } catch (const std::runtime_error&) { caught = true; }
    g_failModOpen = false; g_throwOriginalOpen = false;
    Check(caught && g_openCalls.size() == 2, "A native open exception propagates after exactly one fallback call");
    ClearCalls(); g_failModWhole = true; g_throwOriginalWhole = true;
    caught = false;
    char output[128]{};
    try { EnterWhole(kAsset, output, sizeof(output)); } catch (const std::runtime_error&) { caught = true; }
    g_failModWhole = false; g_throwOriginalWhole = false;
    Check(caught && g_wholeCalls.size() == 2, "A native whole-file exception is never retried");
    ClearCalls(); g_failModWave = true; g_throwOriginalWave = true;
    caught = false;
    unsigned format = 0;
    try { EnterWave(nullptr, nullptr, "audio/test.wav", &format); } catch (const std::runtime_error&) { caught = true; }
    g_failModWave = false; g_throwOriginalWave = false;
    Check(caught && g_waveCalls.size() == 2, "A native WAVE exception is never retried");
}

void TestReadLease() {
    auto asset = batman_mods::FindExternalAsset(kAsset);
    Check(asset && asset->readLease, "The actual resolver returns a retained file lease");
    const auto path = asset->absolutePath;
    HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD writerError = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Check(writer == INVALID_HANDLE_VALUE && writerError == ERROR_SHARING_VIOLATION,
          "A writer cannot alter the validated file before the native open");
    Check(!DeleteFileW(path.c_str()) && GetLastError() == ERROR_SHARING_VIOLATION,
          "The checked file cannot be removed before the native open");
    Check(!MoveFileW((g_gameRoot/L"mods/STUFF").c_str(), (g_gameRoot/L"mods/STUFF-moved").c_str()),
          "The checked directory chain cannot be renamed during native open");
    HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(reader != INVALID_HANDLE_VALUE, "Native read sharing remains compatible with the retained lease");
    CloseHandle(reader);
    auto secondLease = asset;
    asset.reset();
    Check(!DeleteFileW(path.c_str()), "Copied assets retain their shared lease");
    secondLease.reset();
    writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "The lease releases after the operation");
    CloseHandle(writer);
}

}  // namespace

int main() {
    try {
        RunMemoryGuardFixtures();
        Check(test::CheckRollbackPolicy(&FakeOpen, false), "Never-exposed staging hooks can be freed");
        Check(test::CheckRollbackPolicy(&FakeOpen, true), "Possibly exposed originals survive rollback");
        const fs::path fixture = FixtureDirectory();
        g_gameRoot = fixture / L"game";
        fs::create_directories(g_gameRoot);
        g_modsPrefix = Normalize((g_gameRoot / L"mods").string()) + '\\';
        WriteFile(g_gameRoot / L"STUFF/TEXT/ENGLISH.TXT", kOriginalBytes);
        WriteFile(g_gameRoot / L"mods/STUFF/TEXT/ENGLISH.TXT", kReplacement);
        WriteFile(g_gameRoot / L"mods/empty.bin", "");
        WriteFile(g_gameRoot / L"mods/new_asset.bin", kNestedReplacement);
        WriteFile(g_gameRoot / L"mods/textures/asset.dds", std::string("\0\1\2\3", 4));
        WriteFile(g_gameRoot / L"mods/audio/test.wav", "fake WAVE bytes for native-parser fixture");
        // These sentinels make traversal/absolute rejection substantive: an
        // unsafe resolver could otherwise find real files at these locations.
        WriteFile(g_gameRoot / L"outside.txt", "outside mods boundary");
        WriteFile(g_gameRoot / L"mods/TEXT/ENGLISH.TXT", "reachable only if dot-dot were normalized");
        WriteFile(fixture / L"outside.bin", "outside configured game boundary");
        Check(!test::Configure(g_gameRoot, {}), "Incomplete native targets are rejected");
        {
            HookFixture hooks;
            hooks.Configure();
            TestResolution();
            TestReadLease();
            TestOpen();
            TestMetadata();
            TestWholeFile();
            TestWave();
            TestFallbackExceptions();
        }
        ClearCalls();
        Check(EnterOpen(kAsset, 0, nullptr, 1) == kOriginalOpen, "Removing fixture hooks restores original functions");
        test::ResolvedPath resolved;
        Check(!test::Resolve(kAsset, resolved), "Reset removes resolver configuration");
        std::cout << "PASS: " << g_checks << " checks; only our five fixture functions were patched.\n";
        std::wcout << L"Fixture files retained for review: " << fixture.wstring() << L"\n";
        return 0;
    } catch (const std::exception& error) {
        test::Reset();
        std::cerr << "FAIL after " << g_checks << " checks: " << error.what() << "\n";
        return 1;
    }
}
