#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bone_eater::input {
struct ScopedPoint { double x = 0, y = 0; };
struct AdaptiveMotionSettings {
    bool enabled = false;
    double lowMaxGain = 1.0, highMaxGain = .7;
    double speedStart = .15, speedFull = .8;
    double rampUpMs = 100, rampDownMs = 140;
    bool edgePan = false;
    double edgeBand = .04, edgeDwellMs = 200, edgeMaxSpeed = .25;
};
struct ScopedMotionSettings {
    double lowGain = .25, highGain = .10, lowMs = 35, highMs = 55;
    AdaptiveMotionSettings adaptive;
};

// Input is the calibrated main-view point, not the cabinet or UI coordinate.
// Delta accumulation also handles mouse emulation without depending on a gun
// brand. Source ownership/scene validity is certified by the caller.
class ScopedMotion {
public:
    void reset() noexcept { *this = {}; }
    ScopedPoint update(ScopedPoint raw, bool scoped, bool higher, std::uint64_t now,
                       const ScopedMotionSettings& settings) noexcept {
        if (!scoped) suspended_ = false;
        if (settings.adaptive.enabled && scoped && seeded_ &&
                (!std::isfinite(raw.x) || !std::isfinite(raw.y) ||
                 raw.x < 0 || raw.x > 1920 || raw.y < 0 || raw.y > 1080)) {
            suspended_ = true; resetSpeed(); edgeDirection_ = {};
            return output_;
        }
        if (!std::isfinite(raw.x) || !std::isfinite(raw.y)) { reset(); return raw; }
        // An off-screen absolute sample has no reliable direction for edge pan.
        // Preserve the original fixed-mode clamping behavior.
        if (settings.adaptive.enabled && scoped &&
                (raw.x < 0 || raw.x > 1920 || raw.y < 0 || raw.y > 1080)) return raw;
        raw.x = std::clamp(raw.x, 0.0, 1920.0);
        raw.y = std::clamp(raw.y, 0.0, 1080.0);
        if (settings.adaptive.enabled && scoped && seeded_ &&
                (suspended_ || now < lastMs_ || now - lastMs_ > 250)) {
            suspended_ = false; raw_ = raw; target_ = output_;
            scoped_ = true; returning_ = false;
            lastMs_ = now; resetSpeed();
            adaptiveGain_ = higher ? settings.highGain : settings.lowGain;
            higher_ = higher; edgeDirection_ = {}; edgeSince_ = now;
            return output_;
        }
        if (!seeded_ || now < lastMs_ || now - lastMs_ > 250) {
            seeded_ = true; scoped_ = scoped; higher_ = higher; raw_ = target_ = output_ = raw;
            resetSpeed(); edgeDirection_ = {}; edgeSince_ = now;
            adaptiveGain_ = higher ? settings.highGain : settings.lowGain;
            lastMs_ = now; returning_ = false; return raw;
        }
        const double elapsed = static_cast<double>(now - lastMs_);
        lastMs_ = now;
        if (scoped && !scoped_) {
            // Begin at the currently displayed point, including a return blend.
            target_ = output_; returning_ = false;
            resetSpeed(); adaptiveGain_ = higher ? settings.highGain : settings.lowGain;
            edgeDirection_ = {}; edgeSince_ = now;
        } else if (!scoped && scoped_) {
            // Rejoin direct absolute aim in a bounded 120ms, without changing
            // the shot independently of the visible reticle.
            returnOffset_ = {output_.x - raw.x, output_.y - raw.y};
            returnMs_ = now; returning_ = true;
            resetSpeed(); edgeDirection_ = {};
        }
        if (scoped) {
            double gain = higher ? settings.highGain : settings.lowGain;
            if (settings.adaptive.enabled) {
                if (higher != higher_) {
                    resetSpeed(); adaptiveGain_ = gain;
                    edgeDirection_ = {}; edgeSince_ = now;
                }
                // Ignore same-timestamp duplicates. Keep the previous raw point
                // so the next timed sample includes their movement once.
                if (elapsed > 0) {
                    const double dx = (raw.x - raw_.x) / 1920.0;
                    const double dy = (raw.y - raw_.y) / 1080.0;
                    const double distance = std::hypot(dx, dy);
                    if (distance <= .4) {
                        // Filter signed velocity so alternating hand tremor does
                        // not look like sustained travel in one direction.
                        const double speed = distance * 1000.0 / elapsed;
                        const double velocityScale = speed > 4 ? 4 / speed : 1;
                        const double speedBlend = -std::expm1(-elapsed / 80.0);
                        velocity_.x += (dx * 1000.0 / elapsed * velocityScale - velocity_.x) * speedBlend;
                        velocity_.y += (dy * 1000.0 / elapsed * velocityScale - velocity_.y) * speedBlend;
                        const double filteredSpeed = std::hypot(velocity_.x, velocity_.y);
                        const double span = settings.adaptive.speedFull - settings.adaptive.speedStart;
                        // Schmitt thresholds plus a short deliberate-motion
                        // interval prevent gain chatter and single-sample boosts.
                        const double start = settings.adaptive.speedStart + std::min(.1 * settings.adaptive.speedStart, span * .5);
                        if (travelActive_) {
                            if (filteredSpeed < .8 * settings.adaptive.speedStart) {
                                travelActive_ = false; travelPending_ = false;
                            }
                        } else if (filteredSpeed >= start && speed >= start) {
                            if (!travelPending_) { travelPending_ = true; travelSince_ = now; }
                            else if (now - travelSince_ >= 40) travelActive_ = true;
                        } else travelPending_ = false;
                        const double fraction = travelActive_ ?
                            std::clamp((filteredSpeed - settings.adaptive.speedStart) / span, 0.0, 1.0) : 0;
                        const double smooth = fraction * fraction * (3 - 2 * fraction);
                        const double maximum = higher ? settings.adaptive.highMaxGain : settings.adaptive.lowMaxGain;
                        const double desired = gain + (maximum - gain) * smooth;
                        const double response = desired > adaptiveGain_ ? settings.adaptive.rampUpMs : settings.adaptive.rampDownMs;
                        adaptiveGain_ += (desired - adaptiveGain_) * -std::expm1(-elapsed / response);
                        gain = adaptiveGain_;
                        target_.x = std::clamp(target_.x + (raw.x - raw_.x) * gain, 0.0, 1920.0);
                        target_.y = std::clamp(target_.y + (raw.y - raw_.y) * gain, 0.0, 1080.0);
                    } else { // Implausible single sample: reanchor without moving aim.
                        resetSpeed(); adaptiveGain_ = gain;
                        edgeDirection_ = {}; edgeSince_ = now;
                    }
                    if (settings.adaptive.edgePan) {
                        const auto direction = edgeDirection(raw, settings.adaptive.edgeBand);
                        const bool movingInward = (raw.x - raw_.x) * direction.x < 0 ||
                            (raw.y - raw_.y) * direction.y < 0;
                        if (movingInward || direction.x != edgeDirection_.x || direction.y != edgeDirection_.y) {
                            edgeDirection_ = direction; edgeSince_ = now;
                        } else if ((direction.x || direction.y) && now - edgeSince_ >= settings.adaptive.edgeDwellMs) {
                            // Only integrate the part of this interval after dwell.
                            const double panMs = std::min(elapsed,
                                static_cast<double>(now - edgeSince_) - settings.adaptive.edgeDwellMs);
                            const double scale = (direction.x && direction.y) ? .7071067811865476 : 1.0;
                            target_.x = std::clamp(target_.x + direction.x * scale * settings.adaptive.edgeMaxSpeed * 1920.0 * panMs / 1000.0, 0.0, 1920.0);
                            target_.y = std::clamp(target_.y + direction.y * scale * settings.adaptive.edgeMaxSpeed * 1080.0 * panMs / 1000.0, 0.0, 1080.0);
                        }
                    } else edgeDirection_ = {};
                }
            } else {
                target_.x = std::clamp(target_.x + (raw.x - raw_.x) * gain, 0.0, 1920.0);
                target_.y = std::clamp(target_.y + (raw.y - raw_.y) * gain, 0.0, 1080.0);
            }
            const double ms = higher ? settings.highMs : settings.lowMs;
            const double blend = ms <= 0 ? 1 : -std::expm1(-elapsed / ms);
            output_.x += (target_.x - output_.x) * blend;
            output_.y += (target_.y - output_.y) * blend;
        } else if (returning_) {
            const double t = std::min(1.0, (now - returnMs_) / 120.0);
            const double amount = 1 - t * t * (3 - 2 * t);
            output_ = {std::clamp(raw.x + returnOffset_.x * amount, 0.0, 1920.0),
                       std::clamp(raw.y + returnOffset_.y * amount, 0.0, 1080.0)};
            if (t == 1) returning_ = false;
        } else output_ = raw;
        if (!(scoped && settings.adaptive.enabled && elapsed == 0)) raw_ = raw;
        scoped_ = scoped; higher_ = higher;
        return output_;
    }
private:
    void resetSpeed() noexcept {
        velocity_ = {}; travelActive_ = false; travelPending_ = false; travelSince_ = 0;
    }
    static ScopedPoint edgeDirection(ScopedPoint point, double band) noexcept {
        return {point.x <= band * 1920.0 ? -1.0 : point.x >= (1 - band) * 1920.0 ? 1.0 : 0.0,
                point.y <= band * 1080.0 ? -1.0 : point.y >= (1 - band) * 1080.0 ? 1.0 : 0.0};
    }
    bool seeded_ = false, scoped_ = false, higher_ = false, returning_ = false, suspended_ = false;
    std::uint64_t lastMs_ = 0, returnMs_ = 0, edgeSince_ = 0;
    bool travelActive_ = false, travelPending_ = false;
    std::uint64_t travelSince_ = 0;
    double adaptiveGain_ = 0;
    ScopedPoint raw_, target_, output_, returnOffset_, edgeDirection_, velocity_;
};
} // namespace bone_eater::input
