#include "render/native_rear_hud.h"
#include "render/native_hud.h"
#include "render/native_viewport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bone_eater::render {
namespace {

using LoadLayout = void(__fastcall*)(void*);
LoadLayout originalLoad = nullptr;
std::uintptr_t moduleBase = 0;
thread_local bool inHook = false;
constexpr std::uintptr_t kLayoutVtable = 0x10C9FA8;
constexpr std::uintptr_t kRootVtable = 0x10CEB48; // GuiRect, including asset Null roots.
constexpr std::size_t kMaxRoots = 11; // Native constructor requires count < 12.

template<typename T>
bool read(std::uintptr_t owner, std::size_t offset, T& value) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
            owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &value, sizeof(T), &copied) && copied == sizeof(T);
}

template<typename T>
bool write(std::uintptr_t owner, std::size_t offset, const T& value) noexcept {
    SIZE_T copied = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(owner + offset),
        &value, sizeof(T), &copied) && copied == sizeof(T);
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T result {};
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

bool same(float first, float second) noexcept {
    return std::isfinite(first) && std::isfinite(second) && std::fabs(first - second) <= 0.001f;
}

struct Layout {
    std::uintptr_t object = 0;
    std::uint32_t state = 0, substate = 0, count = 0, display = 0;
    std::array<char, 32> name {};
    std::array<std::uintptr_t, kMaxRoots> roots {};
    std::array<std::uint32_t, kMaxRoots> groups {};
};

bool layout(std::uintptr_t object, Layout& result) noexcept {
    std::uintptr_t vtable = 0;
    if (!read(object, 0, vtable) || vtable != moduleBase + kLayoutVtable ||
            !read(object, 0x18, result.state) || !read(object, 0x1C, result.substate) ||
            !read(object, 0x3C90, result.display) || !read(object, 0x3C98, result.count) ||
            result.display != 0 || result.count == 0 || result.count > kMaxRoots ||
            !read(object, 0x368, result.name) ||
            !std::memchr(result.name.data(), 0, result.name.size())) return false;
    result.object = object;
    return read(object, 0x428, result.roots) && read(object, 0x3F8, result.groups);
}

bool sameLayout(const Layout& first, const Layout& second) noexcept {
    if (first.object != second.object || first.state != second.state ||
            first.substate != second.substate || first.count != second.count ||
            first.display != second.display || first.name != second.name) return false;
    for (std::size_t index = 0; index < first.count; ++index) {
        if (first.roots[index] != second.roots[index] || first.groups[index] != second.groups[index]) return false;
    }
    return true;
}

struct Root {
    std::uintptr_t object = 0;
    std::array<unsigned char, 0xC0> before {};
    std::array<float, 2> oldPosition {}, oldScale {};
    std::array<float, 2> newPosition {}, newScale {};
};

bool prepareRoot(const Layout& owner, std::size_t index, const NativeHudGeometry& front, Root& root) noexcept {
    root.object = owner.roots[index];
    if (!read(root.object, 0, root.before) ||
            field<std::uintptr_t>(root.before, 0) != moduleBase + kRootVtable ||
            field<std::uintptr_t>(root.before, 8) != root.object ||
            field<std::uintptr_t>(root.before, 0x10) != 0 ||
            field<std::uint32_t>(root.before, 0x3C) != owner.groups[index] ||
            std::memcmp(root.before.data() + 0x40, owner.name.data(), owner.name.size())) return false;

    root.oldPosition = {{field<float>(root.before, 0x60), field<float>(root.before, 0x64)}};
    root.oldScale = {{field<float>(root.before, 0xA0), field<float>(root.before, 0xA4)}};
    // Native -2Display centers rear 800x1280 UI in a 768-wide canvas, while its
    // Y centering incorrectly follows the now-wide main resource height.
    // Only accept that exact unmodified source geometry at the load boundary.
    const float originalX = (static_cast<float>(front.sourceWidth) - 800.0f) * 0.5f;
    const float originalY = (static_cast<float>(front.mainHeight) - 1280.0f) * 0.5f;
    if (!same(root.oldPosition[0], originalX) || !same(root.oldPosition[1], originalY) ||
            !same(root.oldScale[0], 1.0f) || !same(root.oldScale[1], 1.0f) ||
            !same(field<float>(root.before, 0x90), 0.0f) ||
            !same(field<float>(root.before, 0x94), 0.0f) ||
            !same(field<float>(root.before, 0xB0), 0.0f)) return false;
    const float width = field<float>(root.before, 0x98), height = field<float>(root.before, 0x9C);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
            width > 4096 || height > 4096) return false;

    const float scale = front.width / front.sourceWidth;
    if (!std::isfinite(scale) || scale <= 0 || scale > 2 ||
            !same(scale, front.height / front.sourceHeight)) return false;
    root.newPosition = {{front.left + originalX * scale,
        front.top + (static_cast<float>(front.sourceHeight) - 1280.0f) * 0.5f * scale}};
    root.newScale = {{scale, scale}};
    return std::isfinite(root.newPosition[0]) && std::isfinite(root.newPosition[1]);
}

bool rollback(const std::array<Root, kMaxRoots>& roots, std::size_t count) noexcept {
    bool restored = true;
    while (count) {
        const Root& root = roots[--count];
        // These are still the synchronously validated load-owned objects. No
        // pointer survives the callback for later rollback or scene tracking.
        const bool position = write(root.object, 0x60, root.oldPosition);
        const bool scale = write(root.object, 0xA0, root.oldScale);
        restored = restored && position && scale;
    }
    return restored;
}

void fit(const Layout& before) {
    Layout after;
    if (!layout(before.object, after) || after.state != 2 || after.substate != 1 ||
            after.count != before.count || after.name != before.name) return;
    const NativeHudGeometry front = readNativeHudGeometry();
    if (!front.valid || front.mainWidth != 1920 || front.mainHeight != 1080 ||
            front.sourceWidth != 768 || front.sourceHeight != 1366) {
        log_warning("bone-eater", "Rear UI fit skipped for '{}': front fit is not committed", after.name.data());
        return;
    }

    std::array<Root, kMaxRoots> roots {};
    for (std::size_t index = 0; index < after.count; ++index) {
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (after.roots[index] == after.roots[prior]) return;
        }
        if (!prepareRoot(after, index, front, roots[index])) {
            log_warning("bone-eater", "Rear UI fit skipped for '{}': root {} identity/source geometry differs",
                after.name.data(), index);
            return;
        }
    }

    Layout repeated;
    if (!layout(after.object, repeated) || !sameLayout(after, repeated)) return;
    const NativeHudGeometry frontAgain = readNativeHudGeometry();
    if (!frontAgain.valid || frontAgain.helper != front.helper || frontAgain.sprite != front.sprite ||
            frontAgain.left != front.left || frontAgain.top != front.top ||
            frontAgain.width != front.width || frontAgain.height != front.height) return;
    for (std::size_t index = 0; index < after.count; ++index) {
        std::array<unsigned char, 0xC0> repeatedRoot {};
        if (!read(roots[index].object, 0, repeatedRoot) || repeatedRoot != roots[index].before) return;
    }

    for (std::size_t index = 0; index < after.count; ++index) {
        if (!write(roots[index].object, 0x60, roots[index].newPosition) ||
                !write(roots[index].object, 0xA0, roots[index].newScale)) {
            log_warning("bone-eater", "Rear UI fit write failed for '{}'; rollback={}",
                after.name.data(), rollback(roots, index + 1));
            return;
        }
    }
    log_info("bone-eater", "Rear UI fit committed for '{}': {} roots, position {},{}, scale {}",
        after.name.data(), after.count, roots[0].newPosition[0], roots[0].newPosition[1], roots[0].newScale[0]);
}

struct ReentryGuard {
    ReentryGuard() noexcept { inHook = true; }
    ~ReentryGuard() { inHook = false; }
};

void __fastcall load(void* object) {
    if (inHook) { originalLoad(object); return; }
    ReentryGuard guard;
    Layout before;
    const bool eligible = layout(reinterpret_cast<std::uintptr_t>(object), before) &&
        before.state == 1 && before.substate == 1;
    originalLoad(object);
    if (eligible) {
        try { fit(before); }
        catch (...) { /* Diagnostics must not replace a successful native load. */ }
    }
}

} // namespace

void installNativeRearHudFit(void* module) noexcept {
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_REAR_HUD", setting, 8) != 3 ||
                std::wcscmp(setting, L"fit") || originalLoad) return;
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native rear UI fit disabled: {}", status);
            return;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 40> expected {{
            0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x54,0x41,0x56,
            0x41,0x57,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x48,0x8b,
            0x05,0xe3,0x4e,0x1b,0x01,0x48,0x33,0xc4
        }};
        constexpr std::array<unsigned char, 77> centering {{
            0x41,0x83,0xbe,0x90,0x3c,0x00,0x00,0x00,0x75,0x43,0x48,0x83,0x3d,0x17,0x02,0x2d,
            0x01,0x00,0x74,0x05,0x0f,0x28,0xc7,0xeb,0x04,0x41,0x0f,0x28,0xc0,0xf3,0x0f,0x2c,
            0xc0,0x2d,0x20,0x03,0x00,0x00,0x66,0x0f,0x6e,0xc0,0x48,0x8b,0x05,0x48,0x98,0x1d,
            0x01,0x0f,0xb7,0x48,0x2a,0x81,0xe9,0x00,0x05,0x00,0x00,0x0f,0x5b,0xc0,0x66,0x0f,
            0x6e,0xc9,0xf3,0x0f,0x59,0xc6,0x0f,0x5b,0xc9,0xf3,0x0f,0x59,0xce
        }};
        std::array<unsigned char, 40> found {};
        std::array<unsigned char, 77> centeringFound {};
        if (!read(moduleBase, 0x1081C0, found) || found != expected ||
                !read(moduleBase, 0x108C6F, centeringFound) || centeringFound != centering) {
            log_warning("bone-eater", "Native rear UI fit disabled: instruction bytes differ");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        originalLoad = reinterpret_cast<LoadLayout>(moduleBase + 0x1081C0);
        if (!detour::trampoline_try(originalLoad, &load, &originalLoad)) {
            log_warning("bone-eater", "Native rear UI fit hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native rear LayoutedUI fit enabled; requires committed front fit");
    } catch (...) {}
}

} // namespace bone_eater::render
