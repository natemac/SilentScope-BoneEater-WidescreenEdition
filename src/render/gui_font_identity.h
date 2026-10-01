#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bone_eater::render {
// A transient snapshot only. All pointers are numeric identity tokens; resolve
// again in the same render callback after native submission, then retain only
// an immutable material/epoch/VB/ordinal span in the frame selection ledger.
struct GuiFontIdentity {
    std::uint64_t frame = 0;
    std::uintptr_t owner = 0, gui = 0, parent = 0, adapter = 0, wrapper = 0;
    std::uintptr_t renderer = 0, material = 0, camera = 0;
    unsigned group = 0;
};

inline bool sameGuiFontIdentity(const GuiFontIdentity& a, const GuiFontIdentity& b) noexcept {
    return a.frame && a.frame == b.frame && a.owner == b.owner && a.gui == b.gui &&
        a.parent == b.parent && a.adapter == b.adapter && a.wrapper == b.wrapper &&
        a.renderer == b.renderer && a.material == b.material && a.camera == b.camera && a.group == b.group;
}

namespace gui_font_detail {
template<class T, std::size_t N>
T field(const std::array<unsigned char, N>& data, std::size_t offset) noexcept {
    T result {};
    std::memcpy(&result, data.data() + offset, sizeof(result));
    return result;
}
template<std::size_t N>
bool named(const std::array<unsigned char, N>& data, const char* expected) noexcept {
    return std::memchr(data.data() + 0x40, 0, 32) &&
        std::strcmp(reinterpret_cast<const char*>(data.data() + 0x40), expected) == 0;
}
}

// Reader must be a safe bounded, nonthrowing read(base, offset, T&) callback.
// Caller must already have verified/pinned the exact native module and own the
// synchronous render-thread boundary; this helper never calls native methods.
// False clears output. It is never a certificate that a hidden node emitted no
// quad. Fresh identity and actual append counts remain required at submission.
template<class Reader>
bool readPlayerNameIdentity(std::uintptr_t module, std::uintptr_t expectedOwner,
                            std::uintptr_t expectedGui, std::uint64_t frame,
                            Reader&& read, GuiFontIdentity& output) noexcept {
    using gui_font_detail::field;
    output = {};
    if (!module || !expectedOwner || !expectedGui || !frame) return false;
    GuiFontIdentity id;
    id.frame = frame;
    std::uintptr_t managerAddress = 0, vtable = 0, linked = 0;
    unsigned state = 0;
    if (!read(module, 0x13dcf78, managerAddress) || !read(managerAddress, 0, vtable) || vtable != module + 0x10ca298 ||
        !read(managerAddress, 0x48, state) || state != 3 ||
        !read(managerAddress, 0xd0, id.owner) || id.owner != expectedOwner ||
        !read(id.owner, 0, vtable) || vtable != module + 0x10ca1d8 ||
        !read(id.owner, 0x48, state) || state != 3 ||
        !read(id.owner, 0xe0, id.group) || !id.group || id.group >= 32 ||
        !read(id.owner, 0x140, id.parent) || !id.parent ||
        !read(id.owner, 0x1b8, id.gui) || id.gui != expectedGui) return false;
    std::array<unsigned char, 0x130> guiBytes {};
    std::array<unsigned char, 0x60> parentBytes {};
    if (!read(id.gui, 0, guiBytes) || field<std::uintptr_t>(guiBytes, 0) != module + 0x10ceae8 ||
        field<std::uintptr_t>(guiBytes, 8) != id.gui || field<std::uintptr_t>(guiBytes, 0x10) != id.parent ||
        field<unsigned>(guiBytes, 0x3c) != id.group || field<unsigned>(guiBytes, 0x12c) != 2 ||
        !gui_font_detail::named(guiBytes, "lcd_bt_PlayerName") ||
        !read(id.parent, 0, parentBytes) || field<std::uintptr_t>(parentBytes, 0) != module + 0x10ceba8 ||
        field<std::uintptr_t>(parentBytes, 8) != id.parent || field<unsigned>(parentBytes, 0x3c) != id.group ||
        !gui_font_detail::named(parentBytes, "root_upper")) return false;
    id.adapter = field<std::uintptr_t>(guiBytes, 0x30);
    id.wrapper = field<std::uintptr_t>(guiBytes, 0x110);
    unsigned selector = 0, flags = 0;
    if (!id.wrapper || !read(id.adapter, 0, vtable) || vtable != module + 0x10cec58 ||
        !read(id.adapter, 0x40, linked) || linked != id.wrapper ||
        !read(id.wrapper, 0x90, selector) || selector != 2 ||
        !read(id.wrapper, 0x18, id.renderer) ||
        !read(id.renderer, 0, vtable) || vtable != module + 0x10cc0d8 ||
        !read(id.renderer, 0x570, selector) || selector != 2 ||
        !read(id.renderer, 0x1a0, flags) || !(flags & 0x800000) ||
        !read(id.renderer, 0x1a8, id.camera) || !id.camera ||
        !read(module, 0x13db5b0, linked) || linked != id.camera ||
        !read(id.renderer, 0x308, id.material) ||
        !read(id.material, 0, vtable) || vtable != module + 0x706570) return false;
    output = id;
    return true;
}
}
