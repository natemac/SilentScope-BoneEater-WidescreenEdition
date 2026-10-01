#pragma once

#include "render/rear_illumination_math.h"
#include <algorithm>

namespace bone_eater::render {

// Only the authored, axis-aligned 1000x1480 solid white overscan leaf uses
// this policy. Preserve its contribution within the committed front panel;
// remove the exposed border. This is a desktop composition policy, not a
// reinterpretation of the original 800-wide rear framebuffer.
inline bool clipRankingBacking(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after) noexcept {
    const double expectedWidth = 768.0 * 1080.0 / 1366.0;
    const std::array<double, 4> expected {{(1920.0 - expectedWidth) / 2, 0, expectedWidth, 1080}};
    const std::array<double, 4> actual {{left, top, width, height}};
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.001) return false;
    const std::array<double, 2> extent {{1000, 1480}}, origin {{left, top}}, size {{width, height}};
    RearIlluminationPose result;
    for (unsigned i = 0; i < 2; ++i) {
        if (!std::isfinite(before.position[i]) || std::fabs(before.position[i]) > 4096 ||
                !std::isfinite(before.scale[i]) || before.scale[i] <= 0 || before.scale[i] > 2) return false;
        const double oldLeft = static_cast<double>(before.position[i]) - 0.1;
        const double oldRight = oldLeft + extent[i] * before.scale[i];
        // Never enlarge or invent missing backing. A changed pose that does
        // not fully cover the measured front footprint is left native.
        if (oldLeft > origin[i] || oldRight < origin[i] + size[i]) return false;
        result.position[i] = static_cast<float>(origin[i] + 0.1);
        result.scale[i] = static_cast<float>(size[i] / extent[i]);
    }
    after = result;
    return true;
}


// SDMask is a vertical gradient. Only change its horizontal footprint; retain
// exact Y, Y scale and source/UVs, including any native vertical orientation.
inline bool fitRankingMaskHorizontal(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after) noexcept {
    const double expectedWidth = 768.0 * 1080.0 / 1366.0;
    const std::array<double, 4> expected {{(1920.0 - expectedWidth) / 2, 0, expectedWidth, 1080}};
    const std::array<double, 4> actual {{left, top, width, height}};
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.001) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (!std::isfinite(before.position[i]) || std::fabs(before.position[i]) > 4096 ||
                !std::isfinite(before.scale[i]) || std::fabs(before.scale[i]) > 2 || before.scale[i] == 0) return false;
    const double oldLeft = static_cast<double>(before.position[0]) - 0.1;
    if (before.scale[0] <= 0 || oldLeft > left || oldLeft + 850.0 * before.scale[0] < left + width) return false;
    auto result = before;
    result.position[0] = left + 0.1f;
    result.scale[0] = width / 850.0f;
    after = result;
    return true;
}

} // namespace bone_eater::render
