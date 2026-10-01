#pragma once

namespace bone_eater::camera {

// Read-only, exact-binary-gated sampling. Call from the main output's Present
// hook. Enabled only by BONE_EATER_CAMERA_TELEMETRY=1 before process startup;
// disabled calls do no recurring allocation, file access, or memory sampling.
// Samples at most 5 Hz to desktop/cameras-v2.csv and reports unresolved identities.
// Requires bcrypt.lib. No game functions are called and no game memory is written.
// A Present sample cannot establish camera-update/culling/aim calculation order;
// fields can also change concurrently between ReadProcessMemory operations.
void observeCameraState() noexcept;

} // namespace bone_eater::camera
