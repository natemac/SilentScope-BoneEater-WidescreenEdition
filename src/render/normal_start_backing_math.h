#pragma once
#include "render/ranking_backing_math.h"

namespace bone_eater::render {
inline bool fitNormalStartWhiteHorizontal(float left, float top, float width, float height,
        const RearIlluminationPose& before, RearIlluminationPose& after) noexcept {
    RearIlluminationPose bounded;
    // Reuse the exact 1000x1480 coverage/finite/front-rectangle validation,
    // but borrow only X. Native vertical overscan and animation remain intact.
    if (!clipRankingBacking(left, top, width, height, before, bounded)) return false;
    bounded.position[1] = before.position[1]; bounded.scale[1] = before.scale[1];
    after = bounded; return true;
}
} // namespace bone_eater::render
