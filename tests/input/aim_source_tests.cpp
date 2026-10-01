#include "input/aim_state.h"
#include "input/sample_age.h"

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
GunSourceClock::time_point at(std::int64_t ms) {
    return GunSourceClock::time_point(std::chrono::duration_cast<GunSourceClock::duration>(std::chrono::milliseconds(ms)));
}
GunSourceOutput active() {
    GunSourceOutput output;
    output.mode = GunSourceMode::SelectedHid;
    output.state = GunSourceState::Active;
    output.aim = {true, 12345, 54321, GunSourceMode::SelectedHid, {7, 25, at(1000)}};
    output.buttons = {true, true, false};
    output.armed = output.sourceUsable = true;
    output.consumptionGeneration = 31;
    return output;
}
void originalReceiptSurvivesRepeatedCabinetPublication() {
    AimState state;
    auto output = active();
    state.publishSelected(output, 100ms, at(1010), 1);
    const auto before = state.read();
    CHECK(before.valid && before.aimKnown && before.armed && before.triggerHeld && before.scopeRightHeld);
    CHECK(before.sourceIdentity.session == 7 && before.sourceIdentity.generation == 25);
    CHECK(before.sourceIdentity.receivedAt == at(1000) && before.sampledAt == at(1010));
    CHECK(before.sourceConsumptionGeneration == 31 && before.cabinetGeneration == 1);
    ++output.consumptionGeneration;
    state.publishSelected(output, 100ms, at(1099), 2);
    const auto after = state.read();
    CHECK(after.generation == before.generation + 1 && after.cabinetGeneration == 2);
    CHECK(after.sourceConsumptionGeneration == 32 && after.sourceIdentity.generation == 25);
    CHECK(after.sourceIdentity.receivedAt == at(1000) && after.sampledAt == at(1099));
    CHECK(aimUsableAt(after, at(1100), 250ms));
    CHECK(!aimUsableAt(after, at(1100) + GunSourceClock::duration(1), 250ms));
}
void cabinetAndSourceFreshnessAreIndependent() {
    AimState state;
    state.publishSelected(active(), 2000ms, at(1000), 1);
    const auto value = state.read();
    CHECK(aimUsableAt(value, at(1250), 250ms));
    CHECK(!aimUsableAt(value, at(1251), 250ms));
    CHECK(!aimUsableAt(value, at(999), 250ms));
    CHECK(!aimUsableAt(value, at(1000), 0ms));
    CHECK(!aimUsableAt(value, at(1000), (std::chrono::milliseconds::max)()));
}
void lifecycleReleasePreservesPointButIsNeverEligible() {
    AimState state;
    auto output = active();
    state.publishSelected(output, 100ms, at(1000), 3);
    output.state = GunSourceState::Disconnected;
    output.armed = output.sourceUsable = false;
    // Defensive publisher must disregard even malformed held bits after loss.
    state.publishSelected(output, 100ms, at(1001), 3);
    const auto released = state.read();
    CHECK(!released.valid && !released.armed && !released.scopeIntentHeld() && !released.triggerHeld);
    CHECK(released.rawX == 12345 && released.rawY == 54321 && released.aimKnown);
    CHECK(released.sourceIdentity.generation == 25 && released.sourceIdentity.receivedAt == at(1000));
    CHECK(released.cabinetGeneration == 3 && released.generation == 2);
    CHECK(!aimUsableAt(released, at(1001), 250ms));
}
void unknownAimAndMalformedSourcesFailClosed() {
    for (unsigned mutation = 0; mutation < 7; ++mutation) {
        AimState state;
        auto output = active();
        switch (mutation) {
            case 0: output.aim.known = false; break;
            case 1: output.aim.identity.session = 0; break;
            case 2: output.aim.identity.generation = 0; break;
            case 3: output.aim.identity.receivedAt = at(1001); break;
            case 4: output.mode = GunSourceMode::Legacy; break;
            case 5: output.aim.source = GunSourceMode::Legacy; break;
            default: output.sourceUsable = false; break;
        }
        state.publishSelected(output, 100ms, at(1000), 1);
        const auto value = state.read();
        CHECK(!value.valid && !value.triggerHeld && !value.scopeIntentHeld());
        CHECK(!aimUsableAt(value, at(1000), 250ms));
        if (!output.aim.known) CHECK(!value.aimKnown && value.rawX == 0x7FFF && value.rawY == 0x7FFF);
    }
}
void legacyPublicationKeepsItsExistingEligibility() {
    AimState state;
    state.publish(0, 65535, true, false, true);
    const auto value = state.read();
    CHECK(value.sourceMode == GunSourceMode::Legacy && value.valid && value.rawX == 0 && value.rawY == 65535);
    CHECK(value.scopeIntentHeld() && value.triggerHeld);
    CHECK(value.sourceIdentity.generation == 0 && value.sourceMaximumAge == 0ms);
    CHECK(aimUsableAt(value, value.sampledAt, 250ms));
    CHECK(aimUsableAt(value, value.sampledAt + 250ms, 250ms));
    CHECK(!aimUsableAt(value, value.sampledAt + 251ms, 250ms));
}
void signedClockLimitsStaySafe() {
    const auto min = (GunSourceClock::time_point::min)(), max = (GunSourceClock::time_point::max)();
    CHECK(!sampleAgeWithin(min, max, 100ms));
    CHECK(!sampleAgeWithin(max, min, 100ms));
    CHECK(sampleAgeWithin(min, min + GunSourceClock::duration(1), 100ms));
    CHECK(sampleAgeWithin(max, max, selectedHidMaximumReportAge));
}
}
int main() {
    try { originalReceiptSurvivesRepeatedCabinetPublication(); cabinetAndSourceFreshnessAreIndependent();
        lifecycleReleasePreservesPointButIsNeverEligible(); unknownAimAndMalformedSourcesFailClosed();
        legacyPublicationKeepsItsExistingEligibility(); signedClockLimitsStaySafe(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Passed 6 source-aware aim publication cases\n";
}
