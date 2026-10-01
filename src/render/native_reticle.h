#pragma once

namespace bone_eater::render {

// Called on the native UI thread. Must be nonblocking and return true only
// while the replacement main-view reticle/lens is healthy and recently drawn.
using DesktopReticleReady = bool(*)();

// BONE_EATER_DESKTOP_RETICLE=1 enables this exact-build visual-only experiment.
// Registers exact owner-bound GunCrossChair and BattleUILcd reticle branches
// after native animation, then suppresses only their native sprite commits.
// Menu guides and Battle lines/edge sprites are individually named. CCRoot is
// excluded from generic ancestry suppression; only an exact owned side-meter
// leaf commit may temporarily hide it, with a separate replacement handshake.
// Their original
// visibility is restored synchronously before each commit hook returns.
// Native gun state, aim, bullet-hole siblings and show/hide control bytes are
// untouched. Install after module load and before game entry.
void installNativeReticleSuppression(void* module, DesktopReticleReady ready) noexcept;
// Bits1/2 report fresh native left/right visible owned meter commits, reset
// after each battle update. Suppression additionally requires a successful
// replacement draw for that side; CCRoot alone never publishes eligibility.
unsigned nativeSideMetersVisible() noexcept;

} // namespace bone_eater::render
