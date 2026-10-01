#pragma once
#include <cmath>
#include <cstdint>

namespace bone_eater::render {
// A Start transition can also originate in a ranking/movie attract state.
// Only carry the title art through an exit when this same owner presented it.
class TitlePresentationPolicy {
    std::uintptr_t owner_ = 0;
public:
    void reset() noexcept { owner_ = 0; }
    bool admit(std::uintptr_t owner, unsigned phase, float alpha) noexcept {
        if (!owner || !std::isfinite(alpha) || alpha < 0 || alpha > 1) { reset(); return false; }
        if (phase == 6 || phase == 7) { owner_ = owner; return true; }
        if ((phase == 8 || phase == 15 || phase == 16) && owner_ == owner) return true;
        reset(); return false;
    }
};
}
