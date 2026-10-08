#include "input/scope_control.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace bone_eater::input {

void ScopeControl::reset() noexcept {
    state_ = {}; pressedAt_ = 0; openingPress_ = false;
}
ScopeControlSnapshot ScopeControl::update(const ScopeControlInput& input,
        const ScopeControlConfig& config) noexcept {
    const bool validConfig = config.holdMs >= 100 && config.holdMs <= 1000;
    const bool changed = state_.contextIdentity != input.contextIdentity ||
        state_.nativeScopeOwner != input.nativeScopeOwner || state_.nativeInputOwner != input.nativeInputOwner;
    const bool backwards = input.nowMs < state_.updatedAtMs;
    const bool stale = !backwards && state_.contextIdentity &&
        input.nowMs - state_.updatedAtMs > 250;
    if (!input.usable || !input.contextIdentity || !validConfig || changed || backwards || stale) reset();
    state_.updatedAtMs = input.nowMs;
    state_.contextIdentity = input.contextIdentity;
    state_.nativeScopeOwner = input.nativeScopeOwner;
    state_.nativeInputOwner = input.nativeInputOwner;
    if (!input.usable || !input.contextIdentity || !validConfig || backwards) return state_;
    if (!state_.armed) {
        state_.armed = !input.buttonsHeld;
        // Held-at-entry cannot become a new press after focus/context recovery.
        state_.buttonsHeld = input.buttonsHeld;
        return state_;
    }
    if (input.buttonsHeld && !state_.buttonsHeld) {
        pressedAt_ = input.nowMs;
        openingPress_ = !state_.enabled;
        state_.enabled = true;
        state_.higher = false;
    }
    const bool heldLong = state_.buttonsHeld && input.nowMs - pressedAt_ >= config.holdMs;
    if (input.buttonsHeld && heldLong) state_.higher = true;
    if (!input.buttonsHeld && state_.buttonsHeld) {
        // A release after a scheduling gap is still a hold if elapsed time
        // crossed the threshold, even without an intervening high-zoom sample.
        if ((!openingPress_ && !heldLong) || (heldLong && config.exitOnHoldRelease))
            state_.enabled = false;
        state_.higher = false;
        openingPress_ = false;
    }
    state_.buttonsHeld = input.buttonsHeld;
    return state_;
}

bool scopeZoomNeedsEdge(bool higher, std::uint32_t nativeMode) noexcept {
    // Physical playtest confirmed native mode0 is the stronger magnification.
    return (nativeMode == 0 && !higher) || (nativeMode == 1 && higher);
}

namespace {
bool environment(const char* name, char* value, std::size_t capacity) noexcept {
#ifdef _WIN32
    const auto length = GetEnvironmentVariableA(name, value, static_cast<DWORD>(capacity));
    return length > 0 && length < capacity;
#else
    const auto* source = std::getenv(name);
    if (!source || std::strlen(source) >= capacity) return false;
    std::strcpy(value, source); return true;
#endif
}
struct Shared {
    std::mutex mutex;
    ScopeControl control;
    ScopeControlConfig config;
    bool requested = false;
    Shared() {
        char mode[32] {}, hold[32] {}, release[32] {};
        if (environment("BONE_EATER_SCOPE_HOLD_RELEASE", release, sizeof(release)))
            config.exitOnHoldRelease = std::strcmp(release, "lower") != 0;
        requested = environment("BONE_EATER_SCOPE_MODE", mode, sizeof(mode)) &&
            std::strcmp(mode, "toggle_hold") == 0;
        if (environment("BONE_EATER_SCOPE_HOLD_MS", hold, sizeof(hold))) {
            char* end = nullptr;
            const auto value = std::strtoul(hold, &end, 10);
            if (end && !*end && value >= 100 && value <= 1000)
                config.holdMs = static_cast<std::uint32_t>(value);
        }
    }
};
Shared& shared() {
    // Native hooks and selected-input cleanup may publish during CRT teardown.
    // Keep the mutex and controller alive until process exit, like AimState.
    static Shared* value = new Shared;
    return *value;
}
}

void initializeScopeControlFromEnvironment() noexcept { try { (void)shared(); } catch (...) {} }
bool scopeControlRequested() noexcept { try { return shared().requested; } catch (...) { return false; } }
ScopeControlSnapshot publishScopeControl(const ScopeControlInput& input) noexcept {
    try {
        auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.requested) { s.control.reset(); return {}; }
        return s.control.update(input, s.config);
    } catch (...) { return {}; }
}
ScopeControlSnapshot readScopeControlSnapshot() noexcept {
    try { auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex); return s.control.read(); }
    catch (...) { return {}; }
}
void resetScopeControl() noexcept {
    try { auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex); s.control.reset(); }
    catch (...) {}
}
} // namespace bone_eater::input
