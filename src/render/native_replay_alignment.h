#pragma once

#include "render/checked_sprite_geometry.h"
#include <array>
#include <cstdint>

namespace bone_eater::render {

// Default-off BONE_EATER_NATIVE_REPLAY_ALIGN=1. Requires native HUD fit and the
// installed shared GUI-commit hook; install before game entry. Only short-mode
// KillCamera's named rear mask is positioned. Native replay content/fades stay.
void installNativeReplayAlignment(void* module, bool sharedCommitReady) noexcept;

struct ReplayIdentity {
    std::uintptr_t owner = 0;
    std::array<std::uintptr_t, 3> gui {}, sprites {}, materials {}, textures {}, cameras {};
    std::uint32_t group = 0;
    bool operator==(const ReplayIdentity&) const = default;
};

// POD saved token for the shared native commit's SEH finally. Restoration has
// no age gate and must run after an attempted partial write or native exception.
struct ReplayAlignment {
    ReplayIdentity identity;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    bool attempted = false;
};

ReplayAlignment beginNativeReplayAlignmentCommit(void* object, std::uintptr_t originalCaller) noexcept;
void restoreNativeReplayAlignmentCommit(const ReplayAlignment& token) noexcept;

} // namespace bone_eater::render
