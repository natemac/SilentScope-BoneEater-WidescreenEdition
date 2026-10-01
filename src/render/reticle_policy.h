#pragma once

#include <cmath>
#include <cstdint>

namespace bone_eater::render {

enum class ReticleKind { None, Menu, Gameplay, Lens };
struct ReticleDecision {
    ReticleKind kind = ReticleKind::None;
    float x = 0, y = 0;
};

struct SideMeterPlacement {
    unsigned mask = 0;
    float left = 0, top = 0, right = 0, bottom = 0, scale = 0;
};

inline SideMeterPlacement chooseSideMeters(bool replacement, ReticleKind kind, unsigned nativeMask,
        double left, double top, double right, double bottom,
        double clientWidth, double clientHeight, double targetWidth, double targetHeight) noexcept {
    if (!replacement || (kind != ReticleKind::Gameplay && kind != ReticleKind::Lens) ||
            !nativeMask || nativeMask > 3 || !std::isfinite(left) || !std::isfinite(top) ||
            !std::isfinite(right) || !std::isfinite(bottom) || !std::isfinite(clientWidth) ||
            !std::isfinite(clientHeight) || !std::isfinite(targetWidth) || !std::isfinite(targetHeight) ||
            clientWidth <= 0 || clientHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
            targetWidth > 32768 || targetHeight > 32768 || left < 0 || top < 0 ||
            right > clientWidth || bottom > clientHeight || right <= left || bottom <= top) return {};
    SideMeterPlacement result;
    result.mask = nativeMask;
    result.left = static_cast<float>(left / clientWidth * targetWidth);
    result.right = static_cast<float>(right / clientWidth * targetWidth);
    result.top = static_cast<float>(top / clientHeight * targetHeight);
    result.bottom = static_cast<float>(bottom / clientHeight * targetHeight);
    result.scale = (result.bottom - result.top) / 1366.0f;
    if (!(result.scale > 0) || result.right <= result.left || result.bottom <= result.top) return {};
    return result;
}

// Menu hit tests consume the desktop input point. Gameplay consumes the final
// native ray, which can differ because of native precision/aim transitions.
// Never substitute menu input for a missing gameplay ray.
inline ReticleDecision chooseReticle(bool replacement, bool lensRequested,
        bool desktopValid, bool gameplay, std::uint64_t desktopAt,
        std::uint64_t now, float desktopX, float desktopY,
        bool rayFresh, bool scopeOffKnown, bool scopeOff, float rayX, float rayY,
        bool sourceFresh) noexcept {
    if (!desktopValid || !desktopAt || now < desktopAt || now - desktopAt > 250 ||
            !std::isfinite(desktopX) || !std::isfinite(desktopY)) return {};
    if (!gameplay) return replacement ?
        ReticleDecision{ReticleKind::Menu, desktopX, desktopY} : ReticleDecision{};
    if (!rayFresh || !scopeOffKnown || scopeOff ||
            !std::isfinite(rayX) || !std::isfinite(rayY)) return {};
    if (lensRequested && sourceFresh) return {ReticleKind::Lens, rayX, rayY};
    return replacement ? ReticleDecision{ReticleKind::Gameplay, rayX, rayY} : ReticleDecision{};
}

} // namespace bone_eater::render
