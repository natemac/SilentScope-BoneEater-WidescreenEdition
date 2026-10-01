#pragma once

struct ID3D11Device;

namespace bone_eater::render {

// Capture exact native vertex output signatures before game shaders are created.
void observeWideHudDevice(ID3D11Device* device) noexcept;

// The default-on wider HUD preserves the native pass and publishes only a
// complete, paired front/rear result. BONE_EATER_WIDE_HUD=0 retains the native
// layout. Install after verifying the game DLL, before entry.
// Optional BONE_EATER_FRONT_OBSERVE=1 enables metadata diagnostics. Additional
// BONE_EATER_FRONT_CAPTURE=1 enables bounded GPU readback (may stall); both are
// suppressed by quiet diagnostics and neither is required for the HUD.
void installNativeFrontObserver(void* module) noexcept;

} // namespace bone_eater::render
