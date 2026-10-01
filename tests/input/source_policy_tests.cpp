#include "input/source_policy.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bone_eater::input;

namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

// Deliberately non-default, separated usages; these are synthetic fixtures,
// not proposed assignments for either physical XGUNNER.
const SelectedGunButtonMapping mapping {4u, 16u, 1u};
constexpr std::uint32_t fire = 0x0008, scope = 0x8000, leftScope = 0x0001;

GunSampleIdentity identity(std::uint64_t generation, std::uint64_t session = 1) {
    return {session, generation, GunSourceClock::time_point(std::chrono::milliseconds(generation))};
}
GunSourceInput legacy(std::uint64_t generation, std::uint32_t x = 12000, std::uint32_t y = 48000,
        GunButtons buttons = {}, std::uint64_t session = 1) {
    GunSourceInput input;
    input.legacy = {GunSampleStatus::Valid, x, y, buttons, identity(generation, session)};
    return input;
}
GunSourceInput selected(std::uint64_t generation, std::uint32_t x = 19000, std::uint32_t y = 37000,
        std::uint32_t buttons = 0, std::uint64_t session = 1) {
    GunSourceInput input;
    input.selected = {GunSampleStatus::Valid, x, y, buttons, identity(generation, session)};
    return input;
}
void neutral(const GunSourceOutput& output, GunSourceState state) {
    CHECK(output.state == state);
    CHECK(output.buttons.neutral());
    CHECK(!output.armed);
    CHECK(!output.sourceUsable);
}
void sameAim(const GunAimPoint& a, const GunAimPoint& b) {
    CHECK(a.known == b.known);
    CHECK(a.x == b.x && a.y == b.y);
    CHECK(a.source == b.source);
    CHECK(a.identity.session == b.identity.session);
    CHECK(a.identity.generation == b.identity.generation);
    CHECK(a.identity.receivedAt == b.identity.receivedAt);
}
void armSelected(GunSourcePolicy& policy) {
    CHECK(policy.configure(GunSourceMode::SelectedHid, mapping) == GunConfigureResult::Changed);
    neutral(policy.update(selected(1)), GunSourceState::Transition);
    CHECK(policy.update(selected(1)).armed);
}

void mappingIsExplicitAndBounded() {
    CHECK(!validSelectedGunButtonMapping({}));
    CHECK(!validSelectedGunButtonMapping({1u, {}, {}}));
    CHECK(!validSelectedGunButtonMapping({{}, 2u, {}}));
    CHECK(!validSelectedGunButtonMapping({0u, 2u, {}}));
    CHECK(!validSelectedGunButtonMapping({1u, 17u, {}}));
    CHECK(!validSelectedGunButtonMapping({1u, 2u, 0u}));
    CHECK(!validSelectedGunButtonMapping({1u, 1u, {}}));
    CHECK(!validSelectedGunButtonMapping({1u, 2u, 1u}));
    CHECK(!validSelectedGunButtonMapping({1u, 2u, 2u}));
    CHECK(validSelectedGunButtonMapping({1u, 16u, {}}));
    CHECK(validSelectedGunButtonMapping(mapping));
    CHECK(!mapSelectedGunButtons(0x10000, mapping));
    CHECK(!mapSelectedGunButtons(0, {}));
    CHECK(mapSelectedGunButtons(0x0400, mapping)->neutral()); // Unassigned usage ignored.
    const auto onlyFire = *mapSelectedGunButtons(fire, mapping);
    CHECK(onlyFire.trigger && !onlyFire.scopeRight && !onlyFire.scopeLeft);
    const auto onlyRight = *mapSelectedGunButtons(scope, mapping);
    CHECK(!onlyRight.trigger && onlyRight.scopeRight && !onlyRight.scopeLeft);
    const auto onlyLeft = *mapSelectedGunButtons(leftScope, mapping);
    CHECK(!onlyLeft.trigger && !onlyLeft.scopeRight && onlyLeft.scopeLeft);
    const auto all = *mapSelectedGunButtons(65535, mapping);
    CHECK(all.trigger && all.scopeRight && all.scopeLeft);
    CHECK(!mapSelectedGunButtons(leftScope, {4u, 16u, {}})->scopeLeft);
}

void initialStateDoesNotInventAimOrButtonEdges() {
    GunSourcePolicy policy;
    const auto initial = policy.snapshot();
    CHECK(initial.mode == GunSourceMode::Legacy);
    CHECK(!initial.aim.known);
    CHECK(initial.consumptionGeneration == 0);
    neutral(initial, GunSourceState::Transition);
    neutral(policy.update({}), GunSourceState::Transition);
    neutral(policy.update({}), GunSourceState::Unavailable);
    CHECK(!policy.snapshot().aim.known);
    auto input = legacy(1, 0, 65535, {true, true, true});
    neutral(policy.update(input), GunSourceState::AwaitNeutral);
    CHECK(!policy.snapshot().aim.known);
    neutral(policy.update(input), GunSourceState::AwaitNeutral);
    input = legacy(2, 0, 65535);
    const auto output = policy.update(input);
    CHECK(output.state == GunSourceState::Active && output.armed && output.sourceUsable);
    CHECK(output.aim.known && output.aim.x == 0 && output.aim.y == 65535);
    CHECK(output.aim.source == GunSourceMode::Legacy);
}

void legacyTuplePassesWithoutAxisInversionOrSmoothing() {
    GunSourcePolicy policy;
    neutral(policy.update(legacy(1)), GunSourceState::Transition);
    CHECK(policy.update(legacy(1)).armed);
    const auto output = policy.update(legacy(2, 65535, 0, {true, false, true}));
    CHECK(output.aim.x == 65535 && output.aim.y == 0);
    CHECK(output.buttons.trigger && !output.buttons.scopeRight && output.buttons.scopeLeft);
    CHECK(output.aim.identity.generation == 2);
    CHECK(output.aim.identity.receivedAt == identity(2).receivedAt);
    const auto repeated = policy.update(legacy(2, 65535, 0, {true, false, true}));
    sameAim(output.aim, repeated.aim);
    CHECK(repeated.buttons.trigger && repeated.armed);
    CHECK(repeated.consumptionGeneration == output.consumptionGeneration + 1);
}

void selectedSourceOwnsEveryGunField() {
    GunSourcePolicy policy;
    armSelected(policy);
    auto input = selected(2, 10, 60000, scope);
    input.legacy = legacy(99, 65535, 0, {true, true, true}).legacy;
    const auto output = policy.update(input);
    CHECK(output.mode == GunSourceMode::SelectedHid && output.aim.source == output.mode);
    CHECK(output.aim.x == 10 && output.aim.y == 60000);
    CHECK(!output.buttons.trigger && output.buttons.scopeRight && !output.buttons.scopeLeft);
    CHECK(output.aim.identity.generation == 2);
    input.legacy = legacy(100, 0, 65535).legacy;
    const auto repeated = policy.update(input);
    sameAim(output.aim, repeated.aim);
    CHECK(repeated.buttons.scopeRight);
    input.selected.status = GunSampleStatus::Disconnected;
    input.legacy = legacy(101, 32000, 32000, {true, false, false}).legacy;
    const auto disconnected = policy.update(input);
    neutral(disconnected, GunSourceState::Disconnected);
    sameAim(output.aim, disconnected.aim);
    CHECK(disconnected.mode == GunSourceMode::SelectedHid); // Never silently falls back.
}

void legacyIgnoresUnselectedHid() {
    GunSourcePolicy policy;
    policy.update(legacy(1));
    policy.update(legacy(1));
    auto input = legacy(2, 200, 500, {false, true, false});
    input.selected = selected(100, 65535, 0, fire | scope | leftScope).selected;
    const auto output = policy.update(input);
    CHECK(output.aim.x == 200 && output.aim.y == 500 && output.aim.identity.generation == 2);
    CHECK(!output.buttons.trigger && output.buttons.scopeRight && !output.buttons.scopeLeft);
}

void explicitSwitchReleasesBeforeRearmingIncomingSource() {
    GunSourcePolicy policy;
    policy.update(legacy(1));
    policy.update(legacy(1));
    const auto previous = policy.update(legacy(2, 111, 222, {true, true, true}));
    CHECK(policy.configure(GunSourceMode::SelectedHid, mapping) == GunConfigureResult::Changed);
    neutral(policy.snapshot(), GunSourceState::Transition);
    sameAim(previous.aim, policy.snapshot().aim);
    neutral(policy.update(selected(1, 60000, 50000, fire | scope)), GunSourceState::Transition);
    neutral(policy.update(selected(1, 60000, 50000, fire | scope)), GunSourceState::AwaitNeutral);
    sameAim(previous.aim, policy.snapshot().aim);
    CHECK(policy.update(selected(2, 50000, 40000)).armed);
    const auto held = policy.update(selected(3, 40000, 30000, fire));
    CHECK(held.buttons.trigger);
    CHECK(policy.configure(GunSourceMode::Legacy) == GunConfigureResult::Changed);
    neutral(policy.update(legacy(8, 1, 2, {true, false, false})), GunSourceState::Transition);
    neutral(policy.update(legacy(8, 1, 2, {true, false, false})), GunSourceState::AwaitNeutral);
    sameAim(held.aim, policy.snapshot().aim);
    const auto returned = policy.update(legacy(9, 3, 4));
    CHECK(returned.armed && returned.aim.source == GunSourceMode::Legacy);
    CHECK(returned.aim.x == 3 && returned.aim.y == 4);
    CHECK(returned.selectionGeneration == previous.selectionGeneration + 2);
}

void configurationValidationIsAtomicAndIdempotent() {
    GunSourcePolicy policy;
    armSelected(policy);
    const auto held = policy.update(selected(2, 10, 20, fire | scope));
    CHECK(policy.configure(GunSourceMode::SelectedHid, {}) == GunConfigureResult::InvalidMapping);
    CHECK(policy.configure(static_cast<GunSourceMode>(-1), mapping) == GunConfigureResult::InvalidMode);
    sameAim(held.aim, policy.snapshot().aim);
    CHECK(policy.snapshot().buttons.trigger && policy.snapshot().buttons.scopeRight);
    CHECK(policy.snapshot().selectionGeneration == held.selectionGeneration);
    CHECK(policy.configure(GunSourceMode::SelectedHid, mapping) == GunConfigureResult::Unchanged);
    CHECK(policy.snapshot().buttons.trigger);
    const SelectedGunButtonMapping changed {16u, 4u, 1u};
    CHECK(policy.configure(GunSourceMode::SelectedHid, changed) == GunConfigureResult::Changed);
    neutral(policy.snapshot(), GunSourceState::Transition);
    neutral(policy.update(selected(2, 10, 20, fire | scope)), GunSourceState::Transition);
    neutral(policy.update(selected(2, 10, 20, fire | scope)), GunSourceState::AwaitNeutral);
    CHECK(policy.update(selected(3)).armed);
    const auto remapped = policy.update(selected(4, 10, 20, scope));
    CHECK(remapped.buttons.trigger && !remapped.buttons.scopeRight);
}

void everyLossReleasesAndRequiresFreshNeutral() {
    const std::array<GunSampleStatus, 4> statuses {{GunSampleStatus::Unavailable,
        GunSampleStatus::Stale, GunSampleStatus::Disconnected, GunSampleStatus::Invalid}};
    const std::array<GunSourceState, 4> states {{GunSourceState::Unavailable,
        GunSourceState::Stale, GunSourceState::Disconnected, GunSourceState::InvalidSample}};
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        GunSourcePolicy policy;
        armSelected(policy);
        const auto held = policy.update(selected(2, 60000, 300, fire | scope | leftScope));
        auto lost = selected(2, 0, 0);
        lost.selected.status = statuses[i];
        neutral(policy.update(lost), states[i]);
        sameAim(held.aim, policy.snapshot().aim);
        // Reclassifying a cached report as Valid must not replay its held buttons.
        neutral(policy.update(selected(2, 60000, 300, fire | scope | leftScope)),
            GunSourceState::AwaitFreshReport);
        neutral(policy.update(selected(3, 61000, 100, fire)), GunSourceState::AwaitNeutral);
        sameAim(held.aim, policy.snapshot().aim);
        const auto released = policy.update(selected(4, 62000, 50));
        CHECK(released.armed && released.buttons.neutral());
        CHECK(released.aim.x == 62000 && released.aim.y == 50);
        CHECK(policy.update(selected(5, 62000, 50, fire)).buttons.trigger);
    }
}

void focusCannotRearmCachedOrRegressingReports() {
    GunSourcePolicy policy;
    armSelected(policy);
    const auto before = policy.update(selected(10, 1000, 2000, scope));
    auto input = selected(12, 3000, 4000);
    input.focused = false;
    neutral(policy.update(input), GunSourceState::Unfocused);
    input = selected(11, 9000, 9000);
    input.focused = false;
    neutral(policy.update(input), GunSourceState::Unfocused);
    sameAim(before.aim, policy.snapshot().aim);
    neutral(policy.update(selected(12, 3000, 4000)), GunSourceState::AwaitFreshReport);
    // A fresh held button still cannot cross the focus boundary.
    neutral(policy.update(selected(13, 3000, 4000, scope)), GunSourceState::AwaitNeutral);
    const auto resumed = policy.update(selected(14, 3000, 4000));
    CHECK(resumed.armed && resumed.buttons.neutral());
    CHECK(resumed.aim.identity.generation == 14);
    CHECK(policy.update(selected(15, 3000, 4000, scope)).buttons.scopeRight);
}

void sessionChangeHasIndependentNeutralBoundary() {
    GunSourcePolicy policy;
    armSelected(policy);
    const auto old = policy.update(selected(100, 400, 500, fire));
    neutral(policy.update(selected(1, 600, 700, fire, 2)), GunSourceState::Transition);
    neutral(policy.update(selected(1, 600, 700, fire, 2)), GunSourceState::AwaitNeutral);
    sameAim(old.aim, policy.snapshot().aim);
    const auto resumed = policy.update(selected(2, 800, 900, 0, 2));
    CHECK(resumed.armed && resumed.aim.identity.session == 2);
    // Even a new session that is already neutral publishes a release boundary.
    neutral(policy.update(selected(1, 1000, 1100, 0, 3)), GunSourceState::Transition);
    CHECK(policy.update(selected(1, 1000, 1100, 0, 3)).armed);
}

void sameGenerationMustRemainOneImmutableTuple() {
    for (int mutation = 0; mutation < 5; ++mutation) {
        GunSourcePolicy policy;
        armSelected(policy);
        const auto old = policy.update(selected(2, 100, 200, scope));
        auto torn = selected(2, 100, 200, scope);
        switch (mutation) {
            case 0: ++torn.selected.x; break;
            case 1: ++torn.selected.y; break;
            case 2: torn.selected.rawButtons |= fire; break;
            case 3: torn.selected.rawButtons |= 0x0400; break; // Unmapped bits are report data too.
            default: torn.selected.identity.receivedAt += std::chrono::milliseconds(1); break;
        }
        neutral(policy.update(torn), GunSourceState::IncoherentSample);
        sameAim(old.aim, policy.snapshot().aim);
        neutral(policy.update(selected(2, 100, 200, scope)), GunSourceState::AwaitFreshReport);
        CHECK(policy.update(selected(3, 100, 200)).armed);
    }
}

void generationRegressionCannotReplayAnOldNeutralReport() {
    GunSourcePolicy policy;
    armSelected(policy);
    const auto held = policy.update(selected(20, 500, 600, fire));
    neutral(policy.update(selected(19, 100, 200)), GunSourceState::IncoherentSample);
    neutral(policy.update(selected(20, 500, 600, fire)), GunSourceState::AwaitFreshReport);
    sameAim(held.aim, policy.snapshot().aim);
    CHECK(policy.update(selected(21, 900, 1000)).armed);
}

void malformedSamplesReleaseWithoutClampingOrCorruptingAim() {
    for (int mutation = 0; mutation < 6; ++mutation) {
        GunSourcePolicy policy;
        armSelected(policy);
        const auto old = policy.update(selected(2, 100, 200, fire));
        auto invalid = selected(3, 100, 200, fire);
        switch (mutation) {
            case 0: invalid.selected.x = 65536; break;
            case 1: invalid.selected.y = 0xFFFFFFFFu; break;
            case 2: invalid.selected.rawButtons = 65536; break;
            case 3: invalid.selected.identity.session = 0; break;
            case 4: invalid.selected.identity.generation = 0; break;
            default: invalid.selected.status = static_cast<GunSampleStatus>(-1); break;
        }
        neutral(policy.update(invalid), GunSourceState::InvalidSample);
        sameAim(old.aim, policy.snapshot().aim);
        CHECK(policy.update(selected(4, 0, 65535)).armed);
    }
    GunSourcePolicy legacyPolicy;
    legacyPolicy.update(legacy(1));
    legacyPolicy.update(legacy(1));
    neutral(legacyPolicy.update(legacy(2, 65536, 0, {true, true, true})), GunSourceState::InvalidSample);
}

void freshnessRemainsAnExplicitCallerDecision() {
    GunSourcePolicy policy;
    armSelected(policy);
    // No arbitrary HID cadence/time limit is inferred by this portable layer.
    // These synthetic receipt stamps are intentionally not tied to wall clock.
    auto input = selected(2, 10, 20, scope);
    input.selected.identity.receivedAt = GunSourceClock::time_point {};
    const auto held = policy.update(input);
    CHECK(held.armed && held.buttons.scopeRight);
    // A higher generation with an older receipt stamp is accepted only because
    // this injected caller explicitly classifies it Valid; policy never clocks it.
    CHECK(held.aim.identity.receivedAt == GunSourceClock::time_point {});
    for (int i = 0; i < 100; ++i) CHECK(policy.update(input).buttons.scopeRight);
    sameAim(held.aim, policy.snapshot().aim);
    input.selected.status = GunSampleStatus::Stale;
    neutral(policy.update(input), GunSourceState::Stale);
    input.selected.status = GunSampleStatus::Valid;
    neutral(policy.update(input), GunSourceState::AwaitFreshReport);
}

void returnedSnapshotsCannotAlterPolicyState() {
    GunSourcePolicy policy;
    armSelected(policy);
    const auto output = policy.update(selected(2, 123, 456, scope));
    auto copy = policy.snapshot();
    copy.aim.x = 0;
    copy.buttons.trigger = true;
    copy.mode = GunSourceMode::Legacy;
    sameAim(output.aim, policy.snapshot().aim);
    CHECK(!policy.snapshot().buttons.trigger && policy.snapshot().buttons.scopeRight);
    CHECK(policy.snapshot().mode == GunSourceMode::SelectedHid);
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] {
        {"explicit bounded button mappings", mappingIsExplicitAndBounded},
        {"initial neutral and unknown aim", initialStateDoesNotInventAimOrButtonEdges},
        {"legacy whole tuple", legacyTuplePassesWithoutAxisInversionOrSmoothing},
        {"selected exclusive whole tuple", selectedSourceOwnsEveryGunField},
        {"legacy ignores unselected HID", legacyIgnoresUnselectedHid},
        {"explicit source transition", explicitSwitchReleasesBeforeRearmingIncomingSource},
        {"atomic configuration and remapping", configurationValidationIsAtomicAndIdempotent},
        {"loss releases and rearms", everyLossReleasesAndRequiresFreshNeutral},
        {"focus and monotonic barrier", focusCannotRearmCachedOrRegressingReports},
        {"new session boundary", sessionChangeHasIndependentNeutralBoundary},
        {"immutable coherent tuple", sameGenerationMustRemainOneImmutableTuple},
        {"generation regression", generationRegressionCannotReplayAnOldNeutralReport},
        {"malformed samples", malformedSamplesReleaseWithoutClampingOrCorruptingAim},
        {"caller-selected freshness", freshnessRemainsAnExplicitCallerDecision},
        {"snapshot copy isolation", returnedSnapshotsCannotAlterPolicyState},
    };
    for (const auto& test : tests) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Passed " << (sizeof(tests) / sizeof(tests[0])) << " source policy cases\n";
    return 0;
}
