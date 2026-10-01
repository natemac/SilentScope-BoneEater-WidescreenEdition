#pragma once

#include "render/rear_illumination_math.h"

namespace bone_eater::render {

// Dialogue's -2Display reset deliberately removes its original root translation
// after the generic rear fit has applied scale. Accept exactly that observed
// state, then extend only the illumination across the landscape canvas.
// Portrait/text remain in their native front layer, with native proportions.
// A second invocation on already adapted geometry is rejected.
inline bool alignDialogueIllumination(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after) noexcept {
    constexpr double scale = 1080.0 / 1366.0;
    constexpr double expectedWidth = 768.0 * scale;
    constexpr double expectedLeft = (1920.0 - expectedWidth) * 0.5;
    const std::array<double, 8> actual {{left, top, width, height,
        before.position[0], before.position[1], before.scale[0], before.scale[1]}};
    const std::array<double, 8> expected {{expectedLeft, 0.0, expectedWidth, 1080.0,
        0.0, 270.0 * scale, scale, scale}};
    for (std::size_t i = 0; i < actual.size(); ++i)
        if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.001) return false;
    RearIlluminationPose result = before;
    result.position[0] = 0.1f; // Native commit subtracts .1 from cached X.
    result.scale[0] = 1920.0f / 800.0f;
    result.position[1] += top;
    if (!std::isfinite(result.position[0]) || !std::isfinite(result.position[1])) return false;
    after = result;
    return true;
}

} // namespace bone_eater::render
