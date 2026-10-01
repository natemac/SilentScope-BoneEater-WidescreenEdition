#pragma once

namespace bone_eater::render {

// Startup/load-time experiment for the exact supported native build.
// BONE_EATER_NATIVE_REAR_HUD=fit fits recognized MAIN LayoutedUI roots into
// the same portrait canvas as native_hud's front-LCD fit. Requires that fit,
// native -2Display, and 1920x1080 main resources. Original scene/camera rendering
// is untouched; standalone cinematic image commands are outside this seam.
void installNativeRearHudFit(void* module) noexcept;

} // namespace bone_eater::render
