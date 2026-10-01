#include "camera/native_camera_hook.h"
#include "diagnostics/optional_csv.h"
#include "camera/main_fov_zoom.h"
#include "camera/native_framing_observer.h"
#include "render/native_viewport.h"
#include "render/native_display.h"
#include "render/native_dof_probe.h"

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

using Recalculate = void(__fastcall*)(void*);
Recalculate originalRecalculate = nullptr;
using SetProperty = bool(__fastcall*)(void*, std::uint64_t, const void*);
SetProperty setProperty = nullptr;
bool preserveVertical = false;
float mainViewZoom = 1.0f; // Configured once before installing the native seam.
std::atomic<bool> opticsFailed {false};
std::atomic<DWORD> opticsThread {0};
std::uintptr_t gameBase = 0;
constexpr std::uintptr_t kCameraVtable = 0x6FDF48;
constexpr std::size_t kKeyCount = 256;
constexpr unsigned kSamplesPerSecond = 32;

struct Anchor {
    std::uintptr_t wrapper = 0, vtable = 0, target = 0, owned = 0;
    std::uintptr_t targetParameter = 0, ownedParameter = 0;
    bool valid = false;
};

struct Snapshot {
    const char* status = "unresolved";
    std::uintptr_t camera = 0, vtable = 0, parameter = 0, parameterVtable = 0;
    std::uint32_t references = 0, generation = 0, consumedGeneration = 0;
    std::uint8_t slot = 0, flags = 0;
    float ordinaryZoom = 0, systemZoom = 0, focal = 0, filmWidth = 0;
    float aspect = 0, viewportRatio = 0, width = 0, height = 0;
    float p00 = 0, p11 = 0, adjustedP00 = 0, adjustedP11 = 0;
    Anchor active, scope;
    std::uintptr_t manager = 0, managerCamera = 0;
};

struct Key {
    std::uintptr_t camera = 0, caller = 0;
    DWORD thread = 0;
    std::uint64_t calls = 0, lastSample = 0;
};

struct Observer {
    std::mutex mutex;
    std::recursive_mutex opticsMutex;
    std::atomic<bool> failed {false};
    std::atomic<std::uint64_t> busyDrops {0};
    std::array<Key, kKeyCount> keys {};
    FILE* output = nullptr;
    std::uint64_t start = 0, sequence = 0, budgetEpoch = 0, keyOverflow = 0;
    unsigned budgetUsed = 0;
};

// Process-lifetime storage: the detour can remain reachable during DLL teardown.
// It is initialized only by the opt-in installer, never allocated on the hot path.
Observer* observer = nullptr;
thread_local bool insideObserver = false;

template<typename T>
bool read(std::uintptr_t base, std::size_t offset, T& value) noexcept {
    if (!base || base > std::numeric_limits<std::uintptr_t>::max() - offset ||
            base + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T count = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(base + offset),
        &value, sizeof(T), &count) && count == sizeof(T);
}

bool nativeCamera(std::uintptr_t camera, std::uintptr_t& parameter) noexcept {
    std::uintptr_t vtable = 0;
    return read(camera, 0, vtable) && vtable == gameBase + kCameraVtable &&
        read(camera, 0xDF0, parameter) && parameter;
}

Anchor anchor(std::uintptr_t global) noexcept {
    Anchor result;
    if (!read(gameBase, global, result.wrapper)) return result;
    if (!result.wrapper) { result.valid = true; return result; }
    if (!read(result.wrapper, 0, result.vtable)) return result;
    const auto kind = result.vtable - gameBase;
    const bool derived = kind == 0x10C5D70 || kind == 0x10C7350 ||
        kind == 0x10C4B60 || kind == 0x10C73D0 || kind == 0x10C6200;
    if (!derived && kind != 0x10CB750) return result;
    if (!read(result.wrapper, 8, result.target)) return result;
    if (result.target && !nativeCamera(result.target, result.targetParameter)) return result;
    if (derived) {
        if (!read(result.wrapper, 0x60, result.owned)) return result;
        if (result.owned && !nativeCamera(result.owned, result.ownedParameter)) return result;
    }
    std::uintptr_t again = 0, targetAgain = 0, vtableAgain = 0, ownedAgain = 0;
    result.valid = read(gameBase, global, again) && again == result.wrapper &&
        read(result.wrapper, 0, vtableAgain) && vtableAgain == result.vtable &&
        read(result.wrapper, 8, targetAgain) && targetAgain == result.target &&
        (!derived || (read(result.wrapper, 0x60, ownedAgain) && ownedAgain == result.owned));
    return result;
}

bool positive(float value) noexcept { return std::isfinite(value) && value > 0; }

Snapshot snapshot(std::uintptr_t camera) noexcept {
    Snapshot result;
    result.camera = camera;
    result.active = anchor(0x13DB588);
    result.scope = anchor(0x13DCF88);
    if (read(gameBase, 0x12E1930, result.manager) && result.manager)
        read(result.manager, 0x340, result.managerCamera);
    if (!read(camera, 0, result.vtable) || result.vtable != gameBase + kCameraVtable) {
        result.status = "camera_vtable_unresolved";
        return result;
    }
    if (!read(camera, 0xDF0, result.parameter) || !result.parameter) {
        result.status = "parameter_unresolved";
        return result;
    }
    if (!read(result.parameter, 0, result.parameterVtable) ||
            !read(result.parameter, 8, result.references) ||
            !read(result.parameter, 0x70, result.generation) ||
            !read(result.parameter, 0x14, result.ordinaryZoom) ||
            !read(result.parameter, 0x18, result.systemZoom) ||
            !read(result.parameter, 0x28, result.focal) ||
            !read(result.parameter, 0x44, result.filmWidth) ||
            !read(camera, 0xE0C, result.consumedGeneration) ||
            !read(camera, 0xE8E, result.slot) || !read(camera, 0xE93, result.flags) ||
            !read(camera, 0xDC4, result.aspect) || !read(camera, 0xEB0, result.viewportRatio) ||
            !read(camera, 0xEB4, result.width) || !read(camera, 0xEB8, result.height) ||
            !read(camera, 0x940, result.p00) || !read(camera, 0x954, result.p11) ||
            !read(camera, 0xA00, result.adjustedP00) || !read(camera, 0xA14, result.adjustedP11)) {
        result.status = "fields_unreadable";
        return result;
    }
    std::uintptr_t parameterAgain = 0, vtableAgain = 0;
    std::uint32_t generationAgain = 0;
    if (!read(camera, 0xDF0, parameterAgain) || parameterAgain != result.parameter ||
            !read(camera, 0, vtableAgain) || vtableAgain != result.vtable ||
            !read(result.parameter, 0x70, generationAgain) || generationAgain != result.generation) {
        result.status = "fields_changed_during_read";
        return result;
    }
    // Before recalculation, derived coefficients may legitimately be stale/zero.
    if (!positive(result.ordinaryZoom) || !positive(result.systemZoom) ||
            !positive(result.focal) || !positive(result.filmWidth) ||
            !positive(result.aspect) || !positive(result.viewportRatio) ||
            !positive(result.width) || !positive(result.height) ||
            result.width > 32768 || result.height > 32768) {
        result.status = "optics_or_dimensions_invalid";
        return result;
    }
    result.status = "resolved";
    return result;
}

struct Ticket {
    bool sample = false;
    std::uint64_t sequence = 0, calls = 0, elapsed = 0;
    std::uintptr_t camera = 0, caller = 0;
    DWORD thread = 0;
    bool opticsApplied = false;
    bool opticsRestored = false, opticsUnrecovered = false;
    float baselineZoom = 0, temporaryZoom = 0;
};

Ticket beginSample(std::uintptr_t camera, std::uintptr_t caller) {
    Ticket ticket;
    if (!observer || !observer->output || observer->failed) return ticket;
    std::unique_lock<std::mutex> guard(observer->mutex, std::try_to_lock);
    if (!guard) { ++observer->busyDrops; return ticket; }
    const DWORD thread = GetCurrentThreadId();
    Key* found = nullptr;
    for (auto& key : observer->keys) {
        if (!key.calls || (key.camera == camera && key.caller == caller && key.thread == thread)) {
            found = &key;
            break;
        }
    }
    if (!found) { ++observer->keyOverflow; return ticket; }
    const bool fresh = !found->calls;
    found->camera = camera;
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
    ticket.sample = true;
    ticket.sequence = ++observer->sequence;
    ticket.calls = found->calls;
    ticket.elapsed = now - observer->start;
    ticket.camera = camera;
    ticket.caller = caller;
    ticket.thread = thread;
    return ticket;
}

void writeAnchor(FILE* output, const Anchor& value) {
    std::fprintf(output, ",%u,%p,%p,%p,%p,%p,%p", value.valid ? 1u : 0u,
        reinterpret_cast<void*>(value.wrapper), reinterpret_cast<void*>(value.vtable),
        reinterpret_cast<void*>(value.target), reinterpret_cast<void*>(value.owned),
        reinterpret_cast<void*>(value.targetParameter), reinterpret_cast<void*>(value.ownedParameter));
}

void writeSnapshot(const Ticket& ticket, const char* stage, const Snapshot& value) {
    FILE* file = observer->output;
    const bool inModule = ticket.caller >= gameBase && ticket.caller < gameBase + 0x1511000;
    std::fprintf(file,
        "2,%lu,%llu,%llu,%lu,%s,%p,%p,0x%llx,%llu,%s,%p,%p,%p,%lu,%lu,%lu,%u,%u,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(ticket.sequence), static_cast<unsigned long long>(ticket.elapsed),
        static_cast<unsigned long>(ticket.thread), stage, reinterpret_cast<void*>(ticket.camera),
        reinterpret_cast<void*>(ticket.caller), static_cast<unsigned long long>(inModule ? ticket.caller - gameBase : 0),
        static_cast<unsigned long long>(ticket.calls), value.status,
        reinterpret_cast<void*>(value.vtable), reinterpret_cast<void*>(value.parameter),
        reinterpret_cast<void*>(value.parameterVtable), static_cast<unsigned long>(value.references),
        static_cast<unsigned long>(value.generation), static_cast<unsigned long>(value.consumedGeneration),
        static_cast<unsigned>(value.slot), static_cast<unsigned>(value.flags),
        value.ordinaryZoom, value.systemZoom, value.focal, value.filmWidth,
        value.aspect, value.viewportRatio, value.width, value.height,
        value.p00, value.p11, value.adjustedP00, value.adjustedP11);
    writeAnchor(file, value.active);
    writeAnchor(file, value.scope);
    std::fprintf(file, ",%p,%p,%llu,%llu,%u,%u,%.9g,%.9g,%u,%u,%u\n", reinterpret_cast<void*>(value.manager),
        reinterpret_cast<void*>(value.managerCamera),
        static_cast<unsigned long long>(observer->busyDrops.load()),
        static_cast<unsigned long long>(observer->keyOverflow),
        preserveVertical ? 1u : 0u, ticket.opticsApplied ? 1u : 0u,
        ticket.baselineZoom, ticket.temporaryZoom, opticsFailed ? 1u : 0u,
        ticket.opticsRestored ? 1u : 0u, ticket.opticsUnrecovered ? 1u : 0u);
}

void finishSample(const Ticket& ticket, const Snapshot& before, const Snapshot& after) {
    std::unique_lock<std::mutex> guard(observer->mutex, std::try_to_lock);
    if (!guard) { ++observer->busyDrops; return; }
    if (observer->failed) return;
    writeSnapshot(ticket, "before", before);
    writeSnapshot(ticket, "after", after);
    std::fflush(observer->output);
    if (std::ferror(observer->output)) observer->failed = true;
}

struct RecursionGuard {
    RecursionGuard() noexcept { insideObserver = true; }
    ~RecursionGuard() { insideObserver = false; }
};

// Temporary native property changes exist only around the engine's own matrix
// and cache construction. A later camera copy sees the script's original zoom;
// it cannot inherit a permanently scaled value and compound the correction.
struct OpticalOverride {
    void* camera = nullptr;
    std::uintptr_t parameter = 0;
    float baseline = 0, applied = 0;
    std::uint32_t generation = 0;
    bool active = false;
    Ticket& ticket;

    OpticalOverride(const Snapshot& value, Ticket& report) : ticket(report) {
        if (!preserveVertical || opticsFailed || !setProperty ||
                std::strcmp(value.status, "resolved") ||
                value.parameterVtable != gameBase + 0x6FDEE8 || value.references != 1 ||
                value.slot != 0 || (value.flags & 1) ||
                value.width != 1920 || value.height != 1080 ||
                value.managerCamera != value.camera ||
                !value.active.valid || value.active.target != value.camera ||
                value.active.vtable == gameBase + 0x10C7350 ||
                !value.scope.valid || value.scope.target == value.camera || value.scope.owned == value.camera ||
                value.scope.targetParameter == value.parameter || value.scope.ownedParameter == value.parameter) return;
        const float originalAspect = render::originalNativeMainAspect();
        if (!(originalAspect > 0.5f && originalAspect < 0.7f)) return;
        DWORD expectedThread = 0;
        const DWORD currentThread = GetCurrentThreadId();
        opticsThread.compare_exchange_strong(expectedThread, currentThread);
        if (opticsThread.load() != currentThread) {
            opticsFailed = true;
            try { log_warning("bone-eater", "Native vertical-FOV experiment disabled: main recalculation thread changed"); }
            catch (...) {}
            return;
        }
        baseline = value.systemZoom;
        const auto correction = mainFovZoom(baseline, originalAspect, value.viewportRatio, mainViewZoom);
        if (!correction.valid) return;
        applied = correction.temporaryZoom;
        camera = reinterpret_cast<void*>(value.camera);
        parameter = value.parameter;
        generation = value.generation;
        // Recheck the selected object immediately before invoking the setter.
        std::uintptr_t current = 0;
        std::uint32_t refs = 0, serial = 0;
        float zoom = 0;
        if (!nativeCamera(value.camera, current) || current != parameter ||
                !read(parameter, 8, refs) || refs != 1 ||
                !read(parameter, 0x70, serial) || serial != generation ||
                !read(parameter, 0x18, zoom) || zoom != baseline) return;
        active = setProperty(camera, 0x15, &applied);
    }

    ~OpticalOverride() {
        if (!active) return;
        std::uintptr_t current = 0;
        std::uint32_t refs = 0, serial = 0;
        float zoom = 0;
        const bool same = nativeCamera(reinterpret_cast<std::uintptr_t>(camera), current) &&
            current == parameter && read(parameter, 8, refs) && refs > 0 &&
            read(parameter, 0x18, zoom) && zoom == applied &&
            read(parameter, 0x70, serial);
        // A generation advance in another field is not a reason to leave our
        // temporary zoom behind. Restore the same live parameter only if its
        // current zoom is still ours, then disable on any invariant surprise.
        ticket.opticsRestored = same && setProperty(camera, 0x15, &baseline);
        ticket.opticsUnrecovered = !ticket.opticsRestored;
        if (!ticket.opticsRestored || refs != 1 || serial != generation + 1u) {
            opticsFailed = true;
            try { log_warning("bone-eater", "Native vertical-FOV invariant changed; restored={}, unrecovered={}",
                ticket.opticsRestored, ticket.opticsUnrecovered); }
            catch (...) {}
        }
        // Restoration increments the native generation. Do not forge consumed
        // generations or cache-valid bits to avoid the next native recomputation.
    }
};

__declspec(noinline) void __fastcall recalculate(void* camera) {
    if (insideObserver || !observer) {
        originalRecalculate(camera);
        return;
    }
    // Serialize every recalculation entry while optical overrides are enabled,
    // including foreign threads, before reading any potential baseline value.
    // This protects this seam; it does not claim to lock unrelated engine reads.
    std::unique_lock<std::recursive_mutex> opticalLock(observer->opticsMutex, std::defer_lock);
    if (preserveVertical) opticalLock.lock();
    RecursionGuard recursionGuard;
    Ticket ticket;
    Snapshot before;
    try {
        ticket = beginSample(reinterpret_cast<std::uintptr_t>(camera),
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        if (ticket.sample || preserveVertical) before = snapshot(reinterpret_cast<std::uintptr_t>(camera));
    } catch (...) {
        observer->failed = true;
        ticket.sample = false;
    }
    // Original engine exceptions/return behavior are not caught or altered.
    // No observer mutex is held across this call, including on nested callbacks.
    {
        OpticalOverride optics(before, ticket);
        ticket.opticsApplied = optics.active;
        ticket.baselineZoom = optics.baseline;
        ticket.temporaryZoom = optics.applied;
        originalRecalculate(camera);
    }
    if (ticket.sample) {
        try { finishSample(ticket, before, snapshot(ticket.camera)); }
        catch (...) { observer->failed = true; }
    }
}

} // namespace

void installNativeCameraObserver(void* module) noexcept {
    render::installNativeDofProbe(module);
    installNativeFramingObserver(module);
    try {
        if (originalRecalculate) return; // Installed options never change mid-process.
        wchar_t setting[4] {};
        const bool observe = diagnostics::optionalOutputEnabled() &&
            GetEnvironmentVariableW(L"BONE_EATER_NATIVE_CAMERA_OBSERVE", setting, 4) == 1 && setting[0] == L'1';
        setting[0] = 0;
        preserveVertical = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_VERTICAL_FOV", setting, 4) == 1 && setting[0] == L'1';
        if (preserveVertical) {
            char zoomSetting[64] {};
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetEnvironmentVariableA("BONE_EATER_NATIVE_FOV_ZOOM", zoomSetting,
                static_cast<DWORD>(sizeof(zoomSetting)));
            const bool absent = !length && GetLastError() == ERROR_ENVVAR_NOT_FOUND;
            const auto parsed = absent ? std::optional<float>(1.0f) :
                (length && length < sizeof(zoomSetting) ?
                    parseMainFovZoom(std::string_view(zoomSetting, length)) : std::nullopt);
            if (!parsed) {
                log_warning("bone-eater", "Native vertical-FOV override rejected: BONE_EATER_NATIVE_FOV_ZOOM must be a finite decimal from1.0 through1.5; this launch retains native horizontal framing");
                preserveVertical = false;
            } else mainViewZoom = *parsed;
        }
        if (!observe && !preserveVertical) return;
        const char* status = render::verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native camera observer disabled: {}", status);
            return;
        }
        gameBase = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> expected {{
            0x40,0x53,0x55,0x56,0x57,0x48,0x81,0xec,0x18,0x01,0x00,0x00,0x48,0x8b,0x05,0xc5,
            0xf1,0x05,0x01,0x48,0x33,0xc4,0x48,0x89,0x44,0x24,0x70,0x48,0x8b,0x01,0x48,0x8b
        }};
        std::array<unsigned char, 32> found {};
        if (!read(gameBase, 0x25DEF0, found) || found != expected) {
            log_warning("bone-eater", "Native camera observer disabled: instruction bytes differ");
            return;
        }
        if (preserveVertical) {
            constexpr std::array<unsigned char, 32> setterBytes {{
                0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x49,
                0x8b,0xf8,0x48,0x8b,0xf2,0x48,0x8b,0xd9,0xe8,0xb3,0xc5,0x10,0x00,0x84,0xc0,0x75
            }};
            constexpr std::array<unsigned char, 21> branchBytes {{
                0x48,0x8b,0x83,0xf0,0x0d,0x00,0x00,0xf3,0x0f,0x10,0x07,0xff,0x40,0x70,
                0xf3,0x0f,0x11,0x40,0x18,0xb0,0x01
            }};
            std::array<unsigned char, 21> branchFound {};
            if (!read(gameBase, 0x2616D0, found) || found != setterBytes ||
                    !read(gameBase, 0x26182C, branchFound) || branchFound != branchBytes) {
                log_warning("bone-eater", "Native vertical-FOV experiment disabled: setter bytes differ");
                preserveVertical = false;
            } else setProperty = reinterpret_cast<SetProperty>(gameBase + 0x2616D0);
        }
        // The installed detour and original trampoline remain process-lifetime.
        // Pin the checked identity so an unload/reused base cannot invalidate them.
        HMODULE pinnedModule = nullptr;
        constexpr DWORD pinFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        if (!GetModuleHandleExW(pinFlags, reinterpret_cast<LPCWSTR>(module), &pinnedModule) ||
                pinnedModule != module) {
            log_warning("bone-eater", "Native camera observer disabled: module lifetime could not be pinned");
            return;
        }
        if (!observer) observer = new Observer;
        observer->output = diagnostics::openOptionalCsv(L"desktop/camera-hook-v2.csv", [](FILE* file) {
            std::fputs("schema,pid,sequence,elapsed_ms,thread,stage,camera,caller,caller_rva,call_count,status,"
                "camera_vtable,parameter,parameter_vtable,parameter_refs,parameter_generation,consumed_generation,slot,flags,"
                "ordinary_zoom,system_zoom,focal,film_width,aspect,viewport_ratio,width,height,p00,p11,adjusted_p00,adjusted_p11,"
                "active_valid,active_wrapper,active_vtable,active_target,active_owned,active_target_parameter,active_owned_parameter,"
                "scope_valid,scope_wrapper,scope_vtable,scope_target,scope_owned,scope_target_parameter,scope_owned_parameter,"
                "manager,manager_camera,busy_drops,key_overflow,fov_requested,fov_applied,baseline_zoom,temporary_zoom,fov_failed,fov_restored,fov_unrecovered\n", file);
        }, L"a");
        if (diagnostics::optionalOutputEnabled() && !observer->output) {
            try { log_warning("bone-eater", "Camera CSV unavailable; functional camera hook will still be installed"); }
            catch (...) {}
        }
        observer->start = observer->budgetEpoch = GetTickCount64();
        // Upstream captures *orig as MH_EnableHook's target before CreateHook.
        // A null value would mean MH_ALL_HOOKS and enable unrelated pending hooks.
        originalRecalculate = reinterpret_cast<Recalculate>(gameBase + 0x25DEF0);
        if (!detour::trampoline_try(originalRecalculate,
                &recalculate, &originalRecalculate)) {
            log_warning("bone-eater", "Native camera observer hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native camera hook installed; vertical FOV experiment={}, main view zoom={:.6g}, sampling cap {}/s",
            preserveVertical, mainViewZoom, kSamplesPerSecond);
    } catch (...) {
        // Optional diagnostic failure must not prevent baseline boot.
    }
}

} // namespace bone_eater::camera
