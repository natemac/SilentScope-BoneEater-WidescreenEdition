#include "render/native_normal_start_backing.h"
#include "render/normal_start_backing_math.h"
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
Update originalCreditUpdate = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::atomic<ULONGLONG> nextReport {0};
std::atomic<std::uint64_t> quarterApplied {0}, quarterRestored {0};
using GuiBytes = std::array<unsigned char, 0x130>;
using Nodes = std::array<GuiBytes, 2>;
struct Route {
    std::uintptr_t manager = 0, parent = 0, child = 0;
    bool operator==(const Route&) const = default;
};
struct Access { std::uintptr_t leaf = 0; ULONGLONG observed = 0; bool valid = false; };
struct Certificate {
    Route route;
    std::array<NormalStartWhiteIdentity, 4> layers; // main, front, Top, Btm
    bool operator==(const Certificate&) const = default;
};
struct Observed {
    Certificate identity; Access access;
    FirstCreditQuarterIdentity quarterIdentity; Access quarterAccess;
    FirstCreditQuarterIdentity artworkIdentity; Access artworkAccess; bool artworkValid = false;
    ULONGLONG observed = 0; bool valid = false, quarterValid = false;
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
        try { log_warning("bone-eater", "Normal Start backing correction disabled: {}", reason); } catch (...) {}
    }
}

bool collectRoute(Route& route) noexcept {
    std::array<unsigned char, 0x48> manager {};
    std::array<unsigned char, 0x50> parent {};
    std::array<unsigned char, 0x18> child {};
    if (!read(base, 0x13DD000, route.manager) || !read(route.manager, 0, manager) ||
            field<std::uintptr_t>(manager, 0) != base + 0x10C6B38 ||
            field<std::uint32_t>(manager, 0x38) != 4 || field<std::int32_t>(manager, 0x3C) != -1) return false;
    route.parent = field<std::uintptr_t>(manager, 0x40);
    if (!read(route.parent, 0, parent) || field<std::uintptr_t>(parent, 0) != base + 0x10C6FA0 ||
            parent[8] || parent[9] || field<std::uint32_t>(parent, 0x10) != 1 ||
            field<std::uint32_t>(parent, 0x14) != 8) return false;
    route.child = field<std::uintptr_t>(parent, 0x48);
    return read(route.child, 0, child) && field<std::uintptr_t>(child, 0) == base + 0x10CA7A8 &&
        !child[8] && !child[9] && field<std::uint32_t>(child, 0x10) == 1 &&
        (field<std::uint32_t>(child, 0x14) == 0 || field<std::uint32_t>(child, 0x14) == 1 ||
         field<std::uint32_t>(child, 0x14) == 3 || field<std::uint32_t>(child, 0x14) == 4 ||
         field<std::uint32_t>(child, 0x14) == 5);
}

// Kind 0 is the sole writable resident main white. Other roles only certify
// the observed settled composition. Restore calls kind0 structural-only.
bool collectLayer(unsigned kind, NormalStartWhiteIdentity& id, Nodes* output = nullptr,
        bool eligible = true) noexcept {
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
                (mask ? (color >> 24) != 0 : front ? color != 0xFF000000u : (color & 0xFFFFFFu) != 0xFFFFFFu) ||
                (front && field<float>(layout, 0x58) != 1.0f)) return false;
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
        if (field<float>(leaf, 0xB8) != 1 || (kind != 0 && field<float>(leaf, 0xBC) != (mask ? 0.0f : 1.0f)) ||
                (front && (field<float>(root, 0xB8) != 1 || field<float>(root, 0xBC) != 1))) return false;
        // Native resident fades animate both Root alpha fields. Keep the
        // horizontal fit through that fade while preserving all alpha bytes.
        // Update and GUI propagation need not expose matching alpha snapshots.
        // Both Root values are independently finite and bounded above.
    }
    if (output) *output = nodes; return true;
}
constexpr std::array<float, 8> whiteUv {{
    0.78271484375f,0.00048828125f, 0.78271484375f,0.00146484375f,
    0.78369140625f,0.00146484375f, 0.78369140625f,0.00048828125f}};
constexpr std::array<float, 8> normalArtworkUv {{
    0.00048828125f,0.47412109375f,0.00048828125f,0.9462890625f,
    0.39013671875f,0.9462890625f,0.39013671875f,0.47412109375f}};
// Restoration starts from the saved typed owner link, not the current scene.
// Its structural path excludes lifecycle/configuration/fade/pose eligibility.
bool collectQuarter(std::uintptr_t parentPointer, std::uintptr_t childPointer,
        FirstCreditQuarterIdentity& id, Nodes* output = nullptr, bool eligible = true, unsigned role = 2) noexcept {
    if (role != 1 && role != 2) return false;
    std::array<unsigned char, 0x50> parent {};
    std::array<unsigned char, 0x28> child {};
    if (!read(parentPointer, 0, parent) || field<std::uintptr_t>(parent, 0) != base + 0x10C6FA0 ||
            field<std::uintptr_t>(parent, 0x48) != childPointer ||
            !read(childPointer, 0, child) || field<std::uintptr_t>(child, 0) != base + 0x10CA7A8) return false;
    id.parent = parentPointer; id.child = childPointer;
    std::uintptr_t vt = 0;
    std::array<std::uint16_t, 2> extent {};
    id.layout = field<std::uintptr_t>(child, 0x20);
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
    std::array<GuiBytes, 4> nodes {};
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
    constexpr std::array<const char*, 3> names {{"BG", "BG2", "White"}};
    auto pointer = field<std::uintptr_t>(root, 0x18);
    std::uintptr_t previous = 0;
    for (unsigned i = 0; i < 3; ++i) {
        auto& node = nodes[i + 1];
        if (!pointer || pointer == id.root) return false;
        for (unsigned j = 0; j < i; ++j) if (pointer == id.leaves[j]) return false;
        if (!read(pointer, 0, node) || field<std::uintptr_t>(node, 0) != base + 0x10CEBA8 ||
                field<std::uintptr_t>(node, 8) != pointer || field<std::uintptr_t>(node, 0x10) != id.root ||
                field<std::uintptr_t>(node, 0x18) || field<std::uintptr_t>(node, 0x20) != previous ||
                field<std::uint32_t>(node, 0x3C) != id.group || !named(node, names[i]) ||
                field<std::array<float, 2>>(node, 0x98) != std::array<float, 2>{{800, i == 2 ? 1480.0f : 969.0f}}) return false;
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
            (eligible && (!read(base, 0x13DB5A0, camera) || id.camera != camera)) || !read(id.camera, 0, vt) || vt != base + 0x6FDF48 ||
            !read(id.sprite, 0x308, id.material) || !read(id.material, 0, vt) || vt != base + 0x706570 ||
            !read(id.material, 0x50, id.texture) || !read(id.texture, 0, key) || key != (role == 2 ? 0x5553455200061280ULL : 0x5553455200061300ULL) ||
            !read(id.texture, 0x18, extent) || extent != std::array<std::uint16_t, 2>{{2048, 2048}} ||
            !read(id.record, 0, record) || field<std::uintptr_t>(record, 0) != base + 0x10CEC08 ||
            field<std::uintptr_t>(record, 0x40) != id.sprite ||
            (eligible && field<std::array<float, 8>>(record, 0x58) != (role == 2 ? whiteUv : normalArtworkUv))) return false;
    if (eligible) {
        // +58 is an inactive fade accumulator here (observed0), not opacity.
        if (!std::isfinite(field<float>(layout, 0x58)) || nodes[1][0xC9] != 0 || nodes[2][0xC9] != 1 ||
                field<std::uintptr_t>(nodes[1], 0x110) != field<std::uintptr_t>(nodes[3], 0x110) ||
                !field<std::uintptr_t>(nodes[2], 0x110) || field<std::uintptr_t>(nodes[2], 0x110) == field<std::uintptr_t>(nodes[3], 0x110) ||
                (field<std::uint32_t>(record, 8) & 0xFFFFFFu) != 0xFFFFFFu) return false;
        for (const auto* node : {&root, &nodes[role + 1]})
            for (unsigned offset : {0xB8u,0xBCu}) {
                const float alpha = field<float>(*node, offset);
                if (!std::isfinite(alpha) || alpha < 0 || alpha > 1) return false;
            }
        for (unsigned i = 0; i < 3; ++i)
            if (field<std::array<float, 2>>(nodes[i + 1], 0x60) != std::array<float, 2>{{0, i == 2 ? -100.0f : 88.0f}} ||
                    field<std::array<float, 2>>(nodes[i + 1], 0xA0) != std::array<float, 2>{{1,1}}) return false;
    }
    if (output) *output = {{nodes[0], nodes[role + 1]}};
    return true;
}

bool quarterPose(const Nodes& nodes, RearIlluminationPose& result, unsigned role = 2) noexcept {
    const float localY = role == 2 ? -100.0f : 88.0f;
    const auto& root = nodes[0]; const auto& white = nodes[1];
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

bool collect(Certificate& certificate, Nodes* mainNodes = nullptr) noexcept {
    std::uintptr_t config = 0, frame = 0, vt = 0; unsigned char twoDisplay = 0;
    std::array<std::uint16_t, 2> extent {};
    if (!read(base, 0x13DCF58, config) || !read(config, 0, vt) || vt != base + 0x10C5FA8 ||
            !read(config, 0x1373, twoDisplay) || twoDisplay != 1 ||
            !read(base, 0x12E24E8, frame) || !read(frame, 0, vt) || vt != base + 0x703E78 ||
            !read(frame, 0x28, extent) || extent != std::array<std::uint16_t, 2>{{1920,1080}} ||
            !collectRoute(certificate.route)) return false;
    for (unsigned kind = 0; kind < 4; ++kind)
        if (!collectLayer(kind, certificate.layers[kind], kind == 0 ? mainNodes : nullptr)) return false;
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
    try { log_info("bone-eater", "Normal Start backing pid={} ready={} applied={} restored={} rejected={} failed={} quarter_ready={} quarter_applied={} quarter_restored={}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load(),
        current.quarterValid, quarterApplied.load(), quarterRestored.load()); } catch (...) {}
}
void observe(void* object, const Observed& previous) noexcept {
    if (failed.load() || nestedUpdate) return;
    Certificate first, second;
    if (!collect(first) || first.route.child != reinterpret_cast<std::uintptr_t>(object) ||
            !collect(second) || first != second) return;
    DWORD expected = 0; ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) { failure("First Credit update changed thread"); return; }
    current.identity = first; current.observed = GetTickCount64(); current.valid = true;
    if (previous.valid && previous.identity == first) current.access = previous.access;
    FirstCreditQuarterIdentity quarterFirst, quarterSecond;
    const auto& route = first.route;
    if (collectQuarter(route.parent, route.child, quarterFirst) &&
            quarterFirst.layout != first.layers[0].layout && quarterFirst.layout != first.layers[1].layout &&
            collectQuarter(route.parent, route.child, quarterSecond) && quarterFirst == quarterSecond) {
        current.quarterIdentity = quarterFirst; current.quarterValid = true;
        if (previous.valid && previous.identity == first && previous.quarterValid && previous.quarterIdentity == quarterFirst)
            current.quarterAccess = previous.quarterAccess;
    }
    if (current.quarterValid && collectQuarter(route.parent, route.child, quarterFirst, nullptr, true, 1) &&
            collectQuarter(route.parent, route.child, quarterSecond, nullptr, true, 1) && quarterFirst == quarterSecond) {
        current.artworkIdentity = quarterFirst; current.artworkValid = true;
        if (previous.artworkValid && previous.artworkIdentity == quarterFirst) current.artworkAccess = previous.artworkAccess;
    }
}
void updateObserved(void* object, std::uintptr_t caller) {
    if (!enabled.load(std::memory_order_acquire)) { originalCreditUpdate(object); return; }
    if (updating) { current = {}; nestedUpdate = true; originalCreditUpdate(object); return; }
    const auto previous = current;
    current = {}; updating = true; nestedUpdate = false;
    __try {
        originalCreditUpdate(object);
        if (caller == base + 0xB07DC) observe(object, previous);
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

NormalStartBackingAlignment beginNativeNormalStartBackingCommit(void* object, std::uintptr_t caller) noexcept {
    NormalStartBackingAlignment token;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || updating || committing || !current.valid ||
            caller != base + 0x1E634D) return token;
    const bool artwork = current.artworkValid && reinterpret_cast<std::uintptr_t>(object) == current.artworkIdentity.leaves[1];
    const unsigned role = artwork ? 1u : 2u;
    const bool quarter = artwork || (current.quarterValid && reinterpret_cast<std::uintptr_t>(object) == current.quarterIdentity.leaves[2]);
    if (!quarter && reinterpret_cast<std::uintptr_t>(object) != current.identity.layers[0].leaf) return token;
    const auto observed = current;
    const auto& observedQuarter = artwork ? observed.artworkIdentity : observed.quarterIdentity;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return token;
    const auto hud = readNativeHudGeometry();
    Certificate id; FirstCreditQuarterIdentity quarterId; Nodes nodes {}; RearIlluminationPose before, after;
    if (!sameHud(hud, hud) || !collect(id, quarter ? nullptr : &nodes) || id != observed.identity ||
            (quarter && (!collectQuarter(id.route.parent, id.route.child, quarterId, &nodes, true, role) || quarterId != observedQuarter)) ||
            !(quarter ? quarterPose(nodes, before, role) : pose(nodes, before)) ||
            !(quarter ? fitMenuWhiteHorizontal(hud.left, hud.top, hud.width, hud.height, before, after, !artwork) :
                fitNormalStartWhiteHorizontal(hud.left, hud.top, hud.width, hud.height, before, after))) { ++rejected; return token; }
    const auto leaf = quarter ? quarterId.leaves[role] : id.layers[0].leaf;
    const auto access = permission(leaf, artwork ? observed.artworkAccess : quarter ? observed.quarterAccess : observed.access);
    Certificate repeated; FirstCreditQuarterIdentity repeatedQuarter; Nodes again {}; RearIlluminationPose repeatedPose;
    if (!access.valid || !collect(repeated, quarter ? nullptr : &again) || repeated != id ||
            (quarter && (!collectQuarter(id.route.parent, id.route.child, repeatedQuarter, &again, true, role) || repeatedQuarter != quarterId)) ||
            !(quarter ? quarterPose(again, repeatedPose, role) : pose(again, repeatedPose)) ||
            repeatedPose.position != before.position || repeatedPose.scale != before.scale ||
            !sameHud(hud, readNativeHudGeometry())) { ++rejected; return token; }
    const auto now = GetTickCount64();
    if (!current.valid || current.identity != id || current.observed != observed.observed ||
            now - observed.observed > 100 || now - access.observed > 100 || updating || committing ||
            (quarter && (artwork ? (!current.artworkValid || current.artworkIdentity != quarterId) :
                (!current.quarterValid || current.quarterIdentity != quarterId)))) { ++rejected; return token; }
    if (artwork) current.artworkAccess = access; else if (quarter) current.quarterAccess = access; else current.access = access;
    token.identity = id.layers[0]; token.quarterIdentity = quarterId; token.quarter = quarter;
    token.quarterRole = role;
    token.permission = {leaf, true}; token.original = before; token.attempted = true;
    committing = true;
    if (!writeHorizontal(token.permission, after)) {
        restoreNativeNormalStartBackingCommit(token); failure("White horizontal write failed"); return {};
    }
    if (quarter && ++quarterApplied == 1) {
        try { log_info("bone-eater", "Normal Start quarter layers active: exact three-child source, front X {} width {}; native vertical/fade fields retained", hud.left, hud.width); } catch (...) {}
    }
    if (++applied == 1) {
        try { log_info("bone-eater", "Normal Start backing active: exact Eamusement4/1/8 -> FirstCredit entry/selection/exit, front X {} width {}; native alpha/vertical fields retained", hud.left, hud.width); } catch (...) {}
    }
    return token;
}

void restoreNativeNormalStartBackingCommit(const NormalStartBackingAlignment& token) noexcept {
    if (!token.attempted) return;
    NormalStartWhiteIdentity id; FirstCreditQuarterIdentity quarterId;
    // Do not require current phase/config/fit/age, UV, alpha or vertical pose.
    // Restore only the borrowed X fields; native changes to Y remain untouched.
    const bool matches = token.quarter ?
        (token.quarterRole == 1 || token.quarterRole == 2) && token.permission.sprite == token.quarterIdentity.leaves[token.quarterRole] &&
            collectQuarter(token.quarterIdentity.parent, token.quarterIdentity.child, quarterId, nullptr, false, token.quarterRole) && quarterId == token.quarterIdentity :
        token.permission.sprite == token.identity.leaf && collectLayer(0, id, nullptr, false) && id == token.identity;
    if (!matches)
        failure("White structural ownership changed during native commit");
    else if (!writeHorizontal(token.permission, token.original)) failure("White horizontal restoration failed");
    else { ++restored; if (token.quarter) ++quarterRestored; }
    committing = false;
}

void installNativeNormalStartBacking(void* module, bool sharedCommitReady) noexcept {
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_NORMAL_START_BACKING", setting, 8) != 1 || setting[0] != L'1' || originalCreditUpdate) return;
        if (!sharedCommitReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", setting, 8) != 3 || std::wcscmp(setting, L"fit")) {
            failure("requires native HUD fit and shared GUI commit hook"); return;
        }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 35> updateBytes {{0x40,0x55,0x57,0x48,0x8d,0xac,0x24,0x48,0xf4,0xff,0xff,0x48,0x81,0xec,0xd8,0x0c,0x00,0x00,0x48,0x8b,0x05,0x2f,0xe1,0x15,0x01,0x48,0x33,0xc4,0x48,0x89,0x85,0x50,0x0b,0x00,0x00}};
        constexpr std::array<unsigned char, 10> updateCaller {{0x48,0x8b,0x4e,0x48,0x48,0x8b,0x01,0xff,0x50,0x28}};
        constexpr std::array<unsigned char, 13> commitCaller {{0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        std::uintptr_t slot = 0; float offset = 0;
        if (!bytesEqual(0x15EF80, updateBytes) || !bytesEqual(0xB07D2, updateCaller) ||
                !bytesEqual(0x1E6340, commitCaller) || !bytesEqual(0x1E55BB, geometryBytes) ||
                !read(base, 0x10CA7A8 + 0x28, slot) || slot != base + 0x15EF80 ||
                !read(base, 0x10D0AA4, offset) || offset != 0.1f) { failure("native update/commit guards differ"); return; }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { failure("module pin failed"); return; }
        originalCreditUpdate = reinterpret_cast<Update>(base + 0x15EF80);
        if (!detour::trampoline_try(originalCreditUpdate, &update, &originalCreditUpdate)) { failure("First Credit update hook failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Normal Start backing enabled: exact FirstCredit entry/selection/exit; White and BG2 fitted, native fades retained");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
