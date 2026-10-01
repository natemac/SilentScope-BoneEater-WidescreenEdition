#pragma once
#include <cmath>
#include <cstdint>
#include <array>

namespace bone_eater::render {
struct BootFontContext {
    std::uintptr_t owner = 0, titleFont = 0, rowFont = 0;
    float offset = 0;
};

// drawFont converts x to x*800/100. Move the entire native 800-pixel
// checklist canvas into the middle of the verified wide logical canvas.
inline float bootHorizontalOffset(bool verifiedWide, unsigned width, unsigned height) noexcept {
    return verifiedWide && width == 1920 && height == 1080 ? (1920.f - 800.f) * .5f / 8.f : 0.f;
}

inline bool fittedBootCanvasUv(const std::array<unsigned,2>& dimensions,
        const std::array<float,8>& original, std::array<float,8>& fitted) noexcept {
    if (dimensions != std::array<unsigned,2>{1920,1280} ||
        original != std::array<float,8>{0,0,0,.625f,.78125f,.625f,.78125f,0}) return false;
    fitted=original;
    fitted[4]=fitted[6]=1920.f/2048.f;
    return true;
}

inline float translatedBootX(const BootFontContext& context, std::uintptr_t font,
                            float x, float y) noexcept {
    if (!context.owner || !font || (font != context.titleFont && font != context.rowFont) ||
        context.offset != 70.f || !std::isfinite(x) || !std::isfinite(y)) return x;
    const float translated = x + context.offset;
    return std::isfinite(translated) ? translated : x;
}

// Save/restore even for nested non-diagnosis calls: they must not inherit the
// outer owner's permission. The runtime supplies a thread_local slot.
struct ScopedBootFontContext {
    BootFontContext& slot;
    BootFontContext previous;
    ScopedBootFontContext(BootFontContext& destination, BootFontContext next) noexcept
        : slot(destination), previous(destination) { slot = next; }
    ~ScopedBootFontContext() { slot = previous; }
    ScopedBootFontContext(const ScopedBootFontContext&) = delete;
    ScopedBootFontContext& operator=(const ScopedBootFontContext&) = delete;
};
}
