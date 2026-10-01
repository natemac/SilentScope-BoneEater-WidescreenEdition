#include "render/native_dof_probe.h"
#include "diagnostics/optional_csv.h"
#include "render/native_viewport.h"
#include "render/native_dof_experiment.h"

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
#include <cstring>
#include <limits>
#include <mutex>

namespace bone_eater::render {
namespace {
using Prepare = bool(__fastcall*)(void*, void*);
Prepare originalPrepare = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false};
std::atomic<DWORD> ownerThread {0};
constexpr std::uintptr_t kPrepare = 0x35E800;
constexpr std::uintptr_t kBloomVtable = 0x71B388;
constexpr std::uintptr_t kCameraVtable = 0x6FDF48;
constexpr std::uintptr_t kParameterVtable = 0x6FDEE8;

template<typename T>
bool read(std::uintptr_t object, std::size_t offset, T& result) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &result, sizeof(T), &got) && got == sizeof(T);
}

struct Anchor {
    std::uintptr_t wrapper = 0, vtable = 0, target = 0, owned = 0;
    std::uintptr_t targetParameter = 0, ownedParameter = 0;
    bool valid = false;
    bool operator==(const Anchor&) const = default;
};
bool cameraParameter(std::uintptr_t camera, std::uintptr_t& parameter) noexcept {
    std::uintptr_t vt = 0;
    return read(camera, 0, vt) && vt == base + kCameraVtable &&
        read(camera, 0xDF0, parameter) && read(parameter, 0, vt) && vt == base + kParameterVtable;
}
Anchor anchor(std::uintptr_t global) noexcept {
    Anchor a;
    if (!read(base, global, a.wrapper)) return a;
    if (!a.wrapper) { a.valid = true; return a; }
    if (!read(a.wrapper, 0, a.vtable)) return a;
    const auto vt = a.vtable - base;
    const bool derived = vt == 0x10C4B60 || vt == 0x10C5D70 || vt == 0x10C7350 ||
        vt == 0x10C73D0 || vt == 0x10C6200;
    if ((!derived && vt != 0x10CB750) || !read(a.wrapper, 8, a.target) ||
            !cameraParameter(a.target, a.targetParameter)) return a;
    if (derived && (!read(a.wrapper, 0x60, a.owned) ||
            (a.owned && !cameraParameter(a.owned, a.ownedParameter)))) return a;
    a.valid = true;
    return a;
}

struct Snapshot {
    const char* status = "unresolved";
    const char* role = "unmatched";
    std::uintptr_t object = 0, descriptor = 0, combiner = 0, processor = 0;
    std::uintptr_t camera = 0, parameter = 0, manager = 0, managerCamera = 0;
    Anchor main, scope;
    std::array<unsigned char, 0x74> optical {};
    std::array<float, 6> projection {}; // P00,P11,depth coefficients968/96C,near,far.
    std::array<float, 2> dimensions {};
    std::array<float, 8> coefficients {}; // Native DOF block2020/24,2030..44.
    std::array<float, 16> cache {}; // Native cached inputs2208..2244.
    std::array<unsigned char, 10> cacheFlags {}; //2274..227D.
    unsigned char slot = 0;
    bool exclusiveMain = false;
};
template<typename T>
T field(const Snapshot& s, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value, s.optical.data() + offset, sizeof(value));
    return value;
}
bool sameIdentity(const Snapshot& a, const Snapshot& b) noexcept {
    return a.object == b.object && a.descriptor == b.descriptor && a.combiner == b.combiner &&
        a.processor == b.processor && a.camera == b.camera && a.parameter == b.parameter &&
        a.main == b.main && a.scope == b.scope && a.manager == b.manager &&
        a.managerCamera == b.managerCamera;
}
bool identities(Snapshot& s) noexcept {
    std::uintptr_t vt = 0;
    return read(s.object, 0, vt) && vt == base + kBloomVtable &&
        read(s.object, 0x690, s.combiner) && read(s.combiner, 0x5B0, s.processor) && s.processor &&
        read(s.descriptor, 0x10, s.camera) && cameraParameter(s.camera, s.parameter) &&
        read(base, 0x12E1930, s.manager) && read(s.manager, 0x340, s.managerCamera);
}
Snapshot snapshot(std::uintptr_t object, std::uintptr_t descriptor) noexcept {
    Snapshot s;
    s.object = object; s.descriptor = descriptor;
    if (!identities(s)) { s.status = "native_chain_unresolved"; return s; }
    s.main = anchor(0x13DB588);
    s.scope = anchor(0x13DCF88);
    if (!s.main.valid || !s.scope.valid) { s.status = "camera_anchors_unresolved"; return s; }
    if (s.main.wrapper && s.camera == s.main.target) s.role = "main";
    else if (s.scope.wrapper && (s.camera == s.scope.target || s.camera == s.scope.owned)) s.role = "scope";
    else { s.status = "unmatched_camera"; return s; }
    constexpr std::array<std::size_t, 6> projectionOffsets {{0x940,0x954,0x968,0x96C,0xDE0,0xDE4}};
    constexpr std::array<std::size_t, 8> coefficientOffsets {{0x2020,0x2024,0x2030,0x2034,0x2038,0x203C,0x2040,0x2044}};
    if (!read(s.parameter, 0, s.optical) || !read(s.camera, 0xEB4, s.dimensions) ||
            !read(s.camera, 0xE8E, s.slot) || !read(s.processor, 0x2208, s.cache) ||
            !read(s.processor, 0x2274, s.cacheFlags)) { s.status = "fields_unreadable"; return s; }
    for (std::size_t i = 0; i < projectionOffsets.size(); ++i)
        if (!read(s.camera, projectionOffsets[i], s.projection[i])) { s.status = "projection_unreadable"; return s; }
    for (std::size_t i = 0; i < coefficientOffsets.size(); ++i)
        if (!read(s.processor, coefficientOffsets[i], s.coefficients[i])) { s.status = "coefficients_unreadable"; return s; }
    Snapshot repeated;
    repeated.object = object; repeated.descriptor = descriptor;
    repeated.main = anchor(0x13DB588); repeated.scope = anchor(0x13DCF88);
    std::array<unsigned char, 0x74> opticalAgain {};
    if (!identities(repeated) || !sameIdentity(s, repeated) ||
            !read(s.parameter, 0, opticalAgain) || opticalAgain != s.optical) {
        s.status = "identity_or_optics_changed"; return s;
    }
    constexpr std::array<std::size_t, 10> floatOffsets {{0x10,0x14,0x18,0x20,0x24,0x28,0x2C,0x30,0x44,0x64}};
    for (const auto offset : floatOffsets) if (!std::isfinite(field<float>(s, offset))) {
        s.status = "nonfinite_optics"; return s;
    }
    for (const auto v : s.projection) if (!std::isfinite(v)) { s.status = "nonfinite_projection"; return s; }
    for (const auto v : s.dimensions) if (!std::isfinite(v) || v <= 0 || v > 32768) {
        s.status = "invalid_dimensions"; return s;
    }
    // This is evidence for a possible later main-only experiment, NOT permission
    // to mutate here. Shared scope optics are always observed without alteration.
    s.exclusiveMain = s.camera == s.main.target && s.camera == s.managerCamera && s.slot == 0 &&
        field<std::uint32_t>(s, 8) == 1 && s.scope.wrapper &&
        s.camera != s.scope.target && s.camera != s.scope.owned &&
        s.parameter != s.scope.targetParameter && s.parameter != s.scope.ownedParameter;
    s.status = "resolved";
    return s;
}

struct Key { std::uintptr_t object = 0, camera = 0; ULONGLONG next = 0; };
struct ThreadState { bool inside = false; std::array<Key, 16> keys {}; };
thread_local ThreadState threadState;
struct Output {
    std::mutex mutex;
    FILE* file = nullptr;
    std::atomic<bool> failed {false};
    std::atomic<ULONGLONG> nextBudget {0};
    std::uint64_t sequence = 0;
};
Output* output = nullptr; // Process lifetime: no teardown against an installed hook.
struct Ticket {
    bool sample = false;
    ULONGLONG tick = 0;
    std::uintptr_t caller = 0;
    Snapshot before;
};

std::uintptr_t descriptorCamera(void* descriptor) noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(descriptor);
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - 0x18) return 0;
    __try { return *reinterpret_cast<volatile std::uintptr_t*>(address + 0x10); }
    __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return 0; }
}
Ticket begin(void* object, void* descriptor, std::uintptr_t caller) noexcept {
    Ticket t;
    if (!output || !output->file || output->failed.load(std::memory_order_relaxed)) return t;
    const DWORD owner = ownerThread.load(std::memory_order_acquire);
    if (owner && owner != GetCurrentThreadId()) return t;
    const auto camera = descriptorCamera(descriptor);
    if (!camera) return t;
    Key* key = nullptr;
    for (auto& k : threadState.keys) if (!k.object ||
            (k.object == reinterpret_cast<std::uintptr_t>(object) && k.camera == camera)) { key = &k; break; }
    if (!key) return t; // Fixed storage; new identities beyond16 fail closed.
    const auto now = GetTickCount64();
    if (now < key->next) return t;
    auto budget = output->nextBudget.load(std::memory_order_relaxed);
    if (now < budget || !output->nextBudget.compare_exchange_strong(budget, now + 125)) return t;
    // Budget rejection must not consume this key's interval: otherwise main,
    // which is typically encountered first, could permanently starve scope.
    key->object = reinterpret_cast<std::uintptr_t>(object); key->camera = camera; key->next = now + 1000;
    t.before = snapshot(key->object, reinterpret_cast<std::uintptr_t>(descriptor));
    if (!std::strcmp(t.before.status, "resolved")) {
        DWORD expected = 0;
        ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
        if (ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId()) return t;
    }
    t.sample = true; t.tick = now; t.caller = caller;
    return t;
}

void row(const Ticket& t, const Snapshot& s, const char* phase, bool nativeResult, bool same) {
    auto* f = output->file;
    std::fprintf(f, "1,%lu,%llu,%llu,%lu,%s,%u,%u,%s,%s,%u,%p,0x%llx,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%u,%u,%u,%u",
        GetCurrentProcessId(), output->sequence, t.tick, GetCurrentThreadId(), phase,
        nativeResult ? 1u : 0u, same ? 1u : 0u, s.status, s.role, s.exclusiveMain ? 1u : 0u,
        reinterpret_cast<void*>(t.caller), static_cast<unsigned long long>(t.caller >= base ? t.caller-base : 0),
        reinterpret_cast<void*>(s.object), reinterpret_cast<void*>(s.descriptor), reinterpret_cast<void*>(s.combiner),
        reinterpret_cast<void*>(s.processor), reinterpret_cast<void*>(s.camera), reinterpret_cast<void*>(s.parameter),
        reinterpret_cast<void*>(s.main.target), reinterpret_cast<void*>(s.scope.target),
        reinterpret_cast<void*>(s.scope.owned), reinterpret_cast<void*>(s.managerCamera),
        field<std::uint32_t>(s, 8), field<std::uint32_t>(s, 0x70), unsigned(s.optical[0x6C]), unsigned(s.slot));
    std::fprintf(f, ",%u", field<std::uint32_t>(s, 0x1C));
    constexpr std::array<std::size_t, 10> opticalOffsets {{0x10,0x14,0x18,0x20,0x24,0x28,0x2C,0x30,0x44,0x64}};
    for (const auto offset : opticalOffsets) std::fprintf(f, ",%.9g", field<float>(s, offset));
    for (const auto v : s.dimensions) std::fprintf(f, ",%.9g", v);
    for (const auto v : s.projection) std::fprintf(f, ",%.9g", v);
    for (const auto v : s.coefficients) std::fprintf(f, ",%.9g", v);
    for (const auto v : s.cache) std::fprintf(f, ",%.9g", v);
    for (const auto v : s.cacheFlags) std::fprintf(f, ",%u", unsigned(v));
    std::fputc('\n', f);
}
void finish(const Ticket& t, bool nativeResult) noexcept {
    if (!t.sample) return;
    try {
        const auto after = snapshot(t.before.object, t.before.descriptor);
        std::unique_lock<std::mutex> lock(output->mutex, std::try_to_lock);
        if (!lock || output->failed) return;
        ++output->sequence;
        const bool same = sameIdentity(t.before, after);
        row(t, t.before, "before", false, same);
        row(t, after, "after", nativeResult, same);
        if (std::fflush(output->file) || std::ferror(output->file)) output->failed = true;
    } catch (...) { output->failed = true; }
}

__declspec(noinline) bool __fastcall prepareHook(void* object, void* descriptor) {
    if (!enabled.load(std::memory_order_acquire)) return originalPrepare(object, descriptor);
    if (threadState.inside) {
        const auto active = suspendNativeDofExperiment();
        bool nestedResult = false;
        __try { nestedResult = originalPrepare(object, descriptor); }
        __finally { resumeNativeDofExperiment(active); }
        return nestedResult;
    }
    threadState.inside = true;
    Ticket ticket;
    bool result = false;
    bool nativeCompleted = false;
    __try {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        ticket = begin(object, descriptor, caller);
        beginNativeDofExperiment(object, descriptor, caller);
        result = originalPrepare(object, descriptor); // Exactly once, unchanged arguments and native AL result.
        nativeCompleted = true;
        finish(ticket, result);
    } __finally {
        endNativeDofExperiment(nativeCompleted);
        threadState.inside = false;
    }
    return result;
}
template<std::size_t N>
bool bytes(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(base, rva, actual) && actual == expected;
}
} // namespace

void installNativeDofProbe(void* module) noexcept {
    wchar_t setting[4] {};
    const bool observe = diagnostics::optionalOutputEnabled() &&
        GetEnvironmentVariableW(L"BONE_EATER_DOF_OBSERVE", setting, 4) == 1 && setting[0] == L'1';
    setting[0] = 0;
    const bool experiment = GetEnvironmentVariableW(L"BONE_EATER_DOF_MAIN_OFF", setting, 4) == 1 && setting[0] == L'1';
    if ((!observe && !experiment) || originalPrepare) return;
    try {
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) { log_warning("bone-eater", "DOF probe rejected module: {}", status); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 23> entry {{
            0x48,0x83,0xec,0x28,0x48,0x8b,0x89,0x90,0x06,0x00,0x00,0xe8,0x60,0x94,0xff,0xff,0xb0,0x01,0x48,0x83,0xc4,0x28,0xc3}};
        constexpr std::array<unsigned char, 62> chain {{
            0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0x48,0x8b,0x89,
            0xb0,0x05,0x00,0x00,0x48,0x8b,0xfa,0xb0,0x01,0x48,0x85,0xc9,0x74,0x07,0xe8,0x0d,
            0x1e,0x00,0x00,0x0c,0x01,0x44,0x0f,0xb6,0xc0,0x48,0x8b,0xd7,0x48,0x8b,0xcb,0x48,
            0x8b,0x5c,0x24,0x30,0x48,0x83,0xc4,0x20,0x5f,0xe9,0xc2,0xda,0xff,0xff}};
        std::uintptr_t virtualEntry = 0;
        if (!bytes(kPrepare, entry) || !bytes(0x357C70, chain) ||
                !read(base, kBloomVtable + 0x250, virtualEntry) || virtualEntry != base + kPrepare) {
            log_warning("bone-eater", "DOF probe rejected native instructions or callback vtable"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        output = new Output;
        output->file = diagnostics::openOptionalCsv(L"desktop/dof-probe-v1.csv", [](FILE* file) {
            std::fputs("schema,pid,sequence,tick_ms,thread,phase,native_result,pair_identity_equal,status,role,exclusive_main,caller,caller_rva,"
                "bloom,descriptor,combiner,processor,camera,parameter,main_target,scope_target,scope_owned,manager_camera,refs,generation,optical_flags,slot,lens_kind,"
                "fov,ordinary_zoom,system_zoom,lens_setting,focal,effective_focal,focus_depth,fstop,film_width,param64,width,height,p00,p11,depth_968,depth_96c,near_clip,far_clip", file);
            for (const auto offset : {0x2020,0x2024,0x2030,0x2034,0x2038,0x203C,0x2040,0x2044}) std::fprintf(file, ",processor_%x", offset);
            for (unsigned offset = 0x2208; offset <= 0x2244; offset += 4) std::fprintf(file, ",cache_%x", offset);
            for (unsigned offset = 0x2274; offset <= 0x227D; ++offset) std::fprintf(file, ",flags_%x", offset);
            std::fputc('\n', file);
        }, L"a");
        if (diagnostics::optionalOutputEnabled() && !output->file) {
            try { log_warning("bone-eater", "DOF CSV unavailable; requested prepare hook and experiment remain enabled"); }
            catch (...) {}
        }
        originalPrepare = reinterpret_cast<Prepare>(base + kPrepare); // Seed upstream trampoline_try's enable target.
        if (!detour::trampoline_try(originalPrepare, &prepareHook, &originalPrepare)) return;
        enabled.store(true, std::memory_order_release);
        installNativeDofExperiment(module);
        log_info("bone-eater", "DOF prepare hook installed; optional CSV available={}, requested main-only experiment={}",
            output->file != nullptr, experiment);
    } catch (...) { /* Optional observer failure leaves native rendering unchanged. */ }
}
} // namespace bone_eater::render
