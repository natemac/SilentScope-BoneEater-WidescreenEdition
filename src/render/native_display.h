#pragma once

namespace bone_eater::render {
// Install before game entry. Opt-in only, exact build and instruction guards.
// Uses the engine's startup function; no persistent module bytes are changed.
void installNativeDisplayExperiment(void* module) noexcept;
// Original native aspect captured before a successful wide startup override.
// Zero means no verified override; callers must leave native optics unchanged.
float originalNativeMainAspect() noexcept;
}
