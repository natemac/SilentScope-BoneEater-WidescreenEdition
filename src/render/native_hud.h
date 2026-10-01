#pragma once

#include <cstdint>

namespace bone_eater::render {

struct NativeHudGeometry {
    bool valid = false;
    const char* status = "disabled";
    std::uint32_t mainWidth = 0, mainHeight = 0;
    std::uint32_t sourceWidth = 768, sourceHeight = 1366;
    float left = 0, top = 0, width = 0, height = 0;
    std::uintptr_t helper = 0, sprite = 0;
};

// BONE_EATER_NATIVE_HUD=fit enables an exact-build, startup-only geometry
// adaptation of the game's existing -2Display front-LCD sprite. Install before
// game entry. Requires the separately enabled native -2Display and 1920x1080
// main resources. It leaves native blend/source/animation behavior intact.
void installNativeHudFit(void* module) noexcept;

// The rectangle is in native MAIN LOGICAL pixels, not HWND/backbuffer pixels.
// Full LCD source UV [0,1]^2 maps onto it without additional one-pixel overdraw.
// The initial policy fits the entire front layer in every state; it does not
// identify menus, reflow widgets, or distinguish gameplay from cinematic UI.
// Publication occurs only after the native CSprite has committed the fit.
// Reads revalidate native identities/geometry; unresolved/changed state is invalid.
NativeHudGeometry readNativeHudGeometry() noexcept;

} // namespace bone_eater::render
