#pragma once

#include "render/checked_sprite_geometry.h"

namespace bone_eater::render {

// Default off: BONE_EATER_NATIVE_MENU_MARGIN=1. Only MainMenu scene 2,
// primary 1, settled secondary 2/9/12/17 and observed transitions 1/3/10/11/14.
// Exact Main_Quarter White/BG/BG2/BG3; never shared resident fades or other menus.
void installNativeMenuMargin(void* module, bool sharedCommitReady) noexcept;

struct MenuMarginIdentity {
    std::uintptr_t manager = 0, menu = 0, layout = 0, root = 0;
    std::array<std::uintptr_t, 4> leaves {};
    std::uintptr_t sprite = 0, material = 0, texture = 0, camera = 0, record = 0;
    std::uint32_t group = 0;
    bool operator==(const MenuMarginIdentity&) const = default;
};

struct MenuMarginAlignment {
    MenuMarginIdentity identity;
    SpriteGeometryPermission permission;
    RearIlluminationPose original;
    unsigned role = 3;
    bool attempted = false;
};

MenuMarginAlignment beginNativeMenuMarginCommit(void* object, std::uintptr_t caller) noexcept;
void restoreNativeMenuMarginCommit(const MenuMarginAlignment& token) noexcept;

} // namespace bone_eater::render
