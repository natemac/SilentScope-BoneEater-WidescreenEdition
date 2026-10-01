#include "render/native_options_backing.h"
#include "render/normal_start_backing_math.h"
#include "render/loading_screen_identity.h"
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
Update originalOptionsUpdate = nullptr;
Update originalBattleUpdate = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::atomic<ULONGLONG> nextReport {0};
using GuiBytes = std::array<unsigned char, 0x130>;
using Nodes = std::array<GuiBytes, 2>;
struct Route {
    std::uintptr_t manager = 0, parent = 0, child = 0;
    std::uint32_t loadingPhase = 0;
    bool operator==(const Route&) const = default;
};
struct Access { std::uintptr_t leaf = 0; ULONGLONG observed = 0; bool valid = false; };
struct Certificate {
    Route route;
    std::array<OptionsWhiteIdentity, 4> layers; // main, front, Top, Btm
    bool operator==(const Certificate&) const = default;
};
struct Observed {
    Certificate identity; Access access;
    ULONGLONG observed = 0; bool valid = false;
};
thread_local Observed current;
thread_local bool updating = false, committing = false, nestedUpdate = false;

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
        try { log_warning("bone-eater", "Options backing correction disabled: {}", reason); } catch (...) {}
    }
}

bool collectRoute(Route& route) noexcept {
    std::array<unsigned char, 0x48> manager {};
    std::array<unsigned char, 0x20> parent {};
    std::array<unsigned char, 0x14> child {};
    if (!read(base, 0x13DD000, route.manager) || !read(route.manager, 0, manager) ||
            field<std::uintptr_t>(manager, 0) != base + 0x10C6B38 ||
            field<std::int32_t>(manager, 0x3C) != -1) return false;
    route.parent = field<std::uintptr_t>(manager, 0x40);
    // Loading and its exact still-opaque resident-white handoff share one
    // certificate. Ordinary phase-2 gameplay remains excluded.
    if (field<std::uint32_t>(manager, 0x38) == 5) {
        std::uintptr_t vt = 0;
        if (!read(route.parent, 0, vt) || vt != base + 0x10C6E48 ||
                !read(route.parent, 0x1190, route.loadingPhase)) return false;
        if(route.loadingPhase==2) {
            LoadingScreenIdentity tail;
            if(!readLoadingScreen(base,[](auto p,auto o,auto& v){return read(p,o,v);},tail)||
               tail.scene!=route.parent||tail.phase!=2)return false;
        } else if(route.loadingPhase!=11&&route.loadingPhase!=1)return false;
        route.loadingPhase=1;
        route.child = route.parent;
        return true;
    }
    if (field<std::uint32_t>(manager, 0x38) != 3) return false;
    if (!read(route.parent, 0, parent) || field<std::uintptr_t>(parent, 0) != base + 0x10C7308 ||
            parent[8] || parent[9] || field<std::uint32_t>(parent, 0x10) != 2) return false;
    route.child = field<std::uintptr_t>(parent, 0x18);
    return read(route.child, 0, child) && field<std::uintptr_t>(child, 0) == base + 0x10CA6F0 &&
        !child[8] && !child[9] && (field<std::uint32_t>(child, 0x10) == 75 ||
        field<std::uint32_t>(child, 0x10) == 76 || field<std::uint32_t>(child, 0x10) == 77);
}

// Kind 0 is the sole writable resident main white. Other roles only certify
// the observed settled composition. Restore calls kind0 structural-only.
bool collectLayer(unsigned kind, OptionsWhiteIdentity& id, Nodes* output = nullptr,
        bool eligible = true, bool loading = false) noexcept {
    if (kind > 3) return false;
    const bool front = kind == 1, mask = kind >= 2;
    const unsigned index = front ? 2 : mask ? 1 : 0;
    std::uintptr_t vt = 0;
    if (!read(base, 0x13DCF48, id.resident) || !read(id.resident, front ? 0x120 : 0x128, id.layout)) return false;
    std::array<unsigned char, 0x70> layout {};
    std::uint32_t display = 0, count = 0; std::array<char, 32> rootName {};
    if (!read(id.layout, 0, layout) || field<std::uintptr_t>(layout, 0) != base + 0x10C9FA8 ||
            (eligible && (field<std::uint32_t>(layout, 0x18) != 2 || field<std::uint32_t>(layout, 0x1C) != 1)) ||
            field<std::uint32_t>(layout, 0x64 + index * 4) != (front ? 0xB8Cu : mask ? 0xBF6u : 0xBAAu) ||
            !read(id.layout, 0x3C90, display) || display != (front ? 1u : 0u) ||
            !read(id.layout, 0x3C98, count) || count != (front ? 3u : 2u) ||
            !read(id.layout, 0x368, rootName) || !std::memchr(rootName.data(), 0, rootName.size()) ||
            std::strcmp(rootName.data(), "Root") || !read(id.layout, 0x3F8 + index * 4, id.group) ||
            !id.group || id.group >= 32 || !read(id.layout, 0x428 + index * 8, id.root)) return false;
    Nodes nodes {}; std::array<unsigned char, 0xD0> root {};
    if (!read(id.root, 0, root)) return false;
    std::memcpy(nodes[0].data(), root.data(), root.size());
    if (field<std::uintptr_t>(root, 0) != base + 0x10CEB48 || field<std::uintptr_t>(root, 8) != id.root ||
            field<std::uintptr_t>(root, 0x10) || field<std::uintptr_t>(root, 0x20) || field<std::uintptr_t>(root, 0x28) ||
            field<std::uint32_t>(root, 0x3C) != id.group || !named(nodes[0], "Root") ||
            field<std::array<float, 2>>(root, 0x98) != (kind == 0 ? std::array<float, 2>{{20,20}} : std::array<float, 2>{{5,5}})) return false;
    const auto first = field<std::uintptr_t>(root, 0x18);
    GuiBytes top {};
    if (mask && (!read(first, 0, top) || field<std::uintptr_t>(top, 0) != base + 0x10CEBA8 ||
            field<std::uintptr_t>(top, 8) != first || field<std::uintptr_t>(top, 0x10) != id.root ||
            field<std::uintptr_t>(top, 0x18) || field<std::uintptr_t>(top, 0x20) || !named(top, "Top") ||
            field<std::uint32_t>(top, 0x3C) != id.group)) return false;
    const auto second = mask ? field<std::uintptr_t>(top, 0x28) : 0;
    if (mask && (!second || second == first)) return false;
    id.leaf = kind == 3 ? second : first; id.sibling = mask ? (kind == 3 ? first : second) : 0;
    auto& leaf = nodes[1];
    if (!read(id.leaf, 0, leaf) || field<std::uintptr_t>(leaf, 0) != base + 0x10CEBA8 ||
            field<std::uintptr_t>(leaf, 8) != id.leaf || field<std::uintptr_t>(leaf, 0x10) != id.root ||
            field<std::uintptr_t>(leaf, 0x18) || field<std::uintptr_t>(leaf, 0x20) != (kind == 3 ? first : 0) ||
            field<std::uintptr_t>(leaf, 0x28) != (kind == 2 ? second : 0) ||
            field<std::uint32_t>(leaf, 0x3C) != id.group || !named(leaf, mask ? (kind == 2 ? "Top" : "Btm") : "white") ||
            field<std::array<float, 2>>(leaf, 0x98) != (front ? std::array<float, 2>{{968,1566}} : mask ?
                std::array<float, 2>{{850,64}} : std::array<float, 2>{{1000,1480}})) return false;
    id.sprite = field<std::uintptr_t>(leaf, 0x110); id.record = field<std::uintptr_t>(leaf, 0x30);
    std::uintptr_t camera = 0, backlink = 0; std::uint32_t selector = 0, color = 0;
    std::uint64_t key = 0; std::array<std::uint16_t, 2> extent {};
    if (!read(id.sprite, 0, vt) || vt != base + 0x10CC0D8 ||
            !read(id.sprite, 0x570, selector) || selector != (front ? 2u : 0u) ||
            !read(id.sprite, 0x1A8, id.camera) || !read(id.camera, 0, vt) || vt != base + 0x6FDF48 ||
            (eligible && (!read(base, 0x13DB5A0 + selector * 8, camera) || id.camera != camera)) ||
            !read(id.sprite, 0x308, id.material) || !read(id.material, 0, vt) || vt != base + 0x706570 ||
            !read(id.material, 0x50, id.texture) || !read(id.texture, 0, key) ||
            key != (mask ? 0x5553455200055180ULL : 0x5553455200052B00ULL) ||
            !read(id.texture, 0x18, extent) || extent != std::array<std::uint16_t, 2>{{64,64}} ||
            !read(id.record, 0, vt) || vt != base + 0x10CEC08 ||
            !read(id.record, 0x40, backlink) || backlink != id.sprite) return false;
    if (eligible) {
        if (root[0xC9] != 1 || leaf[0xC9] != 1 || !read(id.record, 8, color) ||
                (mask ? (color >> 24) != 0 : front ? color != (loading ? 0u : 0xFF000000u) : (color & 0xFFFFFFu) != 0xFFFFFFu) ||
                (front && field<float>(layout, 0x58) != (loading ? 0.0f : 1.0f))) return false;
        const float fade = field<float>(layout, 0x58);
        if (!mask && (!std::isfinite(fade) || fade < 0 || fade > 1)) return false;
        for (const auto* node : {&nodes[0], &leaf}) {
            if (field<std::array<float, 2>>(*node, 0x90) != std::array<float, 2>{} ||
                    field<std::array<float, 2>>(*node, 0xB0) != std::array<float, 2>{}) return false;
            for (const auto offset : {0xB8u,0xBCu}) {
                const float alpha = field<float>(*node, offset);
                if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) return false;
            }
        }
        if (field<float>(leaf, 0xB8) != 1 || (kind != 0 && field<float>(leaf, 0xBC) != (mask || (front && loading) ? 0.0f : 1.0f)) ||
                (front && !loading && (field<float>(root, 0xB8) != 1 || field<float>(root, 0xBC) != 1))) return false;
        // The native resident fade is applied to Root's local AND cached alpha.
        // Requiring an opaque main Root drops the width correction for the
        // entire entry/exit fade, exposing the original wider white strips.
        // Do not require cached Root alpha to equal the current layout fade:
        // native Update precedes GUI propagation. Both have already passed
        // finite [0,1] checks, and this correction never writes either value.
    }
    if (output) *output = nodes; return true;
}
bool collect(Certificate& certificate, Nodes* mainNodes = nullptr) noexcept {
    std::uintptr_t config = 0, frame = 0, vt = 0; unsigned char twoDisplay = 0;
    std::array<std::uint16_t, 2> extent {};
    if (!read(base, 0x13DCF58, config) || !read(config, 0, vt) || vt != base + 0x10C5FA8 ||
            !read(config, 0x1373, twoDisplay) || twoDisplay != 1 ||
            !read(base, 0x12E24E8, frame) || !read(frame, 0, vt) || vt != base + 0x703E78 ||
            !read(frame, 0x28, extent) || extent != std::array<std::uint16_t, 2>{{1920,1080}} ||
            !collectRoute(certificate.route)) return false;
    for (unsigned kind = 0; kind < 4; ++kind)
        if (!collectLayer(kind, certificate.layers[kind], kind == 0 ? mainNodes : nullptr, true, certificate.route.loadingPhase != 0)) return false;
    const auto& main = certificate.layers[0]; const auto& front = certificate.layers[1];
    const auto& top = certificate.layers[2]; const auto& bottom = certificate.layers[3];
    return main.resident == front.resident && main.resident == top.resident && main.resident == bottom.resident &&
        main.layout != front.layout && main.layout == top.layout && main.layout == bottom.layout &&
        main.root != front.root && main.root != top.root && front.root != top.root && top.root == bottom.root &&
        top.sibling == bottom.leaf && bottom.sibling == top.leaf;
}
bool pose(const Nodes& nodes, RearIlluminationPose& result) noexcept {
    const auto& root = nodes[0]; const auto& white = nodes[1];
    if (field<std::array<float, 2>>(white, 0x60) != std::array<float, 2>{{-100,-100}} ||
            field<std::array<float, 2>>(white, 0xA0) != std::array<float, 2>{{1,1}}) return false;
    const auto position = field<std::array<float, 2>>(root, 0x70);
    const auto scale = field<std::array<float, 2>>(root, 0xA8);
    if (position != field<std::array<float, 2>>(root, 0x60) || scale != field<std::array<float, 2>>(root, 0xA0)) return false;
    result.position = field<std::array<float, 2>>(white, 0x70); result.scale = field<std::array<float, 2>>(white, 0xA8);
    if (result.scale != scale) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const double expected = position[i] - 100.0 * scale[i];
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
    try { log_info("bone-eater", "Options backing pid={} ready={} applied={} restored={} rejected={} failed={}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load()); } catch (...) {}
}
void observe(void* object, const Observed& previous, const Route& before) noexcept {
    if (failed.load() || nestedUpdate) return;
    Certificate first, second;
    if (!collect(first) || first.route.child != reinterpret_cast<std::uintptr_t>(object) ||
            !collect(second) || first != second || first.route != before) return;
    DWORD expected = 0; ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) { failure("Options update changed thread"); return; }
    current.identity = first; current.observed = GetTickCount64(); current.valid = true;
    if (previous.valid && previous.identity == first) current.access = previous.access;

}
void updateObserved(void* object, std::uintptr_t caller) {
    if (!enabled.load(std::memory_order_acquire)) { originalOptionsUpdate(object); return; }
    if (updating) { current = {}; nestedUpdate = true; originalOptionsUpdate(object); return; }
    const auto previous = current;
    current = {}; updating = true; nestedUpdate = false;
    Route before;
    const bool eligibleBefore = caller == base + 0xCC478 && collectRoute(before) &&
        before.child == reinterpret_cast<std::uintptr_t>(object);
    __try {
        originalOptionsUpdate(object);
        if (eligibleBefore) observe(object, previous, before);
    } __finally { updating = false; }
    report();
}
void __fastcall update(void* object) {
    updateObserved(object, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
}
void __fastcall updateBattle(void* object) {
    if (!enabled.load(std::memory_order_acquire)) { originalBattleUpdate(object); return; }
    if (updating) { current = {}; nestedUpdate = true; originalBattleUpdate(object); return; }
    const auto previous = current;
    current = {}; updating = true; nestedUpdate = false;
    Route before;
    const bool eligibleBefore = collectRoute(before) && before.parent == before.child &&
        before.child == reinterpret_cast<std::uintptr_t>(object);
    __try {
        originalBattleUpdate(object);
        if (eligibleBefore) observe(object, previous, before);
    } __finally { updating = false; }
    report();
}
template<std::size_t N> bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {}; return read(base, rva, actual) && actual == expected;
}
} // namespace

OptionsBackingAlignment beginNativeOptionsBackingCommit(void* object, std::uintptr_t caller) noexcept {
    OptionsBackingAlignment token;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || updating || committing || !current.valid ||
            caller != base + 0x1E634D) return token;
    if (reinterpret_cast<std::uintptr_t>(object) != current.identity.layers[0].leaf) return token;
    const auto observed = current;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return token;
    const auto hud = readNativeHudGeometry();
    Certificate id; Nodes nodes {}; RearIlluminationPose before, after;
    if (!sameHud(hud, hud) || !collect(id, &nodes) || id != observed.identity || !pose(nodes, before) ||
            !fitNormalStartWhiteHorizontal(hud.left, hud.top, hud.width, hud.height, before, after)) { ++rejected; return token; }
    // Commit the solid loading fade at full main width. Fixing this leaf before
    // native vertex generation also covers an already-rendered white frame
    // presented after the scene/fade fields advance. Options retain panel fit.
    if(id.route.loadingPhase) { after.position[0]=0.1f;after.scale[0]=1920.f/1000.f; }
    const auto access = permission(id.layers[0].leaf, observed.access);
    Certificate repeated; Nodes again {}; RearIlluminationPose repeatedPose;
    if (!access.valid || !collect(repeated, &again) || repeated != id || !pose(again, repeatedPose) ||
            repeatedPose.position != before.position || repeatedPose.scale != before.scale ||
            !sameHud(hud, readNativeHudGeometry())) { ++rejected; return token; }
    const auto now = GetTickCount64();
    if (!current.valid || current.identity != id || current.observed != observed.observed ||
            now - observed.observed > 100 || now - access.observed > 100 || updating || committing) { ++rejected; return token; }
    current.access = access;
    token.identity = id.layers[0]; token.permission = {id.layers[0].leaf, true}; token.original = before; token.attempted = true;
    committing = true;
    if (!writeHorizontal(token.permission, after)) {
        restoreNativeOptionsBackingCommit(token); failure("White horizontal write failed"); return {};
    }
    if (++applied == 1) {
        try { log_info("bone-eater", "Resident backing active: verified Options or loading certificate, front X {} width {}; native alpha/vertical fields retained", hud.left, hud.width); } catch (...) {}
    }
    return token;
}

void restoreNativeOptionsBackingCommit(const OptionsBackingAlignment& token) noexcept {
    if (!token.attempted) return;
    OptionsWhiteIdentity id;
    // Do not require current phase/config/fit/age, UV, alpha or vertical pose.
    // Restore only the borrowed X fields; native changes to Y remain untouched.
    if (token.permission.sprite != token.identity.leaf || !collectLayer(0, id, nullptr, false) || id != token.identity)
        failure("White structural ownership changed during native commit");
    else if (!writeHorizontal(token.permission, token.original)) failure("White horizontal restoration failed");
    else ++restored;
    committing = false;
}

void installNativeOptionsBacking(void* module, bool sharedCommitReady) noexcept {
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_OPTIONS_BACKING", setting, 8) != 1 || setting[0] != L'1' || originalOptionsUpdate) return;
        if (!sharedCommitReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", setting, 8) != 3 || std::wcscmp(setting, L"fit")) {
            failure("requires native HUD fit and shared GUI commit hook"); return;
        }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 36> updateBytes {{
            0x40,0x55,0x56,0x57,0x48,0x8d,0xac,0x24,0x60,0xfe,0xff,0xff,0x48,0x81,0xec,0xc0,0x02,0x00,0x00,
            0x48,0x8b,0x05,0x5e,0x78,0x16,0x01,0x48,0x33,0xc4,0x48,0x89,0x85,0x50,0x01,0x00,0x00}};
        constexpr std::array<unsigned char, 18> updateCaller {{
            0x48,0x8b,0x4b,0x18,0x48,0x8b,0x01,0xff,0x50,0x28,0x48,0x8b,0x43,0x18,0x83,0x78,0x10,0x66}};
        constexpr std::array<unsigned char, 22> battleBytes {{
            0x48,0x8b,0xc4,0x55,0x41,0x55,0x41,0x56,0x48,0x8d,0xa8,0x38,0xff,0xff,0xff,
            0x48,0x81,0xec,0xb0,0x01,0x00,0x00}};
        constexpr std::array<unsigned char, 13> commitCaller {{0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        std::uintptr_t slot = 0; float offset = 0;
        if (!bytesEqual(0x155850, updateBytes) || !bytesEqual(0xCC46E, updateCaller) ||
                !bytesEqual(0xA79A0, battleBytes) ||
                !bytesEqual(0x1E6340, commitCaller) || !bytesEqual(0x1E55BB, geometryBytes) ||
                !read(base, 0x10CA6F0 + 0x28, slot) || slot != base + 0x155850 ||
                !read(base, 0x10D0AA4, offset) || offset != 0.1f) { failure("native update/commit guards differ"); return; }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { failure("module pin failed"); return; }
        originalOptionsUpdate = reinterpret_cast<Update>(base + 0x155850);
        if (!detour::trampoline_try(originalOptionsUpdate, &update, &originalOptionsUpdate)) { failure("Options update hook failed"); return; }
        originalBattleUpdate = reinterpret_cast<Update>(base + 0xA79A0);
        if (!detour::trampoline_try(originalBattleUpdate, &updateBattle, &originalBattleUpdate)) { failure("Battle handoff update hook failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Resident backing enabled: Options panel fit; loading white full width including opaque handoff; native fade timing retained");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
