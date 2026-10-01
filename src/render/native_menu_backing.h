#pragma once

#include "render/checked_sprite_geometry.h"

namespace bone_eater::render {

// Default off, BONE_EATER_NATIVE_RANKING_BACKING=1. This first correction is
// confined to Title's GeneralRank lifecycle, never other menus or cinematics.
void installNativeRankingBacking(void* module, bool sharedCommitReady) noexcept;

struct RankingWhiteIdentity {
    std::uintptr_t resident = 0, layout = 0, root = 0, leaf = 0, sibling = 0;
    std::uintptr_t sprite = 0, material = 0, texture = 0, camera = 0, record = 0;
    std::uint32_t group = 0;
    unsigned kind = 0; // 0: solid white, 1: SDMask Top, 2: SDMask Btm.
    bool operator==(const RankingWhiteIdentity&) const = default;
};

struct RankingBackingAlignment {
    RankingWhiteIdentity identity;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    bool attempted = false;
};

RankingBackingAlignment beginNativeRankingBackingCommit(void* object, std::uintptr_t caller) noexcept;
void restoreNativeRankingBackingCommit(const RankingBackingAlignment& token) noexcept;

} // namespace bone_eater::render
