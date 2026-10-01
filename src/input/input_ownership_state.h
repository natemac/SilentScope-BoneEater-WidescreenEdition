#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace bone_eater::input {

using OwnershipPoint = std::array<float, 2>;
inline bool sameOwnershipPoint(const OwnershipPoint& a, const OwnershipPoint& b) noexcept {
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
inline bool validOwnershipPoint(const OwnershipPoint& p) noexcept {
    return std::isfinite(p[0]) && std::isfinite(p[1]) &&
        p[0] >= 0 && p[0] <= 1920 && p[1] >= 0 && p[1] <= 1080;
}

enum class OwnershipMatch {
    Completed, NoCompletion, Updating, WrongInput, Stale, ChangedPoint, RepeatedScope,
};
inline const char* ownershipMatchName(OwnershipMatch value) noexcept {
    switch (value) {
        case OwnershipMatch::Completed: return "completed_same_update";
        case OwnershipMatch::Updating: return "update_in_progress";
        case OwnershipMatch::WrongInput: return "different_input";
        case OwnershipMatch::Stale: return "stale_completion";
        case OwnershipMatch::ChangedPoint: return "entry_point_changed";
        case OwnershipMatch::RepeatedScope: return "repeated_scope_invocation";
        default: return "no_completed_conversion";
    }
}

// One instance per native thread. These counters belong to the native input
// update, never a HID report or an age-matched canonical publication. A nested
// or excluded update revokes the outer candidate rather than restoring it.
struct InputOwnershipState {
    std::uint64_t serial = 0, completedTick = 0, scopeCalls = 0;
    std::uintptr_t input = 0;
    OwnershipPoint converted {};
    unsigned conversions = 0, updateDepth = 0;
    bool updating = false, allowed = false, convertedOk = false, completed = false;

    std::uint64_t begin(std::uintptr_t identity, bool eligible) noexcept {
        if (updateDepth != std::numeric_limits<unsigned>::max()) ++updateDepth;
        // Saturation permanently revokes proof; wrapping must not recycle IDs.
        if (serial == std::numeric_limits<std::uint64_t>::max()) {
            completed = allowed = false; updating = true; return 0;
        }
        ++serial; input = identity; allowed = eligible && identity && updateDepth == 1;
        updating = true; completed = convertedOk = false; conversions = 0;
        converted = {}; completedTick = scopeCalls = 0;
        return serial;
    }
    void conversion(std::uint64_t ticket, const OwnershipPoint& point, bool success) noexcept {
        if (!ticket || ticket != serial || !updating) return;
        if (conversions != std::numeric_limits<unsigned>::max()) ++conversions;
        convertedOk = allowed && conversions == 1 && success && validOwnershipPoint(point);
        converted = point;
    }
    void finish(std::uint64_t ticket, bool returned, bool identityValid,
                const OwnershipPoint& nativePoint, std::uint64_t now) noexcept {
        if (!ticket) return;
        if (updateDepth) --updateDepth;
        updating = updateDepth != 0;
        if (ticket != serial) { completed = allowed = false; return; }
        completed = returned && identityValid && allowed && convertedOk && conversions == 1 &&
            !updating && sameOwnershipPoint(converted, nativePoint);
        completedTick = completed ? now : 0;
    }
    void scopeInvocation() noexcept {
        if (scopeCalls != std::numeric_limits<std::uint64_t>::max()) ++scopeCalls;
    }
    OwnershipMatch match(std::uintptr_t identity, const OwnershipPoint& point,
                         std::uint64_t now) const noexcept {
        if (updating) return OwnershipMatch::Updating;
        if (!completed) return OwnershipMatch::NoCompletion;
        if (identity != input) return OwnershipMatch::WrongInput;
        if (now < completedTick || now - completedTick > 100) return OwnershipMatch::Stale;
        if (scopeCalls != 1) return OwnershipMatch::RepeatedScope;
        if (!sameOwnershipPoint(converted, point)) return OwnershipMatch::ChangedPoint;
        return OwnershipMatch::Completed;
    }
};

} // namespace bone_eater::input
