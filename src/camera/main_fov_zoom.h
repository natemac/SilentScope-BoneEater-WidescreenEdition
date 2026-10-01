#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace bone_eater::camera {

// Portable configuration/math shared by the native seam and its tests.
// Full-string, locale-independent decimal parsing; no implicit clamping.
inline std::optional<float> parseMainFovZoom(std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
        std::chars_format::general);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() ||
            !std::isfinite(value) || value < 1.0 || value > 1.5) return std::nullopt;
    return static_cast<float>(value);
}

struct MainFovZoom {
    float factor = 0;
    float temporaryZoom = 0;
    bool valid = false;
};

// The baseline is the CURRENT authored SystemZoom, never a prior override.
// multiplier1 preserves the existing height-correction arithmetic exactly;
// larger values crop symmetrically without modifying the scripted baseline.
inline MainFovZoom mainFovZoom(float baseline, float originalAspect,
        float nativeAspect, float multiplier) noexcept {
    if (!std::isfinite(baseline) || baseline <= 0 ||
            !std::isfinite(originalAspect) || originalAspect <= 0 ||
            !std::isfinite(nativeAspect) || nativeAspect <= 0 ||
            !std::isfinite(multiplier) || multiplier < 1.0f || multiplier > 1.5f) return {};
    const float heightFactor = originalAspect / nativeAspect;
    // Retain the existing correction guard independently of the new zoom.
    if (!std::isfinite(heightFactor) || heightFactor < 0.25f || heightFactor > 0.5f) return {};
    const float factor = heightFactor * multiplier;
    const float temporaryZoom = baseline * factor;
    if (!std::isfinite(temporaryZoom) || temporaryZoom <= 0) return {};
    return {factor, temporaryZoom, true};
}

} // namespace bone_eater::camera
