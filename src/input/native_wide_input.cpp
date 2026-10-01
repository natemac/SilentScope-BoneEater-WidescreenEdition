#include "render/score_results_identity.h"
#include "input/native_wide_input.h"
#include "input/native_input_ownership.h"
#include "input/native_precision_bypass.h"
#include "input/aim_state.h"
#include "input/native_aim.h"
#include "input/scope_control.h"
#include "input/scope_button_events.h"
#include "input/scope_settings.h"
#include "render/native_viewport.h"
#include "render/menu_landscape_layout.h"
#include "render/score_lobby_identity.h"
#include "util/detour.h"
#include "util/logging.h"

#include <intrin.h>
#include <atomic>
#include <cwchar>
#include <limits>
#include <mutex>

namespace bone_eater::input {
namespace {

using UpdateInput = void(__fastcall*)(void*, float);
using GetCoordinate = float(__fastcall*)();
UpdateInput originalUpdate = nullptr;
GetCoordinate originalX = nullptr, originalY = nullptr;
std::uintptr_t moduleBase = 0;
std::uintptr_t arkBase = 0;

struct Domain {
    bool wide = false;
    std::uintptr_t input = 0;
    render::NativeHudGeometry geometry;
};
struct Shared {
    std::mutex mutex;
    Domain domain;
    std::atomic<bool> enabled {false};
    std::atomic<bool> convertedLogged {false};
    std::atomic<bool> faultLogged {false};
    WideInputMapping mapping = WideInputMapping::CabinetMargins; // Immutable after install.
    DesktopInputSnapshot desktop;
    ScopedMotion motion;
    ScopedMotionSettings motionSettings;
    std::vector<int> scopeKeys;
    GunSourceMode lastSourceMode = GunSourceMode::Legacy;
    std::uint64_t lastSourceSession = 0, lastSourceGeneration = 0;
    std::uintptr_t lastMotionScene = 0, lastMotionScope = 0, lastMotionInput = 0;
    bool reportedScope = false, reportedHigh = false, reportedUsable = false;
};
Shared* shared = nullptr; // Process lifetime, including hook teardown.

struct UpdateContext {
    std::uintptr_t input = 0, config = 0;
    std::uint64_t ownershipTicket = 0;
    bool supported = false;
    render::NativeHudGeometry geometry;
};
thread_local UpdateContext* currentUpdate = nullptr;

template<typename T>
bool read(std::uintptr_t owner, std::size_t offset, T& value) noexcept {
    if (!owner || owner > std::numeric_limits<std::uintptr_t>::max() - offset ||
            owner + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(owner + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}

bool sameGeometry(const render::NativeHudGeometry& a,
        const render::NativeHudGeometry& b) noexcept {
    return validWideInputGeometry(a) && validWideInputGeometry(b) &&
        a.helper == b.helper && a.sprite == b.sprite &&
        a.mainWidth == b.mainWidth && a.mainHeight == b.mainHeight &&
        a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}

bool inputIdentity(std::uintptr_t input) noexcept {
    std::uintptr_t actual = 0;
    return input && read(moduleBase, 0x13DCED0, actual) && actual == input;
}

bool gunUiMode(std::uintptr_t& config) noexcept {
    std::uintptr_t repeated = 0;
    std::uint8_t mode = 0, again = 0;
    return read(moduleBase, 0x13DCF58, config) && read(config, 0x136A, mode) && mode == 1 &&
        read(moduleBase, 0x13DCF58, repeated) && repeated == config &&
        read(config, 0x136A, again) && again == mode;
}

struct SceneContext {
    std::uintptr_t manager = 0, scene = 0, scope = 0, window = 0;
    unsigned id = 0;
    bool known = false, gameplay = false, scopeAllowed = false;
};

bool collectScene(SceneContext& result) noexcept {
    std::uintptr_t vt = 0, windowObject = 0;
    int pending = 0;
    std::array<std::uint8_t, 2> stopped {};
    if (!read(moduleBase, 0x13DD000, result.manager) || !read(result.manager, 0, vt) ||
        vt != moduleBase + 0x10C6B38 || !read(result.manager, 0x38, result.id) ||
        !read(result.manager, 0x3C, pending) || pending != -1 ||
        !read(result.manager, 0x40, result.scene) || !read(result.scene, 0, vt) ||
        !read(result.scene, 8, stopped) || stopped[0] || stopped[1]) return false;
    const std::uintptr_t types[] = {0,0x10C7278,0x10C7030,0x10C7308,0x10C6FA0,
        0x10C6E48,0x10C7240,0x10C72B0};
    if (!result.id || result.id >= std::size(types) || vt != moduleBase + types[result.id]) return false;
    if (!read(moduleBase, 0x12E17A0, windowObject) || !read(windowObject, 0x28, result.window)) return false;
    const auto window = reinterpret_cast<HWND>(result.window);
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid) || pid != GetCurrentProcessId() ||
        !IsWindowVisible(window) || IsIconic(window) || GetAncestor(GetForegroundWindow(), GA_ROOT) != window) return false;
    result.known = true;
    result.gameplay = result.id == 5 || result.id == 7;
    if (!result.gameplay) return true;
    std::uintptr_t holder = 0, ui = 0, battle = 0;
    std::uint8_t off = 1, enabled = 0;
    unsigned battleState = 0, script = 1;
    result.scopeAllowed = read(moduleBase, 0x13DCF88, result.scope) && read(result.scope, 0, vt) &&
        vt == moduleBase + 0x10C7350 && read(result.scope, 0x1C2, enabled) && enabled == 1 &&
        read(moduleBase, 0x13DCF48, holder) && read(holder, 0, ui) && read(ui, 0, vt) &&
        vt == moduleBase + 0x10CA5E0 && read(ui, 0x558, off) && off == 0 &&
        read(moduleBase, 0x13DCF78, battle) && read(battle, 0, vt) && vt == moduleBase + 0x10CA298 &&
        read(battle, 0x48, battleState) && battleState == 3 &&
        read(moduleBase, 0x13D9F7C, script) && script == 0;
    return true;
}

SceneContext readScene() noexcept {
    SceneContext a, b;
    if (!collectScene(a) || !collectScene(b) || a.manager != b.manager || a.scene != b.scene ||
        a.id != b.id || a.scope != b.scope || a.window != b.window || a.scopeAllowed != b.scopeAllowed) return {};
    return a;
}

std::string environment(const char* name, const char* fallback) {
    char text[256] {};
    const auto length = GetEnvironmentVariableA(name, text, sizeof(text));
    if (!length) return fallback;
    if (length >= sizeof(text)) throw std::runtime_error("Scope environment setting is too long.");
    return text;
}

void configureMotion() {
    shared->scopeKeys = parseScopeBindings(environment("BONE_EATER_SCOPE_BINDINGS", "ENTER,RBUTTON"));
    configureScopeButtonEvents(shared->scopeKeys);
    shared->motionSettings = readScopedMotionSettings(environment);
    const auto& adaptive = shared->motionSettings.adaptive;
    log_info("bone-eater", "Scoped adaptive motion: enabled={} lowMax={} highMax={} speed={}/{} edgePan={}",
        adaptive.enabled, adaptive.lowMaxGain, adaptive.highMaxGain,
        adaptive.speedStart, adaptive.speedFull, adaptive.edgePan);
}

// The simulated ARK getter uses a different coordinate contract. Desktop
// inversion is valid only for the verified hardware implementation A440.
bool hardwareArkMapping() noexcept {
    std::uintptr_t io = 0, ioAgain = 0, vtable = 0, getter = 0, gun = 0, gunVtable = 0;
    return arkBase && read(arkBase, 0x18BD38, io) &&
        read(io, 0, vtable) && vtable == arkBase + 0xCFCB8 &&
        read(vtable, 0x360, getter) && getter == arkBase + 0xA440 &&
        read(io, 0x78, gun) && io <= std::numeric_limits<std::uintptr_t>::max() - 0x408 &&
        gun == io + 0x408 && read(gun, 0, gunVtable) && gunVtable == arkBase + 0xD04E0 &&
        read(arkBase, 0x18BD38, ioAgain) && ioAgain == io;
}

Domain domain() {
    std::lock_guard<std::mutex> lock(shared->mutex);
    return shared->domain;
}

void publishDomain(bool wide, const UpdateContext& context) {
    std::lock_guard<std::mutex> lock(shared->mutex);
    shared->domain = {wide, context.input, context.geometry};
}

// Both addresses are current native inputs, with a repeated identity check in
// the caller. One contiguous pair is written during the native update thread;
// this does not claim an atomic transaction for arbitrary engine readers.
bool replacePair(std::uintptr_t address, const std::array<float, 2>& expected,
        const std::array<float, 2>& replacement) noexcept {
    std::array<float, 2> current {};
    if (!read(address, 0, current) || std::memcmp(current.data(), expected.data(), sizeof(current))) return false;
    SIZE_T written = 0;
    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), replacement.data(),
            sizeof(replacement), &written) && written == sizeof(replacement)) return true;
    SIZE_T restored = 0;
    WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), expected.data(),
        sizeof(expected), &restored);
    return false;
}

void warnFault() {
    if (!shared->faultLogged.exchange(true))
        log_warning("bone-eater", "Wide input mapping unavailable or changed; unsupported samples retain native coordinates");
}

void finishUiPair(const UpdateContext& context) {
    const Domain active = domain();
    if (!active.wide || active.input != context.input || !inputIdentity(context.input)) return;
    std::array<float, 2> main {}, ui {}, repeated {};
    if (!read(context.input, 0x0C, main) || !read(context.input, 0x24, ui) ||
            !read(context.input, 0x0C, repeated) || main != repeated) return;
    std::uintptr_t config = 0;
    const auto currentGeometry = render::readNativeHudGeometry();
    float uiX=main[0],uiY=main[1];
    {
        auto module=GetModuleHandleW(L"gamendd.dll");std::uintptr_t owner=0;unsigned phase=0;
        if(module&&render::landscapeMenuOwner(reinterpret_cast<std::uintptr_t>(module),
                [](auto p,auto o,auto& value){return read(p,o,value);},owner,phase)&&
           !render::MenuLandscapeLayout::toNative(main[0],main[1],uiX,uiY))uiX=uiY=-1000000.f;
    }
    std::uintptr_t lobbyOwner=0, repeatedLobbyOwner=0;
    unsigned lobbyPhase=0, repeatedLobbyPhase=0;
    const auto lobbyRead=[](auto p,auto o,auto& value){return read(p,o,value);};
    const bool lobby=render::scoreLobbyOwner(moduleBase,lobbyRead,lobbyOwner,lobbyPhase);
    const auto lcd = lobby ? wideToLobbyUi(main[0],main[1],currentGeometry) :
        wideToHud(uiX,uiY,currentGeometry);
    const bool lobbyCoherent = !lobby ||
        (render::scoreLobbyOwner(moduleBase,lobbyRead,repeatedLobbyOwner,repeatedLobbyPhase) &&
         repeatedLobbyOwner==lobbyOwner && repeatedLobbyPhase==lobbyPhase);
    const bool coherent = context.supported && lobbyCoherent && lcd.valid &&
        sameGeometry(context.geometry, currentGeometry) && sameGeometry(active.geometry, currentGeometry) &&
        gunUiMode(config) && config == context.config && inputIdentity(context.input);
    // When a previously converted sample survives an ARK failure, it is still
    // wide. If its geometry can no longer be verified, reject UI hits rather
    // than applying the old portrait scale to an unknown coordinate domain.
    const float outside = -1000000.0f;
    const std::array<float, 2> mapped {{coherent ? lcd.x : outside, coherent ? lcd.y : outside}};
    const bool replaced = replacePair(context.input + 0x24, ui, mapped);
    if (!replaced || !coherent) warnFault();
    if ((lobby || render::scoreResultsReticle(moduleBase,lobbyRead)) && coherent && replaced) {
        // ARK publishes before the native update finalizes the main aim pair.
        // The lobby/results reticle must agree with that final menu coordinate,
        // not the earlier callback sample. Preserve its validity and timestamp.
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->desktop.x = main[0];
        shared->desktop.y = main[1];
    }
}

// Keep both functional and diagnostic TLS scoped to the native call, including
// native SEH. A failed native call never certifies a completed conversion.
void invokeUpdate(void* input, float delta, UpdateContext* context, std::uint64_t ticket) {
    UpdateContext* previous = currentUpdate;
    currentUpdate = context;
    bool returned = false;
    __try { originalUpdate(input, delta); returned = true; }
    __finally {
        currentUpdate = previous;
        if (!returned) finishNativeInputOwnership(ticket, false, false, {});
    }
}

void finishOwnership(const UpdateContext& context) noexcept {
    if (!context.ownershipTicket) return; // No extra reads unless tracing or precision needs proof.
    bool coherent = false;
    OwnershipPoint point {}, repeated {};
    try {
        std::uintptr_t config = 0;
        const auto geometry = render::readNativeHudGeometry();
        coherent = context.supported && inputIdentity(context.input) && gunUiMode(config) &&
            config == context.config && sameGeometry(context.geometry, geometry) &&
            read(context.input, 0x0C, point) && read(context.input, 0x0C, repeated) &&
            sameOwnershipPoint(point, repeated) && inputIdentity(context.input);
    } catch (...) {}
    finishNativeInputOwnership(context.ownershipTicket, true, coherent, point);
}

__declspec(noinline) void __fastcall updateInput(void* input, float delta) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (!shared || !shared->enabled.load() || caller != moduleBase + 0x8D047 || currentUpdate) {
        const auto ticket = beginNativeInputOwnership(reinterpret_cast<std::uintptr_t>(input), false);
        invokeUpdate(input, delta, nullptr, ticket);
        finishNativeInputOwnership(ticket, true, false, {});
        return;
    }
    UpdateContext context;
    context.input = reinterpret_cast<std::uintptr_t>(input);
    context.ownershipTicket = beginNativeInputOwnership(context.input,
        shared->mapping == WideInputMapping::Desktop);
    try {
        context.geometry = render::readNativeHudGeometry();
        context.supported = inputIdentity(context.input) && validWideInputGeometry(context.geometry) &&
            gunUiMode(context.config);
    } catch (...) {}
    invokeUpdate(input, delta, &context, context.ownershipTicket);
    try { finishUiPair(context); } catch (...) {}
    finishOwnership(context);
}

float widgetValue(float original, bool y) {
    if (!shared || !shared->enabled.load()) return original;
    const Domain active = domain();
    if (!active.wide) return original;
    const auto geometry = render::readNativeHudGeometry();
    if (!inputIdentity(active.input) || !sameGeometry(active.geometry, geometry)) return -1000000.0f;
    {
        std::uintptr_t owner=0;unsigned phase=0;
        if(render::landscapeMenuOwner(moduleBase,
                [](auto p,auto o,auto& value){return read(p,o,value);},owner,phase)) {
            std::array<float,2> point{};
            float nx=0,ny=0;
            if(!read(active.input,0x0C,point)||
               !render::MenuLandscapeLayout::toNative(point[0],point[1],nx,ny))return -1000000.0f;
            const auto mapped=wideToWidgetGetter(nx,ny,geometry);
            return mapped.valid?(y?mapped.y:mapped.x):-1000000.0f;
        }
    }
    // One axis at a time is safe here: the native getter reads only that axis,
    // and the committed HUD geometry is immutable throughout this experiment.
    const auto mapped = y ? wideToWidgetGetter(geometry.left, original, geometry) :
        wideToWidgetGetter(original, geometry.top, geometry);
    return mapped.valid ? (y ? mapped.y : mapped.x) : -1000000.0f;
}

__declspec(noinline) float __fastcall getX() {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const float value = originalX();
    if (caller == moduleBase + 0x159B86) {
        try { return widgetValue(value, false); } catch (...) { return -1000000.0f; }
    }
    return value;
}

__declspec(noinline) float __fastcall getY() {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const float value = originalY();
    if (caller == moduleBase + 0x159BA1) {
        try { return widgetValue(value, true); } catch (...) { return -1000000.0f; }
    }
    return value;
}

template<std::size_t N>
bool matches(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(moduleBase, rva, actual) && actual == expected;
}

} // namespace

void installNativeWideInput(void* gamendd) noexcept {
    try {
        wchar_t setting[16] {};
        const auto count = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", setting, 16);
        const bool desktop = count == 7 && !std::wcscmp(setting, L"desktop");
        const bool wide = count == 4 && !std::wcscmp(setting, L"wide");
        if ((!desktop && !wide) || shared) return;
        if (std::strcmp(render::verifyNativeGameModule(gamendd), "verified")) return;
        moduleBase = reinterpret_cast<std::uintptr_t>(gamendd);
        constexpr std::array<unsigned char, 17> updatePrologue {{
            0x4C,0x8B,0xDC,0x49,0x89,0x5B,0x18,0x49,0x89,0x73,0x20,0x55,0x57,0x41,0x54,0x41,0x56}};
        constexpr std::array<unsigned char, 5> updateCall {{0xE8,0xA9,0xBA,0xFF,0xFF}};
        constexpr std::array<unsigned char, 11> xPrologue {{
            0x48,0x83,0xEC,0x28,0x48,0x8B,0x05,0x25,0x21,0x35,0x01}};
        constexpr std::array<unsigned char, 11> yPrologue {{
            0x48,0x83,0xEC,0x28,0x48,0x8B,0x05,0xE5,0x20,0x35,0x01}};
        constexpr std::array<unsigned char, 5> xCall {{0xE8,0x1A,0x12,0xF3,0xFF}};
        constexpr std::array<unsigned char, 5> yCall {{0xE8,0x3F,0x12,0xF3,0xFF}};
        constexpr std::array<unsigned char, 10> uiCommit {{
            0xF3,0x0F,0x11,0x4F,0x28,0xF3,0x0F,0x11,0x47,0x24}};
        constexpr std::array<unsigned char, 11> uiRead {{
            0xF3,0x0F,0x10,0x7D,0x28,0xF3,0x44,0x0F,0x10,0x45,0x24}};
        if (!matches(0x88AF0, updatePrologue) || !matches(0x8D042, updateCall) ||
                !matches(0x8ADA0, xPrologue) || !matches(0x8ADE0, yPrologue) ||
                !matches(0x159B81, xCall) || !matches(0x159B9C, yCall) ||
                !matches(0x8907B, uiCommit) || !matches(0x10D9F4, uiRead)) {
            log_warning("bone-eater", "Wide input mapping disabled: native instruction guards differ");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(gamendd), &pinned) || pinned != gamendd) return;
        shared = new Shared;
        shared->mapping = desktop ? WideInputMapping::Desktop : WideInputMapping::CabinetMargins;
        initializeScopeControlFromEnvironment();
        if (scopeControlRequested()) configureMotion();
        // The observer calls this installer only after verifying and pinning
        // the supported ARK module. Native object checks happen per sample.
        arkBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"arkndd.dll"));
        originalUpdate = reinterpret_cast<UpdateInput>(moduleBase + 0x88AF0);
        originalX = reinterpret_cast<GetCoordinate>(moduleBase + 0x8ADA0);
        originalY = reinterpret_cast<GetCoordinate>(moduleBase + 0x8ADE0);
        // A partial installation stays pass-through: enable only all three.
        if (!detour::trampoline_try(originalX, &getX, &originalX) ||
                !detour::trampoline_try(originalY, &getY, &originalY) ||
                !detour::trampoline_try(originalUpdate, &updateInput, &originalUpdate)) {
            log_warning("bone-eater", "Wide input mapping disabled: paired hook installation incomplete");
            return;
        }
        shared->enabled.store(true);
        if (desktop) installNativeInputOwnershipObserver(gamendd);
        log_info("bone-eater", "Experimental {} input enabled; awaits native HUD fit and gun-to-UI mode. {}",
            desktop ? "desktop" : "wide", desktop ? "Verified ARK margins removed after calibration" : "Cabinet margins preserved");
    } catch (...) {}
}

void convertNativeWideInput(void* output, const std::array<std::uint8_t, 52>& original,
        std::int32_t arkWidth, std::int32_t arkHeight) noexcept {
    if (!shared || !shared->enabled.load() || !currentUpdate) return;
    try {
        const auto& context = *currentUpdate;
        std::uintptr_t config = 0;
        const auto geometry = render::readNativeHudGeometry();
        auto changed = original;
        const bool ready = context.supported && inputIdentity(context.input) &&
            gunUiMode(config) && config == context.config && sameGeometry(context.geometry, geometry) &&
            (shared->mapping != WideInputMapping::Desktop || hardwareArkMapping()) &&
            rewriteWideArkOutput(changed, arkWidth, arkHeight, geometry, shared->mapping);
        bool converted = false;
        OwnershipPoint convertedPoint {};
        if (ready) {
            std::array<float, 2> before {}, after {};
            std::memcpy(before.data(), original.data() + 0x24, sizeof(before));
            std::memcpy(after.data(), changed.data() + 0x24, sizeof(after));
            const auto scene = readScene();
            const auto now = GetTickCount64();
            const auto physical = readAimSnapshot();
            if (physical.sourceMode != shared->lastSourceMode ||
                physical.sourceIdentity.session != shared->lastSourceSession ||
                physical.sourceIdentity.generation < shared->lastSourceGeneration ||
                scene.scene != shared->lastMotionScene || scene.scope != shared->lastMotionScope ||
                context.input != shared->lastMotionInput) {
                resetScopeButtonEvents(); shared->motion.reset();
            }
            shared->lastMotionScene = scene.scene;
            shared->lastMotionScope = scene.scope;
            shared->lastMotionInput = context.input;
            shared->lastSourceMode = physical.sourceMode;
            shared->lastSourceSession = physical.sourceIdentity.session;
            shared->lastSourceGeneration = physical.sourceIdentity.generation;
            const bool usable = scene.known && aimUsableAt(physical, GunSourceClock::now(),
                std::chrono::milliseconds(250));
            if (scopeControlRequested() && scopeControlNativeReady() && scopeStartRoutingReady()) {
                bool held = false;
                if (physical.sourceMode == GunSourceMode::SelectedHid) held = physical.scopeIntentHeld();
                ScopeControlInput request;
                request.nowMs = now; request.contextIdentity = scene.scene;
                request.nativeScopeOwner = scene.scope; request.nativeInputOwner = context.input;
                request.buttonsHeld = held; request.usable = usable && scene.scopeAllowed;
                const auto control = physical.sourceMode == GunSourceMode::SelectedHid
                    ? publishScopeControl(request) : consumeScopeButtonEvents(request);
                if (control.enabled != shared->reportedScope || control.higher != shared->reportedHigh ||
                        request.usable != shared->reportedUsable) {
                    log_info("bone-eater", "Scope control: enabled={} higher={} usable={} armed={} scene={}",
                        control.enabled, control.higher, request.usable, control.armed, scene.id);
                    shared->reportedScope = control.enabled; shared->reportedHigh = control.higher;
                    shared->reportedUsable = request.usable;
                }
                if (request.usable) {
                    const auto filtered = shared->motion.update({after[0],after[1]}, control.enabled,
                        control.higher, now, shared->motionSettings);
                    after = {{static_cast<float>(filtered.x), static_cast<float>(filtered.y)}};
                    std::memcpy(changed.data() + 0x24, after.data(), sizeof(after));
                } else shared->motion.reset();
            } else shared->motion.reset();
            convertedPoint = after;
            const auto address = reinterpret_cast<std::uintptr_t>(output);
            if (address <= std::numeric_limits<std::uintptr_t>::max() - original.size()) {
                std::array<std::uint8_t,52> current {};
                SIZE_T written = 0;
                if (read(address, 0, current) && current == original) {
                    converted = WriteProcessMemory(GetCurrentProcess(), output, changed.data(), changed.size(), &written) && written == changed.size();
                    if (!converted) WriteProcessMemory(GetCurrentProcess(), output, original.data(), original.size(), &written);
                }
            }
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->desktop = {converted && usable, scene.gameplay, after[0], after[1], now};
            }
        }
        if (!converted) {
            resetScopeButtonEvents(); shared->motion.reset();
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->desktop = {};
        }
        convertedNativeInputOwnership(context.ownershipTicket, convertedPoint, converted);
        if (converted) recordNativePrecisionDomain(context.ownershipTicket, context.config, context.geometry);
        publishDomain(converted, context);
        if (converted && !shared->convertedLogged.exchange(true))
            log_info("bone-eater", "{} input active: ARK800x1280 to main1920x1080; native UI uses exact inverse LCD fit",
                shared->mapping == WideInputMapping::Desktop ? "Desktop" : "Wide");
        else if (!converted && context.supported) warnFault();
    } catch (...) {}
}

DesktopInputSnapshot readDesktopInputSnapshot() noexcept {
    if (!shared) return {};
    std::lock_guard<std::mutex> lock(shared->mutex);
    return shared->desktop;
}

bool scopeConsumesStart() noexcept {
    if (!shared || !currentUpdate || !currentUpdate->supported || !scopeControlRequested() ||
        !scopeControlNativeReady() || !scopeStartRoutingReady()) return false;
    if (std::find(shared->scopeKeys.begin(), shared->scopeKeys.end(), VK_RETURN) == shared->scopeKeys.end()) return false;
    const auto physical = readAimSnapshot();
    const auto scene = readScene();
    const auto scope = readScopeControlSnapshot();
    const auto now = GetTickCount64();
    return physical.sourceMode == GunSourceMode::Legacy && scene.scopeAllowed &&
        scope.contextIdentity == scene.scene && scope.nativeScopeOwner == scene.scope &&
        scope.nativeInputOwner == currentUpdate->input && now >= scope.updatedAtMs && now-scope.updatedAtMs <= 100 &&
        aimUsableAt(physical, GunSourceClock::now(), std::chrono::milliseconds(250));
}

} // namespace bone_eater::input
