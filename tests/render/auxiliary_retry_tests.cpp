#include "render/auxiliary_retry_policy.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bone_eater::render;

namespace {
void require(bool condition, int line) {
    if (!condition) throw std::runtime_error("failed at line " + std::to_string(line));
}
#define CHECK(value) require((value), __LINE__)

void synchronizedLoadingSlowdownDoesNotRestoreWindow() {
    // Recorded live regression: startup54Hz, both streams23Hz with a282ms
    // gap, latest callbacks31ms old. Old27Hz threshold restored the scope.
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 23, 23, 31, 31, 282, 282));
    CHECK(!auxiliaryRateNeedsRollback(60, 1000, 23, 23, 0, 0, 44, 44));
    CHECK(!auxiliaryRateNeedsRollback(60, 1000, 30, 30, 0, 0, 34, 34));
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 10, 10, 100, 100, 350, 350));
}

void auxiliarySpecificSlowdownStillRestores() {
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 30, 10, 0, 0, 100, 100));
    CHECK(auxiliaryRateNeedsRollback(60, 1000, 60, 23, 0, 0, 44, 44));
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 0, 23, 0, 0, 44, 44));
    // Both rates must be similar; no exemption merely because main is slower.
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 10, 23, 0, 0, 100, 100));
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 24, 20, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 25, 20, 0, 0, 50, 50));
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 20, 24, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 20, 25, 0, 0, 50, 50));
}

void commonSlowdownCannotMaskLowCadenceOrStaleness() {
    for (unsigned count : {0u, 4u, 5u, 9u}) {
        CHECK(auxiliaryRateNeedsRollback(54, 1000, count, count, 0, 0, 100, 100));
    }
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 23, 23, 101, 0, 44, 44));
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 23, 23, 0, 101, 44, 44));
    // Past stalls cannot be laundered by one fresh callback before worker tick.
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 23, 23, 0, 0, 351, 44));
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 23, 23, 0, 0, 44, 351));
}

void commonSlowdownRequiresBoundedComparableInterval() {
    CHECK(!auxiliaryRateNeedsRollback(54, 1500, 30, 30, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(54, 1501, 30, 30, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(54, 999, 20, 20, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(54, 0, 20, 20, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(0, 1000, 20, 20, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(std::numeric_limits<double>::quiet_NaN(),
        1000, 20, 20, 0, 0, 50, 50));
    CHECK(auxiliaryRateNeedsRollback(std::numeric_limits<double>::infinity(),
        1000, 20, 20, 0, 0, 50, 50));
}

void originalRatePassAndBaselineRemainUnchanged() {
    // This policy adds a narrow exemption to the old threshold, not a new
    // general cadence watchdog. Its caller owns independent freshness checks.
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 60, 27, 0, 0, 40, 40));
    CHECK(!auxiliaryRateNeedsRollback(8, 1000, 30, 5, 0, 0, 200, 200));
    CHECK(auxiliaryRateNeedsRollback(8, 1000, 30, 4, 0, 0, 250, 250));
    CHECK(!auxiliaryRateNeedsRollback(54, 1000, 23, 23, 0, 0, 44, 44));
    // A preceding synchronized dip never trains the baseline down and cannot
    // hide a later scope-only dip or consume the retry budget.
    CHECK(auxiliaryRateNeedsRollback(54, 1000, 60, 23, 0, 0, 44, 44));
    AuxiliaryRetryBudget budget;
    for (unsigned i = 0; i < 3; ++i) {
        if (auxiliaryRateNeedsRollback(54, 1000, 23, 23, 0, 0, 44, 44)) budget.cadenceFailure();
    }
    CHECK(budget.mode() == AuxiliaryRetryBudget::Mode::Active && budget.retries() == 0);
    CHECK(budget.cadenceFailure());
}

struct Fixture {
    AuxiliaryVisibleCadence cadence;
    // Large pre-existing counts must never enter the recent visible baseline.
    AuxiliaryCadenceSample sample {100000, 90000, 90000, 100000, 100000, 0, 0};
    AuxiliaryCadenceResult result;
    Fixture() { result = cadence.observe(sample, true); }
    void step(unsigned elapsed = 200, unsigned mainCalls = 6, unsigned auxiliaryCalls = 6, bool eligible = true) {
        sample.now += elapsed;
        sample.mainCalls += mainCalls;
        sample.auxiliaryCalls += auxiliaryCalls;
        sample.mainLast = sample.auxiliaryLast = sample.now;
        result = cadence.observe(sample, eligible);
    }
    void run(unsigned milliseconds) {
        for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 200) step();
    }
};

void stableRecentBaseline() {
    Fixture f;
    f.run(10000);
    CHECK(!f.result.ready && f.result.stableBins == 0);
    f.run(2000);
    CHECK(!f.result.ready && f.result.stableBins == 2);
    f.run(1000);
    CHECK(f.result.ready && f.result.stableBins == 3);
    CHECK(std::abs(f.cadence.mainBaselineHz() - 30.0) < 1e-9);
    CHECK(std::abs(f.cadence.auxiliaryBaselineHz() - 30.0) < 1e-9);
}

void foregroundLossRestartsCooldown() {
    Fixture f;
    f.run(12000);
    f.step(200, 6, 6, false);
    CHECK(f.result.reset && !f.result.ready);
    f.run(12800);
    CHECK(!f.result.ready);
    f.run(400);
    CHECK(f.result.ready);
}

void unseenGapBetweenWorkerTicksResets() {
    Fixture f;
    f.run(12000);
    ++f.sample.auxiliaryGapSerial;
    f.step(); // Last callback is fresh, but an earlier >100 ms gap still counts.
    CHECK(f.result.reset && !f.result.ready);
    f.run(13000);
    CHECK(f.result.ready);
}

void mainGapAlsoResets() {
    Fixture f;
    f.run(12000);
    ++f.sample.mainGapSerial;
    f.step();
    CHECK(f.result.reset && !f.result.ready);
}

void staleOutputResets() {
    Fixture f;
    f.run(12000);
    f.sample.now += 101;
    f.sample.mainLast = f.sample.now;
    f.result = f.cadence.observe(f.sample, true);
    CHECK(f.result.reset && !f.result.ready);
}

void staleMainResets() {
    Fixture f;
    f.run(12000);
    f.sample.now += 101;
    f.sample.auxiliaryLast = f.sample.now;
    f.result = f.cadence.observe(f.sample, true);
    CHECK(f.result.reset && !f.result.ready);
}

void skippedWorkerCannotClaimContinuousVisibility() {
    Fixture f;
    f.run(12000);
    f.step(1501, 45, 45);
    CHECK(f.result.reset && !f.result.ready);
}

void changedCadenceRestartsInsteadOfUsingMenuBaseline() {
    Fixture f;
    f.run(12000);
    f.step(1000, 15, 15);
    CHECK(f.result.completedBin && f.result.reset && !f.result.ready);
    // 15 Hz then stays stable; it requires a fresh cooldown and three bins.
    for (unsigned i = 0; i < 65; ++i) f.step(200, 3, 3);
    CHECK(f.result.ready);
    CHECK(std::abs(f.cadence.auxiliaryBaselineHz() - 15.0) < 1e-9);
}

void independentlyStableRatesAreNotRequiredToMatch() {
    Fixture f;
    for (unsigned i = 0; i < 65; ++i) f.step(200, 6, 12);
    CHECK(f.result.ready);
    CHECK(std::abs(f.cadence.mainBaselineHz() - 30.0) < 1e-9);
    CHECK(std::abs(f.cadence.auxiliaryBaselineHz() - 60.0) < 1e-9);
}

void lowRatesCannotQualify() {
    Fixture f;
    f.run(10000);
    f.step(1000, 9, 30);
    CHECK(f.result.reset && !f.result.ready);
    Fixture g;
    g.run(10000);
    g.step(1000, 30, 9);
    CHECK(g.result.reset && !g.result.ready);
}

void counterOrClockResetInvalidatesEvidence() {
    Fixture f;
    f.run(12000);
    f.sample.auxiliaryCalls = 0;
    f.step();
    CHECK(f.result.reset && !f.result.ready);
    Fixture g;
    g.run(12000);
    g.sample.now -= 1000;
    g.sample.mainLast = g.sample.auxiliaryLast = g.sample.now;
    g.result = g.cadence.observe(g.sample, true);
    CHECK(g.result.reset && !g.result.ready);
}

void exactlyOneRetryAndRestorationRequired() {
    AuxiliaryRetryBudget policy;
    CHECK(!policy.retry(true));
    CHECK(policy.cadenceFailure());
    CHECK(policy.mode() == AuxiliaryRetryBudget::Mode::Restoring);
    CHECK(!policy.retry(true));
    CHECK(policy.restorationConfirmed());
    CHECK(!policy.retry(false));
    CHECK(policy.retry(true) && policy.retries() == 1);
    CHECK(!policy.cadenceFailure());
    CHECK(policy.mode() == AuxiliaryRetryBudget::Mode::Disabled);
    CHECK(!policy.restorationConfirmed() && !policy.retry(true));
}

void permanentFailuresCannotBecomeRetryable() {
    for (unsigned transition = 0; transition < 4; ++transition) {
        AuxiliaryRetryBudget policy;
        if (transition >= 1) CHECK(policy.cadenceFailure());
        if (transition >= 2) CHECK(policy.restorationConfirmed());
        if (transition >= 3) CHECK(policy.retry(true));
        CHECK(policy.permanentFailure());
        CHECK(!policy.permanentFailure());
        CHECK(!policy.cadenceFailure() && !policy.retry(true));
        CHECK(policy.mode() == AuxiliaryRetryBudget::Mode::Disabled);
    }
}

void visibleStallWaitsForFreshQualificationWithoutSpendingRetry() {
    AuxiliaryRetryBudget policy;
    CHECK(policy.cadenceFailure());
    CHECK(policy.restorationConfirmed());
    Fixture f;
    f.run(13000);
    CHECK(f.result.ready && f.cadence.auxiliaryBaselineHz() > 0);
    // Both streams stop for twenty seconds while safely restored. Cached
    // cadence must clear, and elapsed stall time must not count as cooldown.
    for (unsigned i = 0; i < 100; ++i) {
        f.sample.now += 200;
        f.result = f.cadence.observe(f.sample, true);
        CHECK(!f.result.ready);
        CHECK(f.cadence.mainBaselineHz() == 0 && f.cadence.auxiliaryBaselineHz() == 0);
        CHECK(!policy.retry(f.result.ready));
    }
    CHECK(policy.mode() == AuxiliaryRetryBudget::Mode::Visible && policy.retries() == 0);
    f.step(); // First genuinely fresh observation seeds a new cooldown.
    f.run(12800);
    CHECK(!f.result.ready);
    f.step();
    CHECK(f.result.ready && policy.retry(f.result.ready));
    CHECK(policy.retries() == 1);
    CHECK(!policy.cadenceFailure()); // Any subsequent rate OR stale failure.
    CHECK(policy.mode() == AuxiliaryRetryBudget::Mode::Disabled);
}

void oneResumedStreamCannotQualify() {
    for (bool mainResumes : {false, true}) {
        Fixture f;
        f.run(12000);
        for (unsigned i = 0; i < 100; ++i) {
            f.sample.now += 200;
            if (mainResumes) {
                f.sample.mainLast = f.sample.now;
                f.sample.mainCalls += 6;
            } else {
                f.sample.auxiliaryLast = f.sample.now;
                f.sample.auxiliaryCalls += 6;
            }
            f.result = f.cadence.observe(f.sample, true);
            CHECK(!f.result.ready);
            CHECK(f.cadence.mainBaselineHz() == 0 && f.cadence.auxiliaryBaselineHz() == 0);
        }
    }
}

void freshGapAfterVisibleStallRestartsEntireCooldown() {
    Fixture f;
    f.sample.now += 1000;
    f.result = f.cadence.observe(f.sample, true);
    CHECK(!f.result.ready);
    f.step();
    f.run(12000);
    ++f.sample.mainGapSerial;
    f.step();
    CHECK(f.result.reset && !f.result.ready);
    CHECK(f.cadence.mainBaselineHz() == 0 && f.cadence.auxiliaryBaselineHz() == 0);
    f.run(12800);
    CHECK(!f.result.ready);
    f.step();
    CHECK(f.result.ready);
}
} // namespace

int main() {
    try {
        synchronizedLoadingSlowdownDoesNotRestoreWindow();
        auxiliarySpecificSlowdownStillRestores();
        commonSlowdownCannotMaskLowCadenceOrStaleness();
        commonSlowdownRequiresBoundedComparableInterval();
        originalRatePassAndBaselineRemainUnchanged();
        stableRecentBaseline();
        foregroundLossRestartsCooldown();
        unseenGapBetweenWorkerTicksResets();
        mainGapAlsoResets();
        staleOutputResets();
        staleMainResets();
        skippedWorkerCannotClaimContinuousVisibility();
        changedCadenceRestartsInsteadOfUsingMenuBaseline();
        independentlyStableRatesAreNotRequiredToMatch();
        lowRatesCannotQualify();
        counterOrClockResetInvalidatesEvidence();
        exactlyOneRetryAndRestorationRequired();
        permanentFailuresCannotBecomeRetryable();
        visibleStallWaitsForFreshQualificationWithoutSpendingRetry();
        oneResumedStreamCannotQualify();
        freshGapAfterVisibleStallRestartsEntireCooldown();
        std::cout << "21 auxiliary retry/rate policy cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

