#pragma once
#include "render/checked_sprite_geometry.h"

namespace bone_eater::render {
// Exact Options scene3/parent2/child75,76,77 and Battle loading/opaque handoff.
// Separate front-fade certificates exclude gameplay and unrelated white flashes.
void installNativeOptionsBacking(void* module, bool sharedCommitReady) noexcept;
struct OptionsWhiteIdentity {
    std::uintptr_t resident = 0, layout = 0, root = 0, leaf = 0, sibling = 0;
    std::uintptr_t sprite = 0, material = 0, texture = 0, camera = 0, record = 0;
    std::uint32_t group = 0;
    bool operator==(const OptionsWhiteIdentity&) const = default;
};
struct OptionsBackingAlignment {
    OptionsWhiteIdentity identity;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    bool attempted = false;
};
OptionsBackingAlignment beginNativeOptionsBackingCommit(void* object, std::uintptr_t caller) noexcept;
void restoreNativeOptionsBackingCommit(const OptionsBackingAlignment& token) noexcept;
} // namespace bone_eater::render
