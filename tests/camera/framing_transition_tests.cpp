#include "camera/framing_transition.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace bone_eater::camera;

namespace {
void require(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define REQUIRE(expression) require((expression), #expression, __LINE__)
constexpr FramingVector origin {0, 0, 0, 1};
constexpr FramingVector target {0, 0, 10, 1};
constexpr FramingVector up {0, 1, 0, 1};
constexpr double dt = 1.0 / 60.0;

void createHeldOffset(FramingTransitionState& state) {
    REQUIRE(state.certifyEarly(0)); // Integration verifies the native commit first.
    for (int i = 0; i < 30; ++i)
        state.advanceEarly({{0}, FramingMode::AimReframe, false}, dt);
    state.advanceEarly({{1}, FramingMode::AimReframe, true}, dt);
    REQUIRE(state.policy.state().offsetRadians > 0.1);
    REQUIRE(state.policy.state().scopePanLocked);
    REQUIRE(state.certifyEarly(state.policy.state().offsetRadians));
}

void requireNeutralTransition(const FramingTransitionState& state) {
    REQUIRE(!state.earlyCertified && state.certifiedOffset == 0);
    REQUIRE(state.neutralEarlyRequired);
    REQUIRE(state.policy.state().offsetRadians == 0);
    REQUIRE(state.policy.state().requestedOffsetRadians == 0);
    REQUIRE(!state.policy.state().scopePanLocked);
}

void sampledCinematicRollThenRecovery() {
    FramingTransitionState state;
    createHeldOffset(state);
    // Actual PID27492 early tuple, framing-v1 sequence3013. This transition
    // permanently disabled the previous implementation despite finite data.
    const FramingVector p {-472.141418f, -7979.32324f, -1286.42371f, 1};
    const FramingVector t {8089.42627f, -3339.00659f, -3151.15503f, 1};
    const auto cinematic = pitchTarget(p, t, up, -0.0476423092, state.certifiedOffset);
    REQUIRE(cinematic.status == PitchTargetStatus::UnsupportedRoll);
    REQUIRE(framingPoseDisposition(cinematic.status) == FramingPoseDisposition::SkipNative);
    REQUIRE(std::memcmp(cinematic.target.data(), t.data(), sizeof(t)) == 0);
    state.resetForNativeTransition();
    requireNeutralTransition(state);
    // A late call has no certificate. Repeated unsupported cinematic poses
    // remain native; none accumulates player pitch or revives a held lock.
    for (int i = 0; i < 60; ++i) {
        state.advanceEarly({{0}, FramingMode::AimReframe, true}, dt);
        REQUIRE(!pitchTarget(p, t, up, -0.24, state.policy.state().offsetRadians).accepted());
        state.resetForNativeTransition();
        requireNeutralTransition(state);
    }
    // Supported authored motion resumes at exactly its own target. The old
    // held offset does not jump back in, even if the button remained held.
    const FramingVector nextP {100, 20, -10, 1}, nextT {105, 22, 10, -0.0f};
    state.advanceEarly({{0}, FramingMode::AimReframe, true}, dt);
    const auto neutral = pitchTarget(nextP, nextT, up, 0, state.policy.state().offsetRadians);
    REQUIRE(neutral.accepted());
    REQUIRE(std::memcmp(neutral.target.data(), nextT.data(), sizeof(nextT)) == 0);
    REQUIRE(!state.earlyCertified); // Still cannot reuse before native confirmation.
    REQUIRE(state.certifyEarly(0));
    REQUIRE(state.earlyCertified && state.certifiedOffset == 0 && !state.neutralEarlyRequired);
    state.advanceEarly({{0}, FramingMode::AimReframe, true}, dt);
    REQUIRE(state.policy.state().scopePanLocked && state.policy.state().offsetRadians == 0);
    state.invalidateCertificate();
    state.advanceEarly({{0}, FramingMode::AimReframe, false}, dt);
    const double resumed = state.policy.state().offsetRadians;
    REQUIRE(resumed > 0 && resumed <= state.policy.config().maxSpeedRadiansPerSecond * dt);
    REQUIRE(pitchTarget(nextP, nextT, up, 0, resumed).accepted());
    REQUIRE(state.certifyEarly(resumed));
}

void latePoseRejectionRevokesEarlierOffset() {
    for (const bool early : {true, false}) {
        FramingTransitionState state;
        createHeldOffset(state);
        if (early) state.invalidateCertificate();
        // A late battle tuple can itself become unsupported after the early
        // native call. Never apply stale pitch just to preserve a certificate.
        const auto result = pitchTarget(origin, origin, up, 0, state.policy.state().offsetRadians);
        REQUIRE(result.status == PitchTargetStatus::DegenerateTarget);
        REQUIRE(framingPoseDisposition(result.status) == FramingPoseDisposition::SkipNative);
        state.resetForNativeTransition();
        requireNeutralTransition(state);
        REQUIRE(!state.certifyEarly(0.05));
        requireNeutralTransition(state);
    }
}

void allRepresentabilityLimitsSkipUnchanged() {
    const float large = std::numeric_limits<float>::max();
    const std::vector<PitchTargetResult> limits {
        pitchTarget(origin, target, up, 0.1, 0.1),
        pitchTarget(origin, origin, up, 0, 0.1),
        pitchTarget(origin, target, {0, 0, 0, 1}, 0, 0.1),
        pitchTarget(origin, {0, 10, 10, 1}, up, 0, 1.0),
        pitchTarget({0, large, 0, 1}, {0, large, large, 1}, up, 0, 0.1),
    };
    for (const auto& result : limits) {
        REQUIRE(framingPoseDisposition(result.status) == FramingPoseDisposition::SkipNative);
        FramingTransitionState state;
        createHeldOffset(state);
        state.resetForNativeTransition();
        requireNeutralTransition(state);
        state.advanceEarly({{1}, FramingMode::AimReframe, false}, dt);
        REQUIRE(pitchTarget(origin, target, up, 0, state.policy.state().offsetRadians).accepted());
        REQUIRE(state.certifyEarly(0));
    }
}

void internalFailuresAreNotCinematicSkips() {
    REQUIRE(framingPoseDisposition(PitchTargetStatus::Accepted) == FramingPoseDisposition::Accept);
    for (const auto status : {PitchTargetStatus::NonFinite, PitchTargetStatus::InvalidPitch,
                              static_cast<PitchTargetStatus>(999)})
        REQUIRE(framingPoseDisposition(status) == FramingPoseDisposition::InvariantFailure);
    FramingTransitionState state;
    for (const double invalid : {0.01, -0.01, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        REQUIRE(!state.certifyEarly(invalid));
        requireNeutralTransition(state);
    }
    REQUIRE(state.certifyEarly(0));
    REQUIRE(!state.certifyEarly(state.policy.config().maxOffsetRadians + 0.01));
}

void ordinaryFrameInvalidationPreservesHeldOffset() {
    FramingTransitionState state;
    createHeldOffset(state);
    const auto held = state.policy.state().offsetRadians;
    for (int i = 0; i < 120; ++i) {
        state.invalidateCertificate();
        REQUIRE(!state.earlyCertified && state.certifiedOffset == 0);
        state.advanceEarly({{1}, FramingMode::AimReframe, true}, dt);
        REQUIRE(state.policy.state().offsetRadians == held);
        const float movement = static_cast<float>(i);
        const FramingVector p {movement, 0, 0, 1}, t {movement + 1, 1, 10, 1};
        const auto result = pitchTarget(p, t, up, 0, held);
        REQUIRE(result.accepted() && result.target != t);
        REQUIRE(state.certifyEarly(held));
    }
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, void(*)()>> cases {
        {"sampled cinematic roll and neutral recovery", sampledCinematicRollThenRecovery},
        {"early and late rejection revoke held certificate", latePoseRejectionRevokesEarlierOffset},
        {"pose representability limits recover", allRepresentabilityLimitsSkipUnchanged},
        {"internal failures remain distinct", internalFailuresAreNotCinematicSkips},
        {"ordinary held frames keep script motion", ordinaryFrameInvalidationPreservesHeldOffset},
    };
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << cases.size() << " framing transition cases passed\n";
}
