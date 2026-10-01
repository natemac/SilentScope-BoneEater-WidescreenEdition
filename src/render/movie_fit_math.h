#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace bone_eater::render {

// Native CSprite +580: center X/Y, full width/height. The movie's existing
// padded/flipped UV rectangle remains unchanged; this fits its decoded image.
inline bool fittedMovieGeometry(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
        std::uint32_t canvasWidth, std::uint32_t canvasHeight,
        std::array<float, 4>& geometry) noexcept {
    if (!sourceWidth || !sourceHeight || sourceWidth > 16384 || sourceHeight > 16384 ||
            !canvasWidth || !canvasHeight || canvasWidth > 16384 || canvasHeight > 16384) return false;
    const double scale = std::min(static_cast<double>(canvasWidth) / sourceWidth,
        static_cast<double>(canvasHeight) / sourceHeight);
    geometry = {canvasWidth * 0.5f, canvasHeight * 0.5f,
        static_cast<float>(sourceWidth * scale), static_cast<float>(sourceHeight * scale)};
    return std::all_of(geometry.begin(), geometry.end(), [](float value) { return std::isfinite(value); });
}

} // namespace bone_eater::render
