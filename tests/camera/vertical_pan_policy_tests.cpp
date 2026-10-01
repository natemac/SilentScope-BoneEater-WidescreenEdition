#include "camera/vertical_pan_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bone_eater::camera;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error(std::string("line ") + std::to_string(line) +
                                 ": " + expression);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void near(double actual, double expected, double tolerance = 1e-12) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::ostringstream message;
        message.precision(17);
        message << "expected " << expected << ", received " << actual
                << ", tolerance " << tolerance;
        throw std::runtime_error(message.str());
    }
}

bool sameState(const VerticalPanState& a, const VerticalPanState& b) {
    return a.offsetRadians == b.offsetRadians &&
           a.requestedOffsetRadians == b.requestedOffsetRadians &&
           a.scopePanLocked == b.scopePanLocked;
}

VerticalPanState step(VerticalPanPolicy& policy, double y,
                      double dt = 1.0 / 60.0, bool held = false,
                      FramingMode mode = FramingMode::AimReframe) {
    const auto result = policy.update({{y}, mode, held}, dt);
    REQUIRE(result.status == UpdateStatus::Accepted);
    REQUIRE(sameState(result.state, policy.state()));
    return result.state;
}

void fixedWideStartsNeutral() {
    VerticalPanPolicy policy;
    for (int i = 0; i != 120; ++i) {
        const auto state = step(policy, i % 2 ? 0.0 : 1.0,
                                1.0 / 60.0, false, FramingMode::FixedWide);
        REQUIRE(state.offsetRadians == 0.0);
        REQUIRE(state.requestedOffsetRadians == 0.0);
    }
}

void deadzoneAndMappingAreContinuousAndSymmetric() {
    VerticalPanConfig config;
    config.deadzone = 0.25;
    config.maxOffsetRadians = 0.20;
    VerticalPanPolicy policy(config);
    for (const double y : {0.375, 0.4, 0.5, 0.6, 0.625}) {
        REQUIRE(step(policy, y).requestedOffsetRadians == 0.0);
    }
    near(step(policy, 0.0).requestedOffsetRadians, 0.20);
    near(step(policy, 1.0).requestedOffsetRadians, -0.20);
    near(step(policy, 0.1875).requestedOffsetRadians, 0.10);
    near(step(policy, 0.8125).requestedOffsetRadians, -0.10);
    const double justOutside = step(policy, 0.375 - 1e-9).requestedOffsetRadians;
    REQUIRE(justOutside > 0.0);
    REQUIRE(justOutside < 1e-8);
}

void analyticRateAndExponentialSegmentsAgree() {
    VerticalPanConfig config;
    config.maxOffsetRadians = 0.30;
    config.deadzone = 0.0;
    config.maxSpeedRadiansPerSecond = 0.50;
    config.responseSeconds = 0.10;
    config.maxDeltaSeconds = 1.0;
    VerticalPanPolicy split(config);
    near(step(split, 0.0, 0.4).offsetRadians, 0.20);
    near(step(split, 0.0, 0.1).offsetRadians, 0.25);
    const double expected = 0.30 - 0.05 * std::exp(-1.0);
    near(step(split, 0.0, 0.1).offsetRadians, expected);
    VerticalPanPolicy oneStep(config);
    near(step(oneStep, 0.0, 0.6).offsetRadians, expected);
}

double runAtRate(int hz) {
    VerticalPanPolicy policy;
    for (int i = 0; i != hz / 2; ++i) {
        step(policy, 0.0, 1.0 / hz);
    }
    for (int i = 0; i != hz / 2; ++i) {
        step(policy, 0.85, 1.0 / hz);
    }
    return policy.state().offsetRadians;
}

void constantIntentIsIndependentOfTimestepSubdivision() {
    const double reference = runAtRate(20);
    near(runAtRate(60), reference);
    near(runAtRate(120), reference);
    near(runAtRate(144), reference);
}

void scopeLockFreezesOffsetAndReleaseDoesNotResetIt() {
    VerticalPanPolicy policy;
    for (int i = 0; i != 10; ++i) {
        step(policy, 0.0);
    }
    const double beforeHold = policy.state().offsetRadians;
    REQUIRE(beforeHold > 0.0);
    for (int i = 0; i != 100; ++i) {
        const auto held = step(policy, i % 2 ? 0.0 : 1.0, 1.0 / 60.0, true);
        REQUIRE(held.offsetRadians == beforeHold);
        REQUIRE(held.scopePanLocked);
    }
    const auto released = step(policy, 1.0);
    REQUIRE(!released.scopePanLocked);
    REQUIRE(released.offsetRadians < beforeHold);
    REQUIRE(released.offsetRadians > 0.0);
    REQUIRE(beforeHold - released.offsetRadians <=
            policy.config().maxSpeedRadiansPerSecond / 60.0 + 1e-15);
}

void fixedWideTransitionAndScopeLockDoNotSnap() {
    VerticalPanPolicy policy;
    for (int i = 0; i != 30; ++i) {
        step(policy, 0.0);
    }
    const double beforeSwitch = policy.state().offsetRadians;
    const auto locked = step(policy, 1.0, 1.0 / 60.0, true,
                             FramingMode::FixedWide);
    REQUIRE(locked.offsetRadians == beforeSwitch);
    REQUIRE(locked.requestedOffsetRadians == 0.0);
    double previous = beforeSwitch;
    for (int i = 0; i != 300; ++i) {
        const auto returning = step(policy, 1.0, 1.0 / 60.0, false,
                                    FramingMode::FixedWide);
        REQUIRE(returning.offsetRadians <= previous);
        REQUIRE(returning.offsetRadians >= 0.0);
        REQUIRE(previous - returning.offsetRadians <=
                policy.config().maxSpeedRadiansPerSecond / 60.0 + 1e-15);
        previous = returning.offsetRadians;
    }
    near(previous, 0.0);
}

void rejectedUpdatesAndZeroTimeAreAtomic() {
    VerticalPanPolicy policy;
    step(policy, 0.0);
    step(policy, 0.0, 1.0 / 60.0, true);
    const auto before = policy.state();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const auto check = [&](const VerticalPanInput& input, double dt,
                           UpdateStatus expected) {
        const auto result = policy.update(input, dt);
        REQUIRE(result.status == expected);
        REQUIRE(sameState(result.state, before));
        REQUIRE(sameState(policy.state(), before));
    };
    const VerticalPanInput valid{{1.0}, FramingMode::FixedWide, false};
    for (const double dt : {-0.01, 0.100001, nan, infinity}) {
        check(valid, dt, UpdateStatus::InvalidDeltaTime);
    }
    for (const double y : {-0.0001, 1.0001, nan, infinity}) {
        check({{y}, FramingMode::AimReframe, false}, 0.01,
              UpdateStatus::InvalidAim);
    }
    check({{0.5}, static_cast<FramingMode>(99), false}, 0.01,
          UpdateStatus::InvalidMode);
    check(valid, 0.0, UpdateStatus::NoTimeElapsed);
}

void sceneResetClearsPriorSceneAndLock() {
    VerticalPanPolicy policy;
    step(policy, 0.0);
    step(policy, 0.0, 1.0 / 60.0, true);
    REQUIRE(policy.state().offsetRadians > 0.0);
    policy.resetScene();
    REQUIRE(sameState(policy.state(), VerticalPanState{}));
    const auto heldInNewScene = step(policy, 1.0, 1.0 / 60.0, true);
    REQUIRE(heldInNewScene.offsetRadians == 0.0);
    REQUIRE(heldInNewScene.scopePanLocked);
    REQUIRE(step(policy, 1.0).offsetRadians < 0.0);
}

void invalidConfigurationsAreRejected() {
    std::vector<VerticalPanConfig> configs;
    const auto add = [&](double VerticalPanConfig::* field, double value) {
        VerticalPanConfig config;
        config.*field = value;
        configs.push_back(config);
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const double value : {-0.1, 1.5707963267948966, nan, infinity}) {
        add(&VerticalPanConfig::maxOffsetRadians, value);
    }
    for (const double value : {-0.1, 1.0, nan, infinity}) {
        add(&VerticalPanConfig::deadzone, value);
    }
    for (const auto field : {&VerticalPanConfig::responseSeconds,
                            &VerticalPanConfig::maxSpeedRadiansPerSecond,
                            &VerticalPanConfig::maxDeltaSeconds}) {
        for (const double value : {0.0, -0.1, nan, infinity}) {
            add(field, value);
        }
    }
    for (const auto& config : configs) {
        bool rejected = false;
        try {
            const VerticalPanPolicy policy(config);
            (void)policy;
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE(rejected);
    }
}

void zeroCapDisablesPanAndTinyTimeIsStable() {
    VerticalPanConfig disabled;
    disabled.maxOffsetRadians = 0.0;
    VerticalPanPolicy noPan(disabled);
    REQUIRE(step(noPan, 0.0).offsetRadians == 0.0);
    REQUIRE(step(noPan, 1.0).offsetRadians == 0.0);

    VerticalPanConfig config;
    config.deadzone = 0.0;
    config.maxOffsetRadians = 0.20;
    config.maxSpeedRadiansPerSecond = 1.0;
    config.responseSeconds = 0.50;
    VerticalPanPolicy policy(config);
    const auto state = step(policy, 0.4, 1e-12);
    REQUIRE(state.offsetRadians > 0.0);
    near(state.offsetRadians, 0.04 * -std::expm1(-2e-12), 1e-25);
}

void deterministicStressPreservesBoundsAndSpeed() {
    VerticalPanPolicy policy;
    std::uint32_t seed = 0x3a51c7u;
    const auto randomUnit = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed) /
               static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    };
    for (int i = 0; i != 10000; ++i) {
        const double y = i % 3 == 0 ? 0.0 : (i % 3 == 1 ? 1.0 : randomUnit());
        const double dt = 0.001 + 0.099 * randomUnit();
        const bool held = i % 7 < 3;
        const auto mode = i % 11 == 0 ? FramingMode::FixedWide
                                      : FramingMode::AimReframe;
        const double previous = policy.state().offsetRadians;
        const auto state = step(policy, y, dt, held, mode);
        REQUIRE(std::isfinite(state.offsetRadians));
        REQUIRE(std::abs(state.offsetRadians) <= policy.config().maxOffsetRadians);
        REQUIRE(std::abs(state.offsetRadians - previous) <=
                policy.config().maxSpeedRadiansPerSecond * dt + 1e-14);
        if (held) {
            REQUIRE(state.offsetRadians == previous);
        } else {
            REQUIRE(state.offsetRadians >=
                    std::min(previous, state.requestedOffsetRadians));
            REQUIRE(state.offsetRadians <=
                    std::max(previous, state.requestedOffsetRadians));
        }
    }
}

}  // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"fixed wide starts neutral", fixedWideStartsNeutral},
        {"deadzone and symmetric mapping", deadzoneAndMappingAreContinuousAndSymmetric},
        {"analytic rate and exponential segments", analyticRateAndExponentialSegmentsAgree},
        {"timestep subdivision", constantIntentIsIndependentOfTimestepSubdivision},
        {"scope lock and release", scopeLockFreezesOffsetAndReleaseDoesNotResetIt},
        {"fixed-wide transition under scope lock", fixedWideTransitionAndScopeLockDoNotSnap},
        {"atomic rejected and zero-time updates", rejectedUpdatesAndZeroTimeAreAtomic},
        {"explicit scene reset", sceneResetClearsPriorSceneAndLock},
        {"configuration validation", invalidConfigurationsAreRejected},
        {"zero cap and tiny timestep", zeroCapDisablesPanAndTinyTimeIsStable},
        {"deterministic stress bounds", deterministicStressPreservesBoundsAndSpeed},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0])) << " cases, "
              << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
