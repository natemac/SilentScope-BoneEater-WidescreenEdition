#pragma once
#include "render/checked_sprite_geometry.h"

namespace bone_eater::render {
// Default off; exact Eamusement4/1/8 -> FirstCredit1/{0,1,3,4,5} only.
void installNativeNormalStartBacking(void* module, bool sharedCommitReady) noexcept;
struct NormalStartWhiteIdentity {
    std::uintptr_t resident = 0, layout = 0, root = 0, leaf = 0, sibling = 0;
    std::uintptr_t sprite = 0, material = 0, texture = 0, camera = 0, record = 0;
    std::uint32_t group = 0;
    bool operator==(const NormalStartWhiteIdentity&) const = default;
};
struct FirstCreditQuarterIdentity {
    std::uintptr_t parent = 0, child = 0, layout = 0, root = 0;
    std::array<std::uintptr_t, 3> leaves {};
    std::uintptr_t sprite = 0, material = 0, texture = 0, camera = 0, record = 0;
    std::uint32_t group = 0;
    bool operator==(const FirstCreditQuarterIdentity&) const = default;
};
struct NormalStartBackingAlignment {
    NormalStartWhiteIdentity identity;
    FirstCreditQuarterIdentity quarterIdentity;
    bool quarter = false;
    unsigned quarterRole = 2;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    bool attempted = false;
};
NormalStartBackingAlignment beginNativeNormalStartBackingCommit(void* object, std::uintptr_t caller) noexcept;
void restoreNativeNormalStartBackingCommit(const NormalStartBackingAlignment& token) noexcept;
} // namespace bone_eater::render
