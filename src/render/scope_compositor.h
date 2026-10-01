#pragma once

struct IDXGISwapChain;

namespace bone_eater::render {

// Opt-in GPU-only prototype: BONE_EATER_SCOPE_OVERLAY=1. Copies the measured
// 800x480 MultiDisplay[0] output and composites its centered 480x480 region onto
// ASKA, circle-masked at the final native scope world-ray point. Requires fresh
// intent, ray, source and a verified false native script Scope Off flag.
// BONE_EATER_DESKTOP_RETICLE=1 additionally draws a red main-view ring/guides
// at that point while scope is released (or its texture is unavailable), with
// a committed native HUD fit. Settled menus instead use the verified desktop
// input point for a compact marker; they need neither a scope ray nor texture.
// Unknown scene/input state fails closed; gameplay never falls back to raw aim.
// Call before diagnostics/debug overlay for each real Present. No resources
// belonging to a swapchain survive the call, so ResizeBuffers needs no callback.
void composeScopeD3D11Present(IDXGISwapChain* swapchain, void* knownMainWindow,
                              unsigned int presentFlags) noexcept;
// Previous successful draw handshake for native reticle suppression; expires
// after 100 ms. A failed main draw attempt clears it. A Present rejected before
// main-window classification can retain the previous success until expiry.
bool desktopReticleReady() noexcept;
bool desktopMetersReady(unsigned sideMask) noexcept;

} // namespace bone_eater::render
