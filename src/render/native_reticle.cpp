#include "render/native_reticle.h"
#include "render/scope_compositor.h"
#include "diagnostics/output_policy.h"
#include "render/native_battle_background.h"
#include "render/native_replay_alignment.h"
#include "render/native_achievement_alignment.h"
#include "render/native_menu_backing.h"
#include "render/native_menu_margin.h"
#include "render/native_normal_start_backing.h"
#include "render/native_options_backing.h"
#include "render/native_viewport.h"
#include "render/checked_data_write.h"
#include "render/ranking_notice_identity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <limits>

namespace bone_eater::render {
namespace {

using UpdateGun = void(__fastcall*)(void*, float);
using CommitSprite = void(__fastcall*)(void*);
UpdateGun originalUpdate = nullptr;
UpdateGun originalBattleUpdate = nullptr;
CommitSprite originalCommit = nullptr;
DesktopReticleReady replacementReady = nullptr;
std::uintptr_t moduleBase = 0;
std::atomic<bool> failed {false};
std::atomic<bool> suppressionReported {false};
std::array<std::atomic<ULONGLONG>, 2> meterVisibleAt {};
constexpr std::uintptr_t kGunVtable = 0x10CA460;
constexpr std::uintptr_t kBattleVtable = 0x10CA1D8;
constexpr std::uintptr_t kSpriteVtable = 0x10CEBA8;
constexpr std::uintptr_t kRectVtable = 0x10CEB48;
constexpr std::uintptr_t kObjectVtable = 0x10C9EC8;
// The four menu guides are siblings beneath "root", not descendants of
// root_AimLine. The pinned loader's table12C0E90 assigns them to130..148.
constexpr std::array<const char*, 6> kRootNames {{"root_AimCrossChair", "root_AimLine",
    "lcd_m_line_v_0", "lcd_m_line_h_1", "lcd_m_line_v_1", "lcd_m_line_h_0"}};
constexpr std::array<std::size_t, 6> kGunOffsets {{0x100, 0x108, 0x130, 0x138, 0x140, 0x148}};
constexpr std::array<const char*, 10> kBattleRootNames {{
    "lcd_bt_aim_root", "lcd_bt_line_root",
    "lcd_bt_line_v_0", "lcd_bt_line_h_1", "lcd_bt_line_v_1", "lcd_bt_line_h_0",
    "lcd_bt_Aimedge_0", "lcd_bt_Aimedge_1", "lcd_bt_Aimedge_2", "lcd_bt_Aimedge_3"}};
constexpr std::array<std::size_t, 10> kBattleOffsets {{
    0x1340, 0x1348, 0x1390, 0x1398, 0x13A0, 0x13A8, 0x1370, 0x1378, 0x1380, 0x1388}};
enum class OwnerKind { Gun, Battle };

// Bounded read-only guard diagnostics for the opt-in suppression experiment.
// These do not relax an identity/visibility check or change native call order.
enum class Count : std::size_t {
    Update, OwnerRead, OwnerType, OwnerState, Group, Root0, Root1, RootsReady,
    Commit, Caller, NoRoots, Stale, NotReady, SpriteReject, AncestryReject,
    FinalReject, Suppressed, Total
};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Count::Total)> counts {};
std::atomic<std::uintptr_t> lastOwner {0}, lastCaller {0};
std::atomic<DWORD> lastUpdateThread {0}, lastCommitThread {0};
std::atomic<OwnerKind> lastOwnerKind {OwnerKind::Gun};
std::atomic<ULONGLONG> nextReport {0};
void count(Count value) noexcept { counts[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed); }

struct Roots {
    OwnerKind kind = OwnerKind::Gun;
    std::uintptr_t owner = 0;
    std::array<std::uintptr_t, 10> objects {};
    std::uint32_t group = 0;
    ULONGLONG observed = 0;
    PrivateDataByteAccess writable;
    std::uintptr_t meterRoot = 0;
    PrivateDataByteAccess meterWritable;
    bool valid = false;
};

// UI preparation must run on the same thread as the observed game update.
// Other threads have no registered roots and always pass through unchanged.
thread_local Roots currentRoots;
thread_local Roots currentBattleRoots;
thread_local bool updating = false;
thread_local bool updatingBattle = false;
thread_local bool committing = false;

// Optional, per-thread windowed timing. The normal path never calls QPC.
// Native sprite work is measured separately so it cannot be mistaken for the
// cost of the identity/ancestry guards. No timing result changes suppression.
enum class TimingSection : std::size_t { Guard, Restore, Observe, NativePlain, NativeHidden, Total };
struct TimingSample {
    std::uint64_t count = 0;
    std::uint64_t ticks = 0;
    std::uint64_t maximum = 0;
};
struct TimingWindow {
    std::uint64_t start = 0;
    std::array<TimingSample, static_cast<std::size_t>(TimingSection::Total)> samples {};
    struct Observation {
        TimingSample total;
        PrivateDataObservationTiming operations;
    };
    std::array<Observation, 2> observations {};
    // These envelopes are children of Guard, not additional hook time.
    std::array<Observation, 2> permissions {};
    std::array<std::uint64_t, 2> successfulHides {};
};
bool timingEnabled = false; // Set once, before installing any hook.
std::uint64_t timingFrequency = 0;
thread_local TimingWindow timingWindow;
thread_local unsigned timingDepth = 0;

std::uint64_t timingStart(bool outermost = true) noexcept {
    if (!timingEnabled || !outermost || timingDepth > 1) return 0;
    LARGE_INTEGER value {};
    if (!QueryPerformanceCounter(&value) || value.QuadPart <= 0) return 0;
    const auto tick = static_cast<std::uint64_t>(value.QuadPart);
    if (!timingWindow.start) timingWindow.start = tick;
    return tick;
}

void timingRecord(TimingSection section, std::uint64_t start, TimingSample* detail = nullptr) noexcept {
    if (!start) return;
    LARGE_INTEGER value {};
    if (!QueryPerformanceCounter(&value) || value.QuadPart <= 0) return;
    const auto end = static_cast<std::uint64_t>(value.QuadPart);
    if (end < start) return;
    auto& sample = timingWindow.samples[static_cast<std::size_t>(section)];
    const auto elapsed = end - start;
    ++sample.count;
    sample.ticks += elapsed;
    if (elapsed > sample.maximum) sample.maximum = elapsed;
    if (detail) {
        ++detail->count;
        detail->ticks += elapsed;
        if (elapsed > detail->maximum) detail->maximum = elapsed;
    }
}

void recordObservation(OwnerKind kind, std::uint64_t start,
        const PrivateDataObservationTiming& operations) noexcept {
    if (!start) return;
    auto& observation = timingWindow.observations[kind == OwnerKind::Gun ? 0 : 1];
    timingRecord(TimingSection::Observe, start, &observation.total);
    mergePrivateDataTiming(observation.operations.reads, operations.reads);
    mergePrivateDataTiming(observation.operations.queries, operations.queries);
}

void recordPermission(OwnerKind kind, std::uint64_t start,
        const PrivateDataObservationTiming& operations) noexcept {
    if (!start) return;
    auto& permission = timingWindow.permissions[kind == OwnerKind::Gun ? 0 : 1];
    LARGE_INTEGER value {};
    if (QueryPerformanceCounter(&value) && value.QuadPart > 0 &&
            static_cast<std::uint64_t>(value.QuadPart) >= start) {
        const auto elapsed = static_cast<std::uint64_t>(value.QuadPart) - start;
        ++permission.total.count;
        permission.total.ticks += elapsed;
        if (elapsed > permission.total.maximum) permission.total.maximum = elapsed;
    }
    mergePrivateDataTiming(permission.operations.queries, operations.queries);
}

void reportTiming() {
    if (!timingEnabled || timingDepth > 1 || !timingFrequency || !timingWindow.start) return;
    const auto now = timingStart();
    if (now <= timingWindow.start) return;
    const TimingWindow completed = timingWindow;
    // Reset before reporting. Logging itself is excluded and does not cause
    // an ever-growing total or get counted again in a subsequent window.
    timingWindow = {};
    timingWindow.start = now;
    const double windowMs = 1000.0 * static_cast<double>(now - completed.start) /
        static_cast<double>(timingFrequency);
    constexpr std::array<const char*, 5> names {{"guard", "restore", "observe", "native_plain", "native_hidden"}};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& sample = completed.samples[index];
        const double totalMs = 1000.0 * static_cast<double>(sample.ticks) / static_cast<double>(timingFrequency);
        const double maximumUs = 1000000.0 * static_cast<double>(sample.maximum) / static_cast<double>(timingFrequency);
        log_info("bone-eater", "Reticle timing thread={} window_ms={:.3f} section={} calls={} total_ms={:.3f} mean_us={:.3f} max_us={:.3f}",
            GetCurrentThreadId(), windowMs, names[index], sample.count, totalMs,
            sample.count ? totalMs * 1000.0 / static_cast<double>(sample.count) : 0.0, maximumUs);
    }
    for (std::size_t index = 0; index < completed.observations.size(); ++index) {
        const auto& observation = completed.observations[index];
        if (!observation.total.count) continue;
        const auto& reads = observation.operations.reads;
        const auto& queries = observation.operations.queries;
        const double ms = 1000.0 / static_cast<double>(timingFrequency);
        const double totalMs = observation.total.ticks * ms;
        const double readMs = reads.ticks * ms, queryMs = queries.ticks * ms;
        log_info("bone-eater", "Reticle observe split pid={} thread={} owner={} window_ms={:.3f} observe_calls={} total_ms={:.3f} read_calls={} read_timed={} read_ms={:.3f} read_max_us={:.3f} query_calls={} query_timed={} query_ms={:.3f} query_max_us={:.3f} other_ms={:.3f}",
            GetCurrentProcessId(), GetCurrentThreadId(), index ? "battle_lcd" : "gun", windowMs,
            observation.total.count, totalMs, reads.calls, reads.timedCalls, readMs, reads.maximum * ms * 1000.0,
            queries.calls, queries.timedCalls, queryMs, queries.maximum * ms * 1000.0,
            totalMs > readMs + queryMs ? totalMs - readMs - queryMs : 0.0);
    }
    for (std::size_t index = 0; index < completed.permissions.size(); ++index) {
        const auto& permission = completed.permissions[index];
        const auto& queries = permission.operations.queries;
        if (!permission.total.count && !queries.calls && !completed.successfulHides[index]) continue;
        const double ms = 1000.0 / static_cast<double>(timingFrequency);
        const double totalMs = permission.total.ticks * ms;
        const double queryMs = queries.ticks * ms;
        log_info("bone-eater", "Reticle prepare permission pid={} thread={} owner={} window_ms={:.3f} permission_calls={} total_ms={:.3f} query_calls={} query_timed={} query_ms={:.3f} query_max_us={:.3f} successful_hides={} other_ms={:.3f} included_in=guard",
            GetCurrentProcessId(), GetCurrentThreadId(), index ? "battle_lcd" : "gun", windowMs,
            permission.total.count, totalMs, queries.calls, queries.timedCalls, queryMs,
            queries.maximum * ms * 1000.0, completed.successfulHides[index],
            totalMs > queryMs ? totalMs - queryMs : 0.0);
    }
}

template<typename T>
bool read(std::uintptr_t owner, std::size_t offset, T& value) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
            owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    auto* timing = privateDataObservationTiming;
    const auto begin = timing ? privateDataTimingStart() : 0;
    const BOOL result = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &value, sizeof(T), &copied);
    if (timing) privateDataTimingRecord(timing->reads, begin);
    return result && copied == sizeof(T);
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T result {};
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

void reportGuards() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    // A nested native commit must not reset a window containing its still-live
    // outer timer. This depth is independent of the suppression recursion guard.
    if (timingEnabled && timingDepth > 1) return;
    const auto now = GetTickCount64();
    auto next = nextReport.load(std::memory_order_relaxed);
    if (now < next || !nextReport.compare_exchange_strong(next, now + 5000, std::memory_order_relaxed)) return;
    try {
        const auto value = [](Count item) { return counts[static_cast<std::size_t>(item)].load(std::memory_order_relaxed); };
        log_info("bone-eater", "Reticle guards update={} owner_read={} owner_type={} owner_state={} group={} root0={} root1={} roots_ready={} commit={} caller={} no_roots={} stale={} not_ready={} sprite_reject={} ancestry_reject={} final_reject={} suppressed={} update_thread={} commit_thread={} caller_rva={:x}",
            value(Count::Update), value(Count::OwnerRead), value(Count::OwnerType), value(Count::OwnerState),
            value(Count::Group), value(Count::Root0), value(Count::Root1), value(Count::RootsReady),
            value(Count::Commit), value(Count::Caller), value(Count::NoRoots), value(Count::Stale),
            value(Count::NotReady), value(Count::SpriteReject), value(Count::AncestryReject),
            value(Count::FinalReject), value(Count::Suppressed), lastUpdateThread.load(),
            lastCommitThread.load(), lastCaller.load() - moduleBase);
        const auto owner = lastOwner.load(std::memory_order_relaxed);
        const auto kind = lastOwnerKind.load(std::memory_order_relaxed);
        std::array<unsigned char, 0x110> bytes {};
        if (read(owner, 0, bytes)) {
            std::array<std::uintptr_t, 2> rootObjects {};
            if (kind == OwnerKind::Battle) read(owner, 0x1340, rootObjects);
            else rootObjects = {{field<std::uintptr_t>(bytes, 0x100), field<std::uintptr_t>(bytes, 0x108)}};
            log_info("bone-eater", "Reticle owner snapshot address={:x} vtable_rva={:x} state={} group={} roots={:x},{:x}",
                owner, field<std::uintptr_t>(bytes, 0) - moduleBase, field<std::uint32_t>(bytes, 0x48),
                field<std::uint32_t>(bytes, 0xE0), rootObjects[0], rootObjects[1]);
            for (std::size_t index = 0; index < 2; ++index) {
                const auto object = rootObjects[index];
                std::array<unsigned char, 0xD0> root {};
                if (!read(object, 0, root)) continue;
                std::array<char, 33> name {};
                for (std::size_t letter = 0; letter < 32; ++letter) {
                    const unsigned char character = root[0x40 + letter];
                    if (!character) break;
                    name[letter] = character >= 32 && character < 127 ? static_cast<char>(character) : '?';
                }
                log_info("bone-eater", "Reticle root {} snapshot vtable_rva={:x} self={:x} group={} visible={} name='{}'",
                    index, field<std::uintptr_t>(root, 0) - moduleBase, field<std::uintptr_t>(root, 8),
                    field<std::uint32_t>(root, 0x3C), static_cast<unsigned>(root[0xC9]), name.data());
            }
        }
        reportTiming();
    } catch (...) {}
}

bool writeVisibility(std::uintptr_t object, unsigned char visibility,
                     const PrivateDataBytePermission& permission) noexcept {
    if (!object || visibility > 1 || object > std::numeric_limits<std::uintptr_t>::max() - 0xC9 ||
            !permission.valid || permission.address != object + 0xC9) return false;
    // Exact identity/old-visibility checks remain at the two call sites. These
    // are native heap bytes, so no cross-process write or protection change is
    // needed for the short, synchronous hide/restore around one sprite commit.
    return writePrivateDataByte(permission, visibility);
}

bool battleObjects(std::uintptr_t owner, std::array<std::uintptr_t, 10>& objects) noexcept {
    std::array<unsigned char, 0x70> bytes {};
    if (!read(owner, 0x1340, bytes)) return false;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        objects[index] = field<std::uintptr_t>(bytes, kBattleOffsets[index] - 0x1340);
    }
    return true;
}

bool gunObjects(std::uintptr_t owner, std::array<std::uintptr_t, 10>& objects) noexcept {
    std::array<unsigned char, 0x50> bytes {};
    if (!read(owner, 0x100, bytes)) return false;
    objects = {};
    for (std::size_t index = 0; index < kGunOffsets.size(); ++index)
        objects[index] = field<std::uintptr_t>(bytes, kGunOffsets[index] - 0x100);
    return true;
}

bool namedRoot(std::uintptr_t object, std::uint32_t group, std::size_t index,
               OwnerKind kind = OwnerKind::Gun) noexcept {
    std::array<unsigned char, 0x60> bytes {};
    if (!read(object, 0, bytes) || field<std::uintptr_t>(bytes, 0) != moduleBase + kSpriteVtable ||
            field<std::uintptr_t>(bytes, 8) != object || field<std::uint32_t>(bytes, 0x3C) != group) return false;
    const char* name = reinterpret_cast<const char*>(bytes.data() + 0x40);
    const char* expected = kind == OwnerKind::Battle ? kBattleRootNames[index] : kRootNames[index];
    return std::memchr(name, 0, 32) && !std::strcmp(name, expected);
}

bool namedMeterRoot(std::uintptr_t object, std::uint32_t group) noexcept {
    std::array<unsigned char, 0x60> bytes {};
    return read(object, 0, bytes) && field<std::uintptr_t>(bytes, 0) == moduleBase + kSpriteVtable &&
        field<std::uintptr_t>(bytes, 8) == object && field<std::uint32_t>(bytes, 0x3C) == group &&
        std::memcmp(bytes.data() + 0x40, "lcd_bt_CCRoot", sizeof("lcd_bt_CCRoot")) == 0;
}

// Loader116544/116604 stores the templates;116AF2/116C6A stores
// clones1..63. The four meter arrays have64 entries apiece, not children
// of their templates. Only the two vertical side strips are selected here.
unsigned ownedSideMeter(const Roots& roots, std::uintptr_t object) noexcept {
    if (roots.kind != OwnerKind::Battle || !roots.meterRoot) return false;
    std::array<unsigned char, 0x60> bytes {};
    if (!read(object, 0, bytes) || field<std::uintptr_t>(bytes, 0) != moduleBase + kSpriteVtable ||
            field<std::uintptr_t>(bytes, 8) != object || field<std::uintptr_t>(bytes, 0x10) != roots.meterRoot ||
            field<std::uint32_t>(bytes, 0x3C) != roots.group) return false;
    const char* name = reinterpret_cast<const char*>(bytes.data() + 0x40);
    if (!std::memchr(name, 0, 32)) return false;
    constexpr std::array<const char*, 2> prefixes {{"lcd_bt_meterLeft", "lcd_bt_meterRight"}};
    for (std::size_t side = 0; side < prefixes.size(); ++side) {
        const auto length = std::strlen(prefixes[side]);
        if (std::strncmp(name, prefixes[side], length)) continue;
        unsigned index = 0;
        if (name[length]) {
            if (name[length] != '_' || name[length + 1] < '1' || name[length + 1] > '9') return false;
            const char* suffix = name + length + 1;
            index = static_cast<unsigned>(*suffix++ - '0');
            if (*suffix >= '0' && *suffix <= '9') index = index * 10 + static_cast<unsigned>(*suffix++ - '0');
            if (*suffix || index > 63) return false;
        }
        std::uintptr_t owned = 0, currentRoot = 0;
        const bool valid = read(roots.owner, (side ? 0x19B0 : 0x17B0) + index * 8, owned) && owned == object &&
            read(roots.owner, 0x1338, currentRoot) && currentRoot == roots.meterRoot &&
            namedMeterRoot(currentRoot, roots.group);
        return valid ? 1U << side : 0;
    }
    return false;
}

bool ownerMatches(const Roots& roots) noexcept {
    std::array<unsigned char, 0x110> bytes {};
    std::array<std::uintptr_t, 10> objects {};
    if (!(roots.kind == OwnerKind::Battle ? battleObjects(roots.owner, objects) :
            gunObjects(roots.owner, objects))) return false;
    return roots.valid && read(roots.owner, 0, bytes) &&
        field<std::uintptr_t>(bytes, 0) == moduleBase +
            (roots.kind == OwnerKind::Battle ? kBattleVtable : kGunVtable) &&
        field<std::uint32_t>(bytes, 0x48) == (roots.kind == OwnerKind::Battle ? 3U : 2U) &&
        field<std::uint32_t>(bytes, 0xE0) == roots.group &&
        objects == roots.objects;
}

bool sameOwner(const Roots& left, const Roots& right) noexcept {
    return left.valid && right.valid && left.kind == right.kind && left.owner == right.owner &&
        left.group == right.group && left.objects == right.objects;
}

bool currentObservation(const Roots& roots) noexcept {
    const auto& current = roots.kind == OwnerKind::Battle ? currentBattleRoots : currentRoots;
    return sameOwner(current, roots) && current.observed == roots.observed &&
        GetTickCount64() - roots.observed <= 100;
}

void observeRoots(void* object, OwnerKind kind = OwnerKind::Gun,
                  const Roots* previous = nullptr) noexcept {
    Roots result;
    result.kind = kind;
    result.owner = reinterpret_cast<std::uintptr_t>(object);
    std::array<unsigned char, 0x110> bytes {};
    if (!read(result.owner, 0, bytes)) count(Count::OwnerRead);
    else if (field<std::uintptr_t>(bytes, 0) != moduleBase +
            (kind == OwnerKind::Battle ? kBattleVtable : kGunVtable)) count(Count::OwnerType);
    else if (field<std::uint32_t>(bytes, 0x48) != (kind == OwnerKind::Battle ? 3U : 2U)) count(Count::OwnerState);
    else {
        result.group = field<std::uint32_t>(bytes, 0xE0);
        if (kind == OwnerKind::Battle) battleObjects(result.owner, result.objects);
        else gunObjects(result.owner, result.objects);
        if (!(result.group > 0 && result.group < 32) || result.objects[0] == result.objects[1]) count(Count::Group);
        else if (!namedRoot(result.objects[0], result.group, 0, kind)) count(Count::Root0);
        else if (!namedRoot(result.objects[1], result.group, 1, kind)) count(Count::Root1);
        else {
            result.valid = true;
            const std::size_t amount = kind == OwnerKind::Battle ? kBattleRootNames.size() : kRootNames.size();
            for (std::size_t index = 0; index < amount; ++index) {
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (result.objects[prior] == result.objects[index]) result.valid = false;
                }
                if (!namedRoot(result.objects[index], result.group, index, kind)) result.valid = false;
            }
            result.observed = GetTickCount64();
            result.valid = ownerMatches(result);
            if (result.valid) {
                for (std::size_t index = 0; index < amount; ++index) {
                    if (result.objects[index] > std::numeric_limits<std::uintptr_t>::max() - 0xC9) {
                        result.valid = false;
                        break;
                    }
                }
                if (result.valid) {
                    // Current names, vtables, state, group and ownership were
                    // all rechecked above. A changed identity never inherits
                    // the earlier owner's page-eligibility observation.
                    // Page checks are deferred until an eligible visible root
                    // actually needs changing. Identity validity does not imply
                    // writable permission, and never renews its old timestamp.
                    if (previous && sameOwner(*previous, result)) result.writable = previous->writable;
                    result.valid = ownerMatches(result);
                }
            }
            if (result.valid) count(Count::RootsReady);
        }
    }
    if (kind == OwnerKind::Battle) {
        for (auto& side : meterVisibleAt) side.store(0);
        unsigned char visible = 0;
        if (result.valid && read(result.owner, 0x1338, result.meterRoot) &&
                namedMeterRoot(result.meterRoot, result.group) && read(result.meterRoot, 0xC9, visible) && visible <= 1) {
            if (previous && sameOwner(*previous, result) && previous->meterRoot == result.meterRoot)
                result.meterWritable = previous->meterWritable;
        } else result.meterRoot = 0;
        currentBattleRoots = result;
    }
    else currentRoots = result;
}

void __fastcall update(void* object, float elapsed) {
    if (updating) { originalUpdate(object, elapsed); return; }
    updating = true;
    count(Count::Update);
    lastOwner.store(reinterpret_cast<std::uintptr_t>(object), std::memory_order_relaxed);
    lastOwnerKind.store(OwnerKind::Gun, std::memory_order_relaxed);
    lastUpdateThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const Roots previous = currentRoots;
    currentRoots.valid = false;
    auto* previousTiming = privateDataObservationTiming;
    __try {
        // The native tail call at 138531 runs layout animation before returning.
        originalUpdate(object, elapsed);
        const auto observeStart = timingStart();
        PrivateDataObservationTiming operations;
        if (observeStart) privateDataObservationTiming = &operations;
        observeRoots(object, OwnerKind::Gun, &previous);
        privateDataObservationTiming = previousTiming;
        recordObservation(OwnerKind::Gun, observeStart, operations);
        reportGuards();
    } __finally {
        privateDataObservationTiming = previousTiming;
        updating = false;
    }
}

void __fastcall updateBattle(void* object, float elapsed) {
    if (updatingBattle) { originalBattleUpdate(object, elapsed); return; }
    updatingBattle = true;
    count(Count::Update);
    lastOwner.store(reinterpret_cast<std::uintptr_t>(object), std::memory_order_relaxed);
    lastOwnerKind.store(OwnerKind::Battle, std::memory_order_relaxed);
    lastUpdateThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const Roots previous = currentBattleRoots;
    currentBattleRoots.valid = false;
    for (auto& side : meterVisibleAt) side.store(0);
    auto* previousTiming = privateDataObservationTiming;
    const bool achievementUpdate = beginNativeAchievementUpdate();
    bool nativeCompleted = false;
    __try {
        // BattleUILcd has its own gameplay reticle. Its final layout animation
        // at111BFC completes before this observation; GunCrossChairUI can remain
        // allocated but hidden during battle and must not replace this snapshot.
        originalBattleUpdate(object, elapsed);
        nativeCompleted = true;
        const auto observeStart = timingStart();
        PrivateDataObservationTiming operations;
        if (observeStart) privateDataObservationTiming = &operations;
        observeRoots(object, OwnerKind::Battle, &previous);
        privateDataObservationTiming = previousTiming;
        recordObservation(OwnerKind::Battle, observeStart, operations);
        reportGuards();
    } __finally {
        privateDataObservationTiming = previousTiming;
        if (achievementUpdate) endNativeAchievementUpdate(object, nativeCompleted);
        updatingBattle = false;
    }
}

struct Suppression {
    OwnerKind kind = OwnerKind::Gun;
    std::uintptr_t root = 0;
    std::uint32_t group = 0;
    std::size_t index = 0;
    unsigned char previous = 0;
    PrivateDataBytePermission permission;
    bool attempted = false;
    bool meter = false;
};

void failure(const char* reason) noexcept {
    const bool alreadyFailed = failed.exchange(true, std::memory_order_relaxed);
    if (!alreadyFailed) {
        try { log_warning("bone-eater", "Native reticle suppression disabled: {}", reason); }
        catch (...) {}
    }
}

void restore(const Suppression& suppression) noexcept {
    if (!suppression.attempted) return;
    unsigned char current = 0;
    if (!(suppression.meter ? namedMeterRoot(suppression.root, suppression.group) :
            namedRoot(suppression.root, suppression.group, suppression.index, suppression.kind)) ||
            !read(suppression.root, 0xC9, current) || current > 1) {
        failure("root identity changed during native commit");
        return;
    }
    // A native change to visible is already the saved state. No saved state is
    // retained beyond this synchronous render-only call or into game animation.
    if (current != suppression.previous && !writeVisibility(suppression.root, suppression.previous, suppression.permission)) {
        failure("could not restore root visibility after native commit");
    }
}

PrivateDataByteAccess preparePermission(const Roots& roots) noexcept {
    PrivateDataByteAccess access;
    std::array<std::uintptr_t, 12> addresses {};
    const std::size_t amount = roots.kind == OwnerKind::Battle ? kBattleRootNames.size() : kRootNames.size();
    for (std::size_t index = 0; index < amount; ++index) {
        if (!roots.objects[index] || roots.objects[index] > std::numeric_limits<std::uintptr_t>::max() - 0xC9)
            return access;
        addresses[index] = roots.objects[index] + 0xC9;
    }
    auto* previousTiming = privateDataObservationTiming;
    const auto begin = timingStart();
    PrivateDataObservationTiming operations;
    __try {
        if (begin) privateDataObservationTiming = &operations;
        // This timestamp belongs only to the OS permission observation. A slow
        // query must not refresh the independently observed native owner.
        access = observePrivateDataBytes(addresses.data(), amount, GetTickCount64(), &roots.writable);
    } __finally {
        privateDataObservationTiming = previousTiming;
        recordPermission(roots.kind, begin, operations);
    }
    return access;
}

Suppression prepare(std::uintptr_t object) noexcept {
    Suppression result;
    std::array<unsigned char, 0x40> bytes {};
    const bool readable = read(object, 0, bytes);
    const Roots roots = currentBattleRoots.valid && readable &&
        field<std::uint32_t>(bytes, 0x3C) == currentBattleRoots.group ? currentBattleRoots : currentRoots;
    result.kind = roots.kind;
    if (failed.load(std::memory_order_relaxed) || !roots.valid || !replacementReady) {
        count(Count::NoRoots); return result;
    }
    if (GetTickCount64() - roots.observed > 100) { count(Count::Stale); return result; }
    bool ready = false;
    try { ready = replacementReady(); }
    catch (...) { return result; }
    if (!ready) { count(Count::NotReady); return result; }

    // Most UI objects are excluded by group/type before following parent links.
    if (!readable || field<std::uintptr_t>(bytes, 0) != moduleBase + kSpriteVtable ||
            field<std::uintptr_t>(bytes, 8) != object ||
            field<std::uint32_t>(bytes, 0x3C) != roots.group) { count(Count::SpriteReject); return result; }
    if (const auto meterSide = ownedSideMeter(roots, object)) {
        // Hide the common ancestor only for this exact owned meter's commit.
        // It is deliberately never added to generic ancestry selection below.
        unsigned char leafVisible = 0;
        if (!ownerMatches(roots) || !read(object, 0xC9, leafVisible) || leafVisible != 1 ||
                !read(roots.meterRoot, 0xC9, result.previous) || result.previous != 1) return {};
        // Publish native per-side eligibility before the replacement handshake:
        // this bootstraps its first draw without hiding that first native frame.
        meterVisibleAt[meterSide == 2 ? 1 : 0].store(roots.observed);
        if (!desktopMetersReady(meterSide)) return {};
        const std::uintptr_t address = roots.meterRoot + 0xC9;
        if (address < roots.meterRoot) return {};
        const auto access = observePrivateDataBytes(&address, 1, GetTickCount64(), &roots.meterWritable);
        try { ready = replacementReady(); } catch (...) { return {}; }
        if (!ready || !access.valid || !currentObservation(roots) || !ownerMatches(roots) ||
                ownedSideMeter(roots, object) != meterSide || !desktopMetersReady(meterSide) ||
                !read(object, 0xC9, leafVisible) || leafVisible != 1 ||
                !read(roots.meterRoot, 0xC9, result.previous) || result.previous != 1) return {};
        result.root = roots.meterRoot; result.group = roots.group; result.meter = true;
        result.permission = acquirePrivateDataBytePermission(access, address);
        if (!result.permission.valid) return {};
        currentBattleRoots.meterWritable = access;
        result.attempted = true;
        if (!writeVisibility(result.root, 0, result.permission)) {
            restore(result); failure("could not suppress meter for native commit"); return {};
        }
        return result;
    }
    std::uintptr_t cursor = object;
    std::array<std::uintptr_t, 32> visited {};
    bool matched = false;
    for (std::size_t depth = 0; cursor && depth < visited.size(); ++depth) {
        for (std::size_t prior = 0; prior < depth; ++prior) {
            if (visited[prior] == cursor) { count(Count::AncestryReject); return result; }
        }
        visited[depth] = cursor;
        const std::size_t amount = roots.kind == OwnerKind::Battle ? kBattleRootNames.size() : kRootNames.size();
        for (std::size_t index = 0; index < amount; ++index) {
            if (cursor == roots.objects[index]) {
                result.root = cursor;
                result.group = roots.group;
                result.index = index;
                matched = true;
                break;
            }
        }
        if (matched) break;
        if (depth && !read(cursor, 0, bytes)) { count(Count::AncestryReject); return Suppression {}; }
        const auto vtable = field<std::uintptr_t>(bytes, 0);
        if ((vtable != moduleBase + kSpriteVtable && vtable != moduleBase + kRectVtable &&
                vtable != moduleBase + kObjectVtable) ||
                field<std::uintptr_t>(bytes, 8) != cursor ||
                field<std::uint32_t>(bytes, 0x3C) != roots.group) { count(Count::AncestryReject); return Suppression {}; }
        cursor = field<std::uintptr_t>(bytes, 0x10);
    }
    if (!matched || !ownerMatches(roots) || !namedRoot(result.root, result.group, result.index, roots.kind) ||
            !read(result.root, 0xC9, result.previous) || result.previous != 1) {
        count(Count::FinalReject); return Suppression {};
    }
    // Acquire on demand only after the complete selection and visible-byte
    // checks. The old lease is reused for at most 50ms without timestamp renewal.
    const auto access = preparePermission(roots);
    // Querying may stall. Recheck this exact owner observation and live fields
    // afterwards; a fresh permission cannot revive an expired owner identity.
    if (!access.valid || !currentObservation(roots) || !ownerMatches(roots) ||
            !namedRoot(result.root, result.group, result.index, roots.kind) ||
            !read(result.root, 0xC9, result.previous) || result.previous != 1) {
        count(Count::FinalReject); return Suppression {};
    }
    // The replacement's short-lived draw handshake can also expire while an
    // OS query runs. Do not hide a native sprite on the earlier readiness alone.
    try { ready = replacementReady(); }
    catch (...) { return Suppression {}; }
    if (!ready) { count(Count::NotReady); return Suppression {}; }
    if (!currentObservation(roots)) { count(Count::Stale); return Suppression {}; }
    auto& current = roots.kind == OwnerKind::Battle ? currentBattleRoots : currentRoots;
    current.writable = access;
    result.permission = acquirePrivateDataBytePermission(access, result.root + 0xC9);
    if (!result.permission.valid) { count(Count::FinalReject); return Suppression {}; }

    // Only these named roots are modified; bullet holes sharing their UI group
    // but outside their ancestry are never selected. The original native commit
    // sees the hidden ancestor and writes alpha zero through its normal path.
    result.attempted = true;
    if (!writeVisibility(result.root, 0, result.permission)) {
        restore(result); // Also covers an unexpectedly partial OS write.
        failure("could not suppress root visibility for native commit");
        return Suppression {};
    }
    count(Count::Suppressed);
    if (timingEnabled && timingDepth <= 1)
        ++timingWindow.successfulHides[roots.kind == OwnerKind::Gun ? 0 : 1];
    return result;
}

struct GameplayFooterHide {
    std::uintptr_t object=0;
    PrivateDataBytePermission permission;
    bool attempted=false;
};

GameplayFooterHide hideGameplayFooter(std::uintptr_t object) noexcept {
    GameplayFooterHide result;
    std::array<unsigned char,0xD0> leaf{},parent{},root{};
    if(!read(object,0,leaf)||field<std::uintptr_t>(leaf,0)!=moduleBase+kSpriteVtable||
       field<std::uintptr_t>(leaf,8)!=object||leaf[0xC9]!=1)return result;
    RankingNoticeIdentity notice;
    if(!std::memcmp(leaf.data()+0x40,"Message_BG",11)&&
       readRankingNotice(moduleBase,[](auto p,auto o,auto& v)noexcept{return read(p,o,v);},notice)&&object==notice.background){
        unsigned char visible=0;if(!read(object,0xC9,visible)||visible!=1)return result;
        const auto address=object+0xC9;
        result.permission=acquirePrivateDataBytePermission(observePrivateDataBytes(&address,1,GetTickCount64()),address);
        if(!result.permission.valid)return {};
        result.object=object;result.attempted=true;
        if(!writeVisibility(object,0,result.permission)){writeVisibility(object,1,result.permission);return {};}
        return result;
    }
    if(std::memcmp(leaf.data()+0x40,"FreePlay",9))return result;
    std::uintptr_t manager=0,resident=0,layout=0,layoutRoot=0;
    std::array<unsigned char,0x40> scene{};
    std::array<unsigned char,0x70> layoutBytes{};
    if(!read(moduleBase,0x13DD000,manager)||!read(manager,0,scene)||
       field<std::uintptr_t>(scene,0)!=moduleBase+0x10C6B38||field<unsigned>(scene,0x38)!=5||
       field<int>(scene,0x3C)!=-1||!read(moduleBase,0x13DCF48,resident)||
       !read(resident,0x120,layout)||!read(layout,0,layoutBytes)||
       field<std::uintptr_t>(layoutBytes,0)!=moduleBase+0x10C9FA8||
       field<unsigned>(layoutBytes,0x64)!=0xB28||field<unsigned>(layoutBytes,0x68)!=0xB8B||
       field<unsigned>(layoutBytes,0x6C)!=0xB8C||!read(layout,0x428,layoutRoot))return result;
    const auto parentAddress=field<std::uintptr_t>(leaf,0x10);
    if(!read(parentAddress,0,parent)||!read(layoutRoot,0,root))return result;
    const auto group=field<unsigned>(leaf,0x3C);
    if(field<std::uintptr_t>(parent,0)!=moduleBase+kRectVtable||field<std::uintptr_t>(parent,8)!=parentAddress||
       std::memcmp(parent.data()+0x40,"BtmRoot",8)||field<std::uintptr_t>(parent,0x10)!=layoutRoot||
       field<unsigned>(parent,0x3C)!=group||field<std::uintptr_t>(root,0)!=moduleBase+kRectVtable||
       field<std::uintptr_t>(root,8)!=layoutRoot||field<std::uintptr_t>(root,0x10)!=0||
       std::memcmp(root.data()+0x40,"Root",5)||field<unsigned>(root,0x3C)!=group)return result;
    const auto address=object+0xC9;
    const auto access=observePrivateDataBytes(&address,1,GetTickCount64());
    result.permission=acquirePrivateDataBytePermission(access,address);
    if(!result.permission.valid)return {};
    result.object=object;result.attempted=true;
    if(!writeVisibility(object,0,result.permission)) {
        writeVisibility(object,1,result.permission);return {};
    }
    return result;
}

void restoreGameplayFooter(const GameplayFooterHide& token) noexcept {
    if(!token.attempted)return;
    // The native synchronous commit retains this exact sprite; restore even if
    // a scene transition started during the call. No persistent scene mutation.
    if(!writeVisibility(token.object,1,token.permission))failure("gameplay footer restoration failed");
}

// Keep the SEH boundary free of C++ objects/catches. Restoration also runs when
// the original native commit raises a Windows exception; it is not swallowed.
void invokeCommit(void* object, const Suppression* suppression,
                  const BattleBackgroundSuppression* background,
                  const ReplayAlignment* replay, const RankingBackingAlignment* ranking,
                  const AchievementAlignment* achievement, const MenuMarginAlignment* menuMargin,
                  const NormalStartBackingAlignment* normalStart, const OptionsBackingAlignment* options,
                  const GameplayFooterHide* footer) {
    const auto nativeStart = timingStart();
    __try {
        originalCommit(object);
    } __finally {
        timingRecord(suppression->attempted || background->attempted || replay->attempted || ranking->attempted || achievement->attempted || menuMargin->attempted || normalStart->attempted || options->attempted ?
            TimingSection::NativeHidden : TimingSection::NativePlain, nativeStart);
        const auto restoreStart = timingStart();
        restoreGameplayFooter(*footer);
        restoreNativeOptionsBackingCommit(*options);
        restoreNativeNormalStartBackingCommit(*normalStart);
        restoreNativeMenuMarginCommit(*menuMargin);
        restoreNativeAchievementAlignmentCommit(*achievement);
        restoreNativeRankingBackingCommit(*ranking);
        restoreNativeReplayAlignmentCommit(*replay);
        restoreNativeBattleBackgroundCommit(*background);
        restore(*suppression);
        committing = false;
        timingRecord(TimingSection::Restore, restoreStart);
    }
}

void invokePlainCommit(void* object, bool outermost) {
    const auto nativeStart = timingStart(outermost);
    __try {
        originalCommit(object);
    } __finally {
        timingRecord(TimingSection::NativePlain, nativeStart);
    }
}

void commitObserved(void* object, std::uintptr_t caller) {
    const bool outermost = timingDepth <= 1;
    const auto guardStart = timingStart(outermost);
    count(Count::Commit);
    lastCaller.store(caller, std::memory_order_relaxed);
    lastCommitThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (committing || caller != moduleBase + 0x1E634D) {
        timingRecord(TimingSection::Guard, guardStart);
        invokePlainCommit(object, outermost);
        if (!committing) reportGuards();
        return;
    }
    count(Count::Caller);
    committing = true;
    const Suppression suppression = prepare(reinterpret_cast<std::uintptr_t>(object));
    const BattleBackgroundSuppression background = beginNativeBattleBackgroundCommit(object, caller);
    const ReplayAlignment replay = beginNativeReplayAlignmentCommit(object, caller);
    const RankingBackingAlignment ranking = beginNativeRankingBackingCommit(object, caller);
    const AchievementAlignment achievement = beginNativeAchievementAlignmentCommit(object, caller);
    const MenuMarginAlignment menuMargin = beginNativeMenuMarginCommit(object, caller);
    const NormalStartBackingAlignment normalStart = beginNativeNormalStartBackingCommit(object, caller);
    const OptionsBackingAlignment options = beginNativeOptionsBackingCommit(object, caller);
    const GameplayFooterHide footer=hideGameplayFooter(reinterpret_cast<std::uintptr_t>(object));
    timingRecord(TimingSection::Guard, guardStart);
    invokeCommit(object, &suppression, &background, &replay, &ranking, &achievement, &menuMargin, &normalStart, &options, &footer);
    reportGuards();
    if (suppression.attempted && !failed.load(std::memory_order_relaxed) &&
            !suppressionReported.exchange(true, std::memory_order_relaxed)) {
        try { log_info("bone-eater", "Native reticle visual suppression active; named root visibility restored after each commit"); }
        catch (...) {}
    }
}

void __fastcall commit(void* object) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (!timingEnabled) {
        commitObserved(object, caller);
        return;
    }
    ++timingDepth;
    __try {
        commitObserved(object, caller);
    } __finally {
        --timingDepth;
    }
}

template<std::size_t N>
bool bytesEqual(std::uintptr_t offset, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(moduleBase, offset, actual) && actual == expected;
}

} // namespace

unsigned nativeSideMetersVisible() noexcept {
    if (failed.load(std::memory_order_relaxed)) return 0;
    unsigned result = 0;
    const auto now = GetTickCount64();
    for (unsigned index = 0; index < meterVisibleAt.size(); ++index) {
        const auto last = meterVisibleAt[index].load();
        if (last && now - last <= 100) result |= 1U << index;
    }
    return result;
}

void installNativeReticleSuppression(void* module, DesktopReticleReady ready) noexcept {
    try {
        wchar_t setting[4] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_DESKTOP_RETICLE", setting, 4) != 1 ||
                setting[0] != L'1' || !ready || originalCommit || originalUpdate || originalBattleUpdate) return;
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native reticle suppression disabled: {}", status);
            return;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> updateBytes {{
            0x40,0x53,0x48,0x83,0xec,0x30,0x0f,0x29,0x74,0x24,0x20,0x0f,0x57,0xc0,0x0f,0x28,
            0xf1,0x48,0x8b,0xd9,0x0f,0x2e,0xf0,0x7a,0x0a,0x75,0x08,0xf3,0x0f,0x10,0x35,0xd9
        }};
        constexpr std::array<unsigned char, 32> commitBytes {{
            0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x83,0x79,0x30,0x00,0x48,0x8b,0xd9,0x0f,0x84,
            0x52,0x01,0x00,0x00,0x48,0x8b,0x01,0xff,0x50,0x48,0x83,0xf8,0x02,0x74,0x12,0x48
        }};
        constexpr std::array<unsigned char, 32> battleUpdateBytes {{
            0x48,0x89,0x5c,0x24,0x20,0x41,0x56,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x8b,0x49,
            0x4c,0x0f,0x29,0x74,0x24,0x20,0x0f,0x28,0xf1,0x85,0xc9,0x74,0x2d,0xff,0xc9,0x75
        }};
        constexpr std::array<unsigned char, 13> callBytes {{
            0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff
        }};
        constexpr std::array<unsigned char, 44> ancestryBytes {{
            0x80,0xb9,0xc9,0x00,0x00,0x00,0x00,0x74,0x1d,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,
            0x84,0xd2,0x74,0x15,0x48,0x8b,0x49,0x10,0x48,0x85,0xc9,0x74,0x0c,0x80,0xb9,0xc9,
            0x00,0x00,0x00,0x00,0x75,0xea,0x32,0xc0,0xc3,0xb0,0x01,0xc3
        }};
        if (!bytesEqual(0x1384B0, updateBytes) || !bytesEqual(0x111800, battleUpdateBytes) ||
                !bytesEqual(0x1E5560, commitBytes) ||
                !bytesEqual(0x1E6340, callBytes) || !bytesEqual(0x1E5430, ancestryBytes)) {
            log_warning("bone-eater", "Native reticle suppression disabled: instruction bytes differ");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        wchar_t timingSetting[4] {};
        LARGE_INTEGER frequency {};
        timingEnabled = diagnostics::optionalOutputEnabled() &&
            GetEnvironmentVariableW(L"BONE_EATER_RETICLE_TIMING", timingSetting, 4) == 1 &&
            timingSetting[0] == L'1' && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
        timingFrequency = timingEnabled ? static_cast<std::uint64_t>(frequency.QuadPart) : 0;
        replacementReady = ready;
        originalUpdate = reinterpret_cast<UpdateGun>(moduleBase + 0x1384B0);
        if (!detour::trampoline_try(originalUpdate, &update, &originalUpdate)) {
            failure("post-animation observer hook could not be installed");
            return;
        }
        originalBattleUpdate = reinterpret_cast<UpdateGun>(moduleBase + 0x111800);
        if (!detour::trampoline_try(originalBattleUpdate, &updateBattle, &originalBattleUpdate)) {
            failure("battle post-animation observer hook could not be installed");
            return;
        }
        originalCommit = reinterpret_cast<CommitSprite>(moduleBase + 0x1E5560);
        if (!detour::trampoline_try(originalCommit, &commit, &originalCommit)) {
            failure("native sprite commit hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native reticle suppression enabled with a fresh replacement-render handshake");
        installNativeBattleBackgroundFilter(module, true);
        installNativeReplayAlignment(module, true);
        installNativeRankingBacking(module, true);
        installNativeAchievementAlignment(module, true);
        installNativeMenuMargin(module, true);
        installNativeNormalStartBacking(module, true);
        installNativeOptionsBacking(module, true);
    } catch (...) { failure("hook installation failed"); }
}

} // namespace bone_eater::render
