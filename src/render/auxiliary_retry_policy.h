#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bone_eater::render {

// The render loop presents both outputs on the same thread. Brief shared
// pauses (e.g. resource loading) cannot be diagnosed as offscreen starvation.
// Keep this grace bounded; auxiliary-only stalls retain the350ms watchdog.
inline bool auxiliarySharedPause(std::uint64_t mainAge, std::uint64_t auxiliaryAge) noexcept {
    return mainAge > 350 && auxiliaryAge > 350 && mainAge <= 2000 && auxiliaryAge <= 2000 &&
        std::max(mainAge, auxiliaryAge) - std::min(mainAge, auxiliaryAge) <= 100;
}

// A historical attract-screen baseline must not mistake a shared engine
// slowdown for offscreen auxiliary starvation. This only exempts a bounded,
// fresh interval with similar live rates; it never lowers the stored baseline
// or changes the caller's independent stall/window/Present-result safeguards.
inline bool auxiliaryRateNeedsRollback(double baselineHz, std::uint64_t elapsedMs,
        std::uint64_t mainCalls, std::uint64_t auxiliaryCalls,
        std::uint64_t mainAgeMs, std::uint64_t auxiliaryAgeMs,
        std::uint64_t mainMaximumGapMs, std::uint64_t auxiliaryMaximumGapMs) noexcept {
    if (!elapsedMs || !std::isfinite(baselineHz) || baselineHz <= 0) return true;
    const double auxiliaryHz = auxiliaryCalls * 1000.0 / elapsedMs;
    if (auxiliaryHz >= std::max(5.0, baselineHz * 0.5)) return false;

    const double mainHz = mainCalls * 1000.0 / elapsedMs;
    const bool synchronizedSlowdown = elapsedMs >= 1000 && elapsedMs <= 1500 &&
        mainAgeMs <= 100 && auxiliaryAgeMs <= 100 &&
        mainMaximumGapMs <= 350 && auxiliaryMaximumGapMs <= 350 &&
        mainHz >= 10.0 && auxiliaryHz >= 10.0 &&
        std::max(mainHz, auxiliaryHz) <= std::min(mainHz, auxiliaryHz) * 1.2;
    return !synchronizedSlowdown;
}

// Portable policy only: callers retain ownership, visibility, placement and
// foreground checks. A cadence sample is a successful non-TEST Present count,
// not a measurement of displayed frames or proof that content changed.
struct AuxiliaryCadenceSample {
    std::uint64_t now = 0, mainCalls = 0, auxiliaryCalls = 0;
    std::uint64_t mainLast = 0, auxiliaryLast = 0;
    std::uint64_t mainGapSerial = 0, auxiliaryGapSerial = 0;
};

struct AuxiliaryCadenceResult {
    bool ready = false, completedBin = false, reset = false;
    unsigned stableBins = 0;
    double mainHz = 0, auxiliaryHz = 0;
    std::uint64_t elapsed = 0;
};

class AuxiliaryVisibleCadence {
public:
    static constexpr std::uint64_t cooldownMs = 10000, binMs = 1000;
    static constexpr std::uint64_t maximumBinMs = 1500, freshMs = 100;
    static constexpr unsigned requiredBins = 3;

    void reset() noexcept { *this = {}; }

    AuxiliaryCadenceResult observe(const AuxiliaryCadenceSample& sample, bool eligible) noexcept {
        AuxiliaryCadenceResult result;
        if (!eligible || sample.mainLast > sample.now || sample.auxiliaryLast > sample.now ||
                sample.now - sample.mainLast > freshMs || sample.now - sample.auxiliaryLast > freshMs) {
            result.reset = seeded_;
            reset();
            return result;
        }
        if (!seeded_) { seed(sample); return result; }
        if (sample.now < lastObserved_ || sample.now - lastObserved_ > maximumBinMs ||
                sample.mainCalls < start_.mainCalls || sample.auxiliaryCalls < start_.auxiliaryCalls ||
                sample.mainGapSerial != start_.mainGapSerial ||
                sample.auxiliaryGapSerial != start_.auxiliaryGapSerial) {
            seed(sample);
            result.reset = true;
            return result;
        }
        lastObserved_ = sample.now;
        if (sample.now - visibleSince_ < cooldownMs) {
            start_ = sample;
            return result;
        }
        if (!cooled_) {
            cooled_ = true;
            start_ = sample;
            return result;
        }
        const auto elapsed = sample.now - start_.now;
        if (elapsed > maximumBinMs) {
            seed(sample);
            result.reset = true;
            return result;
        }
        if (elapsed >= binMs) {
            result.completedBin = true;
            result.elapsed = elapsed;
            result.mainHz = (sample.mainCalls - start_.mainCalls) * 1000.0 / elapsed;
            result.auxiliaryHz = (sample.auxiliaryCalls - start_.auxiliaryCalls) * 1000.0 / elapsed;
            start_ = sample;
            if (!std::isfinite(result.mainHz) || !std::isfinite(result.auxiliaryHz) ||
                    result.mainHz < 10.0 || result.auxiliaryHz < 10.0) {
                seed(sample);
                result.reset = true;
                return result;
            }
            if (bins_ && (std::max(mainHigh_, result.mainHz) > std::min(mainLow_, result.mainHz) * 1.2 ||
                    std::max(auxiliaryHigh_, result.auxiliaryHz) > std::min(auxiliaryLow_, result.auxiliaryHz) * 1.2)) {
                // A changed cadence must complete its own full visible cooldown.
                seed(sample);
                result.reset = true;
                return result;
            }
            mainLow_ = bins_ ? std::min(mainLow_, result.mainHz) : result.mainHz;
            mainHigh_ = std::max(mainHigh_, result.mainHz);
            auxiliaryLow_ = bins_ ? std::min(auxiliaryLow_, result.auxiliaryHz) : result.auxiliaryHz;
            auxiliaryHigh_ = std::max(auxiliaryHigh_, result.auxiliaryHz);
            totalMain_ += result.mainHz * elapsed;
            totalAuxiliary_ += result.auxiliaryHz * elapsed;
            totalMs_ += elapsed;
            ++bins_;
        }
        result.stableBins = bins_;
        result.ready = bins_ >= requiredBins;
        return result;
    }

    double mainBaselineHz() const noexcept { return totalMs_ ? totalMain_ / totalMs_ : 0; }
    double auxiliaryBaselineHz() const noexcept { return totalMs_ ? totalAuxiliary_ / totalMs_ : 0; }

private:
    void seed(const AuxiliaryCadenceSample& sample) noexcept {
        reset();
        seeded_ = true;
        visibleSince_ = lastObserved_ = sample.now;
        start_ = sample;
    }
    bool seeded_ = false, cooled_ = false;
    unsigned bins_ = 0;
    AuxiliaryCadenceSample start_ {};
    std::uint64_t visibleSince_ = 0, lastObserved_ = 0, totalMs_ = 0;
    double mainLow_ = 0, mainHigh_ = 0, auxiliaryLow_ = 0, auxiliaryHigh_ = 0;
    double totalMain_ = 0, totalAuxiliary_ = 0;
};

class AuxiliaryRetryBudget {
public:
    enum class Mode { Active, Restoring, Visible, Disabled };
    Mode mode() const noexcept { return mode_; }
    unsigned retries() const noexcept { return retries_; }
    bool permanentFailure() noexcept {
        const bool changed = mode_ != Mode::Disabled;
        mode_ = Mode::Disabled;
        return changed;
    }
    // Rate degradation and callback staleness share this single retry budget.
    // Hard failures must still call permanentFailure directly.
    bool cadenceFailure() noexcept {
        if (mode_ == Mode::Active && retries_ == 0) {
            mode_ = Mode::Restoring;
            return true;
        }
        permanentFailure();
        return false;
    }
    bool restorationConfirmed() noexcept {
        if (mode_ != Mode::Restoring) return false;
        mode_ = Mode::Visible;
        return true;
    }
    bool retry(bool allReady) noexcept {
        if (mode_ != Mode::Visible || retries_ != 0 || !allReady) return false;
        ++retries_;
        mode_ = Mode::Active;
        return true;
    }
private:
    Mode mode_ = Mode::Active;
    unsigned retries_ = 0;
};

} // namespace bone_eater::render
