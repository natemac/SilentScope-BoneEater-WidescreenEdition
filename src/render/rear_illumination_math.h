#pragma once

#include <array>
#include <cmath>

namespace bone_eater::render {

struct RearIlluminationPose {
    std::array<float, 2> position {};
    std::array<float, 2> scale {};
};

// This first experiment is confined to the committed 768x1366 portrait fit
// in the 1920x1080 main canvas. Inputs and outputs are native logical pixels.
// The original front quad is [-.5,-.5,769,1367]; preserving its correspondence
// retains the original registration instead of stretching 800 rear pixels to
// an assumed 768-pixel extent. The GUI commit subsequently subtracts 0.1.
inline bool alignRearIllumination(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after) noexcept {
    constexpr double expectedHeight = 1080.0;
    constexpr double expectedWidth = 768.0 * expectedHeight / 1366.0;
    constexpr double expectedLeft = (1920.0 - expectedWidth) * 0.5;
    const std::array<double, 4> actual {{left, top, width, height}};
    const std::array<double, 4> expected {{expectedLeft, 0.0, expectedWidth, expectedHeight}};
    for (unsigned i = 0; i < 4; ++i)
        if (!std::isfinite(actual[i]) || std::fabs(actual[i] - expected[i]) > 0.001) return false;
    RearIlluminationPose result;
    const std::array<double, 2> factor {{static_cast<double>(width) / 769.0,
        static_cast<double>(height) / 1367.0}};
    const std::array<double, 2> origin {{left, top}};
    for (unsigned i = 0; i < 2; ++i) {
        if (!std::isfinite(before.position[i]) || std::fabs(before.position[i]) > 16384.0f ||
                !std::isfinite(before.scale[i]) || before.scale[i] <= 0.0f || before.scale[i] > 16.0f)
            return false;
        // Native submitted position is q-.1. Map that output relative to the
        // old quad's -.5 origin, then put back the native subtraction once.
        result.position[i] = static_cast<float>(origin[i] + factor[i] *
            (static_cast<double>(before.position[i]) - 0.1 + 0.5) + 0.1);
        result.scale[i] = static_cast<float>(before.scale[i] * factor[i]);
        if (!std::isfinite(result.position[i]) || !std::isfinite(result.scale[i])) return false;
    }
    after = result; // Failure never publishes a partial pose.
    return true;
}

} // namespace bone_eater::render
