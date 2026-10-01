#pragma once

#include "input/source_policy.h"

#include <cstddef>
#include <cstdint>

namespace bone_eater::input {

constexpr std::size_t selectedNddPacketBytes = 56;

// Fill only the three gun bits and four big-endian axis bytes of an existing
// NDD status packet. Service/menu/counter fields remain exactly as supplied.
// The source axes are the calibrated unit domain BEFORE the cabinet protocol's
// one X inversion. Nonusable input holds the last accepted aim and releases
// every gun button; an unknown aim has an explicitly unproven center placeholder.
inline bool packSelectedNddGun(std::uint8_t* packet, std::size_t length,
        const GunSourceOutput& input) noexcept {
    if (!packet || length < selectedNddPacketBytes || input.mode != GunSourceMode::SelectedHid) return false;
    const auto x = input.aim.known ? input.aim.x : std::uint16_t(0x7FFF);
    const auto y = input.aim.known ? input.aim.y : std::uint16_t(0x7FFF);
    const auto invertedX = static_cast<std::uint16_t>(65535u - x);
    std::uint8_t buttons = 0;
    if (input.aim.known && input.aim.source == GunSourceMode::SelectedHid &&
            input.aim.identity.session && input.aim.identity.generation &&
            input.state == GunSourceState::Active && input.armed && input.sourceUsable) {
        if (input.buttons.scopeRight) buttons |= 0x80;
        if (input.buttons.scopeLeft) buttons |= 0x40;
        if (input.buttons.trigger) buttons |= 0x20;
    }
    packet[4] = static_cast<std::uint8_t>((packet[4] & 0x1Fu) | buttons);
    packet[8] = static_cast<std::uint8_t>(invertedX >> 8);
    packet[9] = static_cast<std::uint8_t>(invertedX & 0xFF);
    packet[10] = static_cast<std::uint8_t>(y >> 8);
    packet[11] = static_cast<std::uint8_t>(y & 0xFF);
    return true;
}

} // namespace bone_eater::input
