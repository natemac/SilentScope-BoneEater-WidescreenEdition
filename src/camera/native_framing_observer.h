#pragma once

namespace bone_eater::camera {

// Optional observation of the verified game's CCamera commit. Observe-only is
// BONE_EATER_NATIVE_FRAMING_OBSERVE=1; independently, BONE_EATER_AIM_FRAMING=1
// enables the guarded experimental main-camera pitch/held-offset policy.
// Samples wrapper position/target/up/zoom/roll and native identity before and
// after the original commit. Observe-only does not modify game data or force
// transforms. Pitch mode temporarily changes the exact main wrapper target,
// commits through the native method, then restores the authored wrapper value.
// Install after module load and before game entry.
void installNativeFramingObserver(void* gamendd) noexcept;

} // namespace bone_eater::camera
