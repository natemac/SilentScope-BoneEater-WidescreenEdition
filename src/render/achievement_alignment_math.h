#pragma once

#include "render/rear_illumination_math.h"

namespace bone_eater::render {

// Preserve native 800x82 artwork and its 32-pixel overhang relative to the
// 768-wide title. Fully parked or mixed native-update poses pass through.
inline bool alignAchievementBacking(float left, float top, float width, float height,
        const RearIlluminationPose& before, float titleCachedY,
        RearIlluminationPose& after) noexcept {
    if (before.position[0] != 0 || !std::isfinite(before.position[1]) ||
            before.position[1] <= -82.0f || before.position[1] > 0 ||
            !std::isfinite(titleCachedY) || before.position[1] != titleCachedY ||
            before.scale != std::array<float, 2>{{1, 1}}) return false;
    return alignRearIllumination(left, top, width, height, before, after);
}

} // namespace bone_eater::render
