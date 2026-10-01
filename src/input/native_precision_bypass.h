#pragma once
#include "input/input_ownership_state.h"
#include "render/native_hud.h"

namespace bone_eater::input {
// Default-off legacy precision comparison, independent of optional output.
// Ownership installer shares its ONE Scope detour with this module.
bool nativePrecisionRequested() noexcept;
bool prepareNativePrecisionBypass(void* gamendd) noexcept;
void nativePrecisionScopeInstalled(void* gamendd) noexcept;
// Called only after verified/pinned ARK getter AND final-ray hooks succeed.
void nativePrecisionAimInstalled(void* gamendd) noexcept;
bool nativePrecisionReady() noexcept;
void recordNativePrecisionDomain(std::uint64_t serial, std::uintptr_t config,
    const render::NativeHudGeometry& geometry) noexcept;

struct NativePrecisionScopeToken { void* previous = nullptr; bool owns = false; };
NativePrecisionScopeToken beginNativePrecisionScope(std::uintptr_t owner,
    std::uintptr_t caller, float delta, const InputOwnershipState& input, bool nested) noexcept;
void finishNativePrecisionScope(NativePrecisionScopeToken token, bool returned) noexcept;
void observeNativePrecisionRay(std::uintptr_t camera, float x, float y,
    bool verified, bool scopeKnown, bool scopeOff) noexcept;
} // namespace bone_eater::input
