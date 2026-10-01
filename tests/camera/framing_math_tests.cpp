#include "camera/framing_math.h"
#include "camera/vertical_pan_policy.h"

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
constexpr double pi = 3.14159265358979323846;
const FramingVector origin {0, 0, 0, 1};
const FramingVector target {0, 0, 10, 1};
const FramingVector up {0, 1, 0, 1};

void require(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define REQUIRE(expression) require((expression), #expression, __LINE__)
void near(double actual, double expected, double tolerance = 2.0e-6) {
    REQUIRE(std::isfinite(actual));
    REQUIRE(std::abs(actual - expected) <= tolerance);
}
double distance(const FramingVector& a, const FramingVector& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}
void rejectedUnchanged(const FramingVector& p, const FramingVector& t, const FramingVector& u,
                       double roll, double pitch, PitchTargetStatus expected) {
    const auto result = pitchTarget(p, t, u, roll, pitch);
    REQUIRE(!result.accepted());
    REQUIRE(result.status == expected);
    REQUIRE(std::memcmp(result.target.data(), t.data(), sizeof(t)) == 0);
}

void angleSignDistanceAndW() {
    const FramingVector p {0, 0, 0, -3};
    const FramingVector t {0, 0, 10, -0.0f};
    const auto positive = pitchTarget(p, t, up, 0, pi / 6);
    const auto negative = pitchTarget(p, t, up, 0, -pi / 6);
    REQUIRE(positive.accepted() && negative.accepted());
    near(positive.target[0], 0);
    near(positive.target[1], 5);
    near(positive.target[2], std::sqrt(75.0));
    near(negative.target[1], -5);
    near(negative.target[2], positive.target[2]);
    near(distance(p, positive.target), 10);
    REQUIRE(std::signbit(positive.target[3]));
    REQUIRE(p[3] == -3 && t[1] == 0); // Inputs were not changed.
}

void neutralIsByteExact() {
    const FramingVector p {-13734.8545f, -23884.2695f, 185.448746f, 1};
    const FramingVector t {4725.46875f, -25445.3594f, -92.0093079f, -0.0f};
    for (const double pitch : {0.0, -0.0}) {
        const auto result = pitchTarget(p, t, up, 0, pitch);
        REQUIRE(result.accepted());
        REQUIRE(std::memcmp(result.target.data(), t.data(), sizeof(t)) == 0);
    }
}

void authoredUpIsOrthogonalized() {
    const auto canonical = pitchTarget(origin, target, up, 0, 0.2);
    const auto skewed = pitchTarget(origin, target, {0, 20, 70, 5}, 0, 0.2);
    REQUIRE(canonical.accepted() && skewed.accepted());
    for (std::size_t i = 0; i < 4; ++i) near(skewed.target[i], canonical.target[i]);
    const auto turned = pitchTarget(origin, target, {1, 0, 0, 1}, 0, 0.2);
    REQUIRE(turned.accepted());
    near(turned.target[0], canonical.target[1]);
    near(turned.target[1], 0);
    near(turned.target[2], canonical.target[2]);
}

void translationAndScalePreserveGeometry() {
    const auto base = pitchTarget(origin, target, up, 0, 0.14);
    const FramingVector p {100, -80, 50, 1};
    const FramingVector t {100, -80, 80, 1};
    const auto transformed = pitchTarget(p, t, up, 0, 0.14);
    REQUIRE(base.accepted() && transformed.accepted());
    for (std::size_t i = 0; i < 3; ++i)
        near(static_cast<double>(transformed.target[i]) - p[i], base.target[i] * 3.0, 1.0e-5);
    near(distance(p, transformed.target), 30, 1.0e-5);
}

void invalidInputsFailClosed() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float invalid : {nan, inf, -inf}) {
        for (std::size_t component = 0; component < 4; ++component) {
            auto p = origin, t = target, u = up;
            p[component] = invalid;
            rejectedUnchanged(p, t, u, 0, 0.1, PitchTargetStatus::NonFinite);
            p = origin; t[component] = invalid;
            rejectedUnchanged(p, t, u, 0, 0.1, PitchTargetStatus::NonFinite);
            t = target; u[component] = invalid;
            rejectedUnchanged(p, t, u, 0, 0.1, PitchTargetStatus::NonFinite);
        }
        rejectedUnchanged(origin, target, up, invalid, 0.1, PitchTargetStatus::NonFinite);
        rejectedUnchanged(origin, target, up, 0, invalid, PitchTargetStatus::NonFinite);
    }
    for (const double roll : {-0.01, 0.01})
        rejectedUnchanged(origin, target, up, roll, 0.1, PitchTargetStatus::UnsupportedRoll);
    REQUIRE(pitchTarget(origin, target, up, 1.0e-5, 0.1).accepted());
    for (const double pitch : {-pi / 2, pi / 2, pi, 10.0})
        rejectedUnchanged(origin, target, up, 0, pitch, PitchTargetStatus::InvalidPitch);
}

void degenerateAndPoleCrossingFailClosed() {
    rejectedUnchanged(origin, origin, up, 0, 0, PitchTargetStatus::DegenerateTarget);
    rejectedUnchanged(origin, {0, 0, 1.0e-7f, 1}, up, 0, 0.1, PitchTargetStatus::DegenerateTarget);
    rejectedUnchanged(origin, target, {0, 0, 0, 1}, 0, 0.1, PitchTargetStatus::DegenerateUp);
    rejectedUnchanged(origin, target, {0, 0, 2, 1}, 0, 0.1, PitchTargetStatus::UpParallel);
    rejectedUnchanged(origin, target, {0, 1.0e-6f, -1, 1}, 0, 0.1, PitchTargetStatus::UpParallel);
    // A valid initial basis can become singular against unchanged native up.
    rejectedUnchanged(origin, {0, 10, 10, 1}, up, 0, pi / 4, PitchTargetStatus::UpParallel);
    // Passing through a pole and ending beyond it is also unsafe: unchanged
    // native up would reverse the camera's right axis and flip the view.
    rejectedUnchanged(origin, {0, 10, 10, 1}, up, 0, pi / 3, PitchTargetStatus::UpParallel);
    rejectedUnchanged(origin, {0, -10, 10, 1}, up, 0, -pi / 3, PitchTargetStatus::UpParallel);
}

void unrepresentableOutputFailsClosed() {
    const float large = std::numeric_limits<float>::max();
    rejectedUnchanged({0, large, 0, 1}, {0, large, large, 1}, up, 0, 0.1,
                      PitchTargetStatus::OutputPrecision);
    // At a large origin, a one-unit target cannot retain a meaningful pitch
    // in float coordinates: do not silently return a distorted direction.
    rejectedUnchanged({1.0e8f, 1.0e8f, 0, 1}, {1.0e8f, 1.0e8f, 1, 1}, up, 0, 0.1,
                      PitchTargetStatus::OutputPrecision);
}

void sampledNativeTupleRetainsDistance() {
    const FramingVector p {-13734.8545f, -23884.2695f, 185.448746f, 1};
    const FramingVector t {4725.46875f, -25445.3594f, -92.0093079f, 1};
    for (int i = -80; i <= 80; ++i) {
        const auto result = pitchTarget(p, t, up, 0, i * pi / 1800.0);
        REQUIRE(result.accepted());
        near(distance(p, result.target) / distance(p, t), 1.0, 1.0e-7);
        REQUIRE(result.target[3] == 1);
    }
}

void heldOffsetRetainsScriptMotionWithoutCompounding() {
    VerticalPanPolicy policy;
    for (int i = 0; i < 30; ++i)
        policy.update({{0.0}, FramingMode::AimReframe, false}, 1.0 / 60.0);
    const double heldPitch = policy.state().offsetRadians;
    REQUIRE(heldPitch > 0);
    FramingVector previous {};
    for (int i = 0; i < 100; ++i) {
        const auto state = policy.update({{1.0}, FramingMode::AimReframe, true}, 1.0 / 60.0).state;
        REQUIRE(state.scopePanLocked && state.offsetRadians == heldPitch);
        const float movement = static_cast<float>(i);
        // Translation and native direction both advance during scope hold.
        const FramingVector p {movement, 2 * movement, 0, 1};
        const FramingVector t {movement + movement * 0.1f, 2 * movement, 10, 1};
        const auto current = pitchTarget(p, t, up, 0, state.offsetRadians);
        const auto repeatedCommit = pitchTarget(p, t, up, 0, state.offsetRadians);
        REQUIRE(current.accepted() && repeatedCommit.accepted());
        REQUIRE(current.target == repeatedCommit.target);
        near(distance(p, current.target) / distance(p, t), 1.0, 2.0e-6);
        const double measuredPitch = std::asin((current.target[1] - p[1]) / distance(p, current.target));
        near(measuredPitch, heldPitch, 2.0e-6);
        if (i) REQUIRE(current.target != previous);
        previous = current.target;
    }
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, void(*)()>> cases {
        {"angle sign, distance and W", angleSignDistanceAndW},
        {"neutral is byte exact", neutralIsByteExact},
        {"authored up is orthogonalized", authoredUpIsOrthogonalized},
        {"translation and scale", translationAndScalePreserveGeometry},
        {"invalid inputs fail closed", invalidInputsFailClosed},
        {"degenerate and pole crossing", degenerateAndPoleCrossingFailClosed},
        {"unrepresentable output", unrepresentableOutputFailsClosed},
        {"sampled native tuple", sampledNativeTupleRetainsDistance},
        {"held offset retains script motion", heldOffsetRetainsScriptMotionWithoutCompounding},
    };
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << cases.size() << " framing math cases passed\n";
    return 0;
}
