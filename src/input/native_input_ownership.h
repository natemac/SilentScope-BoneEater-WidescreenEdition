#pragma once
#include "input/input_ownership_state.h"

namespace bone_eater::input {

// Shared Scope boundary. Optional metadata is disabled by quiet; the separately
// opted-in precision feature retains its functional ledger without CSV output.
// Called after the desktop adapter is enabled and game/ARK verification succeeds.
void installNativeInputOwnershipObserver(void* gamendd) noexcept;
// True only after the exact ScopeCamera and native zoom-query hooks both install.
bool scopeControlNativeReady() noexcept;
std::uint64_t beginNativeInputOwnership(std::uintptr_t input, bool eligible) noexcept;
void convertedNativeInputOwnership(std::uint64_t ticket, const OwnershipPoint& point,
    bool converted) noexcept;
void finishNativeInputOwnership(std::uint64_t ticket, bool returned, bool identityValid,
    const OwnershipPoint& nativePoint) noexcept;

// Existing exact CD1C7 observer calls this with its original by-value arguments
// AFTER its one native ray call. No extra ray call, input poll or latest snapshot.
void observeNativeInputOwnershipRay(std::uintptr_t camera, float x, float y,
    bool verified, bool scopeKnown, bool scopeOff) noexcept;

} // namespace bone_eater::input
