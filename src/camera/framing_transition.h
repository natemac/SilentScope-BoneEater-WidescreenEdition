#pragma once

#include "camera/framing_math.h"
#include "camera/vertical_pan_policy.h"

namespace bone_eater::camera {

enum class FramingPoseDisposition { Accept, SkipNative, InvariantFailure };

// These limits describe the current adapter, not invalid native camera data.
// Native cinematic roll, a temporarily singular look-at tuple, or a pitch that
// cannot be represented safely must be allowed through without modification.
// Nonfinite/internal out-of-range pitch remains an integrity failure.
inline FramingPoseDisposition framingPoseDisposition(PitchTargetStatus status) noexcept {
    switch (status) {
    case PitchTargetStatus::Accepted: return FramingPoseDisposition::Accept;
    case PitchTargetStatus::UnsupportedRoll:
    case PitchTargetStatus::DegenerateTarget:
    case PitchTargetStatus::DegenerateUp:
    case PitchTargetStatus::UpParallel:
    case PitchTargetStatus::OutputPrecision: return FramingPoseDisposition::SkipNative;
    default: return FramingPoseDisposition::InvariantFailure;
    }
}

inline const char* framingPoseStatusName(PitchTargetStatus status) noexcept {
    switch (status) {
    case PitchTargetStatus::Accepted: return "accepted";
    case PitchTargetStatus::NonFinite: return "nonfinite";
    case PitchTargetStatus::UnsupportedRoll: return "unsupported_roll";
    case PitchTargetStatus::InvalidPitch: return "invalid_pitch";
    case PitchTargetStatus::DegenerateTarget: return "degenerate_target";
    case PitchTargetStatus::DegenerateUp: return "degenerate_up";
    case PitchTargetStatus::UpParallel: return "up_parallel_or_pole_crossing";
    case PitchTargetStatus::OutputPrecision: return "output_precision";
    default: return "unknown";
    }
}

// Only the integration's owner thread may use this state. Native identity,
// serial/freshness and post-commit restoration checks remain outside it.
// A transient skip happens before writes and revokes late-commit reuse. The
// first later supported EARLY commit is neutral and must be certified after
// its native call before a late commit can use it. A late call cannot rearm.
struct FramingTransitionState {
    VerticalPanPolicy policy;
    bool earlyCertified = false;
    double certifiedOffset = 0;
    bool neutralEarlyRequired = true;

    void invalidateCertificate() noexcept {
        earlyCertified = false;
        certifiedOffset = 0;
    }

    void resetForNativeTransition() noexcept {
        policy.resetScene();
        invalidateCertificate();
        neutralEarlyRequired = true;
    }

    void advanceEarly(const VerticalPanInput& input, double elapsed) {
        // A new/recovered camera starts from its exact authored target, even
        // if the physical scope button remained held across the transition.
        if (!neutralEarlyRequired) policy.update(input, elapsed);
    }

    bool certifyEarly(double offset) noexcept {
        if (!std::isfinite(offset) || std::abs(offset) > policy.config().maxOffsetRadians ||
                (neutralEarlyRequired && offset != 0)) return false;
        certifiedOffset = offset;
        earlyCertified = true;
        neutralEarlyRequired = false;
        return true;
    }
};

} // namespace bone_eater::camera
