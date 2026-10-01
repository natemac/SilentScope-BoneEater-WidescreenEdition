#include "camera/vertical_pan_policy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bone_eater::camera {
namespace {

constexpr double kHalfPi = 1.57079632679489661923;

bool positiveFinite(double value) {
    return std::isfinite(value) && value > 0.0;
}

void validateConfig(const VerticalPanConfig& config) {
    if (!std::isfinite(config.maxOffsetRadians) ||
        config.maxOffsetRadians < 0.0 || config.maxOffsetRadians >= kHalfPi ||
        !std::isfinite(config.deadzone) ||
        config.deadzone < 0.0 || config.deadzone >= 1.0 ||
        !positiveFinite(config.responseSeconds) ||
        !positiveFinite(config.maxSpeedRadiansPerSecond) ||
        !positiveFinite(config.maxDeltaSeconds)) {
        throw std::invalid_argument("Invalid vertical pan configuration");
    }
}

double requestedOffset(const VerticalPanInput& input,
                       const VerticalPanConfig& config) {
    if (input.mode == FramingMode::FixedWide) {
        return 0.0;
    }

    const double centered = 1.0 - 2.0 * input.aim.normalizedY;
    const double magnitude = std::abs(centered);
    if (magnitude <= config.deadzone) {
        return 0.0;
    }

    const double fraction = std::clamp(
        (magnitude - config.deadzone) / (1.0 - config.deadzone), 0.0, 1.0);
    return std::copysign(fraction * config.maxOffsetRadians, centered);
}

// Exact solution, for a constant requested offset during this update, to:
//   d(offset)/dt = clamp((requested - offset) / tau, -speed, speed)
// The linear segment transitions into exponential easing without overshoot.
// Unlike applying an exponential and then clipping its delta, this solution
// preserves timestep subdivision equivalence for a constant target.
double advanceOffset(double current, double requested, double dt,
                     const VerticalPanConfig& config) {
    const double error = requested - current;
    const double distance = std::abs(error);
    if (distance == 0.0) {
        return current;
    }

    const double speed = config.maxSpeedRadiansPerSecond;
    const double tau = config.responseSeconds;
    const double exponentialDistance = speed * tau;
    double movement = 0.0;

    if (distance > exponentialDistance) {
        const double linearDistance = distance - exponentialDistance;
        const double linearSeconds = linearDistance / speed;
        if (dt <= linearSeconds) {
            movement = speed * dt;
        } else {
            movement = linearDistance + exponentialDistance *
                -std::expm1(-(dt - linearSeconds) / tau);
        }
    } else {
        movement = distance * -std::expm1(-dt / tau);
    }

    movement = std::clamp(movement, 0.0, distance);
    const double next = current + std::copysign(movement, error);
    return std::clamp(next, std::min(current, requested),
                      std::max(current, requested));
}

}  // namespace

VerticalPanPolicy::VerticalPanPolicy(const VerticalPanConfig& config)
    : config_(config) {
    validateConfig(config_);
}

VerticalPanUpdate VerticalPanPolicy::update(const VerticalPanInput& input,
                                          double dtSeconds) {
    if (!std::isfinite(dtSeconds) || dtSeconds < 0.0 ||
        dtSeconds > config_.maxDeltaSeconds) {
        return {UpdateStatus::InvalidDeltaTime, state_};
    }
    if (!std::isfinite(input.aim.normalizedY) || input.aim.normalizedY < 0.0 ||
        input.aim.normalizedY > 1.0) {
        return {UpdateStatus::InvalidAim, state_};
    }
    if (input.mode != FramingMode::FixedWide &&
        input.mode != FramingMode::AimReframe) {
        return {UpdateStatus::InvalidMode, state_};
    }
    if (dtSeconds == 0.0) {
        return {UpdateStatus::NoTimeElapsed, state_};
    }

    state_.requestedOffsetRadians = requestedOffset(input, config_);
    state_.scopePanLocked = input.scopeHeld;
    if (!input.scopeHeld) {
        state_.offsetRadians = advanceOffset(
            state_.offsetRadians, state_.requestedOffsetRadians,
            dtSeconds, config_);
    }
    return {UpdateStatus::Accepted, state_};
}

void VerticalPanPolicy::resetScene() noexcept {
    state_ = {};
}

}  // namespace bone_eater::camera
