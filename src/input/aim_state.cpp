#include "aim_state.h"
#include "input/sample_age.h"

namespace bone_eater::input {

namespace {
AimState& runtimeAimState() {
    // Native callbacks and selected-input atexit cleanup can outlive ordinary
    // static destructors. Keep this publication mutex alive until process exit.
    static auto* state = new AimState;
    return *state;
}
}  // namespace

void AimState::publish(std::uint16_t x, std::uint16_t y,
                       bool scopeRight, bool scopeLeft, bool trigger) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.rawX = x;
    snapshot_.rawY = y;
    snapshot_.normalizedX = static_cast<double>(x) / 65535.0;
    snapshot_.normalizedY = static_cast<double>(y) / 65535.0;
    snapshot_.scopeRightHeld = scopeRight;
    snapshot_.scopeLeftHeld = scopeLeft;
    snapshot_.triggerHeld = trigger;
    ++snapshot_.generation;
    snapshot_.sampledAt = std::chrono::steady_clock::now();
    snapshot_.valid = true;
    snapshot_.sourceMode = GunSourceMode::Legacy;
    snapshot_.sourceState = GunSourceState::Active;
    snapshot_.aimKnown = snapshot_.armed = true;
    snapshot_.sourceIdentity = {};
    snapshot_.sourceMaximumAge = std::chrono::milliseconds(0);
    snapshot_.sourceConsumptionGeneration = snapshot_.cabinetGeneration = 0;
}

void AimState::publishSelected(const GunSourceOutput& input, std::chrono::milliseconds maximumReportAge,
        GunSourceClock::time_point consumedAt, std::uint64_t cabinetGeneration) {
    const std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.sourceMode = GunSourceMode::SelectedHid;
    snapshot_.sourceState = input.state;
    snapshot_.aimKnown = input.aim.known;
    snapshot_.armed = input.armed;
    snapshot_.sourceIdentity = input.aim.identity;
    snapshot_.sourceMaximumAge = maximumReportAge;
    snapshot_.sourceConsumptionGeneration = input.consumptionGeneration;
    snapshot_.cabinetGeneration = cabinetGeneration;
    snapshot_.rawX = input.aim.known ? input.aim.x : std::uint16_t(0x7FFF);
    snapshot_.rawY = input.aim.known ? input.aim.y : std::uint16_t(0x7FFF);
    snapshot_.normalizedX = static_cast<double>(snapshot_.rawX) / 65535.0;
    snapshot_.normalizedY = static_cast<double>(snapshot_.rawY) / 65535.0;
    snapshot_.valid = input.mode == GunSourceMode::SelectedHid && input.aim.source == GunSourceMode::SelectedHid &&
        input.state == GunSourceState::Active && input.aim.known && input.armed && input.sourceUsable &&
        input.aim.identity.session && input.aim.identity.generation &&
        sampleAgeWithin(input.aim.identity.receivedAt, consumedAt, maximumReportAge);
    snapshot_.scopeRightHeld = snapshot_.valid && input.buttons.scopeRight;
    snapshot_.scopeLeftHeld = snapshot_.valid && input.buttons.scopeLeft;
    snapshot_.triggerHeld = snapshot_.valid && input.buttons.trigger;
    ++snapshot_.generation;
    snapshot_.sampledAt = consumedAt;
}

AimSnapshot AimState::read() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void publishAimSample(std::uint16_t x, std::uint16_t y,
                      bool scopeRight, bool scopeLeft, bool trigger) {
    runtimeAimState().publish(x, y, scopeRight, scopeLeft, trigger);
}

void publishSelectedAimSample(const GunSourceOutput& input, std::chrono::milliseconds maximumReportAge,
        GunSourceClock::time_point consumedAt, std::uint64_t cabinetGeneration) {
    runtimeAimState().publishSelected(input, maximumReportAge, consumedAt, cabinetGeneration);
}

bool aimUsableAt(const AimSnapshot& sample, GunSourceClock::time_point now,
        std::chrono::milliseconds maximumCabinetAge) noexcept {
    if (!sample.valid || !sampleAgeWithin(sample.sampledAt, now, maximumCabinetAge)) return false;
    if (sample.sourceMode == GunSourceMode::Legacy) return true;
    return sample.sourceMode == GunSourceMode::SelectedHid && sample.aimKnown && sample.armed &&
        sample.sourceState == GunSourceState::Active && sample.sourceIdentity.session && sample.sourceIdentity.generation &&
        sample.sourceIdentity.receivedAt <= sample.sampledAt &&
        sampleAgeWithin(sample.sourceIdentity.receivedAt, now, sample.sourceMaximumAge);
}

AimSnapshot readAimSnapshot() {
    return runtimeAimState().read();
}

}  // namespace bone_eater::input
