#include "render/native_dof_experiment.h"
#include "render/native_dof_decision.h"
#include "render/native_viewport.h"
#include "diagnostics/output_policy.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

extern "C" {
__declspec(thread) bone_eater::render::DofDecision bone_eater_dof_decision {};
void* bone_eater_dof_setup_resume = nullptr;
void* bone_eater_dof_combine_resume = nullptr;
}

namespace bone_eater::render {
namespace {
std::uintptr_t base = 0;
std::atomic<bool> enabled {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<bool> failed {false};
struct Identity {
    std::uintptr_t bloom = 0, combiner = 0, processor = 0, descriptor = 0;
    std::uintptr_t camera = 0, parameter = 0, wrapper = 0, manager = 0;
    std::uintptr_t scope = 0, scopeTarget = 0, scopeOwned = 0, scopeParameter = 0, scopeOwnedParameter = 0;
    std::uint32_t refs = 0;
    std::uint8_t opticalFlags = 0;
    bool operator==(const Identity&) const = default;
};
struct Stats {
    std::uint64_t eligible = 0, completed = 0, setup = 0, combine = 0;
    std::uint64_t setupChanged = 0, combineChanged = 0, rejected = 0, anomalies = 0;
    ULONGLONG nextLog = 0;
    Identity identity;
};
thread_local Stats stats;
template<typename T>
bool read(std::uintptr_t p, std::size_t offset, T& out) noexcept {
    if (!p || p > std::numeric_limits<std::uintptr_t>::max() - offset ||
            p + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(p + offset), &out, sizeof(T), &copied) && copied == sizeof(T);
}
bool cameraParameter(std::uintptr_t camera, std::uintptr_t& parameter) noexcept {
    std::uintptr_t vt = 0;
    return read(camera, 0, vt) && vt == base + 0x6FDF48 && read(camera, 0xDF0, parameter) &&
        read(parameter, 0, vt) && vt == base + 0x6FDEE8;
}
bool inspect(Identity& s) noexcept {
    std::uintptr_t vt = 0, activeCamera = 0, managerCamera = 0;
    std::array<float, 2> dimensions {};
    std::uint8_t slot = 0;
    return read(s.bloom, 0, vt) && vt == base + 0x71B388 &&
        read(s.bloom, 0x690, s.combiner) && read(s.combiner, 0x5B0, s.processor) && s.processor == s.bloom &&
        read(s.descriptor, 0x10, s.camera) && cameraParameter(s.camera, s.parameter) &&
        read(base, 0x13DB588, s.wrapper) && read(s.wrapper, 0, vt) && vt == base + 0x10C4B60 &&
        read(s.wrapper, 8, activeCamera) && activeCamera == s.camera &&
        read(base, 0x12E1930, s.manager) && read(s.manager, 0x340, managerCamera) && managerCamera == s.camera &&
        read(s.camera, 0xE8E, slot) && slot == 0 && read(s.camera, 0xEB4, dimensions) &&
        dimensions[0] == 1920 && dimensions[1] == 1080 &&
        read(s.parameter, 8, s.refs) && s.refs == 1 && read(s.parameter, 0x6C, s.opticalFlags) &&
        read(base, 0x13DCF88, s.scope) && read(s.scope, 0, vt) && vt == base + 0x10C7350 &&
        read(s.scope, 8, s.scopeTarget) && cameraParameter(s.scopeTarget, s.scopeParameter) &&
        read(s.scope, 0x60, s.scopeOwned) && cameraParameter(s.scopeOwned, s.scopeOwnedParameter) &&
        s.camera != s.scopeTarget && s.camera != s.scopeOwned &&
        s.parameter != s.scopeParameter && s.parameter != s.scopeOwnedParameter;
}
void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64();
    if (now < stats.nextLog) return;
    stats.nextLog = now + 5000;
    try {
        log_info("bone-eater", "Main DOF decision experiment pid={} thread={} eligible={} completed={} setup={} combine={} setup_changed={} combine_changed={} rejected={} anomalies={} failed={}",
            GetCurrentProcessId(), GetCurrentThreadId(), stats.eligible, stats.completed, stats.setup, stats.combine,
            stats.setupChanged, stats.combineChanged, stats.rejected, stats.anomalies, failed.load());
    } catch (...) {}
}
template<std::size_t N>
bool bytes(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(base, rva, actual) && actual == expected;
}
}

void beginNativeDofExperiment(void* bloom, void* descriptor, std::uintptr_t caller) noexcept {
    bone_eater_dof_decision = {};
    if (!enabled.load(std::memory_order_acquire) || failed.load(std::memory_order_relaxed)) return;
    if (caller != base + 0x34E88D) return; // Measured main dispatch only; the auxiliary loop is34E927.
    const auto thread = GetCurrentThreadId();
    const auto owner = ownerThread.load(std::memory_order_acquire);
    if (owner && owner != thread) return;
    Identity first, repeated;
    first.bloom = repeated.bloom = reinterpret_cast<std::uintptr_t>(bloom);
    first.descriptor = repeated.descriptor = reinterpret_cast<std::uintptr_t>(descriptor);
    if (!inspect(first) || !inspect(repeated) || !(first == repeated)) { ++stats.rejected; report(); return; }
    DWORD expected = 0;
    ownerThread.compare_exchange_strong(expected, thread);
    if (ownerThread.load(std::memory_order_acquire) != thread) return;
    stats.identity = first;
    ++stats.eligible;
    bone_eater_dof_decision.camera = first.camera;
    bone_eater_dof_decision.parameter = first.parameter;
    bone_eater_dof_decision.processor = first.processor;
    bone_eater_dof_decision.combiner = first.combiner;
    bone_eater_dof_decision.active = 1; // Published last, read only on this same native thread.
}

void endNativeDofExperiment(bool nativeCompleted) noexcept {
    const auto decision = bone_eater_dof_decision;
    bone_eater_dof_decision = {}; // Unconditional cleanup BEFORE reads, validation or logging.
    if (!decision.active) return;
    stats.setup += decision.setupVisits; stats.combine += decision.combineVisits;
    stats.setupChanged += decision.setupChanged; stats.combineChanged += decision.combineChanged;
    if (nativeCompleted) ++stats.completed;
    Identity after;
    after.bloom = stats.identity.bloom; after.descriptor = stats.identity.descriptor;
    // Nothing requires restoring: only private register decisions were masked.
    // Stop future masking on an unexpected native path or ownership transition.
    if (!nativeCompleted || decision.setupVisits != 1 || decision.combineVisits != 1 ||
            !inspect(after) || !(after == stats.identity)) {
        ++stats.anomalies;
        if (!failed.exchange(true, std::memory_order_acq_rel)) {
            try { log_warning("bone-eater", "Main DOF decision experiment disabled after native-path or ownership invariant failure: completed={} setup_visits={} combine_visits={}",
                nativeCompleted, decision.setupVisits, decision.combineVisits); } catch (...) {}
        }
    }
    report();
}

std::uint64_t suspendNativeDofExperiment() noexcept {
    const auto active = bone_eater_dof_decision.active;
    bone_eater_dof_decision.active = 0;
    return active;
}
void resumeNativeDofExperiment(std::uint64_t active) noexcept {
    bone_eater_dof_decision.active = active;
}

void installNativeDofExperiment(void* module) noexcept {
    wchar_t value[4] {};
    if (GetEnvironmentVariableW(L"BONE_EATER_DOF_MAIN_OFF", value, 4) != 1 || value[0] != L'1' ||
            bone_eater_dof_setup_resume) return;
    try {
        if (std::strcmp(verifyNativeGameModule(module), "verified")) return;
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 12> setup {{
            0x45,0x0f,0xb6,0x6c,0x24,0x6c,0x45,0x0f,0xb6,0x74,0x24,0x6c}};
        constexpr std::array<unsigned char, 12> combine {{
            0x44,0x0f,0xb6,0x60,0x6c,0x48,0x8b,0x81,0xf8,0x0d,0x00,0x00}};
        if (!bytes(0x35AF8F, setup) || !bytes(0x355B7A, combine)) {
            log_warning("bone-eater", "Main DOF decision experiment rejected native site bytes"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        // Each stub remains transparent until BOTH hooks are installed and a
        // verified callback publishes a same-thread certificate. Partial install
        // leaves installed stubs as exact pass-throughs to their trampolines.
        bone_eater_dof_setup_resume = reinterpret_cast<void*>(base + 0x35AF95);
        if (!detour::trampoline_try(reinterpret_cast<void*>(base + 0x35AF95),
                reinterpret_cast<void*>(&bone_eater_dof_setup_stub), &bone_eater_dof_setup_resume)) return;
        bone_eater_dof_combine_resume = reinterpret_cast<void*>(base + 0x355B7F);
        if (!detour::trampoline_try(reinterpret_cast<void*>(base + 0x355B7F),
                reinterpret_cast<void*>(&bone_eater_dof_combine_stub), &bone_eater_dof_combine_resume)) return;
        enabled.store(true, std::memory_order_release);
        log_warning("bone-eater", "Experimental main-only DOF disabled by preparation-local register decisions; authored/scope optical state is never written");
    } catch (...) { enabled.store(false, std::memory_order_release); }
}
}
