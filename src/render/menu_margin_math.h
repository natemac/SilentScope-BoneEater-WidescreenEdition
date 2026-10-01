#pragma once

#include "render/rear_illumination_math.h"

namespace bone_eater::render {

// The exact Main_Quarter White leaf is 800x1480. Its horizontal overscan is
// separate from the BG artwork; retain all vertical pose and native fades.
inline bool fitMenuWhiteHorizontal(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after,
        bool fullHeightBacking = true) noexcept {
    const double expectedWidth = 768.0 * 1080.0 / 1366.0;
    const std::array<double, 4> expected {{(1920.0 - expectedWidth) / 2, 0, expectedWidth, 1080}};
    const std::array<double, 4> actual {{left, top, width, height}};
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.001) return false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!std::isfinite(before.position[i]) || std::fabs(before.position[i]) > 4096 ||
                !std::isfinite(before.scale[i]) || before.scale[i] <= 0 || before.scale[i] > 2) return false;
        // Artwork occupies only the central vertical band; its exact authored
        // Y is checked by the typed owner. Only White must cover full height.
        if (i && !fullHeightBacking) continue;
        const double extent = i ? 1480.0 : 800.0;
        const double start = static_cast<double>(before.position[i]) - 0.1;
        if (start > actual[i] || start + extent * before.scale[i] < actual[i] + actual[i + 2]) return false;
    }
    after = before;
    after.position[0] = left + 0.1f;
    after.scale[0] = width / 800.0f;
    return true;
}

} // namespace bone_eater::render
