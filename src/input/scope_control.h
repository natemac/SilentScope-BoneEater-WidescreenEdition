#pragma once
#include <cstdint>

namespace bone_eater::input {

struct ScopeControlConfig { std::uint32_t holdMs = 250; bool exitOnHoldRelease = true; };
struct ScopeControlInput {
    std::uint64_t nowMs = 0;
    // Caller owns source, focus and game-context validation. A changed identity
    // clears the latch and requires neutral; zero is never an eligible context.
    // A publication gap over250ms also clears/rearms, so stale held gestures
    // cannot resume as an unintended zoom after a paused input producer.
    std::uint64_t contextIdentity = 0;
    bool buttonsHeld = false; // OR of all eligible configured bindings.
    bool usable = false;
    std::uintptr_t nativeScopeOwner = 0, nativeInputOwner = 0;
};
struct ScopeControlSnapshot {
    bool enabled = false, higher = false, armed = false, buttonsHeld = false;
    std::uint64_t updatedAtMs = 0, contextIdentity = 0;
    std::uintptr_t nativeScopeOwner = 0, nativeInputOwner = 0;
};

class ScopeControl {
public:
    ScopeControlSnapshot update(const ScopeControlInput& input,
        const ScopeControlConfig& config = {}) noexcept;
    void reset() noexcept;
    ScopeControlSnapshot read() const noexcept { return state_; }
private:
    ScopeControlSnapshot state_;
    std::uint64_t pressedAt_ = 0;
    bool openingPress_ = false;
};

// Native mode0 is higher magnification, mode1 lower (confirmed by playtest);
// modes2/3 are native transitions. Reconcile
// after transitions, never overwrite optical scale or native interpolators.
bool scopeZoomNeedsEdge(bool higher, std::uint32_t nativeMode) noexcept;

// Thread-safe shared intent. Only the authoritative native input boundary
// publishes; render/camera consumers read this same logical latch.
void initializeScopeControlFromEnvironment() noexcept;
bool scopeControlRequested() noexcept;
ScopeControlSnapshot publishScopeControl(const ScopeControlInput& input) noexcept;
ScopeControlSnapshot readScopeControlSnapshot() noexcept;
void resetScopeControl() noexcept;

} // namespace bone_eater::input
