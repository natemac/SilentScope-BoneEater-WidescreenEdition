#include "render/native_menu_margin.h"
#include "render/menu_margin_math.h"
#include "render/native_hud.h"
#include "render/native_viewport.h"
#include "diagnostics/output_policy.h"
#include "util/detour.h"
#include "util/logging.h"
#include <atomic>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <limits>

namespace bone_eater::render {
namespace {
using Update = void(__fastcall*)(void*);
Update originalMenuUpdate = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::atomic<ULONGLONG> nextReport {0};
std::array<std::atomic<std::uint64_t>,4> roleApplied {};
using GuiBytes = std::array<unsigned char, 0x130>;
using Nodes = std::array<GuiBytes, 5>; // Root, BG, BG2, BG3, White.
struct Access { std::uintptr_t leaf = 0; ULONGLONG observed = 0; bool valid = false; };
struct Observed {
    MenuMarginIdentity identity;
    Access access;
    ULONGLONG observed = 0;
    bool valid = false;
};
thread_local Observed current;
thread_local std::array<Observed, 3> backgrounds;
thread_local bool updating = false, committing = false, nestedUpdate = false;
constexpr std::array<float, 8> whiteUv {{
    0.78271484375f,0.00048828125f, 0.78271484375f,0.00146484375f,
    0.78369140625f,0.00146484375f, 0.78369140625f,0.00048828125f}};
constexpr std::array<std::array<float, 8>, 3> backgroundUv {{
    {{0.3916015625f,0.00048828125f,0.3916015625f,0.47265625f,
      0.78125f,0.47265625f,0.78125f,0.00048828125f}},
    {{0.00048828125f,0.00048828125f,0.00048828125f,0.47265625f,
      0.39013671875f,0.47265625f,0.39013671875f,0.00048828125f}},
    {{0.00048828125f,0.00048828125f,0.00048828125f,0.47265625f,
      0.39013671875f,0.47265625f,0.39013671875f,0.00048828125f}}
}};

template<class T> bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}
template<class T, std::size_t N> T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T result {}; std::memcpy(&result, bytes.data() + offset, sizeof(result)); return result;
}
bool named(const GuiBytes& bytes, const char* name) noexcept {
    const auto* actual = reinterpret_cast<const char*>(bytes.data() + 0x40);
    return std::memchr(actual, 0, 32) && !std::strcmp(actual, name);
}
void failure(const char* reason) noexcept {
    if (!failed.exchange(true)) {
        try { log_warning("bone-eater", "Main_Quarter margin correction disabled: {}", reason); } catch (...) {}
    }
}

// Structural collection is also used during restoration. Scene eligibility,
// fade/visibility/UV and output configuration are deliberately optional there.
bool collect(MenuMarginIdentity& id, Nodes* output = nullptr, bool eligible = false, unsigned role = 3) noexcept {
    if (role > 3) return false;
    std::array<unsigned char, 0x48> manager {};
    std::array<unsigned char, 0x28> menu {};
    if (!read(base, 0x13DD000, id.manager) || !read(id.manager, 0, manager) ||
            field<std::uintptr_t>(manager, 0) != base + 0x10C6B38) return false;
    id.menu = field<std::uintptr_t>(manager, 0x40);
    if (!read(id.menu, 0, menu) || field<std::uintptr_t>(menu, 0) != base + 0x10C7030) return false;
    const auto secondary = field<std::uint32_t>(menu, 0x14);
    // Settled screens plus observed entry/exit phases of the same exact
    // Main_Quarter owner. Keep the white backing fitted during animation;
    // unrelated phases, pending scene changes and unknown owners stay native.
    if (eligible && (field<std::uint32_t>(manager, 0x38) != 2 || field<std::int32_t>(manager, 0x3C) != -1 ||
            menu[8] != 0 || menu[9] != 0 || field<std::uint32_t>(menu, 0x10) != 1 ||
            (secondary != 1 && secondary != 2 && secondary != 3 && secondary != 9 &&
             secondary != 10 && secondary != 11 && secondary != 12 && secondary != 14 && secondary != 15 && secondary != 16 && secondary != 17 && secondary != 18))) return false;
    std::uintptr_t vt = 0;
    std::array<std::uint16_t, 2> extent {};
    if (eligible) {
        std::uintptr_t config = 0, frame = 0; unsigned char twoDisplay = 0;
        if (!read(base, 0x13DCF58, config) || !read(config, 0, vt) || vt != base + 0x10C5FA8 ||
                !read(config, 0x1373, twoDisplay) || twoDisplay != 1 ||
                !read(base, 0x12E24E8, frame) || !read(frame, 0, vt) || vt != base + 0x703E78 ||
                !read(frame, 0x28, extent) || extent != std::array<std::uint16_t, 2>{{1920, 1080}}) return false;
    }
    id.layout = field<std::uintptr_t>(menu, 0x20);
    std::array<unsigned char, 0x70> layout {};
    std::array<char, 32> rootName {};
    std::uint32_t display = 0, count = 0;
    if (!read(id.layout, 0, layout) || field<std::uintptr_t>(layout, 0) != base + 0x10C9FA8 ||
            (eligible && (field<std::uint32_t>(layout, 0x18) != 2 || field<std::uint32_t>(layout, 0x1C) != 1)) ||
            field<std::uint32_t>(layout, 0x64) != 0xBA0 ||
            !read(id.layout, 0x3C90, display) || display != 0 || !read(id.layout, 0x3C98, count) || count != 1 ||
            !read(id.layout, 0x368, rootName) || !std::memchr(rootName.data(), 0, rootName.size()) ||
            std::strcmp(rootName.data(), "Root") || !read(id.layout, 0x3F8, id.group) || !id.group || id.group >= 32 ||
            !read(id.layout, 0x428, id.root)) return false;
    Nodes nodes {};
    auto& root = nodes[0];
    // GuiRect's proven base allocation is only0xD0; do not read GuiSprite's
    // trailing storage or a neighboring object/page merely to reuse a buffer.
    std::array<unsigned char, 0xD0> rootBytes {};
    if (!read(id.root, 0, rootBytes)) return false;
    std::memcpy(root.data(), rootBytes.data(), rootBytes.size());
    if (field<std::uintptr_t>(root, 0) != base + 0x10CEB48 ||
            field<std::uintptr_t>(root, 8) != id.root || field<std::uintptr_t>(root, 0x10) ||
            field<std::uintptr_t>(root, 0x20) || field<std::uintptr_t>(root, 0x28) ||
            field<std::uint32_t>(root, 0x3C) != id.group || !named(root, "Root") ||
            field<std::array<float, 2>>(root, 0x98) != std::array<float, 2>{{5, 5}}) return false;
    constexpr std::array<const char*, 4> names {{"BG", "BG2", "BG3", "White"}};
    auto pointer = field<std::uintptr_t>(root, 0x18);
    std::uintptr_t previous = 0;
    for (unsigned i = 0; i < 4; ++i) {
        auto& node = nodes[i + 1];
        if (!pointer || pointer == id.root) return false;
        for (unsigned j = 0; j < i; ++j) if (pointer == id.leaves[j]) return false;
        if (!read(pointer, 0, node) || field<std::uintptr_t>(node, 0) != base + 0x10CEBA8 ||
                field<std::uintptr_t>(node, 8) != pointer || field<std::uintptr_t>(node, 0x10) != id.root ||
                field<std::uintptr_t>(node, 0x18) || field<std::uintptr_t>(node, 0x20) != previous ||
                field<std::uint32_t>(node, 0x3C) != id.group || !named(node, names[i]) ||
                field<std::array<float, 2>>(node, 0x98) != std::array<float, 2>{{800, i == 3 ? 1480.0f : 969.0f}}) return false;
        id.leaves[i] = pointer; previous = pointer; pointer = field<std::uintptr_t>(node, 0x28);
    }
    if (pointer) return false;
    id.sprite = field<std::uintptr_t>(nodes[role + 1], 0x110);
    id.record = field<std::uintptr_t>(nodes[role + 1], 0x30);
    std::uintptr_t camera = 0;
    std::uint32_t selector = 0;
    std::uint64_t key = 0;
    std::array<unsigned char, 0x78> record {};
    if (!read(id.sprite, 0, vt) || vt != base + 0x10CC0D8 ||
            !read(id.sprite, 0x570, selector) || selector != 0 || !read(id.sprite, 0x1A8, id.camera) ||
            !read(base, 0x13DB5A0, camera) || id.camera != camera || !read(camera, 0, vt) || vt != base + 0x6FDF48 ||
            !read(id.sprite, 0x308, id.material) || !read(id.material, 0, vt) || vt != base + 0x706570 ||
            !read(id.material, 0x50, id.texture) || !read(id.texture, 0, key) ||
            key != 0x5553455200061280ULL + (role == 3 ? 0 : role * 0x80ULL) ||
            !read(id.texture, 0x18, extent) || extent != std::array<std::uint16_t, 2>{{2048, 2048}} ||
            !read(id.record, 0, record) || field<std::uintptr_t>(record, 0) != base + 0x10CEC08 ||
            field<std::uintptr_t>(record, 0x40) != id.sprite ||
            (eligible && (role == 3 ? field<std::array<float, 8>>(record, 0x58) != whiteUv :
                (field<std::array<float, 8>>(record, 0x58) != backgroundUv[0] &&
                 field<std::array<float, 8>>(record, 0x58) != backgroundUv[1])))) return false;
    if (output) *output = nodes;
    return true;
}

bool pose(const Nodes& nodes, RearIlluminationPose& result, unsigned role = 3) noexcept {
    if (role > 3) return false;
    const float localY = role == 3 ? -100.0f : 88.0f;
    const auto& root = nodes[0]; const auto& white = nodes[role + 1];
    if (root[0xC9] != 1 || white[0xC9] != 1 ||
            field<std::array<float, 2>>(white, 0x60) != std::array<float, 2>{{0, localY}} ||
            field<std::array<float, 2>>(white, 0xA0) != std::array<float, 2>{{1, 1}}) return false;
    for (const auto* node : {&root, &white}) {
        if (field<std::array<float, 2>>(*node, 0x90) != std::array<float, 2>{} ||
                field<std::array<float, 2>>(*node, 0xB0) != std::array<float, 2>{}) return false;
        for (const auto offset : {0xB8u, 0xBCu}) {
            const auto alpha = field<float>(*node, offset);
            if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) return false;
        }
    }
    const auto rootPosition = field<std::array<float, 2>>(root, 0x70);
    const auto rootScale = field<std::array<float, 2>>(root, 0xA8);
    if (rootPosition != field<std::array<float, 2>>(root, 0x60) ||
            rootScale != field<std::array<float, 2>>(root, 0xA0)) return false;
    result.position = field<std::array<float, 2>>(white, 0x70);
    result.scale = field<std::array<float, 2>>(white, 0xA8);
    if (result.scale != rootScale) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const double expected = rootPosition[i] + (i ? localY : 0.0) * rootScale[i];
        if (!std::isfinite(expected) || !std::isfinite(result.position[i]) || std::fabs(result.position[i] - expected) > 0.001) return false;
    }
    return true;
}

Access permission(std::uintptr_t leaf, const Access& previous) noexcept {
    const auto now = GetTickCount64();
    if (!spriteGeometryAddressValid(leaf)) return {};
    if (previous.valid && previous.leaf == leaf && now >= previous.observed && now - previous.observed <= 50) return previous;
    checked_data_detail::Region region;
    for (const auto offset : {std::uintptr_t(0x70), std::uintptr_t(0xA8)}) {
        const auto address = leaf + offset;
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
        if (sizeof(float) > region.size - (address - region.start)) return {};
    }
    if (GetTickCount64() - now > 100) return {};
    return {leaf, now, true};
}
bool writeComponent(const SpriteGeometryPermission& access, std::uintptr_t offset, float value) noexcept {
    if (!access.valid || !spriteGeometryAddressValid(access.sprite) || (offset != 0x70 && offset != 0xA8)) return false;
    __try {
        auto* target = reinterpret_cast<volatile unsigned char*>(access.sprite + offset);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (unsigned i = 0; i < sizeof(value); ++i) target[i] = bytes[i];
        for (unsigned i = 0; i < sizeof(value); ++i) if (target[i] != bytes[i]) return false;
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool writeHorizontal(const SpriteGeometryPermission& access, const RearIlluminationPose& value) noexcept {
    const bool x = writeComponent(access, 0x70, value.position[0]);
    const bool scale = writeComponent(access, 0xA8, value.scale[0]);
    return x && scale; // Always attempt both, including partial-apply recovery.
}
bool sameHud(const NativeHudGeometry& a, const NativeHudGeometry& b) noexcept {
    return a.valid && b.valid && a.helper == b.helper && a.sprite == b.sprite &&
        a.mainWidth == 1920 && b.mainWidth == 1920 && a.mainHeight == 1080 && b.mainHeight == 1080 &&
        a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64(); auto expected = nextReport.load();
    if (now < expected || !nextReport.compare_exchange_strong(expected, now + 5000)) return;
    try { log_info("bone-eater", "Main_Quarter margin pid={} ready={} applied={} restored={} rejected={} failed={}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load()); } catch (...) {}
    try { log_info("bone-eater", "Main_Quarter role commits BG={} BG2={} BG3={} White={}",
        roleApplied[0].load(),roleApplied[1].load(),roleApplied[2].load(),roleApplied[3].load()); } catch (...) {}
}
void observe(void* object, const Observed& previous) noexcept {
    if (failed.load() || nestedUpdate) return;
    MenuMarginIdentity first, second;
    if (!collect(first, nullptr, true) || first.menu != reinterpret_cast<std::uintptr_t>(object) ||
            !collect(second, nullptr, true) || first != second) return;
    DWORD expected = 0; ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) { failure("MainMenu update changed thread"); return; }
    current.identity = first; current.observed = GetTickCount64(); current.valid = true;
    if (previous.valid && previous.identity == first) current.access = previous.access;
    for (unsigned role = 0; role < backgrounds.size(); ++role) {
        MenuMarginIdentity a, b;
        if (collect(a, nullptr, true, role) && collect(b, nullptr, true, role) && a == b && a.menu == first.menu)
            backgrounds[role] = {a, {}, current.observed, true};
    }
}
void updateObserved(void* object, std::uintptr_t caller) {
    if (!enabled.load(std::memory_order_acquire)) { originalMenuUpdate(object); return; }
    if (updating) { current = {}; backgrounds = {}; nestedUpdate = true; originalMenuUpdate(object); return; }
    const auto previous = current;
    current = {}; backgrounds = {}; updating = true; nestedUpdate = false;
    __try {
        originalMenuUpdate(object);
        if (caller == base + 0xA55B8) observe(object, previous);
    } __finally { updating = false; }
    report();
}
void __fastcall update(void* object) {
    updateObserved(object, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
template<std::size_t N> bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {}; return read(base, rva, actual) && actual == expected;
}
} // namespace

MenuMarginAlignment beginNativeMenuMarginCommit(void* object, std::uintptr_t caller) noexcept {
    MenuMarginAlignment token;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || updating || committing || !current.valid ||
            caller != base + 0x1E634D) return token;
    unsigned role = 0;
    for (; role < 4; ++role) if (reinterpret_cast<std::uintptr_t>(object) == current.identity.leaves[role]) break;
    if (role == 4) return token;
    auto& selected = role == 3 ? current : backgrounds[role];
    const auto observed = selected;
    if (!observed.valid) return token;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return token;
    const auto hud = readNativeHudGeometry();
    MenuMarginIdentity id; Nodes nodes {}; RearIlluminationPose before, after;
    if (!sameHud(hud, hud) || !collect(id, &nodes, true, role) || id != observed.identity || !pose(nodes, before, role) ||
            !fitMenuWhiteHorizontal(hud.left, hud.top, hud.width, hud.height, before, after, role == 3)) { ++rejected; return token; }
    const auto access = permission(id.leaves[role], observed.access);
    MenuMarginIdentity repeated; Nodes again {}; RearIlluminationPose repeatedPose;
    if (!access.valid || !collect(repeated, &again, true, role) || repeated != id || !pose(again, repeatedPose, role) ||
            repeatedPose.position != before.position || repeatedPose.scale != before.scale ||
            !sameHud(hud, readNativeHudGeometry())) { ++rejected; return token; }
    const auto now = GetTickCount64();
    if (!selected.valid || selected.identity != id || selected.observed != observed.observed ||
            now - observed.observed > 100 || now - access.observed > 100 || updating || committing) { ++rejected; return token; }
    selected.access = access;
    token.identity = id; token.permission = {id.leaves[role], true}; token.original = before; token.attempted = true; token.role = role;
    committing = true;
    if (!writeHorizontal(token.permission, after)) {
        restoreNativeMenuMarginCommit(token); failure("White horizontal write failed"); return {};
    }
    if (++applied == 1) {
        try { log_info("bone-eater", "Main_Quarter layer alignment active: exact scene 2 / primary 1, observed transition and settled phases; front X {} width {}; native vertical/fades retained", hud.left, hud.width); } catch (...) {}
    }
    ++roleApplied[role];
    return token;
}

void restoreNativeMenuMarginCommit(const MenuMarginAlignment& token) noexcept {
    if (!token.attempted) return;
    MenuMarginIdentity id;
    // Do not require current phase/config/fit/age, UV, alpha or vertical pose.
    // Restore only the borrowed X fields; native changes to Y remain untouched.
    if (token.role > 3 || token.permission.sprite != token.identity.leaves[token.role] || !collect(id, nullptr, false, token.role) || id != token.identity)
        failure("White structural ownership changed during native commit");
    else if (!writeHorizontal(token.permission, token.original)) failure("White horizontal restoration failed");
    else ++restored;
    committing = false;
}

void installNativeMenuMargin(void* module, bool sharedCommitReady) noexcept {
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_MENU_MARGIN", setting, 8) != 1 || setting[0] != L'1' || originalMenuUpdate) return;
        if (!sharedCommitReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", setting, 8) != 3 || std::wcscmp(setting, L"fit")) {
            failure("requires native HUD fit and shared GUI commit hook"); return;
        }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 36> updateBytes {{
            0x40,0x55,0x56,0x57,0x48,0x8d,0xac,0x24,0xc0,0xfa,0xff,0xff,0x48,0x81,0xec,0x40,0x06,0x00,
            0x00,0x48,0x8b,0x05,0x7e,0x8b,0x20,0x01,0x48,0x33,0xc4,0x48,0x89,0x85,0x00,0x05,0x00,0x00}};
        constexpr std::array<unsigned char, 6> updateCaller {{0x48,0x8b,0x01,0xff,0x50,0x28}};
        constexpr std::array<unsigned char, 13> commitCaller {{0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        std::uintptr_t slot = 0; float offset = 0;
        if (!bytesEqual(0xB4530, updateBytes) || !bytesEqual(0xA55B2, updateCaller) ||
                !bytesEqual(0x1E6340, commitCaller) || !bytesEqual(0x1E55BB, geometryBytes) ||
                !read(base, 0x10C7030 + 0x28, slot) || slot != base + 0xB4530 ||
                !read(base, 0x10D0AA4, offset) || offset != 0.1f) { failure("native update/commit guards differ"); return; }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { failure("module pin failed"); return; }
        originalMenuUpdate = reinterpret_cast<Update>(base + 0xB4530);
        if (!detour::trampoline_try(originalMenuUpdate, &update, &originalMenuUpdate)) { failure("MainMenu update hook failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Main_Quarter margin correction enabled: exact White/BG/BG2/BG3 in verified MainMenu entry, selection and exit phases; native vertical geometry and fades retained");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
