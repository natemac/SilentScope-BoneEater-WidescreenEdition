#pragma once

namespace bone_eater::render {

// Default-off, read-only PostProcessBloom input/cache sampling. Requires the
// exact supported game image; BONE_EATER_DOF_OBSERVE=1 opts in at installation.
// No camera properties, renderer state, resources, or native return values change.
void installNativeDofProbe(void* module) noexcept;

} // namespace bone_eater::render
