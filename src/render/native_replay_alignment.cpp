#include "render/native_replay_alignment.h"
#include "diagnostics/output_policy.h"
#include "render/native_hud.h"
#include "render/native_viewport.h"

#include "util/detour.h"
#include "util/logging.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bone_eater::render {
namespace {
using Update = void(__fastcall*)(void*, float, void*);
Update originalUpdate = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<std::uint64_t> applied {0}, restored {0}, rejected {0};
std::atomic<ULONGLONG> nextReport {0};
constexpr std::array<const char*, 3> kNames {{"base", "SShotEffect", "mask"}};
using GuiBytes = std::array<unsigned char, 0x130>;

struct PoseAccess {
    std::uintptr_t mask = 0;
    ULONGLONG observed = 0;
    bool valid = false;
};
struct Observed {
    ReplayIdentity identity;
    PoseAccess access;
    ULONGLONG observed = 0;
    bool valid = false;
};
thread_local Observed current;
thread_local bool updating = false, committing = false;

template<typename T>
bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}
template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T result {}; std::memcpy(&result, bytes.data() + offset, sizeof(result)); return result;
}
void failure(const char* reason) noexcept {
    if (!failed.exchange(true)) {
        try { log_warning("bone-eater", "Native replay alignment disabled: {}", reason); } catch (...) {}
    }
}

bool collect(ReplayIdentity& id, std::array<GuiBytes, 3>* geometry = nullptr,
        bool requireActive = false) noexcept {
    std::uintptr_t vt = 0, config = 0;
    std::array<unsigned char, 0x238> owner {};
    unsigned char twoDisplay = 0;
    if (!read(base, 0x13DCF90, id.owner) || !read(id.owner, 0, owner) ||
            field<std::uintptr_t>(owner, 0) != base + 0x10C6200 ||
            !read(base, 0x13DCF58, config) || !read(config, 0x1373, twoDisplay) || twoDisplay != 1)
        return false;
    const auto state = field<std::uint32_t>(owner, 0x1B8);
    // Long/full replay and special modes retain all native geometry.
    if (state > 7 || (requireActive && (state < 5 || state > 7)) ||
            owner[0x224] || owner[0x225] || owner[0x226] ||
            field<float>(owner, 0x22C) != 768 || field<float>(owner, 0x230) != 300) return false;
    id.group = field<std::uint32_t>(owner, 0x170);
    if (!id.group || id.group >= 32) return false;
    constexpr std::array<std::size_t, 3> sourceOffsets {{0x150, 0x158, 0x168}};
    constexpr std::array<std::size_t, 3> aliases {{0x190, 0x1A0, 0x198}};
    for (std::size_t i = 0; i < 3; ++i) {
        id.gui[i] = field<std::uintptr_t>(owner, 0x178 + i * 8);
        id.sprites[i] = field<std::uintptr_t>(owner, sourceOffsets[i]);
        if (!id.gui[i] || !id.sprites[i] || field<std::uintptr_t>(owner, aliases[i]) != id.gui[i]) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (id.gui[i] == id.gui[j] || id.sprites[i] == id.sprites[j]) return false;
        GuiBytes gui {};
        if (!read(id.gui[i], 0, gui) || field<std::uintptr_t>(gui, 0) != base + 0x10CEBA8 ||
                field<std::uintptr_t>(gui, 8) != id.gui[i] || field<std::uintptr_t>(gui, 0x10) ||
                field<std::uintptr_t>(gui, 0x18) || field<std::uint32_t>(gui, 0x3C) != id.group ||
                field<std::uintptr_t>(gui, 0x110) != id.sprites[i] || gui[0xC9] > 1) return false;
        const auto* name = reinterpret_cast<const char*>(gui.data() + 0x40);
        if (!std::memchr(name, 0, 32) || std::strcmp(name, kNames[i])) return false;
        std::uint32_t selector = 0;
        std::uint64_t key = 0;
        std::array<std::uint16_t, 2> extent {};
        if (!read(id.sprites[i], 0, vt) || vt != base + 0x10CC0D8 ||
                !read(id.sprites[i], 0x570, selector) || selector != (i == 2 ? 0u : 2u) ||
                !read(id.sprites[i], 0x1A8, id.cameras[i]) || !id.cameras[i] ||
                !read(id.sprites[i], 0x308, id.materials[i]) ||
                !read(id.materials[i], 0, vt) || vt != base + 0x706570 ||
                !read(id.materials[i], 0x50, id.textures[i]) ||
                !read(id.textures[i], 0, key) || !read(id.textures[i], 0x18, extent)) return false;
        if (i == 0) {
            if (key != 0x7274406300001180ULL || extent != std::array<std::uint16_t, 2>{{768, 300}}) return false;
        } else if (key != 0x443330380005a100ULL || extent != std::array<std::uint16_t, 2>{{8, 512}}) return false;
        if (geometry) (*geometry)[i] = gui;
    }
    return id.cameras[0] == id.cameras[1] && id.cameras[0] != id.cameras[2];
}

bool maskPose(const std::array<GuiBytes, 3>& nodes, RearIlluminationPose& pose) noexcept {
    const auto& mask = nodes[2];
    if (nodes[0][0xC9] != 1 || mask[0xC9] != 1 ||
            field<float>(nodes[0], 0x98) != 768 || field<float>(nodes[0], 0x9C) != 300 ||
            field<float>(mask, 0x98) != 800 || field<float>(mask, 0x9C) != 299 ||
            field<float>(mask, 0x60) != 0 || field<float>(mask, 0x64) != 0 ||
            field<float>(mask, 0x90) != 0.5f || field<float>(mask, 0x94) != 0.5f ||
            field<float>(mask, 0xB0) != 0 || field<float>(mask, 0xB4) != 0) return false;
    pose.position = field<std::array<float, 2>>(mask, 0x70);
    pose.scale = field<std::array<float, 2>>(mask, 0xA8);
    return pose.position == std::array<float, 2>{{400, 149.5f}} &&
        std::isfinite(pose.scale[0]) && std::isfinite(pose.scale[1]) &&
        pose.scale[0] > 0 && pose.scale[0] <= 1.01f && pose.scale[0] == pose.scale[1];
}

PoseAccess observePermission(std::uintptr_t mask, const PoseAccess& previous) noexcept {
    const auto now = GetTickCount64();
    if (!spriteGeometryAddressValid(mask)) return {};
    if (previous.valid && previous.mask == mask && now >= previous.observed && now - previous.observed <= 50)
        return previous; // Never renew the original permission-observation age.
    checked_data_detail::Region region;
    for (const std::uintptr_t offset : {std::uintptr_t(0x70), std::uintptr_t(0xA8)}) {
        const auto address = mask + offset;
        if (!region.contains(address) && !checked_data_detail::queryPrivateWritable(address, region)) return {};
        if (8 > region.size - (address - region.start)) return {};
    }
    if (GetTickCount64() - now > 100) return {};
    return {mask, now, true};
}

void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64();
    auto expected = nextReport.load();
    if (now < expected || !nextReport.compare_exchange_strong(expected, now + 5000)) return;
    try { log_info("bone-eater", "Native replay alignment pid={} ready={} aligned={} restored={} rejected={} failed={}",
        GetCurrentProcessId(), current.valid, applied.load(), restored.load(), rejected.load(), failed.load()); }
    catch (...) {}
}

void observe(void* owner, const Observed& previous) noexcept {
    if (!enabled.load(std::memory_order_acquire) || failed.load()) return;
    ReplayIdentity first, second;
    if (!collect(first) || first.owner != reinterpret_cast<std::uintptr_t>(owner) ||
            !collect(second) || first != second) return;
    DWORD expected = 0;
    ownerThread.compare_exchange_strong(expected, GetCurrentThreadId());
    if (ownerThread.load() != GetCurrentThreadId()) { failure("native owner update changed thread"); return; }
    current.identity = first;
    current.observed = GetTickCount64();
    current.valid = true;
    if (previous.valid && previous.identity == first) current.access = previous.access;
    report();
}

void __fastcall update(void* owner, float elapsed, void* nativeCamera) {
    if (updating || !enabled.load(std::memory_order_acquire)) { originalUpdate(owner, elapsed, nativeCamera); return; }
    const Observed previous = current;
    current = {};
    updating = true;
    __try {
        originalUpdate(owner, elapsed, nativeCamera);
        observe(owner, previous);
    } __finally { updating = false; }
}

template<std::size_t N>
bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {}; return read(base, rva, actual) && actual == expected;
}
} // namespace

ReplayAlignment beginNativeReplayAlignmentCommit(void* object, std::uintptr_t caller) noexcept {
    ReplayAlignment result;
    if (!enabled.load(std::memory_order_acquire) || failed.load() || committing || !current.valid ||
            reinterpret_cast<std::uintptr_t>(object) != current.identity.gui[2] || caller != base + 0x1E634D)
        return result; // Unrelated GUI objects require no RPM/clock/query.
    const Observed observed = current;
    if (ownerThread.load() != GetCurrentThreadId() || GetTickCount64() - observed.observed > 100) return result;
    ReplayIdentity id;
    std::array<GuiBytes, 3> nodes {};
    const auto hud = readNativeHudGeometry();
    RearIlluminationPose before, after;
    if (!hud.valid || !collect(id, &nodes, true) || id != observed.identity || !maskPose(nodes, before) ||
            !alignRearIllumination(hud.left, hud.top, hud.width, hud.height, before, after)) {
        ++rejected; return result;
    }
    const auto access = observePermission(id.gui[2], observed.access);
    ReplayIdentity repeated;
    std::array<GuiBytes, 3> repeatedNodes {};
    RearIlluminationPose repeatedPose;
    const auto hudAgain = readNativeHudGeometry();
    if (!access.valid || GetTickCount64() - access.observed > 100 || !current.valid ||
            current.observed != observed.observed || current.identity != observed.identity ||
            GetTickCount64() - observed.observed > 100 ||
            !collect(repeated, &repeatedNodes, true) || repeated != id || !maskPose(repeatedNodes, repeatedPose) ||
            repeatedPose.position != before.position || repeatedPose.scale != before.scale ||
            !hudAgain.valid || hudAgain.helper != hud.helper || hudAgain.sprite != hud.sprite ||
            hudAgain.left != hud.left || hudAgain.top != hud.top || hudAgain.width != hud.width || hudAgain.height != hud.height) {
            ++rejected; return result;
    }
    // The repeated RPM identity pass can itself stall. Its completion must not
    // revive an observation or page permission that expired during that pass.
    const auto beforeWrite = GetTickCount64();
    if (beforeWrite - observed.observed > 100 || beforeWrite - access.observed > 100) {
        ++rejected; return result;
    }
    current.access = access;
    result.identity = id;
    result.permission = {id.gui[2], true};
    result.original = before;
    result.attempted = true;
    committing = true;
    if (!writeSpriteGeometry(result.permission, after)) {
        restoreNativeReplayAlignmentCommit(result); // Restore BOTH spans after any partial prefix.
        failure("mask geometry write failed");
        return {};
    }
    ++applied;
    return result;
}

void restoreNativeReplayAlignmentCommit(const ReplayAlignment& token) noexcept {
    if (!token.attempted) return;
    ReplayIdentity id;
    if (token.permission.sprite != token.identity.gui[2] || !collect(id) || id != token.identity) {
        failure("mask owner/source identity changed during native commit");
    } else if (!writeSpriteGeometry(token.permission, token.original)) {
        failure("mask geometry restoration failed");
    } else ++restored;
    committing = false;
}

void installNativeReplayAlignment(void* module, bool sharedCommitReady) noexcept {
    try {
        wchar_t option[8] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_REPLAY_ALIGN", option, 8) != 1 || option[0] != L'1' || originalUpdate) return;
        if (!sharedCommitReady || GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", option, 8) != 3 ||
                std::wcscmp(option, L"fit")) { failure("requires native HUD fit and shared GUI commit hook"); return; }
        if (std::strcmp(verifyNativeGameModule(module), "verified")) { failure("game module verification failed"); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 40> updateBytes {{
            0x48,0x8b,0xc4,0x55,0x56,0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0x00,0x01,0x00,0x00,0x80,0xb9,0x26,
            0x02,0x00,0x00,0x01,0x44,0x0f,0x29,0x74,0x24,0x50,0x49,0x8b,0xf8,0x44,0x0f,0x28,0xf1,0x48,0x8b,0xf1}};
        constexpr std::array<unsigned char, 36> modeBytes {{
            0xc7,0x83,0x2c,0x02,0x00,0x00,0x00,0x00,0x40,0x44,0x74,0x0d,0x48,0xc7,0x83,0x30,0x02,0x00,
            0x00,0x00,0xc0,0xaa,0x44,0xeb,0x0b,0x48,0xc7,0x83,0x30,0x02,0x00,0x00,0x00,0x00,0x96,0x43}};
        constexpr std::array<unsigned char, 13> callerBytes {{0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        float pixelOffset = 0;
        if (!bytesEqual(0x8DFB0, updateBytes) || !bytesEqual(0x8FEC9, modeBytes) ||
                !bytesEqual(0x1E6340, callerBytes) || !bytesEqual(0x1E55BB, geometryBytes) ||
                !read(base, 0x10D0AA4, pixelOffset) || pixelOffset != 0.1f) {
            failure("instruction/geometry guards differ"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        originalUpdate = reinterpret_cast<Update>(base + 0x8DFB0);
        if (!detour::trampoline_try(originalUpdate, &update, &originalUpdate)) { failure("owner update hook failed"); return; }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Native replay alignment enabled: only short KillCamera mask; native content/visibility/fade preserved");
    } catch (...) { failure("installation failed"); }
}
} // namespace bone_eater::render
