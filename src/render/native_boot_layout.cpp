#include "render/native_boot_layout.h"
#include "render/boot_layout_policy.h"
#include "render/native_display.h"
#include "render/native_viewport.h"
#include "render/checked_data_write.h"
#include "util/detour.h"
#include "util/logging.h"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <vector>
#include <intrin.h>

namespace bone_eater::render {
namespace {
using DiagnosisDraw = std::int32_t(__fastcall*)(void*);
using FontDraw = void(__fastcall*)(void*, const char*, float, float, const void*);
using CanvasCreate = void*(__fastcall*)(void*, unsigned, unsigned);
CanvasCreate originalCreate = nullptr;
using CanvasDraw = void(__fastcall*)(void*);
CanvasDraw originalCanvas = nullptr;
std::atomic<bool> wideCanvas {false};
DiagnosisDraw originalDiagnosis = nullptr;
FontDraw originalFont = nullptr, originalCenteredFont = nullptr;
std::uintptr_t gameBase = 0, arkBase = 0;
bool center = false, attempted = false;
std::atomic<bool> ready {false};
std::atomic<unsigned> reports {0};
std::atomic<bool> reportedEligible {false};
thread_local BootFontContext context {};

template<class T> bool read(std::uintptr_t address, std::size_t offset, T& value) noexcept {
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - offset ||
        address + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + offset),
        &value, sizeof(value), &count) && count == sizeof(value);
}

bool verifyArk(HMODULE module) {
    if (!module || module != GetModuleHandleW(L"arkndd.dll")) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 pe {};
    if (!read(base, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        dos.e_lfanew > 0x100000 || !read(base, dos.e_lfanew, pe) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.OptionalHeader.SizeOfImage != 0x3f7000) return false;
    std::array<wchar_t, 32768> path {};
    auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    struct File { HANDLE value = INVALID_HANDLE_VALUE;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file;
    file.value = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size {};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size) || size.QuadPart != 1681040) return false;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD count = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size()) return false;
    struct Algorithm { BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    std::array<unsigned char, 32> digest {};
    if (BCryptHash(algorithm.value, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size())) < 0) return false;
    constexpr std::array<unsigned char, 32> expected {{
        0xf8,0xda,0x7a,0xdd,0x77,0x6f,0xeb,0x9d,0xde,0x20,0x72,0x4d,0xdd,0x80,0x11,0x2a,
        0x63,0x15,0xa0,0x7d,0x98,0x56,0x87,0x89,0x39,0xc0,0xf7,0xa5,0x2f,0xb9,0x3e,0x98}};
    return digest == expected;
}

bool ownerContext(std::uintptr_t owner, BootFontContext& result) noexcept {
    std::uintptr_t vtable = 0, draw = 0, centered = 0, measure = 0;
    return read(owner, 0, vtable) && vtable == arkBase + 0xde4b8 &&
        read(owner, 0x28, result.titleFont) && result.titleFont &&
        read(owner, 0x30, result.rowFont) && result.rowFont &&
        read(owner, 0x80, draw) && draw == gameBase + 0x5590 &&
        read(owner, 0x88, centered) && centered == gameBase + 0x55c0 &&
        read(owner, 0x90, measure) && measure == gameBase + 0x55f0;
}

bool sameOwner(const BootFontContext& expected) noexcept {
    BootFontContext actual;
    return ownerContext(expected.owner, actual) && actual.titleFont == expected.titleFont &&
        actual.rowFont == expected.rowFont;
}

NativeViewportSnapshot mainViewport() noexcept {
    std::uintptr_t windowObject = 0;
    HWND window = nullptr;
    if (!read(gameBase, 0x12e17a0, windowObject) || !read(windowObject, 0x28, window)) return {};
    return readNativeMainViewport(window);
}

// Allocate the native font surfaces at the wide target width before any glyphs
// are rasterized. Moving an already clipped 800-pixel surface loses text.
void* createCanvas(void* object, unsigned width, unsigned height) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool eligible = ready.load(std::memory_order_acquire) && center &&
        originalNativeMainAspect() > 0 && width == 800 && height == 1280 &&
        (caller == gameBase + 0x41B8 || caller == gameBase + 0x42C2);
    void* result = originalCreate(object, eligible ? 1920u : width, height);
    if (eligible && caller == gameBase + 0x42C2) {
        std::array<unsigned,2> dimensions {};
        wideCanvas.store(result == object && read(reinterpret_cast<std::uintptr_t>(result),
            0x50, dimensions) && dimensions == std::array<unsigned,2>{1920,1280}, std::memory_order_release);
        log_info("bone-eater", "Boot font canvas created: wide={}", wideCanvas.load());
    }
    return result;
}
void font(void* owner, const char* text, float x, float y, const void* color) {
    originalFont(owner, text, translatedBootX(context, reinterpret_cast<std::uintptr_t>(owner), x, y), y, color);
}
void centeredFont(void* owner, const char* text, float x, float y, const void* color) {
    originalCenteredFont(owner, text, translatedBootX(context, reinterpret_cast<std::uintptr_t>(owner), x, y), y, color);
}

void canvas(void* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    std::array<std::uintptr_t,4> surfaces {};
    std::array<unsigned,2> dimensions {};
    std::array<float,8> uv {}, fitted {};
    std::uintptr_t sprite=0, vt=0;
    checked_data_detail::Region region;
    bool owned = false;
    if (wideCanvas.load(std::memory_order_acquire) && read(gameBase,0x13D8E10,surfaces))
        for (auto surface : surfaces) owned = owned || surface == address;
    if (!owned || !read(address,0x50,dimensions) || dimensions != std::array<unsigned,2>{1920,1280} ||
        !read(address,0x20,uv) || !fittedBootCanvasUv(dimensions,uv,fitted) ||
        !read(address,0x18,sprite) || !read(sprite,0,vt) || vt != gameBase+0x10CC0D8 ||
        !checked_data_detail::queryPrivateWritable(address+0x30,region) || !region.contains(address+0x3B)) {
        originalCanvas(object); return;
    }
    // The native font initializer hard-codes 800/1024 even for a wider surface.
    // Its verified 1920-pixel allocation uses a 2048-pixel padded texture.
    __try {
        *reinterpret_cast<float*>(address+0x30)=fitted[4];
        *reinterpret_cast<float*>(address+0x38)=fitted[6];
        originalCanvas(object);
    } __finally {
        *reinterpret_cast<float*>(address+0x30)=uv[4];
        *reinterpret_cast<float*>(address+0x38)=uv[6];
    }
}

// The native call can unwind through Windows SEH as well as C++ exceptions.
// This POD-only boundary restores TLS in both cases without requiring /EHa.
std::int32_t drawWithContext(void* owner, const BootFontContext& next) {
    const BootFontContext previous = context;
    context = next;
    __try { return originalDiagnosis(owner); }
    __finally { context = previous; }
}

std::int32_t diagnosis(void* owner) {
    BootFontContext next;
    NativeViewportSnapshot viewport;
    bool eligible = ready.load(std::memory_order_acquire) &&
        ownerContext(reinterpret_cast<std::uintptr_t>(owner), next);
    if (eligible) {
        viewport = mainViewport();
        const bool wide = viewport.valid && originalNativeMainAspect() > 0 &&
            viewport.resourceWidth == 1920 && viewport.resourceHeight == 1080;
        next.offset = bootHorizontalOffset(wide, viewport.logicalWidth, viewport.logicalHeight);
        eligible = next.offset != 0;
    }
    const float measuredOffset = eligible ? next.offset : 0;
    if (eligible && center && wideCanvas.load(std::memory_order_acquire)) next.owner = reinterpret_cast<std::uintptr_t>(owner);
    else next = {};
    // Native renderer owns both font handles for this synchronous call. Custom
    // checklist rows receive copies of the same callback table (ark 0x878C0),
    // so TLS + exact font ownership covers them without changing that table.
    // No persistent geometry, callbacks, glyph sizes, colors, or metrics change.
    const bool firstEligible = eligible && !reportedEligible.exchange(true, std::memory_order_relaxed);
    if (reports.fetch_add(1, std::memory_order_relaxed) < 4 || firstEligible) {
        try { log_info("bone-eater", "Boot layout mode={} eligible={} owner=0x{:X} logical={}x{} resource={}x{} viewport={},{},{},{} x-offset={}",
            center ? "center" : "observe", eligible, reinterpret_cast<std::uintptr_t>(owner),
            viewport.logicalWidth, viewport.logicalHeight, viewport.resourceWidth, viewport.resourceHeight,
            viewport.left, viewport.top, viewport.right, viewport.bottom, measuredOffset); } catch (...) {}
    }
    return drawWithContext(owner, next);
}
}

void installNativeBootLayout(void* gameModule, void* arkModule) noexcept {
    try {
        wchar_t setting[16] {};
        auto count = GetEnvironmentVariableW(L"BONE_EATER_BOOT_CENTER", setting, 16);
        if (count >= 16 || (count && std::wcscmp(setting, L"1") && std::wcscmp(setting, L"observe")) || attempted) return;
        attempted = true;
        if (std::strcmp(verifyNativeGameModule(gameModule), "verified") || !verifyArk(static_cast<HMODULE>(arkModule))) {
            log_warning("bone-eater", "Boot layout disabled: native module verification failed"); return;
        }
        gameBase = reinterpret_cast<std::uintptr_t>(gameModule);
        arkBase = reinterpret_cast<std::uintptr_t>(arkModule);
        constexpr std::array<unsigned char, 32> drawBytes {{
            0x40,0x53,0x48,0x83,0xec,0x70,0x48,0x8b,0x05,0x33,0x07,0x09,0x00,0x48,0x33,0xc4,
            0x48,0x89,0x44,0x24,0x58,0x48,0x83,0x79,0x78,0x00,0x48,0x8b,0xd9,0x0f,0x84,0x5c}};
        constexpr std::array<unsigned char, 32> fontBytes {{
            0x48,0x83,0xec,0x48,0x48,0x85,0xc9,0x74,0x1f,0x48,0x8b,0x44,0x24,0x70,0xc7,0x44,
            0x24,0x30,0x03,0x00,0x00,0x00,0xc7,0x44,0x24,0x28,0x00,0x00,0x00,0x00,0x48,0x89}};
        constexpr std::array<unsigned char, 32> centeredBytes {{
            0x48,0x83,0xec,0x48,0x48,0x85,0xc9,0x74,0x1f,0x48,0x8b,0x44,0x24,0x70,0xc7,0x44,
            0x24,0x30,0x01,0x00,0x00,0x00,0xc7,0x44,0x24,0x28,0x01,0x00,0x00,0x00,0x48,0x89}};
        constexpr std::array<unsigned char,16> canvasBytes {{
            0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57}};
        std::array<unsigned char,16> foundCanvas {};
        constexpr std::array<unsigned char,16> drawCanvasBytes {{
            0x48,0x89,0x5c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xf0,0x00}};
        std::array<unsigned char,16> foundDrawCanvas {};
        std::array<unsigned char, 32> found {};
        if (!read(arkBase, 0x87010, found) || found != drawBytes ||
            !read(gameBase, 0x5590, found) || found != fontBytes ||
            !read(gameBase, 0x55c0, found) || found != centeredBytes ||
            !read(gameBase, 0x1ABEA0, foundCanvas) || foundCanvas != canvasBytes ||
            !read(gameBase, 0x1AC5B0, foundDrawCanvas) || foundDrawCanvas != drawCanvasBytes) {
            log_warning("bone-eater", "Boot layout disabled: hook instructions differ"); return;
        }
        HMODULE pinnedGame = nullptr, pinnedArk = nullptr;
        constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(gameModule), &pinnedGame) || pinnedGame != gameModule ||
            !GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(arkModule), &pinnedArk) || pinnedArk != arkModule) return;
        center = !count || std::wcscmp(setting, L"1") == 0;
        originalFont = reinterpret_cast<FontDraw>(gameBase + 0x5590);
        originalCenteredFont = reinterpret_cast<FontDraw>(gameBase + 0x55c0);
        originalDiagnosis = reinterpret_cast<DiagnosisDraw>(arkBase + 0x87010);
        originalCreate = reinterpret_cast<CanvasCreate>(gameBase + 0x1ABEA0);
        originalCanvas = reinterpret_cast<CanvasDraw>(gameBase + 0x1AC5B0);
        // All partial installations remain pass-through until every hook succeeds.
        if (!detour::trampoline_try(originalFont, &font, &originalFont) ||
            !detour::trampoline_try(originalCenteredFont, &centeredFont, &originalCenteredFont) ||
            !detour::trampoline_try(originalDiagnosis, &diagnosis, &originalDiagnosis) ||
            !detour::trampoline_try(originalCreate, &createCanvas, &originalCreate) ||
            !detour::trampoline_try(originalCanvas, &canvas, &originalCanvas)) {
            log_warning("bone-eater", "Boot layout disabled: hook installation failed"); return;
        }
        ready.store(true, std::memory_order_release);
        log_info("bone-eater", "Boot layout installed: mode={}; wide font surfaces and owned diagnosis glyph translation", center ? "center" : "observe");
    } catch (...) {}
}
}
