#pragma once

#include "input/selected_hid_bridge.h"
#include "input/sample_age.h"
#include "external/rapidjson/document.h"

namespace bone_eater::input {

// Pure encoding of one bridge status copy. No HID read, policy update or focus
// discovery occurs here. Cached button values describe the last canonical publication, including a
// lifecycle disarm that may occur without another cabinet packet. They are not
// current physical switches. Receipt eligibility is reported separately.
inline rapidjson::Value selectedInputApiStatus(const SelectedHidBridgeStatus& status,
        GunSourceClock::time_point now, rapidjson::Document::AllocatorType& allocator) {
    using rapidjson::Value;
    Value result(rapidjson::kObjectType);
    result.AddMember("schema_version", 1, allocator);
    result.AddMember("snapshot_kind", "cached_selected_publication", allocator);
    result.AddMember("fresh_hid_poll", false, allocator);
    result.AddMember("configured", status.configured, allocator);
    result.AddMember("selected", status.selected, allocator);
    result.AddMember("shutdown_requested", status.shutdownRequested, allocator);
    result.AddMember("cabinet_generation", status.cabinetGeneration, allocator);
    Value lifecycle;
    if (status.runtime) lifecycle.SetInt(static_cast<int>(status.runtime->lifecycle));
    result.AddMember("runtime_lifecycle_code", lifecycle, allocator);

    const auto& aim = status.aim;
    const bool selected = status.selected && aim.sourceMode == GunSourceMode::SelectedHid;
    Value point(rapidjson::kObjectType);
    point.AddMember("known", selected && aim.aimKnown, allocator);
    point.AddMember("x", aim.rawX, allocator);
    point.AddMember("y", aim.rawY, allocator);
    point.AddMember("axis_domain", "pre_protocol_uint16", allocator);
    point.AddMember("source_state_code", static_cast<int>(aim.sourceState), allocator);
    point.AddMember("armed_at_publication", selected && aim.armed, allocator);
    point.AddMember("valid_at_publication", selected && aim.valid, allocator);
    point.AddMember("source_session", selected ? aim.sourceIdentity.session : std::uint64_t(0), allocator);
    point.AddMember("source_generation", selected ? aim.sourceIdentity.generation : std::uint64_t(0), allocator);
    point.AddMember("consumption_generation", selected ? aim.sourceConsumptionGeneration : std::uint64_t(0), allocator);
    point.AddMember("publication_generation", aim.generation, allocator);
    point.AddMember("max_report_age_ms", selected ? aim.sourceMaximumAge.count() : std::int64_t(0), allocator);
    // Epoch ticks remain exact signed integers; consumers must not reinterpret
    // them as wall-clock time or use lossy floating-point subtraction.
    point.AddMember("receipt_steady_ticks", aim.sourceIdentity.receivedAt.time_since_epoch().count(), allocator);
    point.AddMember("publication_steady_ticks", aim.sampledAt.time_since_epoch().count(), allocator);
    point.AddMember("query_steady_ticks", now.time_since_epoch().count(), allocator);
    point.AddMember("steady_period_num", GunSourceClock::period::num, allocator);
    point.AddMember("steady_period_den", GunSourceClock::period::den, allocator);
    const bool receiptEligible = selected && aim.valid && aim.sourceIdentity.session && aim.sourceIdentity.generation &&
        aim.sourceIdentity.receivedAt <= aim.sampledAt && aim.sampledAt <= now &&
        sampleAgeWithin(aim.sourceIdentity.receivedAt, now, aim.sourceMaximumAge);
    point.AddMember("receipt_eligible_at_query", receiptEligible, allocator);
    point.AddMember("scope_right_at_publication", selected && aim.scopeRightHeld, allocator);
    point.AddMember("scope_left_at_publication", selected && aim.scopeLeftHeld, allocator);
    point.AddMember("trigger_at_publication", selected && aim.triggerHeld, allocator);
    result.AddMember("last_publication", point, allocator);
    return result;
}

} // namespace bone_eater::input
