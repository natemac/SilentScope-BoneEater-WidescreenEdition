#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstdint>
#include <limits>

namespace bone_eater::camera {

using NativeTargetVector = std::array<float, 4>;
static_assert(sizeof(NativeTargetVector) == 16);

struct NativeTargetAccess {
    std::uintptr_t wrapper = 0, address = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};

struct NativeTargetPermission {
    std::uintptr_t wrapper = 0, address = 0;
    bool valid = false;
};

inline bool nativeTargetAddress(std::uintptr_t wrapper, std::uintptr_t& address) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (!wrapper || wrapper > maximum - 0x20 - sizeof(NativeTargetVector)) return false;
    address = wrapper + 0x20;
    return true;
}

// The caller must first revalidate its exact native AnimationCamera and target
// identities on their owning commit thread. This lease certifies only the
// wrapper+0x20 sixteen-byte span, never another field in the containing region.
// Reuse cannot renew the original observation time. A prior page check is not
// lifetime synchronization or proof against later protection changes.
inline NativeTargetAccess observeNativeTargetAccess(std::uintptr_t wrapper, ULONGLONG now,
        const NativeTargetAccess* previous = nullptr, bool* queried = nullptr) noexcept {
    if (queried) *queried = false;
    std::uintptr_t address = 0;
    if (!nativeTargetAddress(wrapper, address)) return {};
    if (previous && previous->valid && previous->wrapper == wrapper && previous->address == address &&
            now >= previous->observed && now - previous->observed <= 50) return *previous;
    MEMORY_BASIC_INFORMATION memory {};
    if (queried) *queried = true;
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT || memory.Type != MEM_PRIVATE || memory.Protect != PAGE_READWRITE) return {};
    const auto start = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (address < start || address - start >= memory.RegionSize ||
            sizeof(NativeTargetVector) > memory.RegionSize - (address - start)) return {};
    return {wrapper, address, now, true};
}

inline NativeTargetPermission acquireNativeTargetPermission(const NativeTargetAccess& access,
                                                            std::uintptr_t wrapper) noexcept {
    std::uintptr_t address = 0;
    const auto now = GetTickCount64();
    if (!access.valid || !nativeTargetAddress(wrapper, address) || access.wrapper != wrapper ||
            access.address != address || now < access.observed || now - access.observed > 100) return {};
    return {wrapper, address, true};
}

// A permission belongs to one identity-checked synchronous native commit. The
// saved token intentionally has no age cutoff when restoring after that call.
// Byte-wise stores avoid alignment/aliasing assumptions; partial failure is
// possible, so callers must always attempt their saved baseline on failure.
inline bool writeNativeTarget(const NativeTargetPermission& permission,
                              const NativeTargetVector& value) noexcept {
    std::uintptr_t address = 0;
    if (!permission.valid || !nativeTargetAddress(permission.wrapper, address) || address != permission.address) return false;
    __try {
        auto* destination = reinterpret_cast<volatile unsigned char*>(address);
        const auto* source = reinterpret_cast<const unsigned char*>(value.data());
        for (std::size_t index = 0; index < sizeof(value); ++index) destination[index] = source[index];
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            if (destination[index] != source[index]) return false;
        }
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

} // namespace bone_eater::camera
