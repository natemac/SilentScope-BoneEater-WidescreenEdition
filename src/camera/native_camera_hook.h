#pragma once

namespace bone_eater::camera {

// Install before game entry, after loading the exact supported gamendd.dll.
// BONE_EATER_NATIVE_CAMERA_OBSERVE=1 enables a read-only observer around the
// native recalculation function. The verified module is pinned for hook lifetime.
// That observer alone does not modify camera fields or arguments. The separate
// BONE_EATER_NATIVE_VERTICAL_FOV=1 experiment temporarily applies native System
// Zoom around verified main-camera recalculation and restores the script value.
// It requires the native-wide startup and records restoration/invariant status.
// BONE_EATER_NATIVE_FOV_ZOOM optionally selects a finite decimal multiplier
// [1.0,1.5] relative to preserved height, default1.0. It alone enables nothing.
// Invalid supplied values reject the optical override with an explicit log.
// The integration links the existing detour implementation and bcrypt validator.
void installNativeCameraObserver(void* module) noexcept;

} // namespace bone_eater::camera
