#pragma once

#include <cstddef>
#include <cstdint>

namespace bone_eater::render {

// Call only while an existing diagnostic staging map is valid, before the BMP
// exporter replaces its alpha bytes. This helper does not map/copy GPU resources.
// Accepted formats are 32-bit RGBA/BGRA/BGRX UNORM and their SRGB variants.
// BGRX's fourth byte is explicitly reported as undefined padding, not opacity.
// Appends raw byte statistics to desktop/captures/raw-alpha-v1.csv. The caller's
// capture rate supplies throttling; this is not intended for every-frame use.
void observeCaptureAlpha(const char* role, void* window, std::uint32_t format,
                         std::uint32_t width, std::uint32_t height,
                         std::size_t rowPitch, const void* pixels) noexcept;

} // namespace bone_eater::render
