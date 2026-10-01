#include "render/native_display.h"
#include "render/native_viewport.h"
#include "util/detour.h"
#include "util/logging.h"
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace bone_eater::render {
namespace {
using ConfigureMain = void(__fastcall*)(std::uint32_t, std::uint32_t);
ConfigureMain originalConfigure = nullptr;
std::uintptr_t gameBase = 0;
std::atomic<bool> configured {false};
bool useWide = false;
std::atomic<float> originalAspect {0.0f};

// Only the measured main-display startup call is changed. Other calls, including
// any future unexpected runtime resize, pass through with their original values.
__declspec(noinline) void __fastcall configureMain(std::uint32_t width, std::uint32_t height) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (caller == gameBase + 0x19400D && width == 800 &&
            (height == 1280 || height == 1366) && !configured.exchange(true)) {
        log_info("bone-eater", "Native main setup observed: {}x{}, caller RVA 0x19400D; wide={}",
            width, height, useWide);
        if (useWide) {
            originalAspect.store(static_cast<float>(width) / height);
            width = 1920;
            height = 1080;
        }
    }
    originalConfigure(width, height);
}

template<std::size_t N>
bool matches(std::uintptr_t at, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> found {};
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(at),
        found.data(), found.size(), &read) && read == found.size() && found == expected;
}
}

void installNativeDisplayExperiment(void* module) noexcept {
    try {
        wchar_t setting[16] {};
        const auto count = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_DISPLAY", setting, 16);
        if (!count || count >= 16 || (wcscmp(setting, L"wide") && wcscmp(setting, L"observe"))) return;
        if (originalConfigure) return;
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native display experiment disabled: {}", status);
            return;
        }
        gameBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> prologue {{
            0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x70,0x48,0x8b,0x05,0x77,0x7b,0x12,
            0x01,0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x60,0x8b,0xd9,0x33,0xc9,0x8b,0xfa,0xe8
        }};
        constexpr std::array<unsigned char, 5> call {{0xe8,0x33,0x15,0x00,0x00}};
        if (!matches(gameBase + 0x195540, prologue) || !matches(gameBase + 0x194008, call)) {
            log_warning("bone-eater", "Native display experiment disabled: instruction bytes differ");
            return;
        }
        useWide = wcscmp(setting, L"wide") == 0;
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        // Upstream trampoline_try captures *orig as the MH_EnableHook target.
        // Seed it explicitly; nullptr would enable every pending MinHook hook.
        originalConfigure = reinterpret_cast<ConfigureMain>(gameBase + 0x195540);
        if (!detour::trampoline_try(originalConfigure,
                &configureMain, &originalConfigure)) {
            log_warning("bone-eater", "Native display experiment hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native display experiment installed; mode={}", useWide ? "wide" : "observe");
    } catch (...) {
        // Failure to enable an opt-in experiment must not prevent baseline boot.
    }
}
float originalNativeMainAspect() noexcept { return originalAspect.load(); }
}
