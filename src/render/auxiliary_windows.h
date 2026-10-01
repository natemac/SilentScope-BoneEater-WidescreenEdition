#pragma once

struct IDXGISwapChain;

namespace bone_eater::render {

// Default off. BONE_EATER_AUXILIARY_WINDOWS=offscreen starts a monitored,
// reversible parking experiment for registered game auxiliary HWNDs. It never
// hides/minimizes, changes styles/size, or replaces the native Present result.
// Call after each native Present/Present1, including TEST calls. A real
// non-S_OK result rolls back parking; TEST calls never count as rendered frames.
// A first rate/stale-cadence rollback may retry once after placement is confirmed,
// a ten-second visible cooldown, and three stable recent main/auxiliary bins.
// Visible staleness restarts qualification. All other failures, a second cadence
// failure and explicit restore are terminal.
void observeAuxiliaryPresentResult(IDXGISwapChain* chain, void* knownMainWindow,
                                  unsigned int presentFlags, long result) noexcept;

// Game MoveWindow/SetWindowPos hooks must prefer this override to diagnostic
// layout. While parked, preserve its saved size and offscreen position. During
// rollback, return its saved position until the worker has restored it.
bool auxiliaryWindowPlacement(void* window, int& x, int& y,
                              int& width, int& height) noexcept;

// Diagnostic layout must skip auxiliary observations while this owns placement.
// Main layout remains unchanged. This returns true during parked/restore work.
bool auxiliaryWindowManaged(void* window) noexcept;

// Reversible process-lifetime opt-out; asynchronously restores saved positions.
// No restart is needed to restore. Re-enabling requires a new process so failed
// occlusion experiments cannot repeatedly disrupt the player's display.
void restoreAuxiliaryWindows() noexcept;

} // namespace bone_eater::render
