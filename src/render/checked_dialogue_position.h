#pragma once

#include "render/checked_data_write.h"
#include <limits>

namespace bone_eater::render {

struct DialoguePositionPermission {
    std::uintptr_t sprite = 0;
    bool valid = false;
};

struct DialoguePositionAccess {
    std::uintptr_t sprite = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};

inline bool dialoguePositionAddressValid(std::uintptr_t sprite) noexcept {
    return sprite && sprite <= std::numeric_limits<std::uintptr_t>::max() - 0x70 - 8;
}

// Exactly cached XY (+70..+77). Caller has already revalidated the complete
// same-thread dialogue owner/root/leaf chain; reuse never refreshes lease age.
inline DialoguePositionAccess observeDialoguePosition(std::uintptr_t sprite, ULONGLONG now,
        const DialoguePositionAccess* previous = nullptr) noexcept {
    if (!dialoguePositionAddressValid(sprite) || GetTickCount64() - now > 100) return {};
    if (previous && previous->valid && previous->sprite == sprite && now >= previous->observed &&
            now - previous->observed <= 50) return *previous;
    const auto address = sprite + 0x70;
    checked_data_detail::Region region;
    if (!checked_data_detail::queryPrivateWritable(address, region) ||
            !region.contains(address) || 8 > region.size - (address - region.start) ||
            GetTickCount64() - now > 100) return {};
    return {sprite, now, true};
}

inline DialoguePositionPermission acquireDialoguePosition(const DialoguePositionAccess& access,
        std::uintptr_t sprite) noexcept {
    if (!access.valid || access.sprite != sprite || !dialoguePositionAddressValid(sprite) ||
            GetTickCount64() - access.observed > 100) return {};
    return {sprite, true};
}

// Saved permission is for one synchronous commit only. Restoration deliberately
// has no expiry: the native original may block longer than the acquisition age.
// SEH covers protection changes after observation; a partial write still needs
// the caller's unconditional saved-position restoration attempt.
inline bool writeDialoguePosition(const DialoguePositionPermission& permission,
        const std::array<float, 2>& value) noexcept {
    static_assert(sizeof(value) == 8);
    if (!permission.valid || !dialoguePositionAddressValid(permission.sprite)) return false;
    __try {
        auto* destination = reinterpret_cast<volatile unsigned char*>(permission.sprite + 0x70);
        const auto* source = reinterpret_cast<const unsigned char*>(value.data());
        for (unsigned i = 0; i < 8; ++i) destination[i] = source[i];
        for (unsigned i = 0; i < 8; ++i) if (destination[i] != source[i]) return false;
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

} // namespace bone_eater::render
