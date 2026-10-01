#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace bone_eater::input {

enum class GunSourceMode { Legacy, SelectedHid };
enum class GunSampleStatus { Valid, Unavailable, Stale, Disconnected, Invalid };
enum class GunSourceState {
    Transition, Active, AwaitNeutral, AwaitFreshReport,
    Unavailable, Stale, Disconnected, InvalidSample, IncoherentSample, Unfocused,
};
enum class GunConfigureResult { Unchanged, Changed, InvalidMode, InvalidMapping };

struct GunButtons {
    bool trigger = false, scopeRight = false, scopeLeft = false;
    bool neutral() const noexcept { return !trigger && !scopeRight && !scopeLeft; }
};

// HID usage numbers, not bit indices. No hardware assignments are inferred.
// Trigger and right scope are required; left scope is optional. All assigned
// usages must be distinct and within the proven button1..16 report contract.
struct SelectedGunButtonMapping {
    std::optional<unsigned> trigger, scopeRight, scopeLeft;
};
bool validSelectedGunButtonMapping(const SelectedGunButtonMapping& mapping) noexcept;
std::optional<GunButtons> mapSelectedGunButtons(std::uint32_t rawButtons,
    const SelectedGunButtonMapping& mapping) noexcept;

using GunSourceClock = std::chrono::steady_clock;
struct GunSampleIdentity {
    // Nonzero session and generation belong to the source, not to cabinet polls.
    std::uint64_t session = 0, generation = 0;
    // Source receipt provenance only. Policy compares this within one generation
    // to detect a changed tuple; it does not check clock age, future timestamps,
    // or time monotonicity between generations. Caller owns those validations.
    GunSourceClock::time_point receivedAt {};
};
struct LegacyGunInput {
    GunSampleStatus status = GunSampleStatus::Unavailable;
    std::uint32_t x = 0, y = 0; // Validated unit-domain0..65535; never clamped here.
    GunButtons buttons;
    GunSampleIdentity identity;
};
struct SelectedGunInput {
    GunSampleStatus status = GunSampleStatus::Unavailable;
    std::uint32_t x = 0, y = 0, rawButtons = 0;
    GunSampleIdentity identity;
};
struct GunSourceInput {
    bool focused = true;
    LegacyGunInput legacy;
    SelectedGunInput selected;
};

struct GunAimPoint {
    bool known = false;
    std::uint16_t x = 0x7FFF, y = 0x7FFF; // Placeholder only when known=false.
    GunSourceMode source = GunSourceMode::Legacy;
    GunSampleIdentity identity; // Origin of these coordinates, even when held.
};
struct GunSourceOutput {
    GunSourceMode mode = GunSourceMode::Legacy;
    GunSourceState state = GunSourceState::Transition;
    GunAimPoint aim;
    GunButtons buttons;
    // sourceUsable means eligible for gameplay, not just transport-valid.
    bool sourceUsable = false, armed = false;
    std::uint64_t selectionGeneration = 1, consumptionGeneration = 0;
};

// Single-owner portable policy. It does no device I/O, OS polling, smoothing,
// timeout selection, native calibration or protocol inversion. Caller supplies
// coherent copied inputs and determines source status/freshness externally.
// In particular, status=Valid attests to the adapter's receipt-time checks and
// chosen freshness policy. A higher generation alone does not prove freshness.
// Publish the returned value through a separate synchronized runtime boundary.
class GunSourcePolicy {
public:
    // A rejected configuration changes no state. Caller must handle rejection.
    // Reapplying an identical configuration does not release an active hold.
    GunConfigureResult configure(GunSourceMode mode,
        const SelectedGunButtonMapping& mapping = {}) noexcept;
    GunSourceOutput update(GunSourceInput input) noexcept;
    GunSourceOutput snapshot() const noexcept { return output_; }

private:
    struct Candidate {
        GunSourceState unavailable = GunSourceState::Unavailable;
        bool usable = false;
        std::uint16_t x = 0, y = 0;
        std::uint32_t rawButtons = 0;
        GunButtons buttons;
        GunSampleIdentity identity;
    };
    Candidate candidate(const GunSourceInput& input) const noexcept;
    void release(GunSourceState state) noexcept;
    void remember(const Candidate& sample) noexcept;
    void accept(const Candidate& sample) noexcept;
    static bool identical(const Candidate& a, const Candidate& b) noexcept;

    SelectedGunButtonMapping mapping_;
    GunSourceOutput output_;
    Candidate seen_;
    bool haveSeen_ = false, transition_ = true, needNewReport_ = false;
};

} // namespace bone_eater::input
