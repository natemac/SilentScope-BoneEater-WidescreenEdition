#include "input/selected_hid_adapter.h"
#include "input/selected_hid_config.h"

#include <limits>
#include <type_traits>

namespace bone_eater::input {
namespace {
static_assert(std::is_same_v<HidClock, GunSourceClock>);
using ClockRep = HidClock::duration::rep;
static_assert(std::is_integral_v<ClockRep>);
// The shared common-duration age ceiling is safe to convert to the clock only
// while that clock can represent its count range. Fail compilation on an
// unsupported narrower clock representation instead of risking a narrowing
// duration conversion on an otherwise well-formed profile.
static_assert(std::numeric_limits<ClockRep>::digits >=
    std::numeric_limits<SelectedHidAgeComparison::rep>::digits);
using UnsignedClockRep = std::make_unsigned_t<ClockRep>;

bool olderThan(HidClock::time_point receipt, HidClock::time_point now,
        std::chrono::milliseconds maximum) noexcept {
    // Called only after rejecting future receipts and bounding maximum so its
    // clock conversion fits. Unsigned subtraction preserves the mathematical
    // nonnegative difference even across signed min/max epoch counts, where
    // evaluating (now - receipt) in the native signed representation could UB.
    const auto elapsed = static_cast<UnsignedClockRep>(now.time_since_epoch().count()) -
        static_cast<UnsignedClockRep>(receipt.time_since_epoch().count());
    const auto limit = std::chrono::duration_cast<HidClock::duration>(maximum).count();
    return elapsed > static_cast<UnsignedClockRep>(limit);
}
} // namespace

GunConfigureResult SelectedHidAdapter::configure(const SelectedGunButtonMapping& mapping) noexcept {
    const auto result = policy_.configure(GunSourceMode::SelectedHid, mapping);
    if (result == GunConfigureResult::Changed) {
        seen_ = {};
        haveSeen_ = false;
    }
    if (result == GunConfigureResult::Changed || result == GunConfigureResult::Unchanged) configured_ = true;
    return result;
}

SelectedHidValidation SelectedHidAdapter::validate(const SelectedHidSample& sample,
        std::chrono::milliseconds maxAge, HidClock::time_point now,
        GunSampleStatus& status) noexcept {
    using Validation = SelectedHidValidation;
    status = GunSampleStatus::Invalid;
    if (maxAge.count() <= 0 || maxAge > selectedHidMaximumReportAge) return Validation::InvalidMaximumAge;
    if (sample.status != SelectedHidStatus::Active) {
        if (sample.valid) return Validation::InconsistentStatus;
        switch (sample.status) {
            case SelectedHidStatus::Unselected: status = GunSampleStatus::Unavailable; return Validation::Unselected;
            case SelectedHidStatus::Waiting: status = GunSampleStatus::Unavailable; return Validation::Waiting;
            case SelectedHidStatus::InvalidReport: return Validation::InvalidReport;
            case SelectedHidStatus::Disconnected: status = GunSampleStatus::Disconnected; return Validation::Disconnected;
            case SelectedHidStatus::Stopped: status = GunSampleStatus::Disconnected; return Validation::Stopped;
            case SelectedHidStatus::Stale: status = GunSampleStatus::Stale; return Validation::Stale;
            default: return Validation::InconsistentStatus;
        }
    }
    if (!sample.valid) return Validation::InconsistentStatus;
    if (!sample.session || !sample.generation) return Validation::InvalidIdentity;
    if (sample.timestamp > now) return Validation::FutureReceipt;
    if (haveSeen_ && sample.session == seen_.session) {
        if (sample.generation < seen_.generation) return Validation::GenerationRegression;
        if (sample.timestamp < seen_.timestamp) return Validation::ReceiptRegression;
        if (sample.generation == seen_.generation && (sample.timestamp != seen_.timestamp ||
                sample.x != seen_.x || sample.y != seen_.y || sample.buttons != seen_.buttons))
            return Validation::IncoherentReport;
    }
    if (olderThan(sample.timestamp, now, maxAge)) {
        status = GunSampleStatus::Stale;
        return Validation::Stale;
    }
    // Only a coherent, presently eligible source report advances this barrier.
    // A rejected future/corrupt receipt cannot poison subsequent valid reports.
    // A new opaque session starts its own sequence/receipt history; the source
    // policy independently supplies the session transition's neutral boundary.
    seen_ = sample;
    haveSeen_ = true;
    status = GunSampleStatus::Valid;
    return Validation::ReadyForPolicy;
}

SelectedHidAdapterResult SelectedHidAdapter::update(SelectedHidSample sample,
        std::chrono::milliseconds maxAge, HidClock::time_point now, bool focused) noexcept {
    SelectedHidAdapterResult result;
    result.candidate = {GunSampleStatus::Invalid, sample.x, sample.y, sample.buttons,
        {sample.session, sample.generation, sample.timestamp}};
    if (!configured_) {
        // No implicit mapping or legacy polling when configuration was absent
        // or rejected. This adapter has not acquired any input ownership yet.
        result.output = policy_.snapshot();
        return result;
    }
    result.validation = validate(sample, maxAge, now, result.candidate.status);
    GunSourceInput input;
    input.focused = focused;
    input.selected = result.candidate;
    result.output = policy_.update(input);
    return result;
}

} // namespace bone_eater::input
