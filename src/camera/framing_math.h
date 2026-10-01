#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace bone_eater::camera {

using FramingVector = std::array<float, 4>;

enum class PitchTargetStatus {
    Accepted,
    NonFinite,
    UnsupportedRoll,
    InvalidPitch,
    DegenerateTarget,
    DegenerateUp,
    UpParallel,
    OutputPrecision,
};

struct PitchTargetResult {
    PitchTargetStatus status;
    FramingVector target;
    bool accepted() const noexcept { return status == PitchTargetStatus::Accepted; }
};

// Stateless look-at framing. Positive pitch moves the look direction toward
// authored up. Read fresh P/T/up each time, including while the policy's pitch
// offset is held: script motion and recoil must continue underneath the offset.
// This initial implementation intentionally supports only near-zero roll.
// Inputs are never modified; rejection returns the original target unchanged.
inline PitchTargetResult pitchTarget(const FramingVector& position,
                                     const FramingVector& target,
                                     const FramingVector& up,
                                     double rollRadians,
                                     double pitchRadians) noexcept {
    constexpr double halfPi = 1.57079632679489661923;
    constexpr double maxRoll = 1.0e-5;
    constexpr double minDistance = 1.0e-6; // Native world units.
    constexpr double minUpSine = 1.0e-4; // Reject an ill-conditioned look-at basis.
    constexpr double maxFloatError = 2.0e-5;
    const auto reject = [&](PitchTargetStatus status) noexcept {
        return PitchTargetResult {status, target};
    };
    if (!std::isfinite(rollRadians) || !std::isfinite(pitchRadians))
        return reject(PitchTargetStatus::NonFinite);
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(position[i]) || !std::isfinite(target[i]) ||
                !std::isfinite(up[i])) return reject(PitchTargetStatus::NonFinite);
    }
    if (std::abs(rollRadians) > maxRoll) return reject(PitchTargetStatus::UnsupportedRoll);
    if (std::abs(pitchRadians) >= halfPi) return reject(PitchTargetStatus::InvalidPitch);

    std::array<double, 3> direction {}, normalizedUp {}, pitchUp {}, rotated {};
    double distanceSquared = 0.0, upSquared = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        direction[i] = static_cast<double>(target[i]) - position[i];
        distanceSquared += direction[i] * direction[i];
        upSquared += static_cast<double>(up[i]) * up[i];
    }
    const double distance = std::sqrt(distanceSquared);
    const double upLength = std::sqrt(upSquared);
    if (distance <= minDistance) return reject(PitchTargetStatus::DegenerateTarget);
    if (upLength <= minDistance) return reject(PitchTargetStatus::DegenerateUp);
    double upDot = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        direction[i] /= distance;
        normalizedUp[i] = static_cast<double>(up[i]) / upLength;
        upDot += direction[i] * normalizedUp[i];
    }
    double pitchUpSquared = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        pitchUp[i] = normalizedUp[i] - upDot * direction[i];
        pitchUpSquared += pitchUp[i] * pitchUp[i];
    }
    const double pitchUpLength = std::sqrt(pitchUpSquared);
    if (pitchUpLength <= minUpSine) return reject(PitchTargetStatus::UpParallel);
    // Crossing either authored-up pole would let native look-at flip its roll,
    // even if the final vector itself were no longer parallel. Reject the
    // entire crossing rather than testing only the endpoint's conditioning.
    const double angleToUp = std::atan2(pitchUpLength, upDot);
    const double poleMargin = std::asin(minUpSine);
    if (pitchRadians >= angleToUp - poleMargin ||
            pitchRadians <= angleToUp - 2.0 * halfPi + poleMargin)
        return reject(PitchTargetStatus::UpParallel);
    // Preserve exact original bytes for a neutral offset after validating the
    // native basis; no normalization/round-trip drift accumulates at zero pitch.
    if (pitchRadians == 0.0) return {PitchTargetStatus::Accepted, target};

    const double cosine = std::cos(pitchRadians), sine = std::sin(pitchRadians);
    double rotatedDotUp = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        rotated[i] = direction[i] * cosine + pitchUp[i] / pitchUpLength * sine;
        rotatedDotUp += rotated[i] * normalizedUp[i];
    }
    double rotatedPerpendicularSquared = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double perpendicular = rotated[i] - rotatedDotUp * normalizedUp[i];
        rotatedPerpendicularSquared += perpendicular * perpendicular;
    }
    // The unchanged native up must also define a valid basis after the pitch.
    if (rotatedPerpendicularSquared <= minUpSine * minUpSine)
        return reject(PitchTargetStatus::UpParallel);

    FramingVector result = target; // Preserve target W, including its sign bit.
    double roundedDistanceSquared = 0.0, directionErrorSquared = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double component = static_cast<double>(position[i]) + distance * rotated[i];
        if (!std::isfinite(component) || std::abs(component) > std::numeric_limits<float>::max())
            return reject(PitchTargetStatus::OutputPrecision);
        result[i] = static_cast<float>(component);
        const double rounded = static_cast<double>(result[i]) - position[i];
        roundedDistanceSquared += rounded * rounded;
        const double error = rounded / distance - rotated[i];
        directionErrorSquared += error * error;
    }
    // Large world coordinates can erase a small offset when packed as floats.
    // Fail closed instead of returning a distorted distance or direction.
    if (std::abs(std::sqrt(roundedDistanceSquared) / distance - 1.0) > maxFloatError ||
            directionErrorSquared > maxFloatError * maxFloatError)
        return reject(PitchTargetStatus::OutputPrecision);
    return {PitchTargetStatus::Accepted, result};
}

} // namespace bone_eater::camera
