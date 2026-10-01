#include "input/native_input_ownership.h"
#include "input/native_precision_bypass.h"
#include "input/scope_control.h"
#include "diagnostics/optional_csv.h"
#include "input/selected_hid_bridge.h"
#include "render/native_viewport.h"
#include "util/detour.h"
#include "util/logging.h"

#include <intrin.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <mutex>

namespace bone_eater::input {
namespace {
using ScopeUpdate = void(__fastcall*)(void*, float);
ScopeUpdate originalScope = nullptr;
using PressedQuery = bool(__fastcall*)(unsigned);
PressedQuery originalPressed = nullptr;
std::atomic<bool> scopeControlReady {false};
std::uintptr_t base = 0;
struct Shared {
    std::atomic<bool> enabled {false};
    std::atomic<ULONGLONG> nextSample {0};
    std::atomic<unsigned> rows {0};
    std::mutex outputMutex;
    FILE* file = nullptr;
};
Shared* shared = nullptr; // Pinned hook and process-lifetime storage.
bool attempted = false;

struct CachedSource {
    bool configured = false, selected = false, shutdown = false, runtime = false;
    bool usable = false, armed = false, aimValid = false;
    int state = -1, validation = -1, lifecycle = -1;
    std::uint64_t cabinet = 0, publication = 0, session = 0, generation = 0;
};
thread_local InputOwnershipState ownership;
thread_local CachedSource sourceContext;
thread_local unsigned scopeDepth = 0;

template<class T> bool read(std::uintptr_t owner, std::size_t offset, T& result) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
        owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &result, sizeof(T), &copied) && copied == sizeof(T);
}
bool vtable(std::uintptr_t object, std::uintptr_t rva) noexcept {
    std::uintptr_t value = 0;
    return read(object, 0, value) && value == base + rva;
}
template<std::size_t N> bool matches(std::uintptr_t rva,
        const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> bytes {};
    return read(base, rva, bytes) && bytes == expected;
}
bool enabled() noexcept { return shared && shared->enabled.load(std::memory_order_relaxed); }

struct ScopeState {
    std::uintptr_t scope = 0, scopeNative = 0, scopeOwned = 0, input = 0, config = 0;
    std::uintptr_t mainWrapper = 0, mainNative = 0, manager = 0;
    std::uintptr_t offHolder = 0, offUi = 0;
    OwnershipPoint point {}, gains {}, anchor {}, offsets {};
    float zoom = 0;
    std::uint32_t state = 0, mode = 0, script = 0, held = 0, pressed = 0;
    std::uint8_t filtered = 0, scopeEnabled = 0, scopeOff = 1;
    bool offKnown = false;
};
bool identityEqual(const ScopeState& a, const ScopeState& b) noexcept {
    return a.scope == b.scope && a.scopeNative == b.scopeNative && a.scopeOwned == b.scopeOwned &&
        a.input == b.input && a.config == b.config && a.mainWrapper == b.mainWrapper &&
        a.mainNative == b.mainNative && a.manager == b.manager && a.offHolder == b.offHolder &&
        a.offUi == b.offUi;
}
bool equal(const ScopeState& a, const ScopeState& b) noexcept {
    return identityEqual(a, b) && sameOwnershipPoint(a.point, b.point) &&
        sameOwnershipPoint(a.gains, b.gains) && sameOwnershipPoint(a.anchor, b.anchor) &&
        sameOwnershipPoint(a.offsets, b.offsets) && a.zoom == b.zoom && a.state == b.state &&
        a.mode == b.mode && a.script == b.script && a.held == b.held && a.pressed == b.pressed &&
        a.filtered == b.filtered && a.scopeEnabled == b.scopeEnabled && a.scopeOff == b.scopeOff &&
        a.offKnown == b.offKnown;
}
bool collect(std::uintptr_t scope, ScopeState& out) noexcept {
    std::uintptr_t published = 0, managerCamera = 0;
    OwnershipPoint size {};
    std::uint8_t slot = 255, uiMode = 0;
    out.scope = scope;
    if (!read(base, 0x13DCF88, published) || published != scope || !vtable(scope, 0x10C7350) ||
        !read(scope, 8, out.scopeNative) || !read(scope, 0x60, out.scopeOwned) ||
        !vtable(out.scopeNative, 0x6FDF48) || !vtable(out.scopeOwned, 0x6FDF48) ||
        !read(base, 0x13DCED0, out.input) || !read(base, 0x13DCF58, out.config) ||
        !vtable(out.config, 0x10C5FA8) || !read(out.config, 0x136A, uiMode) || uiMode != 1 ||
        !read(base, 0x13DB588, out.mainWrapper) || !read(out.mainWrapper, 8, out.mainNative) ||
        !vtable(out.mainNative, 0x6FDF48) || out.mainNative == out.scopeNative ||
        out.mainNative == out.scopeOwned || !read(base, 0x12E1930, out.manager) ||
        !read(out.manager, 0x340, managerCamera) || managerCamera != out.mainNative ||
        !read(out.mainNative, 0xEB4, size) || size != OwnershipPoint{{1920,1080}} ||
        !read(out.mainNative, 0xE8E, slot) || slot != 0 ||
        !read(out.input, 0x0C, out.point) || !read(out.input, 0, out.held) ||
        !read(out.input, 8, out.pressed) || !read(out.config, 0x12D8, out.gains) ||
        !read(scope, 0x190, out.anchor) || !read(scope, 0x1B8, out.offsets) ||
        !read(scope, 0x174, out.zoom) || !read(scope, 0x1C4, out.state) || out.state > 4 ||
        !read(scope, 0x1C8, out.mode) || out.mode > 3 ||
        !read(scope, 0x1C1, out.filtered) || out.filtered > 1 ||
        !read(scope, 0x1C2, out.scopeEnabled) || out.scopeEnabled > 1 ||
        !read(base, 0x13D9F7C, out.script)) return false;
    for (const auto& pair : {out.point, out.gains, out.anchor, out.offsets})
        for (const auto value : pair) if (!std::isfinite(value)) return false;
    if (!std::isfinite(out.zoom)) return false;
    out.offKnown = read(base, 0x13DCF48, out.offHolder) && read(out.offHolder, 0, out.offUi) &&
        vtable(out.offUi, 0x10CA5E0) && read(out.offUi, 0x558, out.scopeOff) && out.scopeOff <= 1;
    return true;
}
bool repeated(std::uintptr_t scope, ScopeState& out) noexcept {
    ScopeState again {};
    return collect(scope, out) && collect(scope, again) && equal(out, again);
}

// Only the four verified native zoom queries inside this exact Scope update
// receive a controlled edge. No cabinet/game held or pressed field is changed;
// all other native consumers continue to receive their original query result.
struct ZoomQueryContext {
    std::uintptr_t owner = 0, scene = 0;
    bool owns = false, valid = false, higher = false, delivered = false;
};
thread_local ZoomQueryContext zoomQuery;
bool currentZoomScene(std::uintptr_t expected) noexcept {
    std::uintptr_t manager = 0, scene = 0, again = 0;
    std::int32_t pending = 0;
    return expected && read(base, 0x13DD000, manager) && vtable(manager, 0x10C6B38) &&
        read(manager, 0x3C, pending) && pending == -1 && read(manager, 0x40, scene) &&
        scene == expected && read(base, 0x13DD000, again) && again == manager;
}
ZoomQueryContext beginZoomQuery(std::uintptr_t owner, std::uintptr_t caller, bool nested) noexcept {
    ZoomQueryContext result;
    result.owner = owner;
    result.owns = scopeControlReady.load(std::memory_order_acquire) && caller == base + 0xAA923;
    if (!result.owns || nested) return result;
    const auto intent = readScopeControlSnapshot();
    result.scene = static_cast<std::uintptr_t>(intent.contextIdentity);
    const auto now = GetTickCount64();
    ScopeState state;
    result.valid = intent.armed && intent.contextIdentity && now >= intent.updatedAtMs &&
        now - intent.updatedAtMs <= 100 && repeated(owner, state) && state.scopeEnabled &&
        intent.nativeScopeOwner == owner && intent.nativeInputOwner == state.input &&
        state.offKnown && !state.scopeOff && state.script == 0 && currentZoomScene(result.scene);
    result.higher = intent.enabled && intent.higher;
    return result;
}
__declspec(noinline) bool __fastcall scopePressed(unsigned button) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool modeZeroBranch = (caller == base + 0xCCBB5 && button == 1) ||
        (caller == base + 0xCCBC3 && button == 2);
    const bool modeOneBranch = (caller == base + 0xCCB3B && button == 1) ||
        (caller == base + 0xCCB49 && button == 2);
    if (!scopeControlReady.load(std::memory_order_acquire) || !zoomQuery.owns ||
            (!modeZeroBranch && !modeOneBranch)) return originalPressed(button);
    if (!zoomQuery.valid || zoomQuery.delivered) return false;
    std::uintptr_t published = 0;
    std::uint32_t mode = 99;
    if (!read(base, 0x13DCF88, published) || published != zoomQuery.owner ||
        !vtable(published, 0x10C7350) || !read(published, 0x1C8, mode) ||
        mode != (modeZeroBranch ? 0u : 1u) || !currentZoomScene(zoomQuery.scene)) return false;
    const bool edge = scopeZoomNeedsEdge(zoomQuery.higher, mode);
    zoomQuery.delivered = edge;
    return edge;
}

bool installZoomQuery() noexcept {
    constexpr std::array<unsigned char, 18> entry {{
        0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0x05,0xB3,0x21,0x35,0x01,
        0x8B,0xD9,0x48,0x85,0xC0}};
    constexpr std::array<unsigned char, 32> modeOne {{
        0xB9,1,0,0,0,0xE8,0xD5,0xE1,0xFB,0xFF,0x84,0xC0,0x75,0x12,
        0xB9,2,0,0,0,0xE8,0xC7,0xE1,0xFB,0xFF,0x84,0xC0,0x0F,0x84,0xCE,0,0,0}};
    constexpr std::array<unsigned char, 28> modeZero {{
        0xB9,1,0,0,0,0xE8,0x5B,0xE1,0xFB,0xFF,0x84,0xC0,0x75,0x0E,
        0xB9,2,0,0,0,0xE8,0x4D,0xE1,0xFB,0xFF,0x84,0xC0,0x74,0x58}};
    if (!matches(0x8AD10, entry) || !matches(0xCCB31, modeOne) || !matches(0xCCBAB, modeZero))
        return false;
    originalPressed = reinterpret_cast<PressedQuery>(base + 0x8AD10);
    return detour::trampoline_try(originalPressed, &scopePressed, &originalPressed);
}

struct Observation {
    bool sampled = false, beforeValid = false, afterValid = false, nested = false;
    bool returned = false, rayVerified = false, rayScopeKnown = false, rayScopeOff = true;
    bool rayInputRead = false;
    std::uint64_t tick = 0, endTick = 0, serialAtRay = 0;
    std::uintptr_t owner = 0, rayCamera = 0;
    DWORD thread = 0;
    unsigned rayCalls = 0;
    float delta = 0;
    OwnershipPoint rayPoint {}, rayInput {};
    InputOwnershipState input;
    CachedSource source;
    ScopeState before, after;
    OwnershipMatch match = OwnershipMatch::NoCompletion;
};
thread_local Observation* activeScope = nullptr;

bool reserveSample(ULONGLONG now) noexcept {
    auto next = shared->nextSample.load(std::memory_order_relaxed);
    return now >= next && shared->nextSample.compare_exchange_strong(next, now + 200,
        std::memory_order_relaxed);
}
void beginScope(Observation& row, std::uintptr_t owner, float delta,
        std::uintptr_t caller, bool nested) noexcept {
    if (!enabled()) return;
    if (activeScope) activeScope->nested = true;
    if (caller != base + 0xAA923) return;
    const auto now = GetTickCount64();
    if (nested || !reserveSample(now)) return;
    row.sampled = true; row.tick = now; row.thread = GetCurrentThreadId();
    row.owner = owner; row.delta = delta; row.input = ownership; row.source = sourceContext;
    row.beforeValid = repeated(owner, row.before);
    if (row.beforeValid) row.match = row.input.match(row.before.input, row.before.point, now);
}

struct CsvRow {
    char bytes[8192] {};
    std::size_t used = 0;
    bool valid = true;
    void append(const char* format, ...) noexcept {
        if (!valid) return;
        va_list args; va_start(args, format);
        const int count = vsnprintf(bytes + used, sizeof(bytes) - used, format, args);
        va_end(args);
        if (count < 0 || static_cast<std::size_t>(count) >= sizeof(bytes) - used) { valid = false; return; }
        used += static_cast<std::size_t>(count);
    }
    void number(std::uint64_t x) noexcept { append(",%llu", static_cast<unsigned long long>(x)); }
    void signedNumber(int x) noexcept { append(",%d", x); }
    void pointer(std::uintptr_t x) noexcept { append(",0x%llx", static_cast<unsigned long long>(x)); }
    void scalar(float x) noexcept { append(",%.9g", static_cast<double>(x)); }
    void point(const OwnershipPoint& p) noexcept { scalar(p[0]); scalar(p[1]); }
};
constexpr char kHeader[] =
    "schema,pid,thread,tick_ms,end_tick_ms,status,returned,before_valid,after_valid,nested,"
    "serial,serial_after,completed,completed_tick_ms,scope_calls,conversion_calls,input,converted_x,converted_y,"
    "scope,scope_native,scope_owned,main_wrapper,main_native,config,manager,"
    "pre_x,pre_y,post_x,post_y,gain_x,gain_y,anchor_x,anchor_y,offset_x,offset_y,"
    "state_before,state_after,mode_before,mode_after,zoom_before,zoom_after,filtered,scope_enabled,"
    "script,scope_off_known,scope_off,held,pressed,delta,"
    "ray_calls,ray_verified,ray_camera,ray_x,ray_y,ray_serial,ray_input_read,ray_input_x,ray_input_y,"
    "ray_scope_known,ray_scope_off,cached_configured,cached_selected,cached_shutdown,cached_runtime,"
    "cached_source_usable,cached_armed,cached_aim_valid,cached_state,cached_validation,cached_lifecycle,"
    "cached_cabinet_generation,cached_publication_generation,cached_source_session,cached_source_generation\n";

const char* rowStatus(const Observation& row) noexcept {
    if (!row.returned) return "native_exception";
    if (row.nested) return "nested_scope";
    if (!row.beforeValid) return "entry_identity_unresolved";
    if (!row.afterValid || !identityEqual(row.before, row.after)) return "return_identity_changed";
    if (row.input.serial != ownership.serial) return "update_changed_during_scope";
    return ownershipMatchName(row.match);
}
CsvRow formatRow(const Observation& row) noexcept {
    CsvRow out;
    out.append("1"); out.number(GetCurrentProcessId()); out.number(row.thread);
    out.number(row.tick); out.number(row.endTick); out.append(",%s", rowStatus(row));
    out.number(row.returned); out.number(row.beforeValid); out.number(row.afterValid); out.number(row.nested);
    out.number(row.input.serial); out.number(ownership.serial); out.number(row.input.completed);
    out.number(row.input.completedTick); out.number(row.input.scopeCalls); out.number(row.input.conversions);
    out.pointer(row.input.input); out.point(row.input.converted);
    out.pointer(row.owner); out.pointer(row.before.scopeNative); out.pointer(row.before.scopeOwned);
    out.pointer(row.before.mainWrapper); out.pointer(row.before.mainNative);
    out.pointer(row.before.config); out.pointer(row.before.manager);
    out.point(row.before.point); out.point(row.after.point); out.point(row.before.gains);
    out.point(row.before.anchor); out.point(row.before.offsets);
    out.number(row.before.state); out.number(row.after.state); out.number(row.before.mode); out.number(row.after.mode);
    out.scalar(row.before.zoom); out.scalar(row.after.zoom); out.number(row.before.filtered);
    out.number(row.before.scopeEnabled); out.number(row.before.script); out.number(row.before.offKnown);
    out.number(row.before.scopeOff); out.number(row.before.held); out.number(row.before.pressed); out.scalar(row.delta);
    out.number(row.rayCalls); out.number(row.rayVerified); out.pointer(row.rayCamera); out.point(row.rayPoint);
    out.number(row.serialAtRay); out.number(row.rayInputRead); out.point(row.rayInput);
    out.number(row.rayScopeKnown); out.number(row.rayScopeOff);
    const auto& source = row.source;
    out.number(source.configured); out.number(source.selected); out.number(source.shutdown); out.number(source.runtime);
    out.number(source.usable); out.number(source.armed); out.number(source.aimValid);
    out.signedNumber(source.state); out.signedNumber(source.validation); out.signedNumber(source.lifecycle);
    out.number(source.cabinet); out.number(source.publication); out.number(source.session); out.number(source.generation);
    out.append("\n"); return out;
}
void finishScope(Observation& row, bool returned) noexcept {
    if (!row.sampled) return;
    row.returned = returned; row.endTick = GetTickCount64();
    row.afterValid = repeated(row.owner, row.after);
    // Throttle from completion too, so a stalled observation cannot cause an
    // immediately adjacent sample. This remains diagnostic callback coverage.
    auto next = shared->nextSample.load(std::memory_order_relaxed);
    while (next < row.endTick + 200 && !shared->nextSample.compare_exchange_weak(
        next, row.endTick + 200, std::memory_order_relaxed)) {}
    try {
        std::unique_lock<std::mutex> lock(shared->outputMutex, std::try_to_lock);
        if (!lock.owns_lock() || !shared->file) return;
        const auto output = formatRow(row);
        if (!output.valid || fwrite(output.bytes, 1, output.used, shared->file) != output.used ||
                fflush(shared->file) || ferror(shared->file)) {
            shared->enabled.store(false);
            fclose(shared->file); shared->file = nullptr;
            try { log_warning("bone-eater", "Input ownership diagnostic stopped: optional CSV output failed"); } catch (...) {}
        } else if (shared->rows.fetch_add(1) + 1 >= 36000) {
            shared->enabled.store(false);
            fclose(shared->file); shared->file = nullptr;
            try { log_info("bone-eater", "Input ownership diagnostic reached its 36000-row limit"); } catch (...) {}
        }
    } catch (...) {}
}

// No destructible C++ locals in the SEH boundary. Nested invocations suspend
// the outer ray target even when the inner call is not sampled. The original
// exception continues unchanged; all TLS is restored before optional output.
void invokeScope(void* owner, float delta, std::uintptr_t caller) {
    Observation row {};
    Observation* previous = activeScope;
    const bool nested = scopeDepth != 0;
    if (caller == base + 0xAA923) ownership.scopeInvocation();
    beginScope(row, reinterpret_cast<std::uintptr_t>(owner), delta, caller, nested);
    const auto precision = beginNativePrecisionScope(reinterpret_cast<std::uintptr_t>(owner),
        caller, delta, ownership, nested);
    const auto previousZoom = zoomQuery;
    zoomQuery = beginZoomQuery(reinterpret_cast<std::uintptr_t>(owner), caller, nested);
    ++scopeDepth; activeScope = row.sampled ? &row : nullptr;
    bool returned = false;
    __try { originalScope(owner, delta); returned = true; }
    __finally {
        zoomQuery = previousZoom;
        activeScope = previous; --scopeDepth;
        finishNativePrecisionScope(precision, returned);
        finishScope(row, returned);
    }
}
__declspec(noinline) void __fastcall scopeObserved(void* owner, float delta) {
    if (!enabled() && !nativePrecisionReady() && !scopeControlNativeReady()) { originalScope(owner, delta); return; }
    invokeScope(owner, delta, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
} // namespace

void installNativeInputOwnershipObserver(void* gamendd) noexcept {
    try {
        if (attempted) return;
        wchar_t value[4] {};
        const bool diagnostic = diagnostics::optionalOutputEnabled() &&
            GetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT_OWNERSHIP_OBSERVE", value, 4) == 1 && value[0] == L'1';
        const bool scopeControl = scopeControlRequested();
        if (!diagnostic && !nativePrecisionRequested() && !scopeControl) return;
        attempted = true;
        if (std::strcmp(render::verifyNativeGameModule(gamendd), "verified")) return;
        base = reinterpret_cast<std::uintptr_t>(gamendd);
        constexpr std::array<unsigned char, 28> entry {{
            0x48,0x8B,0xC4,0x53,0x48,0x81,0xEC,0xB0,0,0,0,0x80,0xB9,0xC2,1,0,0,0,
            0x0F,0x29,0x70,0xE8,0x48,0x8B,0xD9,0x0F,0x28,0xF1}};
        constexpr std::array<unsigned char, 16> caller {{
            0x48,0x8B,0x8F,0xD8,0x11,0,0,0x0F,0x28,0xCE,0x48,0x8B,0x11,0xFF,0x52,0x08}};
        std::uintptr_t dispatch = 0;
        if (!matches(0xCC9E0, entry) || !matches(0xAA913, caller) ||
            !read(base, 0x10C7358, dispatch) || dispatch != base + 0xCC9E0) {
            log_warning("bone-eater", "Input ownership observer disabled: ScopeCamera instruction guards differ"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(gamendd), &pinned) || pinned != gamendd) return;
        shared = new Shared;
        const bool precision = prepareNativePrecisionBypass(gamendd);
        if (diagnostic) {
            shared->file = diagnostics::openOptionalCsv(L"desktop/native-input-ownership-v1.csv",
                [](FILE* file) { fputs(kHeader, file); });
            if (!shared->file) {
                try { log_warning("bone-eater", "Input ownership diagnostic unavailable: optional CSV could not be opened; functional input is unaffected"); } catch (...) {}
            }
        }
        if (!precision && !shared->file && !scopeControl) return;
        originalScope = reinterpret_cast<ScopeUpdate>(base + 0xCC9E0);
        if (!detour::trampoline_try(originalScope, &scopeObserved, &originalScope)) {
            if (shared->file) fclose(shared->file); shared->file = nullptr;
            log_warning("bone-eater", "Input ownership observer disabled: scope hook unavailable"); return;
        }
        shared->enabled.store(shared->file != nullptr);
        nativePrecisionScopeInstalled(gamendd);
        if (scopeControl) {
            const bool ready = installZoomQuery();
            scopeControlReady.store(ready, std::memory_order_release);
            if (ready) log_info("bone-eater", "Scope toggle/hold native zoom adapter enabled; native optical interpolation retained");
            else log_warning("bone-eater", "Scope toggle/hold unavailable: verified native zoom-query hook could not be installed");
        }
        if (shared->file) log_info("bone-eater", "Input ownership diagnostic enabled: completed desktop updates and ScopeCamera, at most 5 samples/sec; this recorder does not alter aim");
    } catch (...) {}
}

bool scopeControlNativeReady() noexcept { return scopeControlReady.load(std::memory_order_acquire); }

std::uint64_t beginNativeInputOwnership(std::uintptr_t input, bool eligible) noexcept {
    if (!enabled() && !nativePrecisionReady()) return 0;
    sourceContext = {};
    return ownership.begin(input, eligible);
}
void convertedNativeInputOwnership(std::uint64_t ticket, const OwnershipPoint& point, bool converted) noexcept {
    if ((!enabled() && !nativePrecisionReady()) || !ticket) return;
    ownership.conversion(ticket, point, converted);
    if (!enabled() || !converted || ticket != ownership.serial) return;
    // This is intentionally CACHED context, not proof that this exact cabinet
    // publication produced this native update. Never polls/starts a device and
    // never changes source eligibility. Retained loss coordinates stay visible.
    const auto status = readSelectedHidBridgeStatus();
    sourceContext.configured = status.configured; sourceContext.selected = status.selected;
    sourceContext.shutdown = status.shutdownRequested; sourceContext.cabinet = status.cabinetGeneration;
    sourceContext.publication = status.aim.generation; sourceContext.aimValid = status.aim.valid;
    sourceContext.session = status.aim.sourceIdentity.session;
    sourceContext.generation = status.aim.sourceIdentity.generation;
    sourceContext.runtime = status.runtime.has_value();
    if (status.runtime) {
        const auto& snapshot = *status.runtime;
        sourceContext.state = static_cast<int>(snapshot.input.output.state);
        sourceContext.usable = snapshot.input.output.sourceUsable;
        sourceContext.armed = snapshot.input.output.armed;
        sourceContext.validation = static_cast<int>(snapshot.input.validation);
        sourceContext.lifecycle = static_cast<int>(snapshot.lifecycle);
    }
}
void finishNativeInputOwnership(std::uint64_t ticket, bool returned, bool identityValid,
        const OwnershipPoint& point) noexcept {
    if ((!enabled() && !nativePrecisionReady()) || !ticket) return;
    ownership.finish(ticket, returned, identityValid, point, GetTickCount64());
}
void observeNativeInputOwnershipRay(std::uintptr_t camera, float x, float y,
        bool verified, bool scopeKnown, bool scopeOff) noexcept {
    if (!enabled() || !activeScope) return;
    auto& row = *activeScope;
    if (row.rayCalls != std::numeric_limits<unsigned>::max()) ++row.rayCalls;
    row.rayPoint = {{x,y}}; row.rayCamera = camera; row.serialAtRay = ownership.serial;
    row.rayVerified = verified && row.beforeValid && camera == row.before.mainNative &&
        std::isfinite(x) && std::isfinite(y);
    row.rayScopeKnown = scopeKnown; row.rayScopeOff = scopeOff;
    row.rayInputRead = row.beforeValid && read(row.before.input, 0x0C, row.rayInput);
}
} // namespace bone_eater::input
