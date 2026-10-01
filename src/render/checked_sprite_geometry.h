#pragma once

#include "render/checked_data_write.h"
#include "render/rear_illumination_math.h"
#include <limits>

namespace bone_eater::render {

struct SpriteGeometryPermission {
    std::uintptr_t sprite = 0;
    bool valid = false;
};

struct SpriteGeometryAccess {
    std::array<std::uintptr_t, 2> sprites {};
    ULONGLONG observed = 0;
    bool valid = false;
};

inline bool spriteGeometryAddressValid(std::uintptr_t sprite) noexcept {
    return sprite && sprite <= std::numeric_limits<std::uintptr_t>::max() - 0xA8 - 8;
}

// Exact two eight-byte spans per validated sprite, never the intervening data.
// Call only after rebuilding the complete owning-thread native identity chain.
// A page snapshot is not lifetime synchronization; short lease reuse retains
// its original timestamp and is allowed only for the identical owner/objects.
inline SpriteGeometryAccess observeSpriteGeometry(const std::array<std::uintptr_t, 2>& sprites,
        ULONGLONG now, const SpriteGeometryAccess* previous = nullptr) noexcept {
    if (!spriteGeometryAddressValid(sprites[0]) || !spriteGeometryAddressValid(sprites[1]) ||
            sprites[0] == sprites[1] || GetTickCount64() - now > 100) return {};
    if (previous && previous->valid && previous->sprites == sprites && now >= previous->observed &&
            now - previous->observed <= 50) return *previous;
    checked_data_detail::Region region;
    for (const auto sprite : sprites) {
        for (const std::uintptr_t offset : {std::uintptr_t(0x70), std::uintptr_t(0xA8)}) {
            const auto address = sprite + offset;
            if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
            if (8 > region.size - (address - region.start)) return {};
        }
    }
    if (GetTickCount64() - now > 100) return {};
    return {sprites, now, true};
}

inline SpriteGeometryPermission acquireSpriteGeometry(const SpriteGeometryAccess& access,
        std::uintptr_t sprite) noexcept {
    if (!access.valid || !spriteGeometryAddressValid(sprite) ||
            GetTickCount64() - access.observed > 100 ||
            (sprite != access.sprites[0] && sprite != access.sprites[1])) return {};
    return {sprite, true};
}

// Single owned leaf variant: query only its XY and scale spans.
inline SpriteGeometryAccess observeSingleSpriteGeometry(std::uintptr_t sprite,
        ULONGLONG now, const SpriteGeometryAccess* previous = nullptr) noexcept {
    if (!spriteGeometryAddressValid(sprite) || GetTickCount64() - now > 100) return {};
    const std::array<std::uintptr_t,2> identity {{sprite,0}};
    if (previous && previous->valid && previous->sprites == identity && now >= previous->observed &&
            now - previous->observed <= 50) return *previous;
    checked_data_detail::Region region;
    for (const auto offset : {std::uintptr_t(0x70),std::uintptr_t(0xA8)}) {
        const auto address = sprite + offset;
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address,region)) return {};
        if (8 > region.size - (address-region.start)) return {};
    }
    if (GetTickCount64() - now > 100) return {};
    return {identity,now,true};
}

// Saved permission is valid only for this synchronous identity-checked commit.
// No age gate is applied during restoration. A partial write is possible, so
// callers must attempt BOTH saved spans even after a failed application.
inline bool writeSpriteGeometrySpan(const SpriteGeometryPermission& permission, std::uintptr_t offset,
        const std::array<float, 2>& values) noexcept {
    static_assert(sizeof(values) == 8);
    if (!permission.valid || !spriteGeometryAddressValid(permission.sprite) ||
            (offset != 0x70 && offset != 0xA8)) return false;
    __try {
        auto* destination = reinterpret_cast<volatile unsigned char*>(permission.sprite + offset);
        const auto* source = reinterpret_cast<const unsigned char*>(values.data());
        for (unsigned i = 0; i < 8; ++i) destination[i] = source[i];
        for (unsigned i = 0; i < 8; ++i) if (destination[i] != source[i]) return false;
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

inline bool writeSpriteGeometry(const SpriteGeometryPermission& permission,
        const RearIlluminationPose& pose) noexcept {
    // Deliberately no short-circuit: restoring the second span is still required
    // if access to the first span was lost after permission observation.
    const bool position = writeSpriteGeometrySpan(permission, 0x70, pose.position);
    const bool scale = writeSpriteGeometrySpan(permission, 0xA8, pose.scale);
    return position && scale;
}

} // namespace bone_eater::render
