#pragma once

#include "render/checked_sprite_geometry.h"
#include <array>
#include <cstdint>

namespace bone_eater::render {

// BONE_EATER_NATIVE_ACHIEVEMENT_ALIGN=1; requires native HUD fit and the
// existing shared LCD-update/final-GUI-commit hooks. No second detour.
void installNativeAchievementAlignment(void* module, bool sharedHooksReady) noexcept;

// Pair only when begin returns true. Call end from the native update's SEH
// finally, passing true only after the original update returned normally.
// While an update is in flight, no previous certificate remains eligible.
bool beginNativeAchievementUpdate() noexcept;
void endNativeAchievementUpdate(void* lcd, bool completed) noexcept;

struct AchievementIdentity {
    std::uintptr_t manager = 0, main = 0, lcd = 0;
    std::array<std::uintptr_t, 7> gui {};
    std::array<std::uintptr_t, 2> sprites {}, materials {}, textures {}, cameras {}, records {};
    std::array<std::uint32_t, 2> groups {};
    bool operator==(const AchievementIdentity&) const = default;
};

struct AchievementAlignment {
    AchievementIdentity identity;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    bool attempted = false;
};

AchievementAlignment beginNativeAchievementAlignmentCommit(void* object,
    std::uintptr_t originalCaller) noexcept;
void restoreNativeAchievementAlignmentCommit(const AchievementAlignment& token) noexcept;

} // namespace bone_eater::render
