#include "camera/native_framing_observer.h"
#include "diagnostics/optional_csv.h"
#include "camera/checked_target_write.h"
#include "camera/framing_math.h"
#include "camera/framing_transition.h"
#include "input/aim_state.h"
#include "input/native_aim.h"
#include "input/native_wide_input.h"
#include "input/scope_control.h"
#include "input/native_input_ownership.h"
#include "render/native_viewport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <intrin.h>
#include <share.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace bone_eater::camera {
namespace {

using Commit = void(__fastcall*)(void*);
Commit originalCommit = nullptr;
std::uintptr_t moduleBase = 0;
bool installAttempted = false; // Installation is before game entry.
bool writeTiming = false; // Opt-in measurement; immutable after installation.
LONGLONG timingFrequency = 0;
thread_local bool inside = false;
constexpr std::uintptr_t kCameraVtable = 0x6FDF48;
constexpr std::size_t kKeys = 128;
constexpr unsigned kSamplesPerSecond = 16;
using Vector = std::array<float, 4>;
using WrapperBytes = std::array<unsigned char, 0x48>;

struct Identity {
    std::uintptr_t wrapper = 0, vtable = 0, camera = 0, owned = 0;
    bool valid = false;
};

struct Snapshot {
    const char* status = "unresolved";
    Identity wrapper, active, scope;
    std::uintptr_t manager = 0, managerCamera = 0;
    std::uintptr_t cameraVtable = 0, targetObject = 0, targetVtable = 0;
    std::uintptr_t targetSetter = 0, upObject = 0;
    Vector position {}, target {}, up {}, nativeTargetLocal {};
    float zoom = 0, roll = 0, width = 0, height = 0;
    std::uint8_t slot = 0;
    bool nativeTargetLocalKnown = false;
    bool sameActiveWrapper = false, sameManagerCamera = false, excludesScope = false;
};

struct Key {
    std::uintptr_t wrapper = 0, caller = 0;
    DWORD thread = 0;
    std::uint64_t calls = 0, lastSample = 0;
};

struct Observer {
    std::mutex mutex;
    std::atomic<bool> failed {false};
    std::atomic<std::uint64_t> busyDrops {0};
    std::array<Key, kKeys> keys {};
    FILE* output = nullptr;
    std::uint64_t start = 0, budgetEpoch = 0, sequence = 0, keyOverflow = 0;
    unsigned budgetUsed = 0;
};

// The detour can remain reachable during shutdown. Neither this storage nor its
// stream/mutex is destroyed before the process; Windows closes the file handle.
Observer* observer = nullptr;

// Experimental player offset: advance only at the AnimationCamera commit
// before ScopeCamera evaluates its ray. The later battle commit reuses it.
struct PanState {
    FramingTransitionState framing;
    std::atomic<DWORD> thread {0};
    std::atomic<bool> failed {false};
    std::uintptr_t wrapper = 0, camera = 0;
    std::chrono::steady_clock::time_point earlyAt {};
    std::uint64_t earlySerial = 0, applied = 0, restored = 0;
    std::uint64_t poseSkips = 0;
    ULONGLONG poseLoggedAt = 0;
    ULONGLONG loggedAt = 0;
    ULONGLONG writeTimingAt = 0;
    std::uint64_t writeCalls = 0, writeTicks = 0, maxWriteTicks = 0;
    std::uint64_t permissionCalls = 0, permissionQueries = 0, permissionTicks = 0, maxPermissionTicks = 0;
    NativeTargetAccess targetAccess;
    std::uintptr_t accessCamera = 0, accessTargetObject = 0;
    FILE* output = nullptr;
    bool outputFailed = false; // Diagnostic sink only; never a pan invariant failure.
};
PanState* pan = nullptr;

struct PanOverride {
    std::uintptr_t wrapper = 0, camera = 0, targetObject = 0;
    Vector baseline {}, pitched {};
    NativeTargetPermission permission;
    std::uint64_t caller = 0, serial = 0;
    double offset = 0;
    bool attempted = false, applied = false, restored = false, locked = false;
};

struct Ticket {
    bool sample = false;
    std::uint64_t sequence = 0, calls = 0, elapsed = 0;
    std::uintptr_t wrapper = 0, caller = 0;
    DWORD thread = 0;
};

template<typename T>
bool read(std::uintptr_t owner, std::size_t offset, T& value) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
            owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool derivedWrapper(std::uintptr_t vtable) noexcept {
    const auto rva = vtable - moduleBase;
    return rva == 0x10C5D70 || rva == 0x10C4B60 || rva == 0x10C7350 ||
        rva == 0x10C73D0 || rva == 0x10C6200;
}

bool nativeCamera(std::uintptr_t camera) noexcept {
    std::uintptr_t vtable = 0;
    return camera && read(camera, 0, vtable) && vtable == moduleBase + kCameraVtable;
}

Identity identity(std::uintptr_t wrapper) noexcept {
    Identity result;
    result.wrapper = wrapper;
    if (!wrapper) { result.valid = true; return result; }
    if (!read(wrapper, 0, result.vtable)) return result;
    const bool derived = derivedWrapper(result.vtable);
    if (!derived && result.vtable != moduleBase + 0x10CB750) return result;
    if (!read(wrapper, 8, result.camera) || (result.camera && !nativeCamera(result.camera))) return result;
    // +60 is not a field of base CCamera. Read it only for proven derived types.
    if (derived && (!read(wrapper, 0x60, result.owned) ||
            (result.owned && !nativeCamera(result.owned)))) return result;
    std::uintptr_t vtableAgain = 0, cameraAgain = 0, ownedAgain = 0;
    result.valid = read(wrapper, 0, vtableAgain) && vtableAgain == result.vtable &&
        read(wrapper, 8, cameraAgain) && cameraAgain == result.camera &&
        (!derived || (read(wrapper, 0x60, ownedAgain) && ownedAgain == result.owned));
    return result;
}

bool same(const Identity& a, const Identity& b) noexcept {
    return a.valid && b.valid && a.wrapper == b.wrapper && a.vtable == b.vtable &&
        a.camera == b.camera && a.owned == b.owned;
}

Identity anchor(std::size_t rva) noexcept {
    std::uintptr_t wrapper = 0, again = 0;
    if (!read(moduleBase, rva, wrapper)) return {};
    auto result = identity(wrapper);
    if (!read(moduleBase, rva, again) || again != wrapper) result.valid = false;
    return result;
}

bool finite(const Vector& value) noexcept {
    for (float component : value) if (!std::isfinite(component)) return false;
    return true;
}

Snapshot snapshot(std::uintptr_t wrapper) noexcept {
    Snapshot result;
    result.wrapper = identity(wrapper);
    result.active = anchor(0x13DB588);
    result.scope = anchor(0x13DCF88);
    if (!result.wrapper.valid || !result.wrapper.camera) {
        result.status = "wrapper_identity_unresolved";
        return result;
    }
    WrapperBytes bytes {}, repeated {};
    const auto camera = result.wrapper.camera;
    if (!read(wrapper, 0, bytes) || !read(moduleBase, 0x12E1930, result.manager) ||
            !read(result.manager, 0x340, result.managerCamera) ||
            !read(camera, 0, result.cameraVtable) || result.cameraVtable != moduleBase + kCameraVtable ||
            !read(camera, 0x1B0, result.targetObject) ||
            !read(camera, 0x1B8, result.upObject) || !read(camera, 0xE8E, result.slot) ||
            !read(camera, 0xEB4, result.width) || !read(camera, 0xEB8, result.height)) {
        result.status = "fields_unreadable";
        return result;
    }
    result.position = field<Vector>(bytes, 0x10);
    result.target = field<Vector>(bytes, 0x20);
    result.up = field<Vector>(bytes, 0x30);
    result.zoom = field<float>(bytes, 0x40);
    result.roll = field<float>(bytes, 0x44);
    if (result.targetObject) {
        if (!read(result.targetObject, 0, result.targetVtable) ||
                !read(result.targetVtable, 0xA0, result.targetSetter)) {
            result.status = "native_target_unreadable";
            return result;
        }
        // Only the proven vector-position setter establishes this +80 layout.
        if (result.targetSetter == moduleBase + 0x66F50)
            result.nativeTargetLocalKnown = read(result.targetObject, 0x80, result.nativeTargetLocal);
    }
    std::uintptr_t managerAgain = 0, managerCameraAgain = 0, targetAgain = 0;
    std::uintptr_t targetVtableAgain = 0, upAgain = 0;
    if (!read(wrapper, 0, repeated) || repeated != bytes ||
            !same(result.wrapper, identity(wrapper)) ||
            !same(result.active, anchor(0x13DB588)) || !same(result.scope, anchor(0x13DCF88)) ||
            !read(moduleBase, 0x12E1930, managerAgain) || managerAgain != result.manager ||
            !read(result.manager, 0x340, managerCameraAgain) || managerCameraAgain != result.managerCamera ||
            !read(camera, 0x1B0, targetAgain) || targetAgain != result.targetObject ||
            !read(camera, 0x1B8, upAgain) || upAgain != result.upObject ||
            (result.targetObject && (!read(result.targetObject, 0, targetVtableAgain) ||
                targetVtableAgain != result.targetVtable))) {
        result.status = "identity_or_wrapper_changed";
        return result;
    }
    if (result.nativeTargetLocalKnown) {
        Vector localAgain {};
        result.nativeTargetLocalKnown = read(result.targetObject, 0x80, localAgain) &&
            std::memcmp(localAgain.data(), result.nativeTargetLocal.data(), sizeof(localAgain)) == 0 &&
            finite(result.nativeTargetLocal);
    }
    if (!finite(result.position) || !finite(result.target) || !finite(result.up) ||
            !std::isfinite(result.zoom) || !std::isfinite(result.roll) ||
            !std::isfinite(result.width) || !std::isfinite(result.height) ||
            result.width <= 0 || result.height <= 0 || result.width > 32768 || result.height > 32768) {
        result.status = "nonfinite_or_invalid_dimensions";
        return result;
    }
    result.sameActiveWrapper = result.active.wrapper == wrapper && result.active.camera == camera;
    result.sameManagerCamera = result.managerCamera == camera;
    result.excludesScope = result.scope.wrapper != wrapper && result.scope.camera != camera &&
        result.scope.owned != camera;
    result.status = "resolved";
    return result;
}

void failPan(const char* reason) noexcept {
    if (pan && !pan->failed.exchange(true)) {
        try { log_warning("bone-eater", "Player framing disabled: {}", reason); }
        catch (...) {}
    }
}

NativeTargetPermission targetPermission(const Snapshot& snapshot) noexcept {
    LARGE_INTEGER before {}, after {};
    const bool timed = writeTiming && QueryPerformanceCounter(&before);
    const auto* previous = pan->accessCamera == snapshot.wrapper.camera &&
        pan->accessTargetObject == snapshot.targetObject ? &pan->targetAccess : nullptr;
    bool queried = false;
    pan->targetAccess = observeNativeTargetAccess(snapshot.wrapper.wrapper, GetTickCount64(), previous, &queried);
    pan->accessCamera = snapshot.wrapper.camera;
    pan->accessTargetObject = snapshot.targetObject;
    const auto permission = acquireNativeTargetPermission(pan->targetAccess, snapshot.wrapper.wrapper);
    if (timed && QueryPerformanceCounter(&after) && before.QuadPart > 0 && after.QuadPart >= before.QuadPart) {
        const auto ticks = static_cast<std::uint64_t>(after.QuadPart - before.QuadPart);
        ++pan->permissionCalls;
        if (queried) ++pan->permissionQueries;
        pan->permissionTicks += ticks;
        if (ticks > pan->maxPermissionTicks) pan->maxPermissionTicks = ticks;
    }
    return permission;
}

bool writeTarget(const NativeTargetPermission& permission, const Vector& target) noexcept {
    LARGE_INTEGER before {}, after {};
    const bool timed = writeTiming && QueryPerformanceCounter(&before);
    const bool result = writeNativeTarget(permission, target);
    if (timed && pan && QueryPerformanceCounter(&after) && before.QuadPart > 0 &&
            after.QuadPart >= before.QuadPart) {
        const auto ticks = static_cast<std::uint64_t>(after.QuadPart - before.QuadPart);
        ++pan->writeCalls;
        pan->writeTicks += ticks;
        if (ticks > pan->maxWriteTicks) pan->maxWriteTicks = ticks;
    }
    return result;
}

bool authoredTargetDistinct(const Snapshot& s) noexcept {
    // AnimationCamera resolves its authored source with 277820(entity,5).
    // Read the verified bucket chain; calling that helper would change its
    // iteration index. Some native camera-copy paths share target objects.
    const auto resolve = [&s](std::array<std::uintptr_t, 6>& chain) {
        return read(s.wrapper.wrapper, 0x138, chain[0]) && chain[0] &&
            read(chain[0], 0x2C0, chain[1]) && chain[1] &&
            read(chain[1], 0, chain[2]) && chain[2] &&
            read(chain[2], 0x38, chain[3]) && chain[3] &&
            read(chain[3], 0, chain[4]) && chain[4] == moduleBase + kCameraVtable &&
            read(chain[3], 0x1B0, chain[5]) && chain[5];
    };
    std::array<std::uintptr_t, 6> source {}, repeated {};
    return resolve(source) && source[3] != s.wrapper.camera && source[5] != s.targetObject &&
        resolve(repeated) && source == repeated;
}

bool panIdentity(const Snapshot& s) noexcept {
    if (std::strcmp(s.status, "resolved") || !s.sameActiveWrapper || !s.sameManagerCamera ||
            !s.excludesScope || s.wrapper.vtable != moduleBase + 0x10C4B60 ||
            s.wrapper.owned != s.wrapper.camera || s.slot != 0 || s.width != 1920 || s.height != 1080 ||
            s.targetVtable != moduleBase + 0x6FDC58 || s.targetSetter != moduleBase + 0x66F50 ||
            !s.targetObject || s.upObject || !s.scope.valid || !s.scope.camera ||
            !render::readNativeHudGeometry().valid || !authoredTargetDistinct(s)) return false;
    std::uintptr_t scopeTarget = 0, scopeOwnedTarget = 0;
    if (!read(s.scope.camera, 0x1B0, scopeTarget) || scopeTarget == s.targetObject) return false;
    return !s.scope.owned || (read(s.scope.owned, 0x1B0, scopeOwnedTarget) && scopeOwnedTarget != s.targetObject);
}

// Before-write rejection only. Never call this to recover a failed write,
// restoration, thread assertion or post-native certificate check.
void resetPanTransition() noexcept {
    pan->framing.resetForNativeTransition();
    pan->earlyAt = std::chrono::steady_clock::now();
    pan->targetAccess = {};
    pan->accessCamera = pan->accessTargetObject = 0;
}

void skipPanPose(PitchTargetStatus status, std::uintptr_t caller) noexcept {
    resetPanTransition();
    ++pan->poseSkips;
    const auto now = GetTickCount64();
    if (pan->poseLoggedAt && now - pan->poseLoggedAt < 5000) return;
    pan->poseLoggedAt = now;
    try {
        log_info("bone-eater", "Player framing pose skipped: {} caller=0x{:x} total_skips={}; native pose retained, neutral early rearm required",
            framingPoseStatusName(status), caller, pan->poseSkips);
    } catch (...) {}
}

PanOverride preparePan(const Snapshot& s, std::uintptr_t caller) {
    PanOverride result;
    if (!pan || pan->failed.load()) return result;
    const auto rva = caller - moduleBase;
    if (rva != 0x4518A && rva != 0xAA97D) return result;
    if (!panIdentity(s)) {
        // An unresolved/unrecognized identity is not evidence of cinematic
        // roll. Preserve native behavior and revoke the tracked pair without
        // relaxing its identity guards or disturbing another thread's state.
        if (pan->thread.load() == GetCurrentThreadId() && pan->wrapper == s.wrapper.wrapper)
            resetPanTransition();
        return result;
    }
    DWORD expected = 0;
    const DWORD thread = GetCurrentThreadId();
    pan->thread.compare_exchange_strong(expected, thread);
    if (pan->thread.load() != thread) { failPan("main commit moved to another thread"); return result; }
    const auto now = std::chrono::steady_clock::now();
    if (rva == 0x4518A) {
        pan->framing.invalidateCertificate();
        if (pan->wrapper != s.wrapper.wrapper || pan->camera != s.wrapper.camera) {
            resetPanTransition();
            pan->wrapper = s.wrapper.wrapper;
            pan->camera = s.wrapper.camera;
        }
        const double elapsed = std::chrono::duration<double>(now - pan->earlyAt).count();
        pan->earlyAt = now;
        ++pan->earlySerial;
        const auto aim = input::readAimSnapshot();
        const auto accepted = input::readNativeAimSnapshot();
        const auto ray = input::readNativeRaySnapshot();
        constexpr auto freshness = std::chrono::milliseconds(250);
        const auto desktop = input::cabinetToDesktop(accepted.logicalX, accepted.logicalY,
            accepted.observedArkWidth, accepted.observedArkHeight, render::readNativeHudGeometry());
        const bool intentFresh = input::aimUsableAt(aim, now, freshness);
        const bool active = intentFresh && accepted.valid &&
            now - accepted.sampledAt <= freshness && desktop.valid && ray.valid &&
            now - ray.sampledAt <= freshness && ray.camera == s.wrapper.camera &&
            ray.scopeOffKnown && !ray.scopeOff;
        VerticalPanInput request;
        request.aim.normalizedY = active ? desktop.y / 1080.0 : 0.5;
        request.mode = active ? FramingMode::AimReframe : FramingMode::FixedWide;
        // Physical hold owns the lock even if an optical/script gate is briefly
        // unavailable; losing a previous ray must not move a held framing offset.
        const auto control = input::readScopeControlSnapshot();
        const auto controlNow = GetTickCount64();
        request.scopeHeld = intentFresh && (input::scopeControlRequested() ?
            (input::scopeControlNativeReady() && control.enabled && controlNow >= control.updatedAtMs &&
             controlNow - control.updatedAtMs <= 100) : aim.scopeIntentHeld());
        // A long stall rejects integration and retains the prior bounded offset.
        pan->framing.advanceEarly(request, elapsed);
    } else if (!pan->framing.earlyCertified || pan->wrapper != s.wrapper.wrapper || pan->camera != s.wrapper.camera ||
            !pan->earlySerial || now - pan->earlyAt > std::chrono::milliseconds(100)) {
        return result;
    }
    const auto pitch = rva == 0xAA97D ? pan->framing.certifiedOffset : pan->framing.policy.state().offsetRadians;
    const auto rotated = pitchTarget(s.position, s.target, s.up, s.roll, pitch);
    if (!rotated.accepted()) {
        if (framingPoseDisposition(rotated.status) == FramingPoseDisposition::SkipNative)
            skipPanPose(rotated.status, rva);
        else failPan("nonfinite or invalid player pitch");
        return result;
    }
    result.wrapper = s.wrapper.wrapper; result.camera = s.wrapper.camera;
    result.targetObject = s.targetObject;
    result.baseline = s.target; result.pitched = rotated.target;
    result.caller = rva; result.serial = pan->earlySerial; result.offset = pitch;
    result.locked = pan->framing.policy.state().scopePanLocked;
    // Easing may converge to a nonzero double too small to change the native
    // float target. Keep policy/certification intact without writing identical
    // coordinates twice per frame indefinitely after recentering.
    if (pitch == 0 || result.pitched == result.baseline) return result;
    // Revalidate immediately before the only write. Every commit uses its own
    // fresh authored/recoil target; no persistent offset enters that baseline.
    const auto again = snapshot(s.wrapper.wrapper);
    if (!panIdentity(again) || !same(s.wrapper, again.wrapper) ||
            again.targetObject != s.targetObject || again.position != s.position ||
            again.target != s.target || again.up != s.up || again.roll != s.roll) {
        resetPanTransition();
        return {};
    }
    result.permission = targetPermission(again);
    if (!result.permission.valid) {
        failPan("target span is not currently authorized private writable data");
        return result;
    }
    result.attempted = true;
    if (!writeTarget(result.permission, result.pitched)) {
        // A guarded byte copy can fail after writing a prefix. Restoration uses
        // this commit's saved capability even if the lease has since expired.
        result.restored = writeTarget(result.permission, result.baseline);
        failPan(result.restored ? "target write failed; baseline restored" : "target write and restoration failed");
        return result;
    }
    result.applied = true;
    ++pan->applied;
    return result;
}

void restorePan(PanOverride* change) noexcept {
    if (!change || !change->applied) return;
    Vector current {};
    const auto id = identity(change->wrapper);
    if (!id.valid || id.vtable != moduleBase + 0x10C4B60 || id.camera != change->camera ||
            change->permission.wrapper != change->wrapper ||
            !read(change->wrapper, 0x20, current) ||
            (current != change->pitched && current != change->baseline)) {
        failPan("wrapper target identity changed during native commit");
        return;
    }
    change->restored = current == change->baseline || writeTarget(change->permission, change->baseline);
    if (change->restored) ++pan->restored;
    else failPan("could not restore wrapper target after native commit");
}

void reportPan(const PanOverride& change) noexcept {
    if (!pan || !pan->output || pan->outputFailed || !change.wrapper) return;
    const auto now = GetTickCount64();
    if (writeTiming) {
        if (!pan->writeTimingAt) pan->writeTimingAt = now;
        else if (now - pan->writeTimingAt >= 5000) {
            try {
                const double milliseconds = 1000.0 / static_cast<double>(timingFrequency);
                log_info("bone-eater", "Framing target-write timing thread={} window_ms={} calls={} total_ms={:.3f} mean_us={:.3f} max_us={:.3f} backend=checked_copy",
                    GetCurrentThreadId(), now - pan->writeTimingAt, pan->writeCalls,
                    pan->writeTicks * milliseconds,
                    pan->writeCalls ? pan->writeTicks * milliseconds * 1000.0 / pan->writeCalls : 0.0,
                      pan->maxWriteTicks * milliseconds * 1000.0);
                log_info("bone-eater", "Framing target-permission timing thread={} window_ms={} calls={} queries={} total_ms={:.3f} mean_us={:.3f} max_us={:.3f}",
                    GetCurrentThreadId(), now - pan->writeTimingAt, pan->permissionCalls, pan->permissionQueries,
                    pan->permissionTicks * milliseconds,
                    pan->permissionCalls ? pan->permissionTicks * milliseconds * 1000.0 / pan->permissionCalls : 0.0,
                    pan->maxPermissionTicks * milliseconds * 1000.0);
            } catch (...) {}
            pan->writeTimingAt = now;
            pan->writeCalls = pan->writeTicks = pan->maxWriteTicks = 0;
            pan->permissionCalls = pan->permissionQueries = pan->permissionTicks = pan->maxPermissionTicks = 0;
        }
    }
    if (!pan->failed.load() && now - pan->loggedAt < 1000) return;
    pan->loggedAt = now;
    std::fprintf(pan->output, "1,%lu,%llu,%lu,0x%llx,%llu,%.12g,%u,%u,%u,%llu,%llu,%u\n",
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(now),
        static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(change.caller),
        static_cast<unsigned long long>(change.serial), change.offset, change.locked ? 1u : 0u,
        change.applied ? 1u : 0u, change.restored ? 1u : 0u,
        static_cast<unsigned long long>(pan->applied), static_cast<unsigned long long>(pan->restored),
        pan->failed.load() ? 1u : 0u);
    std::fflush(pan->output);
    if (std::ferror(pan->output)) {
        pan->outputFailed = true;
        try { log_warning("bone-eater", "Player framing CSV write failed; aim-driven framing remains enabled"); }
        catch (...) {}
    }
}

void certifyEarlyPan(const PanOverride& change) noexcept {
    if (!pan || pan->failed.load() || change.caller != 0x4518A || !change.wrapper ||
            change.serial != pan->earlySerial || change.wrapper != pan->wrapper) return;
    Vector nativeTarget {}, wrapperTarget {};
    std::uintptr_t nativeObject = 0, targetVtable = 0;
    const bool restored = !change.attempted || change.restored;
    if (!restored || !read(change.camera, 0x1B0, nativeObject) || nativeObject != change.targetObject ||
            !read(nativeObject, 0, targetVtable) || targetVtable != moduleBase + 0x6FDC58 ||
            !read(nativeObject, 0x80, nativeTarget) || nativeTarget != change.pitched ||
            !read(change.wrapper, 0x20, wrapperTarget) || wrapperTarget != change.baseline) {
        failPan("early native commit or wrapper restoration did not match the proposed offset");
        return;
    }
    if (!pan->framing.certifyEarly(change.offset))
        failPan("early offset violated neutral rearm or policy bounds");
}

Ticket begin(std::uintptr_t wrapper, std::uintptr_t caller) {
    Ticket ticket;
    if (!observer->output || observer->failed.load(std::memory_order_relaxed)) return ticket;
    std::unique_lock<std::mutex> lock(observer->mutex, std::try_to_lock);
    if (!lock) { ++observer->busyDrops; return ticket; }
    const auto thread = GetCurrentThreadId();
    Key* found = nullptr;
    for (auto& key : observer->keys) {
        if (!key.calls || (key.wrapper == wrapper && key.caller == caller && key.thread == thread)) {
            found = &key;
            break;
        }
    }
    if (!found) { ++observer->keyOverflow; return ticket; }
    const bool fresh = !found->calls;
    found->wrapper = wrapper;
    found->caller = caller;
    found->thread = thread;
    ++found->calls;
    const auto now = GetTickCount64();
    if (!fresh && now - found->lastSample < 1000) return ticket;
    if (now - observer->budgetEpoch >= 1000) {
        observer->budgetEpoch = now;
        observer->budgetUsed = 0;
    }
    if (observer->budgetUsed >= kSamplesPerSecond) return ticket;
    ++observer->budgetUsed;
    found->lastSample = now;
    ticket = {true, ++observer->sequence, found->calls, now - observer->start, wrapper, caller, thread};
    return ticket;
}

void writeIdentity(FILE* file, const Identity& value) {
    std::fprintf(file, ",%u,%p,%p,%p,%p", value.valid ? 1u : 0u,
        reinterpret_cast<void*>(value.wrapper), reinterpret_cast<void*>(value.vtable),
        reinterpret_cast<void*>(value.camera), reinterpret_cast<void*>(value.owned));
}

void writeVector(FILE* file, const Vector& value) {
    std::fprintf(file, ",%.9g,%.9g,%.9g,%.9g", value[0], value[1], value[2], value[3]);
}

void writeRow(const Ticket& ticket, const char* stage, std::uint64_t elapsed, const Snapshot& value) {
    FILE* file = observer->output;
    const bool inModule = ticket.caller >= moduleBase && ticket.caller < moduleBase + 0x1511000;
    std::fprintf(file, "1,%lu,%llu,%llu,%lu,%s,%p,0x%llx,%llu,%s",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(ticket.sequence), static_cast<unsigned long long>(elapsed),
        static_cast<unsigned long>(ticket.thread), stage, reinterpret_cast<void*>(ticket.caller),
        static_cast<unsigned long long>(inModule ? ticket.caller - moduleBase : 0),
        static_cast<unsigned long long>(ticket.calls), value.status);
    writeIdentity(file, value.wrapper);
    std::fprintf(file, ",%p,%p,%p,%p,%p,%p,%p,%u,%.9g,%.9g",
        reinterpret_cast<void*>(value.manager), reinterpret_cast<void*>(value.managerCamera),
        reinterpret_cast<void*>(value.cameraVtable), reinterpret_cast<void*>(value.targetObject),
        reinterpret_cast<void*>(value.targetVtable), reinterpret_cast<void*>(value.targetSetter),
        reinterpret_cast<void*>(value.upObject), static_cast<unsigned>(value.slot), value.width, value.height);
    writeVector(file, value.position);
    writeVector(file, value.target);
    writeVector(file, value.up);
    std::fprintf(file, ",%.9g,%.9g,%u", value.zoom, value.roll, value.nativeTargetLocalKnown ? 1u : 0u);
    writeVector(file, value.nativeTargetLocal);
    writeIdentity(file, value.active);
    writeIdentity(file, value.scope);
    std::fprintf(file, ",%u,%u,%u,%llu,%llu\n", value.sameActiveWrapper ? 1u : 0u,
        value.sameManagerCamera ? 1u : 0u, value.excludesScope ? 1u : 0u,
        static_cast<unsigned long long>(observer->busyDrops.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(observer->keyOverflow));
}

void finish(const Ticket& ticket, const Snapshot& before, const Snapshot& after, std::uint64_t elapsed) {
    std::unique_lock<std::mutex> lock(observer->mutex, std::try_to_lock);
    if (!lock) { ++observer->busyDrops; return; }
    if (observer->failed.load(std::memory_order_relaxed)) return;
    writeRow(ticket, "before", ticket.elapsed, before);
    writeRow(ticket, "after", elapsed, after);
    if (std::fflush(observer->output) || std::ferror(observer->output)) observer->failed = true;
}

// Separate SEH boundary: no C++ cleanup objects, and the original exception is
// propagated unchanged. Never leave the recursion guard set on a native fault.
void invokeOriginal(void* wrapper, PanOverride* change) {
    __try { originalCommit(wrapper); }
    __finally { restorePan(change); inside = false; }
}

__declspec(noinline) void __fastcall commit(void* wrapper) {
    if (inside || !observer || (observer->failed.load(std::memory_order_relaxed) &&
            (!pan || pan->failed.load()))) {
        originalCommit(wrapper);
        return;
    }
    inside = true;
    Ticket ticket;
    Snapshot before;
    PanOverride change;
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Invalidate the tracked early/late pair even when the forthcoming identity
    // snapshot fails. A rejected early write must never be applied only later,
    // after ScopeCamera has already generated its unframed world ray.
    if (pan && caller == moduleBase + 0x4518A && pan->thread.load() == GetCurrentThreadId() &&
            pan->wrapper == reinterpret_cast<std::uintptr_t>(wrapper)) pan->framing.invalidateCertificate();
    try {
        ticket = begin(reinterpret_cast<std::uintptr_t>(wrapper),
            caller);
        const bool candidate = pan && (caller == moduleBase + 0x4518A || caller == moduleBase + 0xAA97D);
        if (ticket.sample || candidate) before = snapshot(reinterpret_cast<std::uintptr_t>(wrapper));
        if (candidate) change = preparePan(before, caller);
    } catch (...) { observer->failed = true; ticket.sample = false; failPan("framing preparation failed"); }
    // No observer lock is held across the one original call.
    invokeOriginal(wrapper, &change);
    certifyEarlyPan(change);
    reportPan(change);
    if (ticket.sample) {
        try {
            const auto after = snapshot(ticket.wrapper);
            finish(ticket, before, after, GetTickCount64() - observer->start);
        } catch (...) { observer->failed = true; }
    }
}

template<std::size_t N>
bool matches(std::size_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(moduleBase, rva, actual) && actual == expected;
}

} // namespace

void installNativeFramingObserver(void* module) noexcept {
    try {
        wchar_t setting[4] {};
        const bool observe = diagnostics::optionalOutputEnabled() &&
            GetEnvironmentVariableW(L"BONE_EATER_NATIVE_FRAMING_OBSERVE", setting, 4) == 1 && setting[0] == L'1';
        setting[0] = 0;
        const bool playerFraming = GetEnvironmentVariableW(L"BONE_EATER_AIM_FRAMING", setting, 4) == 1 && setting[0] == L'1';
        if ((!observe && !playerFraming) || installAttempted) return;
        installAttempted = true;
        setting[0] = 0;
        LARGE_INTEGER frequency {};
        writeTiming = diagnostics::optionalOutputEnabled() && playerFraming &&
            GetEnvironmentVariableW(L"BONE_EATER_FRAMING_TIMING", setting, 4) == 1 && setting[0] == L'1' &&
            QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
        timingFrequency = writeTiming ? frequency.QuadPart : 0;
        const char* status = render::verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native framing observer disabled: {}", status);
            return;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> entry {{
            0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x83,0x79,0x08,0x00,0x48,0x8B,0xD9,0x75,0x18,
            0x4C,0x8D,0x05,0x01,0x6F,0xEC,0x00,0x48,0x8D,0x0D,0x1A,0x6F,0xEC,0x00,0xBA,0x9A}};
        constexpr std::array<unsigned char, 52> dispatch {{
            0x48,0x8B,0x4B,0x08,0x48,0x8B,0x01,0xFF,0x90,0x38,0x01,0x00,0x00,0x48,0x8D,0x53,0x20,
            0x4C,0x8B,0x00,0x48,0x8B,0xC8,0x41,0xFF,0x90,0xA0,0x00,0x00,0x00,0x48,0x8B,0x4B,0x08,
            0xF3,0x0F,0x10,0x5B,0x18,0xF3,0x0F,0x10,0x53,0x14,0xF3,0x0F,0x10,0x4B,0x10,0x48,0x8B,0x01}};
        constexpr std::array<unsigned char, 8> targetGetter {{0x48,0x8B,0x81,0xB0,0x01,0x00,0x00,0xC3}};
        constexpr std::array<unsigned char, 5> tail {{0xE9,0x76,0xAC,0x11,0x00}};
        constexpr std::array<unsigned char, 5> animationCall {{0xE8,0x66,0x66,0x03,0x00}};
        constexpr std::array<unsigned char, 5> scopeCall {{0xE8,0xF5,0xE3,0xFA,0xFF}};
        if (!matches(0x196620, entry) || !matches(0x196648, dispatch) ||
                !matches(0x196E80, targetGetter) || !matches(0x7B9A5, tail) ||
                !matches(0x45185, animationCall) || !matches(0xCD3F6, scopeCall)) {
            log_warning("bone-eater", "Native framing observer disabled: instruction guards differ");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        observer = new Observer;
        observer->output = diagnostics::openOptionalCsv(L"desktop/framing-v1.csv", [](FILE* file) {
            std::fputs(
            "schema,pid,sequence,elapsed_ms,thread,stage,caller,caller_rva,call_count,status,"
            "wrapper_valid,wrapper,wrapper_vtable,camera,owned_camera,manager,manager_camera,"
            "camera_vtable,target_object,target_vtable,target_setter,up_object,slot,width,height,"
            "position_x,position_y,position_z,position_w,target_x,target_y,target_z,target_w,"
            "up_x,up_y,up_z,up_w,zoom,roll,native_target_local_known,"
            "native_target_x,native_target_y,native_target_z,native_target_w,"
            "active_valid,active_wrapper,active_vtable,active_camera,active_owned,"
            "scope_valid,scope_wrapper,scope_vtable,scope_camera,scope_owned,"
            "same_active_wrapper,same_manager_camera,excludes_scope,busy_drops,key_overflow\n", file);
        });
        if (diagnostics::optionalOutputEnabled() && !observer->output) {
            try { log_warning("bone-eater", "Framing CSV unavailable; functional framing hook will still be installed"); }
            catch (...) {}
        }
        observer->start = observer->budgetEpoch = GetTickCount64();
        if (playerFraming) {
            pan = new PanState;
            pan->output = diagnostics::openOptionalCsv(L"desktop/player-framing-v1.csv", [](FILE* file) {
                std::fputs("schema,pid,tick_ms,thread,caller_rva,early_serial,offset_radians,locked,applied,restored,total_applied,total_restored,failed\n", file);
            });
            if (diagnostics::optionalOutputEnabled() && !pan->output) {
                try { log_warning("bone-eater", "Player framing CSV unavailable; aim-driven framing remains enabled"); }
                catch (...) {}
            }
        }
        // Upstream trampoline_try captures *orig for MH_EnableHook; never seed
        // it with null, which means all hooks rather than this specific target.
        originalCommit = reinterpret_cast<Commit>(moduleBase + 0x196620);
        if (!detour::trampoline_try(originalCommit, &commit, &originalCommit)) {
            observer->failed = true;
            log_warning("bone-eater", "Native framing observer hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native framing observer installed; <=1 Hz per tuple, {} paired samples/s cap; player offset={}",
            kSamplesPerSecond, playerFraming);
    } catch (...) {
        if (observer) observer->failed = true;
        // An optional observer failure must not prevent baseline startup.
    }
}

} // namespace bone_eater::camera
