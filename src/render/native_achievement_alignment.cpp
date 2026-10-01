#include "render/native_achievement_alignment.h"
#include "render/achievement_alignment_math.h"
#include "render/native_hud.h"
#include "render/native_viewport.h"
#include "diagnostics/output_policy.h"
#include "util/logging.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bone_eater::render {
namespace {
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::atomic<ULONGLONG> nextReport {0};
using GuiBytes = std::array<unsigned char, 0x130>;
using Nodes = std::array<GuiBytes, 7>;
constexpr std::array<const char*, 7> names {{
    "m_bt_backlight_Nick", "root_marker", "root", "lcd_title_bar",
    "root_marker", "root", "lcd_bt_enemyDmg_root"}};

struct PoseAccess {
    std::uintptr_t sprite = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};
struct Observed {
    AchievementIdentity identity;
    PoseAccess access;
    ULONGLONG observed = 0;
    bool valid = false;
};
thread_local Observed current, previousUpdate;
thread_local bool updating = false, committing = false;

template<class T>
bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}
template<class T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T value {}; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
}
void failure(const char* reason) noexcept {
    if (!failed.exchange(true)) {
        try { log_warning("bone-eater", "Native achievement alignment disabled: {}", reason); } catch (...) {}
    }
}

// Structural ownership only. Restoration deliberately does not depend on
// current battle state, active timer, visibility, front fit or configuration.
bool collect(AchievementIdentity& id, Nodes* geometry = nullptr, bool eligible = false) noexcept {
    std::uintptr_t vt = 0, mainRoot = 0;
    if (!read(base, 0x13DCF78, id.manager) || !read(id.manager, 0, vt) || vt != base + 0x10CA298 ||
            !read(id.manager, 0xD8, id.main) || !read(id.manager, 0xD0, id.lcd) || id.main == id.lcd ||
            !read(id.main, 0, vt) || vt != base + 0x10CA200 ||
            !read(id.lcd, 0, vt) || vt != base + 0x10CA1D8 ||
            !read(id.main, 0x98, id.groups[0]) || !read(id.lcd, 0xE0, id.groups[1]) ||
            !id.groups[0] || id.groups[0] >= 32 || !id.groups[1] || id.groups[1] >= 32 ||
            id.groups[0] == id.groups[1] || !read(id.main, 0xF0, id.gui[0]) ||
            !read(id.lcd, 0x478, id.gui[3]) || !read(id.lcd, 0x4C0, id.gui[6]) ||
            !read(id.main, 0xA0, mainRoot)) return false;
    if (eligible) {
        for (const auto owner : {id.manager, id.main, id.lcd}) {
            std::uint32_t state = 0;
            if (!read(owner, 0x48, state) || state != 3) return false;
        }
        std::uintptr_t config = 0;
        unsigned char twoDisplay = 0;
        if (!read(base, 0x13DCF58, config) || !read(config, 0x1373, twoDisplay) || twoDisplay != 1) return false;
    }
    Nodes nodes {};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const bool root = i == 2 || i == 5;
        if (i == 1 || i == 2 || i == 4 || i == 5)
            id.gui[i] = field<std::uintptr_t>(nodes[i - 1], 0x10);
        for (std::size_t j = 0; j < i; ++j) if (id.gui[i] == id.gui[j]) return false;
        // GuiRect is only read through its proven 0xD0 base, not as a Sprite.
        if (root) {
            std::array<unsigned char, 0xD0> bytes {};
            if (!read(id.gui[i], 0, bytes)) return false;
            std::memcpy(nodes[i].data(), bytes.data(), bytes.size());
        } else if (!read(id.gui[i], 0, nodes[i])) return false;
        const auto& node = nodes[i];
        const auto* name = reinterpret_cast<const char*>(node.data() + 0x40);
        if (field<std::uintptr_t>(node, 0) != base + (root ? 0x10CEB48 : 0x10CEBA8) ||
                field<std::uintptr_t>(node, 8) != id.gui[i] ||
                field<std::uint32_t>(node, 0x3C) != id.groups[i < 3 ? 0 : 1] ||
                !std::memchr(name, 0, 32) || std::strcmp(name, names[i]) ||
                (root && field<std::uintptr_t>(node, 0x10))) return false;
    }
    if (id.gui[2] != mainRoot || field<std::uintptr_t>(nodes[6], 0x10) != id.gui[5] ||
            field<std::uintptr_t>(nodes[0], 0x18)) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& node = nodes[i * 3];
        id.sprites[i] = field<std::uintptr_t>(node, 0x110);
        id.records[i] = field<std::uintptr_t>(node, 0x30);
        std::uint32_t selector = 0;
        std::uint64_t key = 0;
        std::uintptr_t slot = 0, backlink = 0;
        std::array<std::uint16_t, 2> size {};
        if (!read(id.sprites[i], 0, vt) || vt != base + 0x10CC0D8 ||
                !read(id.sprites[i], 0x570, selector) || selector != i * 2 ||
                !read(id.sprites[i], 0x1A8, id.cameras[i]) ||
                !read(base, 0x13DB5A0 + i * 16, slot) || slot != id.cameras[i] ||
                !read(slot, 0, vt) || vt != base + 0x6FDF48 ||
                !read(id.sprites[i], 0x308, id.materials[i]) ||
                !read(id.materials[i], 0, vt) || vt != base + 0x706570 ||
                !read(id.materials[i], 0x50, id.textures[i]) ||
                !read(id.textures[i], 0, key) || key != (i ? 0x555345520004F880ULL : 0x555345520004F780ULL) ||
                !read(id.textures[i], 0x18, size) || size != (i ? std::array<std::uint16_t, 2>{{1024, 512}} :
                    std::array<std::uint16_t, 2>{{512, 1024}}) ||
                !read(id.records[i], 0, vt) || vt != base + 0x10CEC08 ||
                !read(id.records[i], 0x40, backlink) || backlink != id.sprites[i]) return false;
    }
    if (id.sprites[0] == id.sprites[1] || id.records[0] == id.records[1] || id.cameras[0] == id.cameras[1]) return false;
    if (geometry) *geometry = nodes;
    return true;
}

bool nativePose(const AchievementIdentity& id, const Nodes& nodes, RearIlluminationPose& pose) noexcept {
    unsigned char active = 0;
    float elapsed = 0;
    if (!read(id.lcd, 0x4A4, active) || active != 1 || !read(id.lcd, 0x4A0, elapsed) ||
            !std::isfinite(elapsed) || elapsed < 0 || elapsed > 192) return false;
    const std::array<float, 2> zero {{0, 0}}, one {{1, 1}};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto& node = nodes[i];
        if (node[0xC9] != 1 || field<std::array<float, 2>>(node, 0x90) != zero ||
                field<std::array<float, 2>>(node, 0xB0) != zero ||
                field<std::array<float, 2>>(node, 0xA0) != one ||
                field<std::array<float, 2>>(node, 0xA8) != one) return false;
        for (const auto offset : {0xB8, 0xBC}) {
            const auto alpha = field<float>(node, offset);
            if (!std::isfinite(alpha) || alpha <= 0 || alpha > 1) return false;
        }
    }
    const auto nickXY = field<std::array<float, 2>>(nodes[0], 0x70);
    const auto titleXY = field<std::array<float, 2>>(nodes[3], 0x70);
    if (field<std::array<float, 2>>(nodes[0], 0x98) != std::array<float, 2>{{800, 82}} ||
            field<std::array<float, 2>>(nodes[3], 0x98) != std::array<float, 2>{{768, 85}} ||
            field<std::array<float, 2>>(nodes[0], 0x60) != nickXY ||
            field<std::array<float, 2>>(nodes[3], 0x60) != std::array<float, 2>{{80, titleXY[1]}} ||
            nickXY != titleXY) return false;
    for (const auto index : {1u, 2u, 4u, 5u}) {
        const auto& node = nodes[index];
        const std::array<float, 2> local {{index == 5 ? -80.0f : 0.0f, 0}};
        const std::array<float, 2> cached {{index >= 4 ? -80.0f : 0.0f, 0}};
        if (field<std::array<float, 2>>(node, 0x60) != local ||
                field<std::array<float, 2>>(node, 0x70) != cached ||
                field<std::array<float, 2>>(node, 0x98) != zero) return false;
    }
    pose.position = nickXY; pose.scale = one;
    return nickXY[0] == 0 && std::isfinite(nickXY[1]) && nickXY[1] > -82 && nickXY[1] <= 0;
}

PoseAccess permission(std::uintptr_t sprite, const PoseAccess& previous) noexcept {
    const auto now = GetTickCount64();
    if (!spriteGeometryAddressValid(sprite)) return {};
    if (previous.valid && previous.sprite == sprite && now >= previous.observed && now - previous.observed <= 50)
        return previous;
    checked_data_detail::Region region;
    for (const std::uintptr_t offset : {std::uintptr_t(0x70), std::uintptr_t(0xA8)}) {
        const auto address = sprite + offset;
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
        if (8 > region.size - (address - region.start)) return {};
    }
    if (GetTickCount64() - now > 100) return {};
    return {sprite, now, true};
}

bool sameHud(const NativeHudGeometry& a, const NativeHudGeometry& b) noexcept {
    return a.valid && b.valid && a.mainWidth == 1920 && a.mainHeight == 1080 &&
        b.mainWidth == 1920 && b.mainHeight == 1080 && a.sourceWidth == 768 && a.sourceHeight == 1366 &&
        b.sourceWidth == 768 && b.sourceHeight == 1366 && a.helper == b.helper && a.sprite == b.sprite &&
        a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}

void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64(); auto expected = nextReport.load();
    if (now < expected || !nextReport.compare_exchange_strong(expected, now + 5000)) return;
    try { log_info("bone-eater", "Native achievement alignment pid={} ready={} aligned={} restored={} rejected={} failed={}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load()); } catch (...) {}
}
template<std::size_t N>
bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {}; return read(base, rva, actual) && actual == expected;
}
} // namespace

bool beginNativeAchievementUpdate() noexcept {
    if (!enabled.load(std::memory_order_acquire) || failed.load() || updating) return false;
    previousUpdate = current; current = {}; updating = true;
    return true;
}

void endNativeAchievementUpdate(void* lcd, bool completed) noexcept {
    if (!updating) return;
    AchievementIdentity first, second;
    if (completed && !failed.load() && collect(first, nullptr, true) &&
            first.lcd == reinterpret_cast<std::uintptr_t>(lcd) && collect(second, nullptr, true) && first == second) {
        DWORD expected = 0; ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
        if (ownerThread.load() != GetCurrentThreadId()) failure("LCD update changed thread");
        else {
            current.identity = first; current.observed = GetTickCount64(); current.valid = true;
            if (previousUpdate.valid && previousUpdate.identity == first) current.access = previousUpdate.access;
        }
    }
    previousUpdate = {}; updating = false;
    report();
}

AchievementAlignment beginNativeAchievementAlignmentCommit(void* object, std::uintptr_t caller) noexcept {
    AchievementAlignment token;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || updating || committing || !current.valid ||
            reinterpret_cast<std::uintptr_t>(object) != current.identity.gui[0] || caller != base + 0x1E634D) return token;
    const auto observed = current;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return token;
    AchievementIdentity id; Nodes nodes {};
    RearIlluminationPose before, after;
    const auto hud = readNativeHudGeometry();
    if (!sameHud(hud, hud) || !collect(id, &nodes, true) || id != observed.identity || !nativePose(id, nodes, before) ||
            !alignAchievementBacking(hud.left, hud.top, hud.width, hud.height, before,
                field<float>(nodes[3], 0x74), after)) { ++rejected; return token; }
    const auto access = permission(id.gui[0], observed.access);
    AchievementIdentity repeated; Nodes repeatedNodes {}; RearIlluminationPose repeatedPose;
    if (!access.valid || !collect(repeated, &repeatedNodes, true) || repeated != id ||
            !nativePose(repeated, repeatedNodes, repeatedPose) || repeatedPose.position != before.position ||
            repeatedPose.scale != before.scale || !sameHud(hud, readNativeHudGeometry())) { ++rejected; return token; }
    const auto now = GetTickCount64();
    if (!current.valid || current.observed != observed.observed || current.identity != id ||
            now - observed.observed > 100 || now - access.observed > 100 || updating || committing) {
        ++rejected; return token;
    }
    current.access = access;
    token.identity = id; token.permission = {id.gui[0], true}; token.original = before; token.attempted = true;
    committing = true;
    if (!writeSpriteGeometry(token.permission, after)) {
        restoreNativeAchievementAlignmentCommit(token);
        failure("Nick cached geometry write failed"); return {};
    }
    if (++applied == 1) {
        try { log_info("bone-eater", "Native achievement Nick alignment active; original 800x82 artwork and native animation retained"); } catch (...) {}
    }
    return token;
}

void restoreNativeAchievementAlignmentCommit(const AchievementAlignment& token) noexcept {
    if (!token.attempted) return;
    AchievementIdentity id;
    if (token.permission.sprite != token.identity.gui[0] || !collect(id) || id != token.identity)
        failure("Nick owner/source identity changed during native commit");
    else if (!writeSpriteGeometry(token.permission, token.original)) failure("Nick cached geometry restoration failed");
    else ++restored;
    committing = false;
}

void installNativeAchievementAlignment(void* module, bool sharedHooksReady) noexcept {
    try {
        wchar_t setting[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_ACHIEVEMENT_ALIGN", setting, 8) != 1 || setting[0] != L'1' || enabled.load()) return;
        if (!sharedHooksReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", setting, 8) != 3 ||
                std::wcscmp(setting, L"fit")) { failure("requires native HUD fit and shared GUI/LCD hooks"); return; }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        // Shared hook entry prologues may already be detoured. Guard unchanged
        // internal paired-animation and final geometry instructions instead.
        constexpr std::array<unsigned char, 13> callerBytes {{
            0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 38> pairBytes {{
            0x48,0x8b,0x81,0x78,0x04,0x00,0x00,0x48,0x8b,0x0d,0xc1,0x41,0x2c,0x01,0xf3,0x0f,
            0x5c,0xf2,0xc7,0x40,0x60,0x00,0x00,0xa0,0x42,0xf3,0x0f,0x59,0x35,0x1e,0x83,0xfb,
            0x00,0xf3,0x0f,0x11,0x70,0x64}};
        constexpr std::array<unsigned char, 40> rearPairBytes {{
            0x48,0x8b,0x89,0xd8,0x00,0x00,0x00,0x48,0x8b,0x81,0xf0,0x00,0x00,0x00,0xc6,0x80,
            0xc9,0x00,0x00,0x00,0x01,0x48,0x8b,0x81,0xf0,0x00,0x00,0x00,0xc7,0x40,0x60,0x00,
            0x00,0x00,0x00,0xf3,0x0f,0x11,0x70,0x64}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        constexpr std::array<unsigned char, 5> pairedCallBytes {{
            0xe8,0x4e,0x71,0x00,0x00}};
        constexpr std::array<unsigned char, 5> animationCallBytes {{
            0xe8,0xdf,0x68,0x0d,0x00}};
        float offset = 0;
        if (!bytesEqual(0x1E6340, callerBytes) || !bytesEqual(0x118DA9, pairBytes) ||
                !bytesEqual(0x118DF1, rearPairBytes) || !bytesEqual(0x1E55BB, geometryBytes) ||
                !bytesEqual(0x111BED, pairedCallBytes) || !bytesEqual(0x111BFC, animationCallBytes) ||
                !read(base, 0x10D0AA4, offset) || offset != 0.1f) {
            failure("native paired-animation/commit guard differs"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) { failure("module pin failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Native achievement alignment enabled: exact Nick backing only, original 800x82 overhang preserved");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
