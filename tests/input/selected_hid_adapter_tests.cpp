#include "input/selected_hid_adapter.h"
#include "input/selected_hid_config.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bone_eater::input;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

// Synthetic mappings, cadence and timestamps only. No hardware assignment or
// practical freshness timeout is inferred from these portable fixtures.
const SelectedGunButtonMapping mapping {4u, 16u, 1u};
constexpr std::uint16_t fire = 0x0008, scope = 0x8000, leftScope = 0x0001;
constexpr auto maximumAge = 100ms;
HidClock::time_point at(std::int64_t milliseconds) {
    return HidClock::time_point(std::chrono::duration_cast<HidClock::duration>(
        std::chrono::milliseconds(milliseconds)));
}
SelectedHidSample report(std::uint64_t generation, std::uint16_t x = 19000,
        std::uint16_t y = 37000, std::uint16_t buttons = 0,
        std::int64_t receiptMs = 1000, std::uint64_t session = 1) {
    return {true, SelectedHidStatus::Active, x, y, buttons, session, generation, at(receiptMs)};
}
SelectedHidAdapterResult consume(SelectedHidAdapter& adapter, SelectedHidSample sample,
        std::int64_t nowMs = 1000, bool focused = true) {
    return adapter.update(sample, maximumAge, at(nowMs), focused);
}
void neutral(const GunSourceOutput& output, GunSourceState state) {
    CHECK(output.state == state);
    CHECK(output.buttons.neutral());
    CHECK(!output.armed && !output.sourceUsable);
}
void sameAim(const GunAimPoint& a, const GunAimPoint& b) {
    CHECK(a.known == b.known && a.x == b.x && a.y == b.y && a.source == b.source);
    CHECK(a.identity.session == b.identity.session && a.identity.generation == b.identity.generation);
    CHECK(a.identity.receivedAt == b.identity.receivedAt);
}
void provenance(const SelectedGunInput& candidate, const SelectedHidSample& source) {
    CHECK(candidate.x == source.x && candidate.y == source.y && candidate.rawButtons == source.buttons);
    CHECK(candidate.identity.session == source.session && candidate.identity.generation == source.generation);
    CHECK(candidate.identity.receivedAt == source.timestamp);
}
void arm(SelectedHidAdapter& adapter) {
    CHECK(adapter.configure(mapping) == GunConfigureResult::Changed);
    neutral(consume(adapter, report(1)).output, GunSourceState::Transition);
    CHECK(consume(adapter, report(1)).output.armed);
}

void explicitConfigurationOwnsNoInputBeforeSuccess() {
    SelectedHidAdapter adapter;
    CHECK(!adapter.configured());
    auto sample = report(1, 0, 65535, fire | scope);
    auto result = consume(adapter, sample);
    CHECK(result.validation == SelectedHidValidation::NotConfigured);
    CHECK(result.candidate.status == GunSampleStatus::Invalid);
    provenance(result.candidate, sample);
    CHECK(!result.output.aim.known && result.output.consumptionGeneration == 0);
    neutral(result.output, GunSourceState::Transition);
    CHECK(adapter.configure({}) == GunConfigureResult::InvalidMapping);
    CHECK(!adapter.configured());
    CHECK(consume(adapter, sample).validation == SelectedHidValidation::NotConfigured);
    CHECK(adapter.configure(mapping) == GunConfigureResult::Changed);
    result = consume(adapter, sample);
    CHECK(result.output.mode == GunSourceMode::SelectedHid);
    neutral(result.output, GunSourceState::Transition);
    neutral(consume(adapter, sample).output, GunSourceState::AwaitNeutral);
    CHECK(consume(adapter, report(2)).output.armed);
}

void coherentTupleHasNoInversionSmoothingOrRetimestamp() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto sample = report(2, 65535, 0, fire | leftScope);
    const auto result = consume(adapter, sample, 1020);
    CHECK(result.validation == SelectedHidValidation::ReadyForPolicy);
    CHECK(result.candidate.status == GunSampleStatus::Valid);
    provenance(result.candidate, sample);
    CHECK(result.output.aim.x == 65535 && result.output.aim.y == 0);
    CHECK(result.output.buttons.trigger && !result.output.buttons.scopeRight && result.output.buttons.scopeLeft);
    CHECK(result.output.aim.identity.receivedAt == at(1000));
    auto repeated = consume(adapter, sample, 1099);
    sameAim(result.output.aim, repeated.output.aim);
    CHECK(repeated.output.consumptionGeneration == result.output.consumptionGeneration + 1);
    CHECK(repeated.output.aim.identity.generation == 2);
    CHECK(repeated.output.buttons.trigger);
    const auto corner = consume(adapter, report(3, 0, 65535, scope, 1100), 1100);
    CHECK(corner.output.aim.x == 0 && corner.output.aim.y == 65535);
    CHECK(!corner.output.buttons.trigger && corner.output.buttons.scopeRight);
}

void everyTransportStatusReleasesWithoutFallback() {
    struct Case { SelectedHidStatus source; SelectedHidValidation validation;
        GunSampleStatus candidate; GunSourceState state; };
    const Case cases[] {
        {SelectedHidStatus::Unselected, SelectedHidValidation::Unselected, GunSampleStatus::Unavailable, GunSourceState::Unavailable},
        {SelectedHidStatus::Waiting, SelectedHidValidation::Waiting, GunSampleStatus::Unavailable, GunSourceState::Unavailable},
        {SelectedHidStatus::Stale, SelectedHidValidation::Stale, GunSampleStatus::Stale, GunSourceState::Stale},
        {SelectedHidStatus::Disconnected, SelectedHidValidation::Disconnected, GunSampleStatus::Disconnected, GunSourceState::Disconnected},
        {SelectedHidStatus::Stopped, SelectedHidValidation::Stopped, GunSampleStatus::Disconnected, GunSourceState::Disconnected},
        {SelectedHidStatus::InvalidReport, SelectedHidValidation::InvalidReport, GunSampleStatus::Invalid, GunSourceState::InvalidSample},
    };
    for (const auto& test : cases) {
        SelectedHidAdapter adapter;
        arm(adapter);
        const auto held = consume(adapter, report(2, 500, 600, fire | scope | leftScope));
        auto lost = report(3, 10, 20, fire, 1050);
        lost.valid = false;
        lost.status = test.source;
        const auto result = consume(adapter, lost, 1050);
        CHECK(result.validation == test.validation && result.candidate.status == test.candidate);
        provenance(result.candidate, lost); // Diagnostics retain even nonusable raw bits.
        neutral(result.output, test.state);
        sameAim(held.output.aim, result.output.aim);
        CHECK(result.output.mode == GunSourceMode::SelectedHid);
        neutral(consume(adapter, report(2, 500, 600, fire | scope | leftScope), 1050).output,
            GunSourceState::AwaitFreshReport);
        neutral(consume(adapter, report(4, 700, 800, fire, 1051), 1051).output, GunSourceState::AwaitNeutral);
        CHECK(consume(adapter, report(5, 900, 1000, 0, 1052), 1052).output.armed);
    }
}

void contradictoryStatusesAndInvalidIdentitiesCannotDriveButtons() {
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        SelectedHidAdapter adapter;
        arm(adapter);
        const auto held = consume(adapter, report(2, 100, 200, scope));
        auto invalid = report(3, 300, 400, fire);
        SelectedHidValidation expected = SelectedHidValidation::InconsistentStatus;
        switch (mutation) {
            case 0: invalid.valid = false; break;
            case 1: invalid.status = SelectedHidStatus::Disconnected; break;
            case 2: invalid.status = static_cast<SelectedHidStatus>(-1); break;
            case 3: invalid.valid = false; invalid.status = static_cast<SelectedHidStatus>(-1); break;
            case 4: invalid.session = 0; expected = SelectedHidValidation::InvalidIdentity; break;
            default: invalid.generation = 0; expected = SelectedHidValidation::InvalidIdentity; break;
        }
        const auto result = consume(adapter, invalid);
        CHECK(result.validation == expected && result.candidate.status == GunSampleStatus::Invalid);
        neutral(result.output, GunSourceState::InvalidSample);
        sameAim(held.output.aim, result.output.aim);
        provenance(result.candidate, invalid);
        CHECK(consume(adapter, report(4)).output.armed);
    }
}

void exactAgeBoundaryThenOneTickExpiresWithoutRefreshingReceipt() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto sample = report(2, 500, 600, fire);
    const auto atLimit = consume(adapter, sample, 1100);
    CHECK(atLimit.validation == SelectedHidValidation::ReadyForPolicy && atLimit.output.buttons.trigger);
    const auto expired = adapter.update(sample, maximumAge, at(1100) + HidClock::duration(1));
    CHECK(expired.validation == SelectedHidValidation::Stale);
    CHECK(expired.candidate.status == GunSampleStatus::Stale);
    neutral(expired.output, GunSourceState::Stale);
    sameAim(atLimit.output.aim, expired.output.aim);
    provenance(expired.candidate, sample);
    const auto repeated = consume(adapter, sample, 9000);
    CHECK(repeated.validation == SelectedHidValidation::Stale);
    sameAim(atLimit.output.aim, repeated.output.aim);
    CHECK(repeated.candidate.identity.receivedAt == at(1000));
    neutral(consume(adapter, report(3, 500, 600, fire, 9000), 9000).output, GunSourceState::AwaitNeutral);
    CHECK(consume(adapter, report(4, 500, 600, 0, 9001), 9001).output.armed);
}

void futureReceiptRejectsWithoutPoisoningRecovery() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(2, 100, 200, scope));
    const auto future = report(100, 300, 400, fire, 9000);
    const auto result = consume(adapter, future, 1001);
    CHECK(result.validation == SelectedHidValidation::FutureReceipt);
    neutral(result.output, GunSourceState::InvalidSample);
    provenance(result.candidate, future);
    sameAim(held.output.aim, result.output.aim);
    const auto recovered = consume(adapter, report(3, 500, 600, 0, 1001), 1001);
    CHECK(recovered.validation == SelectedHidValidation::ReadyForPolicy && recovered.output.armed);
}

void receiptRegressionRejectsButEqualTimestampNewGenerationWorks() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(10, 100, 200, fire, 1050), 1050);
    const auto regressed = consume(adapter, report(100, 300, 400, 0, 1049), 1050);
    CHECK(regressed.validation == SelectedHidValidation::ReceiptRegression);
    neutral(regressed.output, GunSourceState::InvalidSample);
    sameAim(held.output.aim, regressed.output.aim);
    const auto equal = consume(adapter, report(11, 500, 600, 0, 1050), 1050);
    CHECK(equal.validation == SelectedHidValidation::ReadyForPolicy && equal.output.armed);
    CHECK(equal.output.aim.identity.generation == 11 && equal.output.aim.identity.receivedAt == at(1050));
    CHECK(consume(adapter, report(12, 500, 600, scope, 1050), 1050).output.buttons.scopeRight);
}

void eachImmutableTupleFieldIsCheckedIncludingUnassignedButtons() {
    for (int mutation = 0; mutation < 5; ++mutation) {
        SelectedHidAdapter adapter;
        arm(adapter);
        const auto sample = report(2, 100, 200, scope);
        const auto held = consume(adapter, sample);
        auto torn = sample;
        switch (mutation) {
            case 0: ++torn.x; break;
            case 1: ++torn.y; break;
            case 2: torn.buttons |= fire; break;
            case 3: torn.buttons |= 0x0400; break; // Still report data, despite no binding.
            default: torn.timestamp += HidClock::duration(1); break;
        }
        const auto result = consume(adapter, torn, 1001);
        CHECK(result.validation == SelectedHidValidation::IncoherentReport);
        neutral(result.output, GunSourceState::InvalidSample);
        sameAim(held.output.aim, result.output.aim);
        neutral(consume(adapter, sample, 1001).output, GunSourceState::AwaitFreshReport);
        CHECK(consume(adapter, report(3, 100, 200, 0, 1001), 1001).output.armed);
    }
}

void generationRegressionDoesNotLowerTheNeutralBarrier() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(20, 100, 200, fire));
    const auto result = consume(adapter, report(19, 300, 400, 0, 1001), 1001);
    CHECK(result.validation == SelectedHidValidation::GenerationRegression);
    neutral(result.output, GunSourceState::InvalidSample);
    sameAim(held.output.aim, result.output.aim);
    neutral(consume(adapter, report(20, 100, 200, fire), 1001).output, GunSourceState::AwaitFreshReport);
    CHECK(consume(adapter, report(21, 300, 400, 0, 1001), 1001).output.armed);
}

void focusLossConsumesFreshReportsButCannotRearmThemOnReturn() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(10, 100, 200, scope));
    const auto unfocused = consume(adapter, report(12, 300, 400, 0, 1001), 1001, false);
    CHECK(unfocused.validation == SelectedHidValidation::ReadyForPolicy);
    neutral(unfocused.output, GunSourceState::Unfocused);
    sameAim(held.output.aim, unfocused.output.aim);
    const auto old = consume(adapter, report(11, 300, 400, 0, 1001), 1001, false);
    CHECK(old.validation == SelectedHidValidation::GenerationRegression);
    neutral(old.output, GunSourceState::Unfocused);
    neutral(consume(adapter, report(12, 300, 400, 0, 1001), 1002).output, GunSourceState::AwaitFreshReport);
    neutral(consume(adapter, report(13, 300, 400, scope, 1002), 1002).output, GunSourceState::AwaitNeutral);
    const auto resumed = consume(adapter, report(14, 500, 600, 0, 1003), 1003);
    CHECK(resumed.output.armed && resumed.output.buttons.neutral());
    CHECK(resumed.output.aim.identity.generation == 14);
    CHECK(consume(adapter, report(15, 500, 600, scope, 1004), 1004).output.buttons.scopeRight);
}

void newOpaqueSessionCanRestartItsSequenceAndReceiptHistory() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(100, 100, 200, fire, 1050), 1050);
    const auto newSession = report(1, 300, 400, fire, 1040, 2);
    const auto transition = consume(adapter, newSession, 1050);
    CHECK(transition.validation == SelectedHidValidation::ReadyForPolicy);
    neutral(transition.output, GunSourceState::Transition);
    sameAim(held.output.aim, transition.output.aim);
    neutral(consume(adapter, newSession, 1050).output, GunSourceState::AwaitNeutral);
    const auto resumed = consume(adapter, report(2, 500, 600, 0, 1041, 2), 1050);
    CHECK(resumed.output.armed && resumed.output.aim.identity.session == 2);
    CHECK(resumed.output.aim.identity.receivedAt == at(1041));
    const auto neutralSession = report(1, 700, 800, 0, 1042, 3);
    neutral(consume(adapter, neutralSession, 1050).output, GunSourceState::Transition);
    CHECK(consume(adapter, neutralSession, 1050).output.armed);
}

void configurationIsAtomicAndChangedMappingsResetBothHistories() {
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto held = consume(adapter, report(10, 100, 200, fire, 1050), 1050);
    CHECK(adapter.configure(mapping) == GunConfigureResult::Unchanged);
    CHECK(adapter.snapshot().buttons.trigger);
    CHECK(adapter.configure({4u, 4u, {}}) == GunConfigureResult::InvalidMapping);
    CHECK(adapter.configured() && adapter.snapshot().buttons.trigger);
    CHECK(adapter.snapshot().selectionGeneration == held.output.selectionGeneration);
    CHECK(consume(adapter, report(9, 300, 400), 1050).validation == SelectedHidValidation::GenerationRegression);
    CHECK(adapter.configure({16u, 4u, 1u}) == GunConfigureResult::Changed);
    neutral(adapter.snapshot(), GunSourceState::Transition);
    sameAim(held.output.aim, adapter.snapshot().aim);
    const auto lower = report(1, 500, 600, 0, 1040);
    const auto transition = consume(adapter, lower, 1050);
    CHECK(transition.validation == SelectedHidValidation::ReadyForPolicy);
    neutral(transition.output, GunSourceState::Transition);
    CHECK(consume(adapter, lower, 1050).output.armed);
    const auto remapped = consume(adapter, report(2, 500, 600, scope, 1041), 1050);
    CHECK(remapped.output.buttons.trigger && !remapped.output.buttons.scopeRight);
}

void invalidAgeNeverBecomesUnlimitedInput() {
    const std::array<std::chrono::milliseconds, 3> invalidAges {
        0ms, -1ms, (std::chrono::milliseconds::max)()};
    for (const auto maximum : invalidAges) {
        // A clock whose representation ceiling equals milliseconds::max does
        // not have an unrepresentable positive millisecond fixture.
        if (maximum == selectedHidMaximumReportAge) continue;
        SelectedHidAdapter adapter;
        arm(adapter);
        const auto held = consume(adapter, report(2, 100, 200, scope));
        const auto result = adapter.update(report(3), maximum, at(1000));
        CHECK(result.validation == SelectedHidValidation::InvalidMaximumAge);
        neutral(result.output, GunSourceState::InvalidSample);
        sameAim(held.output.aim, result.output.aim);
        CHECK(consume(adapter, report(3)).output.armed);
    }
    SelectedHidAdapter adapter;
    arm(adapter);
    const auto ceiling = adapter.update(report(2), selectedHidMaximumReportAge, at(1000));
    CHECK(ceiling.validation == SelectedHidValidation::ReadyForPolicy && ceiling.output.armed);
}

void extremeClockEpochsDoNotOverflowSignedDurations() {
    SelectedHidAdapter adapter;
    CHECK(adapter.configure(mapping) == GunConfigureResult::Changed);
    auto minimum = report(1);
    minimum.timestamp = (HidClock::time_point::min)();
    const auto enormous = adapter.update(minimum, maximumAge, (HidClock::time_point::max)());
    CHECK(enormous.validation == SelectedHidValidation::Stale);
    CHECK(enormous.candidate.status == GunSampleStatus::Stale);
    auto maximum = minimum;
    maximum.timestamp = (HidClock::time_point::max)();
    CHECK(adapter.update(maximum, maximumAge, minimum.timestamp).validation == SelectedHidValidation::FutureReceipt);
    const auto nearMinimum = adapter.update(minimum, maximumAge, minimum.timestamp + HidClock::duration(1));
    CHECK(nearMinimum.validation == SelectedHidValidation::ReadyForPolicy && nearMinimum.output.armed);
    CHECK(nearMinimum.output.aim.identity.receivedAt == minimum.timestamp);
    maximum.generation = 2;
    const auto atMaximum = adapter.update(maximum, maximumAge, maximum.timestamp);
    CHECK(atMaximum.validation == SelectedHidValidation::ReadyForPolicy && atMaximum.output.armed);
    CHECK(atMaximum.output.aim.identity.receivedAt == maximum.timestamp);
    CHECK(adapter.update(minimum, maximumAge, maximum.timestamp).validation == SelectedHidValidation::GenerationRegression);
}

void returnedResultsAreIndependentCopies() {
    SelectedHidAdapter adapter;
    arm(adapter);
    auto sample = report(2, 100, 200, scope);
    auto result = consume(adapter, sample);
    const auto original = result.output;
    sample.x = 65535;
    sample.buttons = fire;
    result.output.aim.x = 0;
    result.output.buttons.trigger = true;
    result.candidate.identity.receivedAt = at(9999);
    sameAim(original.aim, adapter.snapshot().aim);
    CHECK(!adapter.snapshot().buttons.trigger && adapter.snapshot().buttons.scopeRight);
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] {
        {"explicit configuration", explicitConfigurationOwnsNoInputBeforeSuccess},
        {"coherent tuple and provenance", coherentTupleHasNoInversionSmoothingOrRetimestamp},
        {"transport loss and neutral recovery", everyTransportStatusReleasesWithoutFallback},
        {"contradictory status and identity", contradictoryStatusesAndInvalidIdentitiesCannotDriveButtons},
        {"age boundary and repeated snapshots", exactAgeBoundaryThenOneTickExpiresWithoutRefreshingReceipt},
        {"future receipt rejection and recovery", futureReceiptRejectsWithoutPoisoningRecovery},
        {"receipt regression and equal timestamps", receiptRegressionRejectsButEqualTimestampNewGenerationWorks},
        {"immutable report tuple", eachImmutableTupleFieldIsCheckedIncludingUnassignedButtons},
        {"generation regression barrier", generationRegressionDoesNotLowerTheNeutralBarrier},
        {"focus loss and fresh neutral rearming", focusLossConsumesFreshReportsButCannotRearmThemOnReturn},
        {"new opaque session", newOpaqueSessionCanRestartItsSequenceAndReceiptHistory},
        {"atomic reconfiguration", configurationIsAtomicAndChangedMappingsResetBothHistories},
        {"invalid maximum age", invalidAgeNeverBecomesUnlimitedInput},
        {"extreme clock epochs", extremeClockEpochsDoNotOverflowSignedDurations},
        {"value-copy isolation", returnedResultsAreIndependentCopies},
    };
    for (const auto& test : tests) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Passed " << (sizeof(tests) / sizeof(tests[0])) << " selected HID adapter cases\n";
    return 0;
}
