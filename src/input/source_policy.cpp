#include "input/source_policy.h"

namespace bone_eater::input {
namespace {
bool validUsage(const std::optional<unsigned>& usage) noexcept {
    return !usage || (*usage >= 1 && *usage <= 16);
}
bool sameMapping(const SelectedGunButtonMapping& a, const SelectedGunButtonMapping& b) noexcept {
    return a.trigger == b.trigger && a.scopeRight == b.scopeRight && a.scopeLeft == b.scopeLeft;
}
GunSourceState unavailableState(GunSampleStatus status) noexcept {
    switch (status) {
        case GunSampleStatus::Unavailable: return GunSourceState::Unavailable;
        case GunSampleStatus::Stale: return GunSourceState::Stale;
        case GunSampleStatus::Disconnected: return GunSourceState::Disconnected;
        default: return GunSourceState::InvalidSample;
    }
}
} // namespace

bool validSelectedGunButtonMapping(const SelectedGunButtonMapping& mapping) noexcept {
    return mapping.trigger && mapping.scopeRight && validUsage(mapping.trigger) &&
        validUsage(mapping.scopeRight) && validUsage(mapping.scopeLeft) &&
        mapping.trigger != mapping.scopeRight &&
        (!mapping.scopeLeft || (mapping.scopeLeft != mapping.trigger && mapping.scopeLeft != mapping.scopeRight));
}

std::optional<GunButtons> mapSelectedGunButtons(std::uint32_t rawButtons,
        const SelectedGunButtonMapping& mapping) noexcept {
    if (rawButtons > 65535 || !validSelectedGunButtonMapping(mapping)) return std::nullopt;
    const auto held = [rawButtons](const std::optional<unsigned>& usage) noexcept {
        return usage && (rawButtons & (1u << (*usage - 1))) != 0;
    };
    return GunButtons {held(mapping.trigger), held(mapping.scopeRight), held(mapping.scopeLeft)};
}

GunConfigureResult GunSourcePolicy::configure(GunSourceMode mode,
        const SelectedGunButtonMapping& mapping) noexcept {
    if (mode != GunSourceMode::Legacy && mode != GunSourceMode::SelectedHid)
        return GunConfigureResult::InvalidMode;
    if (mode == GunSourceMode::SelectedHid && !validSelectedGunButtonMapping(mapping))
        return GunConfigureResult::InvalidMapping;
    const auto selectedMapping = mode == GunSourceMode::SelectedHid ? mapping : SelectedGunButtonMapping {};
    if (mode == output_.mode && sameMapping(mapping_, selectedMapping)) return GunConfigureResult::Unchanged;
    output_.mode = mode;
    mapping_ = selectedMapping;
    ++output_.selectionGeneration;
    haveSeen_ = false;
    needNewReport_ = false;
    transition_ = true;
    release(GunSourceState::Transition);
    return GunConfigureResult::Changed;
}

GunSourcePolicy::Candidate GunSourcePolicy::candidate(const GunSourceInput& input) const noexcept {
    Candidate result;
    std::uint32_t x = 0, y = 0;
    GunSampleStatus status;
    if (output_.mode == GunSourceMode::SelectedHid) {
        status = input.selected.status;
        result.identity = input.selected.identity;
        x = input.selected.x; y = input.selected.y;
        result.rawButtons = input.selected.rawButtons;
        const auto buttons = mapSelectedGunButtons(result.rawButtons, mapping_);
        if (!buttons) { result.unavailable = GunSourceState::InvalidSample; return result; }
        result.buttons = *buttons;
    } else {
        status = input.legacy.status;
        result.identity = input.legacy.identity;
        x = input.legacy.x; y = input.legacy.y;
        result.buttons = input.legacy.buttons;
        result.rawButtons = (result.buttons.trigger ? 1u : 0u) |
            (result.buttons.scopeRight ? 2u : 0u) | (result.buttons.scopeLeft ? 4u : 0u);
    }
    if (status != GunSampleStatus::Valid) {
        result.unavailable = unavailableState(status);
        return result;
    }
    if (x > 65535 || y > 65535 || !result.identity.session || !result.identity.generation) {
        result.unavailable = GunSourceState::InvalidSample;
        return result;
    }
    result.x = static_cast<std::uint16_t>(x);
    result.y = static_cast<std::uint16_t>(y);
    result.usable = true;
    return result;
}

void GunSourcePolicy::release(GunSourceState state) noexcept {
    output_.state = state;
    output_.buttons = {};
    output_.sourceUsable = false;
    output_.armed = false;
    // Keep the last accepted coordinate pair and its original receipt identity.
}

void GunSourcePolicy::remember(const Candidate& sample) noexcept {
    seen_ = sample;
    haveSeen_ = true;
}

bool GunSourcePolicy::identical(const Candidate& a, const Candidate& b) noexcept {
    return a.x == b.x && a.y == b.y && a.rawButtons == b.rawButtons &&
        a.identity.session == b.identity.session && a.identity.generation == b.identity.generation &&
        a.identity.receivedAt == b.identity.receivedAt;
}

void GunSourcePolicy::accept(const Candidate& sample) noexcept {
    output_.aim = {true, sample.x, sample.y, output_.mode, sample.identity};
    output_.buttons = sample.buttons;
    output_.sourceUsable = true;
    output_.armed = true;
    output_.state = GunSourceState::Active;
}

GunSourceOutput GunSourcePolicy::update(GunSourceInput input) noexcept {
    ++output_.consumptionGeneration;
    const auto sample = candidate(input);
    if (!input.focused) {
        // A cached pre-focus report must not rearm on returning focus. Reports
        // observed while unfocused also remain behind the neutral rearm barrier.
        if (sample.usable && (!haveSeen_ || sample.identity.session != seen_.identity.session ||
                sample.identity.generation > seen_.identity.generation)) remember(sample);
        needNewReport_ = haveSeen_;
        release(GunSourceState::Unfocused);
        return output_;
    }
    if (transition_) {
        transition_ = false;
        release(GunSourceState::Transition);
        return output_; // At least one whole neutral cabinet boundary per switch.
    }
    if (!sample.usable) {
        needNewReport_ = haveSeen_;
        release(sample.unavailable);
        return output_;
    }
    if (haveSeen_ && sample.identity.session != seen_.identity.session) {
        remember(sample);
        needNewReport_ = false;
        release(GunSourceState::Transition);
        return output_; // New reader/session cannot inherit an active hold.
    }
    const bool fresh = !haveSeen_ || sample.identity.generation > seen_.identity.generation;
    if (haveSeen_ && (sample.identity.generation < seen_.identity.generation ||
            (!fresh && !identical(sample, seen_)))) {
        needNewReport_ = true;
        release(GunSourceState::IncoherentSample);
        return output_;
    }
    if (fresh) remember(sample);
    if (needNewReport_ && !fresh) {
        release(GunSourceState::AwaitFreshReport);
        return output_;
    }
    needNewReport_ = false;
    if (!output_.armed && !sample.buttons.neutral()) {
        release(GunSourceState::AwaitNeutral);
        return output_;
    }
    accept(sample);
    return output_;
}

} // namespace bone_eater::input
