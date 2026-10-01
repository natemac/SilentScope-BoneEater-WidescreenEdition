#pragma once

#include "input/selected_hid.h"
#include "input/source_policy.h"

#include <chrono>

namespace bone_eater::input {

enum class SelectedHidValidation {
    ReadyForPolicy, NotConfigured, Unselected, Waiting, Stale, Disconnected,
    Stopped, InvalidReport, InconsistentStatus, InvalidIdentity, InvalidMaximumAge,
    FutureReceipt, ReceiptRegression, GenerationRegression, IncoherentReport,
};

struct SelectedHidAdapterResult {
    SelectedHidValidation validation = SelectedHidValidation::NotConfigured;
    // Exact copied source fields plus the adapter's eligibility classification.
    // Even invalid samples retain provenance here; output.buttons is what may
    // actually drive the cabinet and always releases on nonusable input.
    SelectedGunInput candidate;
    GunSourceOutput output;
};

// Portable single-owner adapter: no I/O, clocks, OS focus query, hardware
// assignments, smoothing, axis inversion, or automatic mouse fallback. A runtime
// caller must serialize configure/update, copy one reader snapshot, then obtain
// 'now' from that same steady clock before calling update. Repeated snapshots
// keep the original reader receipt identity instead of acquiring a fresh age.
class SelectedHidAdapter {
public:
    // Invalid configuration changes nothing. An identical mapping preserves
    // state; a changed mapping releases and resets report validation history.
    GunConfigureResult configure(const SelectedGunButtonMapping& mapping) noexcept;
    bool configured() const noexcept { return configured_; }
    GunSourceOutput snapshot() const noexcept { return policy_.snapshot(); }

    SelectedHidAdapterResult update(SelectedHidSample sample,
        std::chrono::milliseconds maxAge, HidClock::time_point now, bool focused = true) noexcept;

private:
    SelectedHidValidation validate(const SelectedHidSample& sample,
        std::chrono::milliseconds maxAge, HidClock::time_point now,
        GunSampleStatus& status) noexcept;

    GunSourcePolicy policy_;
    SelectedHidSample seen_;
    bool configured_ = false, haveSeen_ = false;
};

} // namespace bone_eater::input
