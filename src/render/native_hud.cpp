#include "render/native_hud.h"
#include "render/native_viewport.h"
#include "render/native_front_observer.h"
#include "render/native_boot_layout.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>

namespace bone_eater::render {
namespace {

using UpdateHelper = void(__fastcall*)(void*);
UpdateHelper originalUpdate = nullptr;
std::uintptr_t moduleBase = 0;
constexpr std::uintptr_t kHelperGlobal = 0x13D8E98;
constexpr std::uintptr_t kHelperVtable = 0x10C07A8;
constexpr std::uintptr_t kSpriteVtable = 0x10CD1E8;
constexpr std::uintptr_t kFramebufferGlobal = 0x12E24E8;
constexpr std::uintptr_t kFramebufferVtable = 0x703E78;

struct SharedState {
    std::mutex mutex;
    NativeHudGeometry published;
    std::uintptr_t pendingHelper = 0;
    std::array<float, 4> pendingInputs {};
    std::atomic<bool> warningWritten {false};
};

// The hook stays installed until process exit, including static teardown.
SharedState* shared = nullptr;
thread_local bool inHook = false;

template<typename T>
bool read(std::uintptr_t owner, std::size_t offset, T& value) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
            owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &value, sizeof(T), &copied) && copied == sizeof(T);
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T result {};
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

bool framebuffer(std::uintptr_t& object, std::uint32_t& width, std::uint32_t& height) noexcept {
    std::array<unsigned char, 0x3C> bytes {};
    if (!read(moduleBase, kFramebufferGlobal, object) || !read(object, 0, bytes) ||
            field<std::uintptr_t>(bytes, 0) != moduleBase + kFramebufferVtable) return false;
    width = field<std::uint16_t>(bytes, 0x28);
    height = field<std::uint16_t>(bytes, 0x2A);
    // This prototype is intentionally limited to the measured native-wide mode.
    return width == 1920 && height == 1080 &&
        field<std::uint16_t>(bytes, 0x38) == width && field<std::uint16_t>(bytes, 0x3A) == height;
}

bool helperBytes(std::uintptr_t helper, std::array<unsigned char, 0x50>& bytes) noexcept {
    std::uintptr_t current = 0;
    return read(moduleBase, kHelperGlobal, current) && current == helper &&
        read(helper, 0, bytes) && field<std::uintptr_t>(bytes, 0) == moduleBase + kHelperVtable &&
        field<std::uint32_t>(bytes, 0x30) == 34;
}

struct Preparation {
    bool fit = false;
    NativeHudGeometry geometry;
};

Preparation prepare(std::uintptr_t helper) {
    Preparation result;
    std::array<unsigned char, 0x50> bytes {};
    if (!helperBytes(helper, bytes)) return result;
    if (field<std::uint32_t>(bytes, 0x48) != 0 || field<std::uintptr_t>(bytes, 0x28)) return result;
    std::uintptr_t frame = 0;
    std::uint32_t width = 0, height = 0;
    if (!framebuffer(frame, width, height)) return result;

    const float scale = std::min(static_cast<float>(width) / 768.0f, static_cast<float>(height) / 1366.0f);
    const float fitWidth = 768.0f * scale;
    const float fitHeight = 1366.0f * scale;
    const float centerX = width * 0.5f, centerY = height * 0.5f;
    // Helper order is height,width,centerX,centerY; native code copies these into
    // the CSprite during initialization. Use full texture aspect, without +1.
    const std::array<float, 4> fitted {{fitHeight, fitWidth, centerX, centerY}};
    constexpr std::array<float, 4> original {{1367.0f, 769.0f, 384.0f, 683.0f}};
    std::array<float, 4> current {};
    std::memcpy(current.data(), bytes.data() + 0x38, sizeof(current));
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        const bool ownPending = shared->pendingHelper == helper && current == shared->pendingInputs;
        if (current != original && !ownPending) return result;
    }
    std::array<unsigned char, 0x50> repeated {};
    std::uintptr_t frameAgain = 0;
    std::uint32_t widthAgain = 0, heightAgain = 0;
    if (!helperBytes(helper, repeated) || bytes != repeated ||
            !framebuffer(frameAgain, widthAgain, heightAgain) || frameAgain != frame ||
            widthAgain != width || heightAgain != height) return result;

    if (current != fitted) {
        SIZE_T written = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(helper + 0x38),
                fitted.data(), sizeof(fitted), &written) || written != sizeof(fitted)) {
            // Restore the already validated original input block if an OS write
            // unexpectedly fails. No pointer is retained for later rollback.
            SIZE_T restored = 0;
            const BOOL rollback = WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(helper + 0x38),
                current.data(), sizeof(current), &restored);
            if (!shared->warningWritten.exchange(true)) {
                log_warning("bone-eater", "Native HUD geometry write failed; input rollback={}",
                    rollback && restored == sizeof(current));
            }
            return result;
        }
    }
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->pendingHelper = helper;
        shared->pendingInputs = fitted;
        shared->published.valid = false;
        shared->published.status = "awaiting_native_sprite";
    }
    result.fit = true;
    result.geometry.status = "awaiting_native_sprite";
    result.geometry.helper = helper;
    result.geometry.mainWidth = width;
    result.geometry.mainHeight = height;
    result.geometry.width = fitWidth;
    result.geometry.height = fitHeight;
    result.geometry.left = centerX - fitWidth * 0.5f;
    result.geometry.top = centerY - fitHeight * 0.5f;
    return result;
}

bool committed(NativeHudGeometry& geometry) noexcept {
    std::array<unsigned char, 0x50> bytes {};
    if (!helperBytes(geometry.helper, bytes) || field<std::uint32_t>(bytes, 0x48) != 1) return false;
    const auto sprite = field<std::uintptr_t>(bytes, 0x28);
    if (!sprite || (geometry.sprite && geometry.sprite != sprite)) return false;
    std::uintptr_t vtable = 0;
    std::array<float, 4> rectangle {};
    if (!read(sprite, 0, vtable) || vtable != moduleBase + kSpriteVtable ||
            !read(sprite, 0x580, rectangle)) return false;
    const std::array<float, 4> expected {{geometry.mainWidth * 0.5f, geometry.mainHeight * 0.5f,
        geometry.width, geometry.height}};
    if (rectangle != expected) return false;
    std::uintptr_t frame = 0, helperAgain = 0, spriteAgain = 0;
    std::uint32_t width = 0, height = 0;
    if (!framebuffer(frame, width, height) || width != geometry.mainWidth || height != geometry.mainHeight ||
            !read(moduleBase, kHelperGlobal, helperAgain) || helperAgain != geometry.helper ||
            !read(geometry.helper, 0x28, spriteAgain) || spriteAgain != sprite) return false;
    geometry.sprite = sprite;
    return true;
}

struct ReentryGuard {
    ReentryGuard() noexcept { inHook = true; }
    ~ReentryGuard() { inHook = false; }
};

void __fastcall update(void* helper) {
    if (inHook) { originalUpdate(helper); return; }
    ReentryGuard guard;
    Preparation preparation;
    try { preparation = prepare(reinterpret_cast<std::uintptr_t>(helper)); }
    catch (...) { /* Observer/diagnostic exceptions cannot replace the native update. */ }
    originalUpdate(helper);
    if (preparation.fit && committed(preparation.geometry)) {
        try {
            preparation.geometry.valid = true;
            preparation.geometry.status = "fitted";
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->published = preparation.geometry;
            }
            log_info("bone-eater", "Native front LCD fit committed: {},{} {}x{} within {}x{}",
                preparation.geometry.left, preparation.geometry.top, preparation.geometry.width,
                preparation.geometry.height, preparation.geometry.mainWidth, preparation.geometry.mainHeight);
        } catch (...) {}
    }
}

} // namespace

void installNativeHudFit(void* module) noexcept {
    installNativeBootLayout(module, GetModuleHandleW(L"arkndd.dll"));
    installNativeFrontObserver(module);
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", setting, 8) != 3 ||
                std::wcscmp(setting, L"fit") || originalUpdate) return;
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native HUD fit disabled: {}", status);
            return;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> expected {{
            0x40,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0xf1,0x8b,0x49,0x48,0x85,0xc9,0x74,0x23,
            0xff,0xc9,0x0f,0x85,0x95,0x01,0x00,0x00,0x48,0x8b,0x4e,0x28,0x48,0x8b,0x01,0xff
        }};
        constexpr std::array<unsigned char, 52> commitBytes {{
            0x48,0x8b,0x4e,0x28,0x8b,0x46,0x3c,0xf3,0x0f,0x10,0x46,0x38,0x89,0x81,0x88,0x05,
            0x00,0x00,0xf3,0x0f,0x11,0x81,0x8c,0x05,0x00,0x00,0x48,0x8b,0x4e,0x28,0x8b,0x46,
            0x40,0xf3,0x0f,0x10,0x4e,0x44,0x89,0x81,0x80,0x05,0x00,0x00,0xf3,0x0f,0x11,0x89,
            0x84,0x05,0x00,0x00
        }};
        std::array<unsigned char, 32> found {};
        std::array<unsigned char, 52> commitFound {};
        if (!read(moduleBase, 0x7A60, found) || found != expected ||
                !read(moduleBase, 0x7B98, commitFound) || commitFound != commitBytes) {
            log_warning("bone-eater", "Native HUD fit disabled: instruction bytes differ");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        if (!shared) shared = new SharedState;
        shared->published.status = "awaiting_native_helper";
        // Upstream uses *orig as MH_EnableHook's target; never seed it with null.
        originalUpdate = reinterpret_cast<UpdateHelper>(moduleBase + 0x7A60);
        if (!detour::trampoline_try(originalUpdate, &update, &originalUpdate)) {
            log_warning("bone-eater", "Native HUD fit hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native HUD portrait fit enabled; requires native -2Display and 1920x1080 resources");
    } catch (...) {}
}

NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry result;
    if (!shared) return result;
    try {
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            result = shared->published;
        }
        if (!result.valid) return result;
        if (!committed(result)) {
            result.valid = false;
            result.status = "native_geometry_changed_or_unavailable";
        }
    } catch (...) {
        result.valid = false;
        result.status = "geometry_read_failed";
    }
    return result;
}

} // namespace bone_eater::render
