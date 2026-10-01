#include "render/native_menu_backing.h"
#include "render/ranking_backing_math.h"
#include "render/native_hud.h"
#include "render/native_viewport.h"
#include "diagnostics/output_policy.h"
#include "util/detour.h"
#include "util/logging.h"
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bone_eater::render {
namespace {
using Update = void(__fastcall*)(void*);
Update originalSceneUpdate = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::array<std::atomic<std::uint64_t>, 2> maskApplied {}, maskRestored {};
std::atomic<ULONGLONG> nextReport {0};
using GuiBytes = std::array<unsigned char, 0x130>;
struct Route {
    std::uintptr_t manager = 0, title = 0, rank = 0;
    bool operator==(const Route&) const = default;
};
struct Access {
    std::uintptr_t leaf = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};
struct Observed {
    Route route;
    RankingWhiteIdentity white;
    Access access;
    std::array<RankingWhiteIdentity, 2> masks {};
    std::array<Access, 2> maskAccess {};
    bool masksValid = false;
    ULONGLONG observed = 0;
    bool valid = false;
};
thread_local Observed current;
thread_local bool updating = false, committing = false;

template<class T> bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}
template<class T, std::size_t N> T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T value {}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
void failure(const char* reason) noexcept {
    if (!failed.exchange(true)) {
        try { log_warning("bone-eater", "Ranking backing correction disabled: {}", reason); } catch (...) {}
    }
}

bool collectRoute(Route& route) noexcept {
    std::array<unsigned char, 0x48> manager {};
    std::array<unsigned char, 0x40> title {};
    std::array<unsigned char, 0x38> rank {};
    if (!read(base, 0x13DD000, route.manager) || !read(route.manager, 0, manager) ||
            field<std::uintptr_t>(manager, 0) != base + 0x10C6B38 ||
            field<std::uint32_t>(manager, 0x38) != 1 || field<std::int32_t>(manager, 0x3C) != -1) return false;
    route.title = field<std::uintptr_t>(manager, 0x40);
    if (!read(route.title, 0, title) || field<std::uintptr_t>(title, 0) != base + 0x10C7278 ||
            title[8] != 0 || title[9] != 0 || field<std::uint32_t>(title, 0x10) != 1 ||
            field<std::uint32_t>(title, 0x14) != 12) return false;
    route.rank = field<std::uintptr_t>(title, 0x38);
    // GeneralRank is created, driven, and destroyed entirely inside Title's
    // substate12. Its state0..3 includes load/active/teardown/done, preserving
    // the same geometry throughout native entry/exit fades within that role.
    return read(route.rank, 0, rank) && field<std::uintptr_t>(rank, 0) == base + 0x10C6FF0 &&
        field<std::uint32_t>(rank, 8) <= 3 && rank[0x2D] <= 1 && rank[0x2E] <= 1;
}

bool named(const GuiBytes& bytes, const char* expected) noexcept {
    const auto* name = reinterpret_cast<const char*>(bytes.data() + 0x40);
    return std::memchr(name, 0, 32) && std::strcmp(name, expected) == 0;
}
bool collectWhite(RankingWhiteIdentity& id, std::array<GuiBytes, 2>* geometry = nullptr,
        bool requireCurrentMode = true) noexcept {
    std::uintptr_t config = 0, frame = 0, vt = 0, backlink = 0;
    unsigned char twoDisplay = 0;
    std::array<std::uint16_t, 2> extent {};
    if (requireCurrentMode && (!read(base, 0x13DCF58, config) || !read(config, 0, vt) || vt != base + 0x10C5FA8 ||
            !read(config, 0x1373, twoDisplay) || twoDisplay != 1 ||
            !read(base, 0x12E24E8, frame) || !read(frame, 0, vt) || vt != base + 0x703E78 ||
            !read(frame, 0x28, extent) || extent != std::array<std::uint16_t, 2>{{1920, 1080}})) return false;
    if (!read(base, 0x13DCF48, id.resident) || !read(id.resident, 0x128, id.layout)) return false;
    std::array<unsigned char, 0x70> layout {};
    std::array<char, 32> rootName {};
    std::uint32_t display = 0, count = 0;
    if (!read(id.layout, 0, layout) || field<std::uintptr_t>(layout, 0) != base + 0x10C9FA8 ||
            (requireCurrentMode && (field<std::uint32_t>(layout, 0x18) != 2 || field<std::uint32_t>(layout, 0x1C) != 1)) ||
            field<std::uint32_t>(layout, 0x64) != 0xBAA ||
            !read(id.layout, 0x3C90, display) || display != 0 ||
            !read(id.layout, 0x3C98, count) || count != 2 ||
            !read(id.layout, 0x368, rootName) || !std::memchr(rootName.data(), 0, rootName.size()) ||
            std::strcmp(rootName.data(), "Root") ||
            !read(id.layout, 0x3F8, id.group) || !id.group || id.group >= 32 ||
            !read(id.layout, 0x428, id.root)) return false;
    GuiBytes root {}, leaf {};
    if (!read(id.root, 0, root) || field<std::uintptr_t>(root, 0) != base + 0x10CEB48 ||
            field<std::uintptr_t>(root, 8) != id.root || field<std::uintptr_t>(root, 0x10) ||
            field<std::uintptr_t>(root, 0x20) || field<std::uintptr_t>(root, 0x28) ||
            field<std::uint32_t>(root, 0x3C) != id.group || !named(root, "Root") ||
            field<std::array<float, 2>>(root, 0x98) != std::array<float, 2>{{20, 20}} || root[0xC9] > 1) return false;
    id.leaf = field<std::uintptr_t>(root, 0x18);
    if (!read(id.leaf, 0, leaf) || field<std::uintptr_t>(leaf, 0) != base + 0x10CEBA8 ||
            field<std::uintptr_t>(leaf, 8) != id.leaf || field<std::uintptr_t>(leaf, 0x10) != id.root ||
            field<std::uintptr_t>(leaf, 0x18) || field<std::uintptr_t>(leaf, 0x20) || field<std::uintptr_t>(leaf, 0x28) ||
            field<std::uint32_t>(leaf, 0x3C) != id.group || !named(leaf, "white") ||
            field<std::array<float, 2>>(leaf, 0x98) != std::array<float, 2>{{1000, 1480}} || leaf[0xC9] > 1) return false;
    id.sprite = field<std::uintptr_t>(leaf, 0x110);
    id.record = field<std::uintptr_t>(leaf, 0x30);
    std::uint32_t selector = 0;
    std::uint64_t key = 0;
    if (!read(id.sprite, 0, vt) || vt != base + 0x10CC0D8 ||
            !read(id.sprite, 0x570, selector) || selector != 0 ||
            !read(id.sprite, 0x1A8, id.camera) || !id.camera ||
            !read(id.sprite, 0x308, id.material) || !read(id.material, 0, vt) || vt != base + 0x706570 ||
            !read(id.material, 0x50, id.texture) || !read(id.texture, 0, key) || key != 0x5553455200052B00ULL ||
            !read(id.texture, 0x18, extent) || extent != std::array<std::uint16_t, 2>{{64, 64}} ||
            !read(id.record, 0, vt) || vt != base + 0x10CEC08 ||
            !read(id.record, 0x40, backlink) || backlink != id.sprite) return false;
    if (geometry) *geometry = {{root, leaf}};
    return true;
}


// The resident's second main layout resource is SDMask: exactly one Root and
// the two Top/Btm children. Never enumerate the GUI manager or general menus.
bool collectMask(RankingWhiteIdentity& id, unsigned kind, std::array<GuiBytes, 2>* geometry = nullptr,
        bool requireCurrentMode = true) noexcept {
    if (kind < 1 || kind > 2) return false;
    id.kind = kind;
    std::uintptr_t config = 0, frame = 0, vt = 0;
    unsigned char twoDisplay = 0;
    std::array<std::uint16_t, 2> extent {};
    if (requireCurrentMode && (!read(base, 0x13DCF58, config) || !read(config, 0, vt) || vt != base + 0x10C5FA8 ||
            !read(config, 0x1373, twoDisplay) || twoDisplay != 1 ||
            !read(base, 0x12E24E8, frame) || !read(frame, 0, vt) || vt != base + 0x703E78 ||
            !read(frame, 0x28, extent) || extent != std::array<std::uint16_t, 2>{{1920, 1080}})) return false;
    std::array<unsigned char, 0x70> layout {};
    std::array<char, 32> rootName {};
    std::uint32_t display = 0, count = 0;
    if (!read(base, 0x13DCF48, id.resident) || !read(id.resident, 0x128, id.layout) ||
            !read(id.layout, 0, layout) || field<std::uintptr_t>(layout, 0) != base + 0x10C9FA8 ||
            (requireCurrentMode && (field<std::uint32_t>(layout, 0x18) != 2 || field<std::uint32_t>(layout, 0x1C) != 1)) ||
            field<std::uint32_t>(layout, 0x64) != 0xBAA || field<std::uint32_t>(layout, 0x68) != 0xBF6 ||
            !read(id.layout, 0x3C90, display) || display != 0 ||
            !read(id.layout, 0x3C98, count) || count != 2 ||
            !read(id.layout, 0x368, rootName) || !std::memchr(rootName.data(), 0, rootName.size()) ||
            std::strcmp(rootName.data(), "Root") ||
            !read(id.layout, 0x3FC, id.group) || !id.group || id.group >= 32 ||
            !read(id.layout, 0x430, id.root)) return false;
    GuiBytes root {};
    if (!read(id.root, 0, root) || field<std::uintptr_t>(root, 0) != base + 0x10CEB48 ||
            field<std::uintptr_t>(root, 8) != id.root || field<std::uintptr_t>(root, 0x10) ||
            field<std::uintptr_t>(root, 0x20) || field<std::uintptr_t>(root, 0x28) ||
            field<std::uint32_t>(root, 0x3C) != id.group || !named(root, "Root") ||
            field<std::array<float, 2>>(root, 0x98) != std::array<float, 2>{{5, 5}} || root[0xC9] > 1) return false;
    std::uintptr_t pointer = field<std::uintptr_t>(root, 0x18), previous = 0;
    std::array<std::uintptr_t, 2> children {};
    GuiBytes selected {};
    for (unsigned i = 0; i < 2; ++i) {
        GuiBytes leaf {};
        if (!read(pointer, 0, leaf) || field<std::uintptr_t>(leaf, 0) != base + 0x10CEBA8 ||
                field<std::uintptr_t>(leaf, 8) != pointer || field<std::uintptr_t>(leaf, 0x10) != id.root ||
                field<std::uintptr_t>(leaf, 0x18) || field<std::uintptr_t>(leaf, 0x20) != previous ||
                field<std::uint32_t>(leaf, 0x3C) != id.group || leaf[0xC9] > 1 ||
                field<std::array<float, 2>>(leaf, 0x98) != std::array<float, 2>{{850, 64}}) return false;
        const unsigned role = named(leaf, "Top") ? 1 : (named(leaf, "Btm") ? 2 : 0);
        if (!role || children[role - 1]) return false;
        children[role - 1] = pointer;
        if (role == kind) { id.leaf = pointer; selected = leaf; }
        previous = pointer; pointer = field<std::uintptr_t>(leaf, 0x28);
    }
    if (pointer || !children[0] || !children[1] || children[0] == children[1] ||
            children[0] == id.root || children[1] == id.root) return false;
    id.sibling = children[kind == 1 ? 1 : 0];
    id.sprite = field<std::uintptr_t>(selected, 0x110);
    id.record = field<std::uintptr_t>(selected, 0x30);
    std::uint32_t selector = 0;
    std::uint64_t key = 0;
    std::uintptr_t backlink = 0;
    if (!read(id.sprite, 0, vt) || vt != base + 0x10CC0D8 ||
            !read(id.sprite, 0x570, selector) || selector != 0 ||
            !read(id.sprite, 0x1A8, id.camera) || !id.camera ||
            !read(id.sprite, 0x308, id.material) || !read(id.material, 0, vt) || vt != base + 0x706570 ||
            !read(id.material, 0x50, id.texture) || !read(id.texture, 0, key) || key != 0x5553455200055180ULL ||
            !read(id.texture, 0x18, extent) || extent != std::array<std::uint16_t, 2>{{64, 64}} ||
            !read(id.record, 0, vt) || vt != base + 0x10CEC08 ||
            !read(id.record, 0x40, backlink) || backlink != id.sprite) return false;
    if (geometry) *geometry = {{root, selected}};
    return true;
}

bool collectBacking(RankingWhiteIdentity& id, unsigned kind, std::array<GuiBytes, 2>* geometry = nullptr,
        bool requireCurrentMode = true) noexcept {
    return kind == 0 ? collectWhite(id, geometry, requireCurrentMode) : collectMask(id, kind, geometry, requireCurrentMode);
}

bool backingPose(const std::array<GuiBytes, 2>& nodes, RearIlluminationPose& pose, unsigned kind = 0) noexcept {
    const auto& leaf = nodes[1];
    if (nodes[0][0xC9] != 1 || leaf[0xC9] != 1 ||
            field<std::array<float, 2>>(leaf, 0x60) != (kind == 0 ? std::array<float, 2>{{-100, -100}} :
                std::array<float, 2>{{0, kind == 1 ? -45.0f : 1261.0f}}) ||
            field<std::array<float, 2>>(leaf, 0xA0) != std::array<float, 2>{{1, 1}}) return false;
    for (const auto& node : nodes)
        if (field<std::array<float, 2>>(node, 0x90) != std::array<float, 2>{} ||
                field<std::array<float, 2>>(node, 0xB0) != std::array<float, 2>{}) return false;
    pose.position = field<std::array<float, 2>>(leaf, 0x70);
    pose.scale = field<std::array<float, 2>>(leaf, 0xA8);
    return true; // Alpha/RGB deliberately do not authorize or disqualify a fade.
}


// SDMask changes exactly two FOUR-byte X components, never their Y neighbors.
// The shared permission verifies a superset (the corresponding two8-byte spans).
// As with the full-pose writer, attempt both components after a partial failure.
bool writeMaskComponent(const SpriteGeometryPermission& permission, std::uintptr_t offset, float value) noexcept {
    if (!permission.valid || !spriteGeometryAddressValid(permission.sprite) || (offset != 0x70 && offset != 0xA8)) return false;
    __try {
        auto* destination = reinterpret_cast<volatile unsigned char*>(permission.sprite + offset);
        const auto* source = reinterpret_cast<const unsigned char*>(&value);
        for (unsigned i = 0; i < sizeof(value); ++i) destination[i] = source[i];
        for (unsigned i = 0; i < sizeof(value); ++i) if (destination[i] != source[i]) return false;
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
bool writeBackingGeometry(unsigned kind, const SpriteGeometryPermission& permission,
        const RearIlluminationPose& pose) noexcept {
    if (kind == 0) return writeSpriteGeometry(permission, pose);
    if (kind > 2) return false;
    const bool position = writeMaskComponent(permission, 0x70, pose.position[0]);
    const bool scale = writeMaskComponent(permission, 0xA8, pose.scale[0]);
    return position && scale;
}

Access observePermission(std::uintptr_t leaf, const Access& previous) noexcept {
    const auto now = GetTickCount64();
    if (!spriteGeometryAddressValid(leaf)) return {};
    if (previous.valid && previous.leaf == leaf && now >= previous.observed && now - previous.observed <= 50)
        return previous;
    checked_data_detail::Region region;
    for (const auto offset : {std::uintptr_t(0x70), std::uintptr_t(0xA8)}) {
        const auto address = leaf + offset;
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
        if (8 > region.size - (address - region.start)) return {};
    }
    if (GetTickCount64() - now > 100) return {};
    return {leaf, now, true};
}
bool sameHud(const NativeHudGeometry& a, const NativeHudGeometry& b) noexcept {
    return a.valid && b.valid && a.helper == b.helper && a.sprite == b.sprite &&
        a.mainWidth == 1920 && b.mainWidth == 1920 && a.mainHeight == 1080 && b.mainHeight == 1080 &&
        a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64();
    auto expected = nextReport.load();
    if (now < expected || !nextReport.compare_exchange_strong(expected, now + 5000)) return;
    try { log_info("bone-eater", "Ranking backing pid={} ready={} clipped={} restored={} rejected={} failed={} mask_ready={} top={}/{} bottom={}/{}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load(), current.masksValid,
        maskApplied[0].load(), maskRestored[0].load(), maskApplied[1].load(), maskRestored[1].load()); } catch (...) {}
}
void observe(void* manager, const Observed& previous) noexcept {
    if (!enabled.load(std::memory_order_acquire) || failed.load()) return;
    Route route, again;
    RankingWhiteIdentity white, repeated;
    if (!collectRoute(route) || route.manager != reinterpret_cast<std::uintptr_t>(manager) ||
            !collectWhite(white) || !collectRoute(again) || route != again || !collectWhite(repeated) || white != repeated) return;
    DWORD expected = 0;
    ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) { failure("scene update changed thread"); return; }
    current.route = route; current.white = white; current.observed = GetTickCount64(); current.valid = true;
    if (previous.valid && previous.route == route && previous.white == white) current.access = previous.access;
    std::array<RankingWhiteIdentity, 2> masks {}, repeatedMasks {};
    if (collectMask(masks[0], 1) && collectMask(masks[1], 2) &&
            collectMask(repeatedMasks[0], 1) && collectMask(repeatedMasks[1], 2) && masks == repeatedMasks &&
            masks[0].resident == white.resident && masks[0].layout == white.layout &&
            masks[1].root == masks[0].root && masks[0].root != white.root &&
            masks[0].leaf != white.leaf && masks[1].leaf != white.leaf &&
            collectRoute(again) && again == route) {
        current.masks = masks; current.masksValid = true;
        if (previous.valid && previous.route == route && previous.masksValid && previous.masks == masks)
            current.maskAccess = previous.maskAccess;
    }
    report();
}
void __fastcall update(void* manager) {
    if (updating || !enabled.load(std::memory_order_acquire)) { originalSceneUpdate(manager); return; }
    const Observed previous = current;
    current = {}; updating = true;
    __try { originalSceneUpdate(manager); observe(manager, previous); }
    __finally { updating = false; }
}
template<std::size_t N> bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {}; return read(base, rva, actual) && actual == expected;
}
} // namespace

RankingBackingAlignment beginNativeRankingBackingCommit(void* object, std::uintptr_t caller) noexcept {
    RankingBackingAlignment result;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || committing || !current.valid || caller != base + 0x1E634D) return result;
    const auto pointer = reinterpret_cast<std::uintptr_t>(object);
    unsigned kind = 0;
    if (pointer != current.white.leaf) {
        if (!current.masksValid) return result;
        kind = pointer == current.masks[0].leaf ? 1 : (pointer == current.masks[1].leaf ? 2 : 0);
        if (!kind) return result;
    }
    const Observed observed = current;
    const auto expectedIdentity = kind ? observed.masks[kind - 1] : observed.white;
    const auto previousAccess = kind ? observed.maskAccess[kind - 1] : observed.access;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return result;
    Route route;
    RankingWhiteIdentity white;
    std::array<GuiBytes, 2> nodes {};
    RearIlluminationPose before, after;
    const auto hud = readNativeHudGeometry();
    if (!sameHud(hud, hud) || !collectRoute(route) || route != observed.route ||
            !collectBacking(white, kind, &nodes) || white != expectedIdentity || !backingPose(nodes, before, kind) ||
            !(kind ? fitRankingMaskHorizontal(hud.left, hud.top, hud.width, hud.height, before, after) :
                clipRankingBacking(hud.left, hud.top, hud.width, hud.height, before, after))) { ++rejected; return result; }
    const Access access = observePermission(white.leaf, previousAccess);
    Route repeatedRoute;
    RankingWhiteIdentity repeated;
    std::array<GuiBytes, 2> repeatedNodes {};
    RearIlluminationPose repeatedPose;
    const auto hudAgain = readNativeHudGeometry();
    if (!access.valid || !current.valid || current.observed != observed.observed ||
            current.route != observed.route || current.white != observed.white ||
            (kind && (!current.masksValid || current.masks != observed.masks)) ||
            !collectRoute(repeatedRoute) || repeatedRoute != route ||
            !collectBacking(repeated, kind, &repeatedNodes) || repeated != white || !backingPose(repeatedNodes, repeatedPose, kind) ||
            repeatedPose.position != before.position || repeatedPose.scale != before.scale || !sameHud(hud, hudAgain)) {
        ++rejected; return result;
    }
    const auto now = GetTickCount64();
    if (now - observed.observed > 100 || now - access.observed > 100) { ++rejected; return result; }
    if (kind) current.maskAccess[kind - 1] = access; else current.access = access;
    result.identity = white; result.permission = {white.leaf, true}; result.original = before;
    result.attempted = true; committing = true;
    if (!writeBackingGeometry(kind, result.permission, after)) {
        restoreNativeRankingBackingCommit(result); failure("backing geometry write failed"); return {};
    }
    const auto total = ++applied;
    const bool firstMask = kind && ++maskApplied[kind - 1] == 1;
    if (firstMask) {
        try { log_info("bone-eater", "Ranking SDMask {} first fitted horizontally to {},{}; Y/gradient/alpha/source retained",
            kind == 1 ? "Top" : "Btm", hud.left, hud.width); } catch (...) {}
    } else if (!kind && total == 1) {
        try { log_info("bone-eater", "Ranking backing first clipped: native white overscan to front panel ({}, {}, {}, {}); alpha/color preserved",
            hud.left, hud.top, hud.width, hud.height); } catch (...) {}
    }
    return result;
}

void restoreNativeRankingBackingCommit(const RankingBackingAlignment& token) noexcept {
    if (!token.attempted) return;
    RankingWhiteIdentity white;
    // A role transition or native-call stall must not prevent restoring the
    // still-owned resident leaf. Only its saved ownership/source chain matters.
    if (token.permission.sprite != token.identity.leaf || !collectBacking(white, token.identity.kind, nullptr, false) || white != token.identity)
        failure("white identity changed during native commit");
    else if (!writeBackingGeometry(token.identity.kind, token.permission, token.original)) failure("white geometry restoration failed");
    else { ++restored; if (token.identity.kind) ++maskRestored[token.identity.kind - 1]; }
    committing = false;
}

void installNativeRankingBacking(void* module, bool sharedCommitReady) noexcept {
    try {
        wchar_t option[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_RANKING_BACKING", option, 8) != 1 || option[0] != L'1' || originalSceneUpdate) return;
        if (!sharedCommitReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", option, 8) != 3 || std::wcscmp(option, L"fit")) {
            failure("requires native HUD fit and shared GUI commit hook"); return;
        }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 37> updateBytes {{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,
            0x48,0x8b,0x49,0x40,0x48,0x85,0xc9,0x74,0x4a,0x48,0x8b,0x01,0xff,0x50,0x28,0x83,0x7b,0x3c,
            0xff,0x74,0x3e,0x48,0x8d,0x0d,0x7b,0xb9,0x14,0x01}};
        constexpr std::array<unsigned char, 13> callerBytes {{0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        std::uintptr_t updateSlot = 0;
        float offset = 0;
        if (!bytesEqual(0xA55A0, updateBytes) || !bytesEqual(0x1E6340, callerBytes) ||
                !read(base, 0x10C6B38 + 0x20, updateSlot) || updateSlot != base + 0xA55A0 ||
                !read(base, 0x10D0AA4, offset) || offset != 0.1f) { failure("native instruction/layout guards differ"); return; }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { failure("module pin failed"); return; }
        originalSceneUpdate = reinterpret_cast<Update>(base + 0xA55A0);
        if (!detour::trampoline_try(originalSceneUpdate, &update, &originalSceneUpdate)) { failure("scene update hook failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Ranking backing correction enabled: exact resident white plus Top/Btm SDMask leaves, Title/GeneralRank only; native fades retained");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
