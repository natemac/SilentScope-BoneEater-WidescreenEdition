#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace bone_eater::render {

struct PrivateDataBytePermission {
    std::uintptr_t address = 0;
    bool valid = false;
};

struct PrivateDataByteAccess {
    std::array<std::uintptr_t, 12> addresses {};
    std::size_t count = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};

// Optional instrumentation scoped by an owning-thread observer. No scope means
// no QPC calls or counters. Counts describe actual OS calls; timed counts can
// differ if QPC fails. These values never influence permissions or leases.
struct PrivateDataOperationTiming {
    std::uint64_t calls = 0, timedCalls = 0, ticks = 0, maximum = 0;
};
struct PrivateDataObservationTiming {
    PrivateDataOperationTiming reads, queries;
};
inline thread_local PrivateDataObservationTiming* privateDataObservationTiming = nullptr;

inline std::uint64_t privateDataTimingStart() noexcept {
    LARGE_INTEGER value {};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0 ?
        static_cast<std::uint64_t>(value.QuadPart) : 0;
}
inline void privateDataTimingRecord(PrivateDataOperationTiming& timing, std::uint64_t start) noexcept {
    ++timing.calls;
    if (!start) return;
    const auto end = privateDataTimingStart();
    if (end < start) return;
    const auto elapsed = end - start;
    ++timing.timedCalls;
    timing.ticks += elapsed;
    if (elapsed > timing.maximum) timing.maximum = elapsed;
}
inline void mergePrivateDataTiming(PrivateDataOperationTiming& into,
        const PrivateDataOperationTiming& from) noexcept {
    into.calls += from.calls;
    into.timedCalls += from.timedCalls;
    into.ticks += from.ticks;
    if (from.maximum > into.maximum) into.maximum = from.maximum;
}

namespace checked_data_detail {
struct Region {
    std::uintptr_t start = 0;
    SIZE_T size = 0;
    bool contains(std::uintptr_t address) const noexcept {
        return address >= start && address - start < size;
    }
};

inline bool queryPrivateWritable(std::uintptr_t address, Region& region) noexcept {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION information {};
    auto* timing = privateDataObservationTiming;
    const auto begin = timing ? privateDataTimingStart() : 0;
    const auto queried = VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information));
    if (timing) privateDataTimingRecord(timing->queries, begin);
    if (queried != sizeof(information) ||
            information.State != MEM_COMMIT || information.Type != MEM_PRIVATE ||
            information.Protect != PAGE_READWRITE) return false;
    region = {reinterpret_cast<std::uintptr_t>(information.BaseAddress), information.RegionSize};
    return region.contains(address);
}
} // namespace checked_data_detail

// Rebuild after each native owner update, only from the exact object bytes
// validated in that observation. A previous access may be supplied only after
// the caller independently revalidates an unchanged complete owner/identity.
// Reuse preserves the original page-observation tick, with proactive refresh
// after 50ms and a separate hard 100ms acquisition bound. Sorted addresses let
// one queried region cover several bytes, but authorize only enumerated bytes.
inline PrivateDataByteAccess observePrivateDataBytes(const std::uintptr_t* addresses,
        std::size_t count, ULONGLONG observed, const PrivateDataByteAccess* previous = nullptr) noexcept {
    PrivateDataByteAccess result;
    if (!addresses || !count || count > result.addresses.size() ||
            GetTickCount64() - observed > 100) return result;
    for (std::size_t index = 0; index < count; ++index) {
        if (!addresses[index]) return {};
        result.addresses[index] = addresses[index];
    }
    std::sort(result.addresses.begin(), result.addresses.begin() + count);
    for (std::size_t index = 0; index < count; ++index) {
        if (index && result.addresses[index] == result.addresses[index - 1]) return {};
    }
    if (previous && previous->valid && previous->count == count &&
            GetTickCount64() - previous->observed <= 50 &&
            std::equal(result.addresses.begin(), result.addresses.begin() + count, previous->addresses.begin())) {
        return *previous; // Never renew a lease without querying the page again.
    }
    checked_data_detail::Region region;
    for (std::size_t index = 0; index < count; ++index) {
        const auto address = result.addresses[index];
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
    }
    if (GetTickCount64() - observed > 100) return {};
    result.count = count;
    result.observed = observed;
    result.valid = true;
    return result;
}

// Freshness gates beginning a suppression, never restoration of a byte already
// changed. Copy the returned permission into the synchronous per-commit token.
inline PrivateDataBytePermission acquirePrivateDataBytePermission(const PrivateDataByteAccess& access,
        std::uintptr_t address) noexcept {
    if (!access.valid || access.count > access.addresses.size() ||
            GetTickCount64() - access.observed > 100) return {};
    for (std::size_t index = 0; index < access.count; ++index) {
        if (access.addresses[index] == address) return {address, true};
    }
    return {};
}

// For a caller-validated, game-owned data byte on its native update thread.
// The observation is a page/access guard, not an object-identity or lifetime guarantee:
// callers must still validate their exact owner, object, field and old value.
// Observation excludes executable, mapped and guard pages. The store never
// changes protection; its copied permission is limited to one synchronous
// commit and must not be retained across native object lifetimes.
inline bool writePrivateDataByte(const PrivateDataBytePermission& permission, unsigned char value) noexcept {
    if (!permission.valid || !permission.address) return false;
    __try {
        auto* byte = reinterpret_cast<volatile unsigned char*>(permission.address);
        *byte = value;
        return *byte == value;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                 GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        // A page could become unavailable after VirtualQuery. The caller keeps
        // its normal restoration/failure path; unrelated exceptions propagate.
        return false;
    }
}

// One-shot access for non-hot-path callers. Native UI commit paths use the
// observation-bound overload above rather than querying a large region twice
// for every sprite's synchronous hide and restore.
inline bool writePrivateDataByte(std::uintptr_t address, unsigned char value) noexcept {
    checked_data_detail::Region region;
    if (!checked_data_detail::queryPrivateWritable(address, region)) return false;
    return writePrivateDataByte(PrivateDataBytePermission {address, true}, value);
}

} // namespace bone_eater::render
