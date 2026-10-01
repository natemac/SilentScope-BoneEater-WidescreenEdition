#pragma once

#include "render/checked_data_write.h"
#include "render/checked_sprite_geometry.h"
#include "render/checked_dialogue_position.h"
#include <cstdint>

namespace bone_eater::render {

// Diagnostic-only removal OR alignment of the two original gradient leaves.
// Alignment additionally registers the exact owned Dialogue:bg illumination,
// extending only the glow across the landscape canvas; portrait/text stay native.
// BONE_EATER_BATTLE_BACKGROUND_FILTER=1 and BONE_EATER_BATTLE_BACKGROUND_ALIGN=1
// are mutually exclusive; requesting both rejects this helper. Alignment also
// requires the committed native front-HUD fit. Requires BONE_EATER_DESKTOP_RETICLE=1,
// and an already installed shared native sprite-commit hook. No second detour
// of that commit function is installed here. Install before game entry.
void installNativeBattleBackgroundFilter(void* module, bool sharedCommitReady) noexcept;

// POD token: the shared commit hook must call restore in its SEH __finally,
// including when the original native call raises. Caller is the ORIGINAL
// native return address, captured by that hook before entering helper code.
struct BattleBackgroundSuppression {
    std::uintptr_t owner = 0;
    std::uintptr_t root = 0;
    std::uintptr_t upper = 0;
    std::uintptr_t background = 0;
    std::uintptr_t leaves[2] {};
    std::uint32_t group = 0;
    unsigned index = 0;
    unsigned char previous = 0;
    bool attempted = false;
    // Exact-address permission captured before the hide. Restoration must not
    // depend on a newer owner snapshot or expire after a slow native commit.
    PrivateDataBytePermission permission;
    bool alignment = false;
    SpriteGeometryPermission geometryPermission;
    RearIlluminationPose originalGeometry;
    bool dialogue = false;
    std::uintptr_t dialogueManager = 0;
    std::uintptr_t dialogueLayout = 0;
    DialoguePositionPermission dialoguePermission;
    bool splash = false;
    std::uintptr_t splashManager = 0, splashOwner = 0, splashFront = 0;
    unsigned splashSlot = 0, splashFrontSlot = 0;
    int splashId = -1;
};

BattleBackgroundSuppression beginNativeBattleBackgroundCommit(
    void* sprite, std::uintptr_t nativeCaller) noexcept;
void restoreNativeBattleBackgroundCommit(const BattleBackgroundSuppression& token) noexcept;
// Exact adapter record after successful aligned commit and restoration, until
// the next owner update. The manager dispatches adapters after GUI commits.
bool certifiedNativeBattleBackgroundRecord(std::uintptr_t record,unsigned& index) noexcept;
bool nativeBattleBackgroundHealthy() noexcept;

} // namespace bone_eater::render
