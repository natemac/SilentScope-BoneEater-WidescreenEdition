#pragma once

#include "render/native_hud.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace bone_eater::input {

struct WideInputPoint {
    bool valid = false;
    float x = 0, y = 0;
};

enum class WideInputMapping { CabinetMargins, Desktop };

inline bool validWideInputGeometry(const render::NativeHudGeometry& g) noexcept {
    return g.valid && g.mainWidth == 1920 && g.mainHeight == 1080 &&
        g.sourceWidth == 768 && g.sourceHeight == 1366 &&
        std::isfinite(g.left) && std::isfinite(g.top) &&
        std::isfinite(g.width) && std::isfinite(g.height) &&
        g.left >= 0 && g.top >= 0 && g.width > 0 && g.height > 0 &&
        g.left + g.width <= g.mainWidth + 0.01f &&
        g.top + g.height <= g.mainHeight + 0.01f;
}

// Affine domain conversion preserves ARK's calibrated safe bounds; it does not
// reinterpret device calibration or remove the original cabinet margins.
inline WideInputPoint cabinetToWide(float x, float y, std::int32_t width,
        std::int32_t height, const render::NativeHudGeometry& g) noexcept {
    if (!validWideInputGeometry(g) || width != 800 || height != 1280 ||
            !std::isfinite(x) || !std::isfinite(y) ||
            x < 0 || y < 0 || x > width || y > height) return {};
    return {true, x * (g.mainWidth / 800.0f), y * (g.mainHeight / 1280.0f)};
}

// Invert only the verified hardware ARK viewport margins, AFTER its device
// calibration. A6E8..A734 gives X=32+736*u, Y=132+896*v at 800x1280.
// These accepted coordinates already include native X inversion; do not invert
// again or substitute raw12bit values. Tiny float edge error may clamp, but a
// different/malformed coordinate contract must be rejected.
inline WideInputPoint cabinetToDesktop(float x, float y, std::int32_t width,
        std::int32_t height, const render::NativeHudGeometry& g) noexcept {
    constexpr float tolerance = 0.001f;
    if (!validWideInputGeometry(g) || width != 800 || height != 1280 ||
            !std::isfinite(x) || !std::isfinite(y) ||
            x < 32.0f - tolerance || x > 768.0f + tolerance ||
            y < 132.0f - tolerance || y > 1028.0f + tolerance) return {};
    const float u = (x - 32.0f) / 736.0f;
    const float v = (y - 132.0f) / 896.0f;
    return {true, (u < 0 ? 0 : u > 1 ? 1 : u) * g.mainWidth,
        (v < 0 ? 0 : v > 1 ? 1 : v) * g.mainHeight};
}

// Outside points deliberately remain outside. Clamping into the visible menu
// would allow a click in a side margin to activate an edge control.
inline WideInputPoint wideToHud(float x, float y,
        const render::NativeHudGeometry& g) noexcept {
    if (!validWideInputGeometry(g) || !std::isfinite(x) || !std::isfinite(y)) return {};
    const float mappedX = (x - g.left) / g.width * g.sourceWidth;
    const float mappedY = (y - g.top) / g.height * g.sourceHeight;
    if (!std::isfinite(mappedX) || !std::isfinite(mappedY)) return {};
    return {true, mappedX, mappedY};
}

// The live B85 interactive layout retains portrait hit polygons even though
// the BA0 backdrop uses main-screen coordinates. Invert the presentation for
// the UI pair only; the main aim pair and desktop reticle stay widescreen.
inline WideInputPoint wideToLobbyUi(float x, float y,
        const render::NativeHudGeometry& g) noexcept {
    return wideToHud(x, y, g);
}

// The name-entry gun getter call sites subsequently multiply by 768/800 and
// 1366/1280. Return their pre-scaled values, not LCD pixels twice transformed.
inline WideInputPoint wideToWidgetGetter(float x, float y,
        const render::NativeHudGeometry& g) noexcept {
    const auto lcd = wideToHud(x, y, g);
    return lcd.valid ? WideInputPoint{true, lcd.x * (800.0f / 768.0f),
        lcd.y * (1280.0f / 1366.0f)} : WideInputPoint{};
}

inline bool rewriteWideArkOutput(std::array<std::uint8_t, 52>& output,
        std::int32_t width, std::int32_t height,
        const render::NativeHudGeometry& geometry,
        WideInputMapping mapping = WideInputMapping::CabinetMargins) noexcept {
    float x = 0, y = 0;
    std::int32_t rawX = 0, rawY = 0;
    std::memcpy(&x, output.data() + 0x24, sizeof(x));
    std::memcpy(&y, output.data() + 0x28, sizeof(y));
    std::memcpy(&rawX, output.data() + 0x2C, sizeof(rawX));
    std::memcpy(&rawY, output.data() + 0x30, sizeof(rawY));
    if (rawX < 0 || rawX > 4095 || rawY < 0 || rawY > 4095) return false;
    const auto wide = mapping == WideInputMapping::Desktop ?
        cabinetToDesktop(x, y, width, height, geometry) :
        cabinetToWide(x, y, width, height, geometry);
    if (!wide.valid) return false;
    std::memcpy(output.data() + 0x24, &wide.x, sizeof(wide.x));
    std::memcpy(output.data() + 0x28, &wide.y, sizeof(wide.y));
    return true;
}

// Opt-in BONE_EATER_NATIVE_INPUT=wide (preserved margins) or desktop (full unit
// domain). Install before game entry, after the
// native observer's exact game/ARK verification. This prototype additionally
// requires the committed native HUD fit and the native gun-to-UI input mode.
// Other hardcoded gameplay consumers still require separate integration.
void installNativeWideInput(void* gamendd) noexcept;

struct DesktopInputSnapshot {
    bool valid = false, gameplay = false;
    float x = 0, y = 0;
    std::uint64_t updatedAtMs = 0;
};
DesktopInputSnapshot readDesktopInputSnapshot() noexcept;
// Exact native Start-trigger caller only: route Enter away from Start during
// certified gameplay, leaving menu Start and selected-device inputs intact.
bool scopeConsumesStart() noexcept;

// Called ONLY at the verified ARK observer call site, after publishing the
// unchanged raw result. Writes only the two logical floats in that caller-local
// output. No-op outside the validated game input-update invocation.
void convertNativeWideInput(void* output, const std::array<std::uint8_t, 52>& original,
    std::int32_t arkWidth, std::int32_t arkHeight) noexcept;

} // namespace bone_eater::input
