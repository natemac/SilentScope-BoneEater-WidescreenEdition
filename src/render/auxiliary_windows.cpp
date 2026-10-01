#include "render/auxiliary_windows.h"
#include "render/auxiliary_retry_policy.h"
#include "render/d3d11_diagnostics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include "util/logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>

namespace bone_eater::render {
namespace {

constexpr std::array<const char*, 2> roles {{"multidisplay0", "multidisplay1"}};
constexpr std::array<const wchar_t*, 2> titles {{
    L"Aska MultiDisplay[0](multipssID:33)", L"Aska MultiDisplay[1](multipssID:34)"}};
constexpr std::array<UINT, 2> widths {{800, 768}}, heights {{480, 1366}};
std::atomic<bool> restoreRequested {false};

bool enabled() noexcept {
    static const bool value = [] {
        wchar_t text[32] {};
        const DWORD length = GetEnvironmentVariableW(L"BONE_EATER_AUXILIARY_WINDOWS",
            text, static_cast<DWORD>(std::size(text)));
        return length && length < std::size(text) && _wcsicmp(text, L"offscreen") == 0;
    }();
    return value;
}

struct Output {
    HWND window = nullptr;
    DWORD thread = 0;
    ULONG_PTR classAtom = 0;
    std::uint64_t generation = 0, first = 0, last = 0, calls = 0;
    std::uint64_t parkedAt = 0, rateAt = 0, rateCalls = 0;
    std::uint64_t rateMainCalls = 0, mainRateGap = 0, auxiliaryRateGap = 0, gapSerial = 0, retryGapSerial = 0;
    double baselineHz = 0, retryBaselineHz = 0, retryMainHz = 0;
    AuxiliaryVisibleCadence visibleCadence;
    RECT saved {}, parked {};
    bool managed = false, restoring = false, everParked = false, sharedPause = false;
};

struct State {
    std::mutex mutex;
    std::array<Output, 2> outputs;
    HWND main = nullptr;
    std::uint64_t mainLast = 0, mainCalls = 0, mainGapSerial = 0, nextGeneration = 0, rollbackAt = 0;
    std::uint64_t retryAuthorizedAt = 0, retryMainGapSerial = 0;
    bool everParked = false;
    AuxiliaryRetryBudget retry;
    HANDLE timer = nullptr;
    std::atomic_flag workerRunning = ATOMIC_FLAG_INIT;
};

// Timer storage stays allocated until process termination. An atexit callback
// drains the timer before earlier-created registry/logging objects are destroyed.
State*& stateStorage() noexcept {
    static State* value = nullptr;
    return value;
}
std::mutex& creationMutex() { static auto* value = new std::mutex; return *value; }

bool ours(HWND window, DWORD* thread = nullptr) noexcept {
    DWORD process = 0;
    const DWORD owner = window ? GetWindowThreadProcessId(window, &process) : 0;
    if (thread) *thread = owner;
    return owner && process == GetCurrentProcessId();
}

bool identity(const Output& output, std::size_t slot, bool checkTitle) noexcept {
    DWORD owner = 0;
    if (!ours(output.window, &owner) || owner != output.thread ||
            static_cast<ULONG_PTR>(GetClassLongPtrW(output.window, GCW_ATOM)) != output.classAtom ||
            std::strcmp(registeredD3D11WindowRole(output.window), roles[slot]) != 0 ||
            GetAncestor(output.window, GA_ROOT) != output.window) return false;
    if (!checkTitle) return true;
    wchar_t title[96] {};
    DWORD_PTR copied = 0;
    return SendMessageTimeoutW(output.window, WM_GETTEXT, std::size(title),
        reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
        20, &copied) && std::wcscmp(title, titles[slot]) == 0;
}

bool sameRect(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool parkingRect(const RECT& saved, RECT& parked) noexcept {
    const std::int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const std::int64_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const std::int64_t width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const std::int64_t height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const std::int64_t w = static_cast<std::int64_t>(saved.right) - saved.left;
    const std::int64_t h = static_cast<std::int64_t>(saved.bottom) - saved.top;
    const std::int64_t x = left + width + 128;
    if (width <= 0 || height <= 0 || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
            x < std::numeric_limits<LONG>::min() || x + w > std::numeric_limits<LONG>::max() ||
            top + h > std::numeric_limits<LONG>::max()) return false;
    parked = {static_cast<LONG>(x), static_cast<LONG>(top),
        static_cast<LONG>(x + w), static_cast<LONG>(top + h)};
    return true;
}

void disable(State& state, const char* reason) noexcept {
    bool report = false;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        report = state.retry.permanentFailure();
        for (auto& output : state.outputs) {
            if (output.managed) output.restoring = true;
        }
    }
    if (report) {
        try { log_warning("bone-eater", "Auxiliary offscreen experiment restoring and permanently disabled: {}", reason); }
        catch (...) {}
    }
}

void cadenceRollback(State& state, const Output& output, std::size_t slot, std::uint64_t now,
                  std::uint64_t mainCalls, std::uint64_t mainLast, const char* reason) noexcept {
    bool retryEligible = false;
    unsigned retries = 0;
    std::uint64_t mainGapSerial = 0, auxiliaryGapSerial = 0;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        retryEligible = state.retry.cadenceFailure();
        retries = state.retry.retries();
        mainGapSerial = state.mainGapSerial;
        auxiliaryGapSerial = state.outputs[slot].gapSerial;
        state.rollbackAt = GetTickCount64();
        for (auto& current : state.outputs) {
            if (current.managed) current.restoring = true;
            current.visibleCadence.reset();
            current.retryBaselineHz = current.retryMainHz = 0;
        }
    }
    const auto elapsed = std::max<std::uint64_t>(1, now - output.rateAt);
    try {
        log_warning("bone-eater", "Auxiliary {} cadence rollback ({}): main {:.3f}/s, auxiliary {:.3f}/s over {} ms; "
            "threshold {:.3f}/s, main/auxiliary max gaps {} / {} ms, ages {} / {} ms, retries {}, "
            "gap serials {} / {}, one-time stable-visible-cadence retry eligible {}",
            roles[slot], reason, (mainCalls - output.rateMainCalls) * 1000.0 / elapsed,
            (output.calls - output.rateCalls) * 1000.0 / elapsed, elapsed,
            std::max(5.0, output.baselineHz * 0.5), output.mainRateGap, output.auxiliaryRateGap,
            now - mainLast, now - output.last, retries, mainGapSerial, auxiliaryGapSerial, retryEligible);
    } catch (...) {}
}

bool moveWithoutActivation(HWND window, const RECT& rectangle) noexcept {
    // Resolve the real export instead of passing our own move back through the
    // game's IAT placement policy. A worker thread avoids same-thread WM_MOVE
    // reentry while any Present hook or game renderer is on the call stack.
    using Move = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    static const auto move = reinterpret_cast<Move>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowPos"));
    return move && move(window, nullptr, rectangle.left, rectangle.top, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
        SWP_ASYNCWINDOWPOS) != FALSE;
}

void serviceRecovery(State& state) {
    using Mode = AuxiliaryRetryBudget::Mode;
    std::array<Output, 2> outputs;
    Mode mode;
    HWND main = nullptr;
    std::uint64_t rollbackAt = 0;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        mode = state.retry.mode();
        if (mode != Mode::Restoring && mode != Mode::Visible) return;
        outputs = state.outputs;
        main = state.main;
        rollbackAt = state.rollbackAt;
    }
    bool anyOutput = false;
    for (const auto& output : outputs) {
        if (!output.window || !output.everParked) continue;
        anyOutput = true;
        if (output.managed || output.restoring) {
            if (GetTickCount64() - rollbackAt > 2000) disable(state, "saved placement restoration timed out");
            return;
        }
    }
    if (!anyOutput) { disable(state, "no original auxiliary identity remains for retry"); return; }
    if (mode == Mode::Restoring) {
        std::lock_guard<std::mutex> guard(state.mutex);
        if (!state.retry.restorationConfirmed()) return;
        for (auto& output : state.outputs) output.visibleCadence.reset();
    }
    const bool foreground = ours(main) && GetForegroundWindow() == main &&
        std::strcmp(registeredD3D11WindowRole(main), "main") == 0 &&
        IsWindowVisible(main) && !IsIconic(main);
    bool allReady = true;
    std::array<std::uint64_t, 2> qualifiedMainGaps {}, qualifiedAuxiliaryGaps {};
    for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
        const auto& output = outputs[slot];
        if (!output.window || !output.everParked) continue;
        RECT actual {}, parked {};
        if (!identity(output, slot, true) || !GetWindowRect(output.window, &actual) ||
                !sameRect(actual, output.saved) || !parkingRect(output.saved, parked) ||
                !sameRect(parked, output.parked) || !IsWindowVisible(output.window) || IsIconic(output.window)) {
            disable(state, "restored auxiliary identity, saved placement, topology or visibility changed");
            return;
        }
        AuxiliaryCadenceResult cadence;
        std::uint64_t mainAge = 0, auxiliaryAge = 0, mainGap = 0, auxiliaryGap = 0;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            if (state.retry.mode() != Mode::Visible) return;
            auto& current = state.outputs[slot];
            if (current.generation != output.generation || current.managed || current.restoring) return;
            const auto now = GetTickCount64();
            mainAge = now - state.mainLast;
            auxiliaryAge = now - current.last;
            // Already restored and visible: staleness invalidates qualification,
            // not the remaining retry. Never infer a loading state from a gap.
            cadence = current.visibleCadence.observe({now, state.mainCalls, current.calls,
                state.mainLast, current.last, state.mainGapSerial, current.gapSerial}, foreground);
            mainGap = current.mainRateGap;
            auxiliaryGap = current.auxiliaryRateGap;
            if (cadence.completedBin || cadence.reset) current.mainRateGap = current.auxiliaryRateGap = 0;
            current.retryBaselineHz = cadence.ready ? current.visibleCadence.auxiliaryBaselineHz() : 0;
            current.retryMainHz = cadence.ready ? current.visibleCadence.mainBaselineHz() : 0;
            qualifiedMainGaps[slot] = state.mainGapSerial;
            qualifiedAuxiliaryGaps[slot] = current.gapSerial;
        }
        allReady = allReady && cadence.ready;
        if (cadence.completedBin || cadence.reset) {
            log_info("bone-eater", "Auxiliary {} visible retry cadence: stable bins {}, main {:.3f}/s, "
                "auxiliary {:.3f}/s over {} ms; max gaps {} / {} ms, ages {} / {} ms, "
                "foreground {}, cooldown reset {}", roles[slot], cadence.stableBins,
                cadence.mainHz, cadence.auxiliaryHz, cadence.elapsed, mainGap, auxiliaryGap,
                mainAge, auxiliaryAge, foreground, cadence.reset);
        }
    }
    if (!allReady || !foreground || GetForegroundWindow() != main) return;
    bool retrying = false;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        // Recheck the identities used by all cadence bins before consuming the
        // single retry. A Present callback may have disabled us during queries.
        const auto now = GetTickCount64();
        bool unchanged = state.main == main && now - state.mainLast <= 100;
        for (std::size_t slot = 0; slot < outputs.size(); ++slot) {
            unchanged = unchanged && state.outputs[slot].generation == outputs[slot].generation &&
                (!outputs[slot].everParked || (state.outputs[slot].retryBaselineHz >= 10.0 &&
                    state.mainGapSerial == qualifiedMainGaps[slot] &&
                    state.outputs[slot].gapSerial == qualifiedAuxiliaryGaps[slot] &&
                    now - state.outputs[slot].last <= 100));
        }
        retrying = state.retry.retry(unchanged);
        if (retrying) {
            state.retryAuthorizedAt = GetTickCount64();
            state.retryMainGapSerial = state.mainGapSerial;
            for (auto& output : state.outputs) output.retryGapSerial = output.gapSerial;
        }
    }
    if (retrying) log_info("bone-eater", "Auxiliary one-time retry authorized by restored placement, "
        "10000 ms visible cooldown and three stable main/auxiliary cadence bins; retry 1 of 1");
}

void service(State& state) {
    serviceRecovery(state);
    for (std::size_t slot = 0; slot < state.outputs.size(); ++slot) {
        Output output;
        HWND main = nullptr;
        std::uint64_t mainLast = 0, mainCalls = 0;
        AuxiliaryRetryBudget::Mode mode;
        unsigned retries = 0;
        bool retryFresh = false;
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            output = state.outputs[slot];
            main = state.main;
            mainLast = state.mainLast;
            mainCalls = state.mainCalls;
            mode = state.retry.mode();
            retries = state.retry.retries();
            retryFresh = GetTickCount64() - state.retryAuthorizedAt <= 350 &&
                state.mainGapSerial == state.retryMainGapSerial && output.gapSerial == output.retryGapSerial;
        }
        const auto now = GetTickCount64();
        if (!output.window) continue;
        if (!identity(output, slot, false)) {
            // Never touch a destroyed/recycled/reclassified HWND, even to undo
            // a previous move. Its saved placement belongs to the old identity.
            disable(state, "auxiliary window identity changed");
            std::lock_guard<std::mutex> guard(state.mutex);
            if (state.outputs[slot].generation == output.generation) state.outputs[slot] = {};
            continue;
        }
        if (output.managed && (mode != AuxiliaryRetryBudget::Mode::Active || output.restoring)) {
            RECT actual {};
            if (!GetWindowRect(output.window, &actual)) continue;
            if (actual.left == output.saved.left && actual.top == output.saved.top) {
                {
                    std::lock_guard<std::mutex> guard(state.mutex);
                    if (state.outputs[slot].generation == output.generation) {
                        state.outputs[slot].managed = false;
                        state.outputs[slot].restoring = false;
                    }
                }
                if (!sameRect(actual, output.saved)) disable(state, "saved auxiliary size changed during restoration");
            } else if (identity(output, slot, true)) {
                if (!moveWithoutActivation(output.window, output.saved)) disable(state, "saved placement restoration request failed");
            }
            continue;
        }
        if (mode != AuxiliaryRetryBudget::Mode::Active) continue;
        if (output.managed) {
            const bool sharedPause = auxiliarySharedPause(now - mainLast, now - output.last);
            if (!sharedPause && (now - output.last > 350 || now - mainLast > 350)) {
                cadenceRollback(state, output, slot, now, mainCalls, mainLast,
                    "main or auxiliary Present cadence stalled");
                continue;
            }
            RECT actual {}, target {};
            if (!GetWindowRect(output.window, &actual) || !parkingRect(output.saved, target) ||
                    !sameRect(target, output.parked) || IsIconic(output.window) ||
                    !IsWindowVisible(output.window)) {
                disable(state, "display topology, visibility or window state changed");
                continue;
            }
            // Async requests need time to reach the window thread; do not fight
            // subsequent user/native relocation after the grace period.
            if (now - output.parkedAt > 1000 && !sameRect(actual, output.parked)) {
                disable(state, "offscreen placement was not retained");
                continue;
            }
            if (sharedPause || output.sharedPause) {
                std::lock_guard<std::mutex> guard(state.mutex);
                auto& current = state.outputs[slot];
                if (current.generation != output.generation) continue;
                current.sharedPause = sharedPause;
                // A recovered shared pause starts a new cadence interval;
                // never compare a deliberately idle loading interval to play.
                current.rateAt = now;
                current.rateCalls = current.calls;
                current.rateMainCalls = state.mainCalls;
                current.mainRateGap = current.auxiliaryRateGap = 0;
                continue;
            }
            if (now - output.rateAt >= 1000) {
                bool rollback = false;
                std::uint64_t rateNow = 0;
                {
                    std::lock_guard<std::mutex> guard(state.mutex);
                    auto& current = state.outputs[slot];
                    if (state.retry.mode() != AuxiliaryRetryBudget::Mode::Active || state.main != main ||
                            current.generation != output.generation || !current.managed || current.restoring) continue;
                    rateNow = GetTickCount64();
                    if (rateNow - current.rateAt < 1000) continue;
                    // Decide and advance from one coherent snapshot. A Present
                    // arriving after unlock belongs to the next interval; its
                    // counter and gap evidence must not be discarded here.
                    const bool countersValid = state.mainCalls >= current.rateMainCalls && current.calls >= current.rateCalls;
                    rollback = !countersValid || auxiliaryRateNeedsRollback(current.baselineHz, rateNow - current.rateAt,
                        state.mainCalls - current.rateMainCalls, current.calls - current.rateCalls,
                        rateNow - state.mainLast, rateNow - current.last, current.mainRateGap, current.auxiliaryRateGap);
                    if (rollback) {
                        output = current;
                        mainCalls = state.mainCalls;
                        mainLast = state.mainLast;
                    } else {
                        current.rateAt = rateNow;
                        current.rateCalls = current.calls;
                        current.rateMainCalls = state.mainCalls;
                        current.mainRateGap = current.auxiliaryRateGap = 0;
                    }
                }
                if (rollback) cadenceRollback(state, output, slot, rateNow, mainCalls, mainLast,
                    "auxiliary rate below threshold");
            }
            continue;
        }
        // Start only after a live baseline with the main window in foreground.
        // Never steal focus from another application or the visible scope.
        if (retries && output.everParked && (!retryFresh || now - output.last > 100 || now - mainLast > 100 ||
                GetForegroundWindow() != main)) {
            disable(state, "retry freshness or main foreground changed before parking");
            continue;
        }
        if (output.calls < 30 || now - output.first < 1000 || now - output.last > 100 ||
                now - mainLast > 100 || !ours(main) || GetForegroundWindow() != main ||
                std::strcmp(registeredD3D11WindowRole(main), "main") != 0 ||
                IsIconic(output.window) || !IsWindowVisible(output.window) ||
                !identity(output, slot, true)) continue;
        RECT saved {}, parked {};
        if (!GetWindowRect(output.window, &saved) || !parkingRect(saved, parked)) continue;
        const double baseline = retries ? output.retryBaselineHz : output.calls * 1000.0 / (now - output.first);
        if (baseline < 10.0) continue;
        if (retries && !sameRect(saved, output.saved)) {
            disable(state, "saved placement changed before retry parking");
            continue;
        }
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            auto& current = state.outputs[slot];
            if (state.retry.mode() != AuxiliaryRetryBudget::Mode::Active ||
                    current.generation != output.generation || current.managed) continue;
            current.saved = saved;
            current.parked = parked;
            current.managed = true;
            current.parkedAt = current.rateAt = GetTickCount64();
            current.rateCalls = current.calls;
            current.rateMainCalls = state.mainCalls;
            current.mainRateGap = current.auxiliaryRateGap = 0;
            current.baselineHz = baseline;
            current.everParked = true;
            state.everParked = true;
        }
        if (!identity(output, slot, true) || !moveWithoutActivation(output.window, parked)) {
            disable(state, "parking request failed");
            continue;
        }
        log_info("bone-eater", "Auxiliary {} parked offscreen; auxiliary baseline {:.3f} Present calls/s, "
            "recent visible main {:.3f}/s, threshold {:.3f}/s, retry {} of 1, watchdog active",
            roles[slot], baseline, output.retryMainHz, std::max(5.0, baseline * 0.5), retries);
    }
}

void CALLBACK tick(void* context, BOOLEAN) noexcept {
    auto& state = *static_cast<State*>(context);
    if (state.workerRunning.test_and_set(std::memory_order_acquire)) return;
    try { service(state); }
    catch (...) { disable(state, "window worker exception"); }
    state.workerRunning.clear(std::memory_order_release);
}

void stopTimer() noexcept {
    State* state = stateStorage();
    if (!state) return;
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->retry.permanentFailure();
    }
    if (state->timer) {
        DeleteTimerQueueTimer(nullptr, state->timer, INVALID_HANDLE_VALUE);
        state->timer = nullptr;
    }
}

State* getState(bool create) {
    std::lock_guard<std::mutex> guard(creationMutex());
    State*& state = stateStorage();
    if (state || !create) return state;
    state = new (std::nothrow) State;
    if (!state) return nullptr;
    if (restoreRequested.load(std::memory_order_acquire)) state->retry.permanentFailure();
    if (state->retry.mode() == AuxiliaryRetryBudget::Mode::Disabled) return state;
    if (std::atexit(stopTimer) != 0 ||
            !CreateTimerQueueTimer(&state->timer, nullptr, tick, state, 200, 200, WT_EXECUTEDEFAULT)) {
        state->retry.permanentFailure();
    }
    return state;
}

} // namespace

void observeAuxiliaryPresentResult(IDXGISwapChain* chain, void* knownMainWindow,
                                  unsigned int flags, long result) noexcept {
    if (!enabled() || !chain) return;
    try {
        DXGI_SWAP_CHAIN_DESC description {};
        if (FAILED(chain->GetDesc(&description)) || !description.Windowed ||
                !ours(description.OutputWindow)) return;
        const char* role = registeredD3D11WindowRole(description.OutputWindow);
        const bool main = std::strcmp(role, "main") == 0 && description.OutputWindow == knownMainWindow;
        int slot = -1;
        for (int candidate = 0; candidate < 2; ++candidate) {
            if (std::strcmp(role, roles[candidate]) == 0 &&
                    description.BufferDesc.Width == widths[candidate] &&
                    description.BufferDesc.Height == heights[candidate]) slot = candidate;
        }
        if (!main && slot < 0) return;
        State* state = getState(true);
        if (!state) return;
        if (result != S_OK) {
            // DXGI_STATUS_OCCLUDED is a success-status HRESULT; SUCCEEDED alone
            // would silently accept it and let a parked scope stop rendering.
            disable(*state, result == DXGI_STATUS_OCCLUDED ? "DXGI_STATUS_OCCLUDED" : "native Present returned non-S_OK");
            return;
        }
        if (flags & DXGI_PRESENT_TEST) return;
        bool identityChanged = false;
        {
            std::lock_guard<std::mutex> guard(state->mutex);
            const auto now = GetTickCount64();
            if (main) {
                if (state->main && state->main != description.OutputWindow && state->everParked) {
                    identityChanged = true;
                } else {
                    const auto gap = state->mainCalls ? now - state->mainLast : 0;
                    if (gap > AuxiliaryVisibleCadence::freshMs) ++state->mainGapSerial;
                    for (auto& output : state->outputs) output.mainRateGap = std::max(output.mainRateGap, gap);
                    state->main = description.OutputWindow;
                    state->mainLast = now;
                    ++state->mainCalls;
                }
            } else if (state->retry.mode() != AuxiliaryRetryBudget::Mode::Disabled) {
                auto& output = state->outputs[static_cast<std::size_t>(slot)];
                DWORD thread = 0;
                if (!ours(description.OutputWindow, &thread)) return;
                const auto atom = static_cast<ULONG_PTR>(GetClassLongPtrW(description.OutputWindow, GCW_ATOM));
                if (!atom) return;
                if (output.window != description.OutputWindow || output.thread != thread || output.classAtom != atom) {
                    // Preserve the old saved placement. An identity transition
                    // after any parking attempt permanently cancels recovery.
                    if (state->everParked) {
                        identityChanged = true;
                    } else {
                        output = {};
                        output.window = description.OutputWindow;
                        output.thread = thread;
                        output.classAtom = atom;
                        output.generation = ++state->nextGeneration;
                        output.first = now;
                    }
                }
                if (!identityChanged) {
                    const auto gap = output.calls ? now - output.last : 0;
                    if (gap > AuxiliaryVisibleCadence::freshMs) ++output.gapSerial;
                    output.auxiliaryRateGap = std::max(output.auxiliaryRateGap, gap);
                    output.last = now;
                    ++output.calls;
                }
            }
        }
        if (identityChanged) disable(*state, "main or auxiliary Present window identity changed");
    } catch (...) {}
}

bool auxiliaryWindowPlacement(void* window, int& x, int& y, int& width, int& height) noexcept {
    if (!enabled() || !window) return false;
    try {
        State* state = getState(false);
        if (!state) return false;
        std::lock_guard<std::mutex> guard(state->mutex);
        for (std::size_t slot = 0; slot < state->outputs.size(); ++slot) {
            const auto& output = state->outputs[slot];
            if (!output.managed || output.window != window || !identity(output, slot, false)) continue;
            const auto& desired = output.restoring || state->retry.mode() != AuxiliaryRetryBudget::Mode::Active ?
                output.saved : output.parked;
            x = desired.left; y = desired.top;
            width = desired.right - desired.left; height = desired.bottom - desired.top;
            return true;
        }
    } catch (...) {}
    return false;
}

bool auxiliaryWindowManaged(void* window) noexcept {
    int x = 0, y = 0, width = 0, height = 0;
    return auxiliaryWindowPlacement(window, x, y, width, height);
}

void restoreAuxiliaryWindows() noexcept {
    if (!enabled()) return;
    // An early opt-out must not construct a timer before the window registry;
    // its destructor would otherwise run before the later-registered atexit.
    restoreRequested.store(true, std::memory_order_release);
    try {
        if (State* state = getState(false)) disable(*state, "explicit restore requested");
    } catch (...) {}
}

} // namespace bone_eater::render
