#include "render/native_battle_background.h"
#include "diagnostics/output_policy.h"
#include "render/native_viewport.h"
#include "render/native_hud.h"
#include "render/checked_data_write.h"
#include "render/dialogue_illumination_math.h"
#include "render/splash_illumination_math.h"
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <intrin.h>
#include <mutex>

namespace bone_eater::render {
namespace {

using Update = void(__fastcall*)(void*, float);
Update originalUpdate = nullptr;
using DialogueUpdate = void(__fastcall*)(void*);
DialogueUpdate originalDialogueUpdate = nullptr;
std::uintptr_t moduleBase = 0;
std::atomic<bool> enabled {false}, failed {false};
std::atomic<std::uint64_t> suppressed {0}, restored {0};
std::atomic<std::uint64_t> aligned {0}, geometryRejected {0};
std::atomic<std::uint64_t> dialogueAligned {0}, dialogueRestored {0}, dialogueRejected {0};
bool alignmentMode = false; // Fixed before publishing the installed hook.
std::atomic<ULONGLONG> nextReport {0};
constexpr std::uintptr_t kOwnerVtable = 0x10CA200;
constexpr std::uintptr_t kSpriteVtable = 0x10CEBA8;
constexpr std::uintptr_t kRectVtable = 0x10CEB48;
constexpr std::array<const char*, 2> kNames {{"m_bt_UpperBg0", "m_bt_UpperBg1"}};
using ObjectBytes = std::array<unsigned char, 0xD0>;

struct Observed {
    BattleBackgroundSuppression identity;
    PrivateDataByteAccess access;
    SpriteGeometryAccess geometryAccess;
    ULONGLONG time = 0;
    bool valid = false;
};
thread_local Observed current;
thread_local std::array<std::uintptr_t,60> splashLeaves{};
thread_local bool updating = false;
thread_local bool committing = false;
thread_local std::uintptr_t activeAlignedRecord=0;
thread_local unsigned activeAlignedIndex=2;
struct ObservedDialogue {
    BattleBackgroundSuppression identity;
    SpriteGeometryAccess access;
    ULONGLONG time = 0;
    bool valid = false;
};
thread_local ObservedDialogue currentDialogue;
thread_local bool updatingDialogue = false;

bool observeTimingEnabled = false; // Opt-in, fixed before hook installation.
std::uint64_t observeTimingFrequency = 0;
struct ObserveTimingWindow {
    std::uint64_t start = 0, calls = 0, ticks = 0, maximum = 0;
    PrivateDataObservationTiming operations;
};
thread_local ObserveTimingWindow observeTimingWindow;

void recordObserveTiming(std::uint64_t start, const PrivateDataObservationTiming& operations) noexcept {
    if (!start) return;
    const auto end = privateDataTimingStart();
    if (end < start) return;
    auto& window = observeTimingWindow;
    if (!window.start) window.start = start;
    ++window.calls;
    const auto elapsed = end - start;
    window.ticks += elapsed;
    if (elapsed > window.maximum) window.maximum = elapsed;
    mergePrivateDataTiming(window.operations.reads, operations.reads);
    mergePrivateDataTiming(window.operations.queries, operations.queries);
}

void reportObserveTiming() {
    if (!observeTimingEnabled || !observeTimingFrequency || !observeTimingWindow.start) return;
    const auto now = privateDataTimingStart();
    if (now <= observeTimingWindow.start) return;
    const auto completed = observeTimingWindow;
    observeTimingWindow = {};
    observeTimingWindow.start = now; // Logging is excluded from operation totals.
    if (!completed.calls) return;
    const double ms = 1000.0 / static_cast<double>(observeTimingFrequency);
    const double totalMs = completed.ticks * ms;
    const auto& reads = completed.operations.reads;
    const auto& queries = completed.operations.queries;
    const double readMs = reads.ticks * ms, queryMs = queries.ticks * ms;
    log_info("bone-eater", "Battle background observe split pid={} thread={} owner=battle_main mode={} window_ms={:.3f} observe_calls={} total_ms={:.3f} read_calls={} read_timed={} read_ms={:.3f} read_max_us={:.3f} query_calls={} query_timed={} query_ms={:.3f} query_max_us={:.3f} other_ms={:.3f}",
        GetCurrentProcessId(), GetCurrentThreadId(), alignmentMode ? "align" : "suppress",
        (now - completed.start) * ms, completed.calls, totalMs,
        reads.calls, reads.timedCalls, readMs, reads.maximum * ms * 1000.0,
        queries.calls, queries.timedCalls, queryMs, queries.maximum * ms * 1000.0,
        totalMs > readMs + queryMs ? totalMs - readMs - queryMs : 0.0);
}

template<typename T>
bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    auto* timing = privateDataObservationTiming;
    const auto begin = timing ? privateDataTimingStart() : 0;
    const BOOL result = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(T), &copied);
    if (timing) privateDataTimingRecord(timing->reads, begin);
    return result && copied == sizeof(T);
}

// Opt-in owner evidence for the remaining briefing audit. Never changes data.
DialogueUpdate originalBriefingUpdate = nullptr;
void __fastcall observeBriefing(void* object) {
    originalBriefingUpdate(object);
    static std::atomic<unsigned> reports {0};
    static std::atomic<ULONGLONG> next {0};
    const auto now = GetTickCount64();
    auto due = next.load();
    if (now < due || !next.compare_exchange_strong(due, now + 1000) || reports.fetch_add(1) >= 90) return;
    const auto owner = reinterpret_cast<std::uintptr_t>(object);
    std::uintptr_t vt = 0, actor = 0, effect = 0, filter = 0;
    unsigned state = 0;
    unsigned char multipass = 0;
    if (!read(owner, 0, vt) || vt != moduleBase + 0x10CA2F0 ||
            !read(owner, 0x10, state) || !read(owner, 0x18, actor) || !read(owner, 0x20, effect)) return;
    if (effect) { read(effect, 0x38, filter); read(effect, 0x2B, multipass); }
    try { log_info("bone-eater", "Briefing audit owner=0x{:X} state={} actor=0x{:X} effect=0x{:X} filter=0x{:X} multipass={}",
        owner, state, actor, effect, filter, static_cast<unsigned>(multipass)); } catch (...) {}
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& bytes, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

void failure(const char* reason) noexcept {
    if (!failed.exchange(true, std::memory_order_relaxed)) {
        try { log_warning("bone-eater", "Battle background adaptation disabled: {}", reason); }
        catch (...) {}
    }
}

bool named(std::uintptr_t object, std::uintptr_t type, std::uint32_t group,
        const char* expected, ObjectBytes& bytes) noexcept {
    if (!read(object, 0, bytes) || field<std::uintptr_t>(bytes, 0) != moduleBase + type ||
            field<std::uintptr_t>(bytes, 8) != object ||
            field<std::uint32_t>(bytes, 0x3C) != group || bytes[0xC9] > 1) return false;
    const char* name = reinterpret_cast<const char*>(bytes.data() + 0x40);
    return std::memchr(name, 0, 32) && !std::strcmp(name, expected);
}

bool ownerMatches(const BattleBackgroundSuppression& id) noexcept {
    ObjectBytes owner {};
    return read(id.owner, 0, owner) &&
        field<std::uintptr_t>(owner, 0) == moduleBase + kOwnerVtable &&
        field<std::uint32_t>(owner, 0x48) == 3 &&
        field<std::uint32_t>(owner, 0x98) == id.group &&
        field<std::uintptr_t>(owner, 0xA0) == id.root &&
        field<std::uintptr_t>(owner, 0xC8) == id.upper;
}

// Exact owner -> animated Upper -> fixed background -> two leaf chain.
// +18 is first child, +20 previous sibling and +28 next sibling; the native
// unlink/link sequence at122998..122A58 proves this topology. Requiring the
// complete two-leaf chain avoids registry scans and excludes every marker.
bool chainMatches(const BattleBackgroundSuppression& id,
        std::array<unsigned char, 2>* visibility = nullptr) noexcept {
    if (!id.owner || !id.root || !id.upper || !id.background || !id.leaves[0] || !id.leaves[1] ||
            !id.group || id.group >= 32) return false;
    const std::array<std::uintptr_t, 5> objects {{id.root, id.upper, id.background, id.leaves[0], id.leaves[1]}};
    for (std::size_t i = 0; i < objects.size(); ++i)
        for (std::size_t j = 0; j < i; ++j) if (objects[i] == objects[j]) return false;
    ObjectBytes bytes {};
    if (!ownerMatches(id) || !named(id.root, kRectVtable, id.group, "root", bytes) ||
            field<std::uintptr_t>(bytes, 0x10) != 0 ||
            !named(id.upper, kSpriteVtable, id.group, "root_Upper", bytes) ||
            field<std::uintptr_t>(bytes, 0x10) != id.root ||
            field<std::uintptr_t>(bytes, 0x18) != id.background ||
            !named(id.background, kSpriteVtable, id.group, "root_UpperBg", bytes) ||
            field<std::uintptr_t>(bytes, 0x10) != id.upper ||
            field<std::uintptr_t>(bytes, 0x18) != id.leaves[0] ||
            field<std::uintptr_t>(bytes, 0x20) != 0 || field<std::uintptr_t>(bytes, 0x28) != 0 ||
            field<float>(bytes, 0x60) != -400.0f || field<float>(bytes, 0x64) != 0.0f) return false;
    for (std::size_t i = 0; i < 2; ++i) {
        if (!named(id.leaves[i], kSpriteVtable, id.group, kNames[i], bytes) ||
                field<std::uintptr_t>(bytes, 0x10) != id.background ||
                field<std::uintptr_t>(bytes, 0x18) != 0 ||
                field<std::uintptr_t>(bytes, 0x20) != (i ? id.leaves[0] : 0) ||
                field<std::uintptr_t>(bytes, 0x28) != (i ? 0 : id.leaves[1]) ||
                field<float>(bytes, 0x60) != (i ? 400.0f : 0.0f) ||
                field<float>(bytes, 0x64) != -43.0f ||
                field<float>(bytes, 0x98) != 400.0f || field<float>(bytes, 0x9C) != 481.0f ||
                field<float>(bytes, 0xA0) != 1.0f || field<float>(bytes, 0xA4) != 1.0f) return false;
        if (alignmentMode && (field<float>(bytes, 0x90) != 0.0f || field<float>(bytes, 0x94) != 0.0f ||
                field<float>(bytes, 0xB0) != 0.0f || field<float>(bytes, 0xB4) != 0.0f)) return false;
        if (visibility) (*visibility)[i] = bytes[0xC9];
    }
    return ownerMatches(id);
}

bool closeFloat(float actual, float expected) noexcept {
    return std::isfinite(actual) && std::fabs(actual - expected) <= 0.001f;
}

// This global publishes one complete manager; its +230 field owns this exact
// CDialogueWIndows, whose loaded display-0 layout owns one named Root/bg pair.
bool dialogueOwnerMatches(const BattleBackgroundSuppression& id) noexcept {
    if (!id.dialogue || !id.dialogueManager || !id.owner || !id.dialogueLayout || !id.root ||
            !id.leaves[0] || id.leaves[1] || id.index || !id.group || id.group >= 32) return false;
    std::uintptr_t manager = 0, vtable = 0, owner = 0, layout = 0, root = 0, config = 0;
    std::uint64_t state = 0;
    std::uint32_t layoutState = 0, substate = 0, count = 0, display = 0, group = 0;
    unsigned char twoDisplay = 0;
    std::array<char, 32> name {};
    if (!read(moduleBase, 0x13DCF28, manager) || manager != id.dialogueManager ||
            !read(manager, 0, vtable) || vtable != moduleBase + 0x10C7378 ||
            !read(manager, 0x230, owner) || owner != id.owner ||
            !read(owner, 0, vtable) || vtable != moduleBase + 0x10CA328 ||
            !read(owner, 8, state) || state != 0x34 ||
            !read(owner, 0x18, layout) || layout != id.dialogueLayout ||
            !read(layout, 0, vtable) || vtable != moduleBase + 0x10C9FA8 ||
            !read(layout, 0x18, layoutState) || layoutState != 2 ||
            !read(layout, 0x1C, substate) || substate != 1 ||
            !read(layout, 0x3C90, display) || display != 0 ||
            !read(layout, 0x3C98, count) || count != 1 ||
            !read(layout, 0x3F8, group) || group != id.group ||
            !read(layout, 0x428, root) || root != id.root ||
            !read(layout, 0x368, name) || !std::memchr(name.data(), 0, name.size()) ||
            std::strcmp(name.data(), "Root") ||
            !read(moduleBase, 0x13DCF58, config) || !read(config, 0x1373, twoDisplay) ||
            twoDisplay != 1) return false;
    return true;
}

bool dialogueChainMatches(const BattleBackgroundSuppression& id, bool* visible = nullptr, bool restoring = false) noexcept {
    const std::array<std::uintptr_t, 5> objects {{id.dialogueManager, id.owner,
        id.dialogueLayout, id.root, id.leaves[0]}};
    for (std::size_t i = 0; i < objects.size(); ++i)
        for (std::size_t j = 0; j < i; ++j) if (objects[i] == objects[j]) return false;
    ObjectBytes root {}, leaf {};
    constexpr float scale = 1080.0f / 1366.0f;
    if (!dialogueOwnerMatches(id) || !named(id.root, kRectVtable, id.group, "Root", root) ||
            field<std::uintptr_t>(root, 0x10) || field<std::uintptr_t>(root, 0x20) ||
            field<std::uintptr_t>(root, 0x28) || field<std::uintptr_t>(root, 0x18) != id.leaves[0] ||
            field<float>(root, 0x60) != 0 || field<float>(root, 0x64) != 0 ||
            field<float>(root, 0x70) != 0 || field<float>(root, 0x74) != 0 ||
            field<float>(root, 0x98) != 1 || field<float>(root, 0x9C) != 1 ||
            !closeFloat(field<float>(root, 0xA0), scale) || !closeFloat(field<float>(root, 0xA4), scale) ||
            !closeFloat(field<float>(root, 0xA8), scale) || !closeFloat(field<float>(root, 0xAC), scale) ||
            !named(id.leaves[0], kSpriteVtable, id.group, "bg", leaf) ||
            field<std::uintptr_t>(leaf, 0x10) != id.root || field<std::uintptr_t>(leaf, 0x18) ||
            field<std::uintptr_t>(leaf, 0x20) || field<std::uintptr_t>(leaf, 0x28) ||
            field<float>(leaf, 0x60) != 0 || field<float>(leaf, 0x64) != 270 ||
            field<float>(leaf, 0x98) != 800 || field<float>(leaf, 0x9C) != 211 ||
            field<float>(leaf, 0xA0) != 1 || field<float>(leaf, 0xA4) != 1 ||
            (!restoring && !closeFloat(field<float>(leaf, 0xA8), scale)) || !closeFloat(field<float>(leaf, 0xAC), scale)) return false;
    for (const auto* object : {&root, &leaf})
        for (const std::size_t offset : {std::size_t(0x90), std::size_t(0x94),
                std::size_t(0xB0), std::size_t(0xB4)})
            if (field<float>(*object, offset) != 0) return false;
    if (visible) *visible = root[0xC9] == 1 && leaf[0xC9] == 1;
    // Do not inspect cached leaf XY here: restoration uses this identity check
    // while those exact bytes temporarily contain the translated submission.
    return dialogueOwnerMatches(id);
}

void observeDialogue(void* object, const ObservedDialogue& previous) noexcept {
    ObservedDialogue result;
    auto& id = result.identity;
    id.dialogue = true;
    id.owner = reinterpret_cast<std::uintptr_t>(object);
    ObjectBytes owner {};
    if (!read(id.owner, 0, owner) || field<std::uintptr_t>(owner, 0) != moduleBase + 0x10CA328 ||
            field<std::uint64_t>(owner, 8) != 0x34) return;
    if (!read(moduleBase, 0x13DCF28, id.dialogueManager) ||
            !read(id.owner, 0x18, id.dialogueLayout) ||
            !read(id.dialogueLayout, 0x428, id.root) ||
            !read(id.dialogueLayout, 0x3F8, id.group) || !read(id.root, 0x18, id.leaves[0])) return;
    bool visible = false;
    if (!dialogueChainMatches(id, &visible) || !visible) return;
    result.time = GetTickCount64();
    const auto& old = previous.identity;
    const bool sameIdentity = previous.valid && old.dialogueManager == id.dialogueManager &&
        old.owner == id.owner && old.dialogueLayout == id.dialogueLayout && old.root == id.root &&
        old.leaves[0] == id.leaves[0] && old.group == id.group;
    result.access = observeSingleSpriteGeometry(id.leaves[0], result.time,
        sameIdentity ? &previous.access : nullptr);
    if (!result.access.valid || !dialogueOwnerMatches(id)) return;
    result.valid = true;
    currentDialogue = result;
}

void observe(void* object, const Observed& previous) noexcept {
    Observed result;
    auto& id = result.identity;
    id.owner = reinterpret_cast<std::uintptr_t>(object);
    ObjectBytes bytes {};
    if (!read(id.owner, 0, bytes) || field<std::uintptr_t>(bytes, 0) != moduleBase + kOwnerVtable ||
            field<std::uint32_t>(bytes, 0x48) != 3) return;
    id.group = field<std::uint32_t>(bytes, 0x98);
    id.root = field<std::uintptr_t>(bytes, 0xA0);
    id.upper = field<std::uintptr_t>(bytes, 0xC8);
    if (!read(id.upper, 0x18, id.background) || !read(id.background, 0x18, id.leaves[0]) ||
            !read(id.leaves[0], 0x28, id.leaves[1]) || !chainMatches(id)) return;
    result.time = GetTickCount64();
    std::array<std::uintptr_t, 2> addresses {};
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (id.leaves[i] > std::numeric_limits<std::uintptr_t>::max() - 0xC9) return;
        addresses[i] = id.leaves[i] + 0xC9;
    }
    // Reuse a short permission lease only after rebuilding the complete native
    // chain. The shared helper preserves its original age and exact byte list.
    const auto& old = previous.identity;
    const bool sameIdentity = previous.valid && old.owner == id.owner && old.group == id.group &&
        old.root == id.root && old.upper == id.upper && old.background == id.background &&
        old.leaves[0] == id.leaves[0] && old.leaves[1] == id.leaves[1];
    if (alignmentMode) {
        const std::array<std::uintptr_t, 2> sprites {{id.leaves[0], id.leaves[1]}};
        result.geometryAccess = observeSpriteGeometry(sprites, result.time,
            sameIdentity ? &previous.geometryAccess : nullptr);
        if (!result.geometryAccess.valid) return;
    } else {
        result.access = observePrivateDataBytes(addresses.data(), addresses.size(), result.time,
            sameIdentity ? &previous.access : nullptr);
        if (!result.access.valid) return;
    }
    if (!ownerMatches(id)) return;
    result.valid = true;
    current = result;
    if(alignmentMode&&!read(id.owner,0x3540,splashLeaves))splashLeaves={};
}

void report() noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    const auto now = GetTickCount64();
    auto previous = nextReport.load(std::memory_order_relaxed);
    if (now < previous || !nextReport.compare_exchange_strong(previous, now + 5000)) return;
    try {
        log_info("bone-eater", "Battle background filter owner={:x} group={} ready={} suppressed={} restored={} failed={} mode={} pid={} aligned={} geometry_rejected={}",
            current.identity.owner, current.identity.group, current.valid,
            suppressed.load(), restored.load(), failed.load(), alignmentMode ? "align" : "suppress",
            GetCurrentProcessId(), aligned.load(), geometryRejected.load());
        if (alignmentMode) log_info("bone-eater", "Dialogue illumination owner={:x} group={} ready={} aligned={} restored={} rejected={} pid={}",
            currentDialogue.identity.owner, currentDialogue.identity.group, currentDialogue.valid,
            dialogueAligned.load(), dialogueRestored.load(), dialogueRejected.load(), GetCurrentProcessId());
        reportObserveTiming();
    } catch (...) {}
}

void __fastcall update(void* object, float elapsed) {
    if (updating) { originalUpdate(object, elapsed); return; }
    updating = true;
    const Observed previous = current;
    current = {}; // Invalidate both identities and permissions before native animation.
    splashLeaves={};
    auto* previousTiming = privateDataObservationTiming;
    __try {
        originalUpdate(object, elapsed); // Native animation at121D64 completes first.
        if (!failed.load(std::memory_order_relaxed)) {
            const auto begin = observeTimingEnabled ? privateDataTimingStart() : 0;
            PrivateDataObservationTiming operations;
            if (begin) privateDataObservationTiming = &operations;
            observe(object, previous);
            privateDataObservationTiming = previousTiming;
            recordObserveTiming(begin, operations);
        }
        report();
    } __finally {
        privateDataObservationTiming = previousTiming;
        updating = false;
    }
}

void __fastcall updateDialogue(void* object) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (updatingDialogue) { originalDialogueUpdate(object); return; }
    updatingDialogue = true;
    const ObservedDialogue previous = currentDialogue;
    currentDialogue = {}; // Rebuild native owner identity after every update.
    __try {
        originalDialogueUpdate(object);
        if (caller == moduleBase + 0xD6421 && !failed.load(std::memory_order_relaxed))
            observeDialogue(object, previous);
        report();
    } __finally {
        updatingDialogue = false;
    }
}

bool writeVisibility(const BattleBackgroundSuppression& token, unsigned char value) noexcept {
    if (token.index >= 2) return false;
    const auto object = token.leaves[token.index];
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - 0xC9 ||
            !token.permission.valid || token.permission.address != object + 0xC9) return false;
    return writePrivateDataByte(token.permission, value);
}

template<std::size_t N>
bool bytesEqual(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(moduleBase, rva, actual) && actual == expected;
}

bool environmentEnabled(const wchar_t* name) noexcept {
    wchar_t value[4] {};
    return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
}

bool readPose(std::uintptr_t object, RearIlluminationPose& pose) noexcept {
    return read(object, 0x70, pose.position) && read(object, 0xA8, pose.scale);
}

bool prepareDialogueAlignment(BattleBackgroundSuppression& result) noexcept {
    const auto object = result.leaves[0];
    const auto front = readNativeHudGeometry();
    RearIlluminationPose replacement, repeated;
    if (!front.valid || front.mainWidth != 1920 || front.mainHeight != 1080 ||
            front.sourceWidth != 768 || front.sourceHeight != 1366 ||
            !readPose(object, result.originalGeometry) ||
            !alignDialogueIllumination(front.left, front.top, front.width, front.height,
                result.originalGeometry, replacement)) return false;
    result.geometryPermission = acquireSpriteGeometry(currentDialogue.access, object);
    bool visible = false;
    if (!result.geometryPermission.valid || !dialogueChainMatches(result, &visible) || !visible ||
            !readPose(object, repeated) || repeated.position != result.originalGeometry.position ||
            repeated.scale != result.originalGeometry.scale) return false;
    const auto frontAgain = readNativeHudGeometry();
    if (!frontAgain.valid || frontAgain.helper != front.helper || frontAgain.sprite != front.sprite ||
            frontAgain.left != front.left || frontAgain.top != front.top ||
            frontAgain.width != front.width || frontAgain.height != front.height) return false;
    result.alignment = true;
    result.attempted = true;
    committing = true;
    if (!writeSpriteGeometry(result.geometryPermission, replacement)) {
        restoreNativeBattleBackgroundCommit(result); // Also restores a partially applied XY span.
        failure("dialogue illumination position application failed");
        result = {};
        return false;
    }
    dialogueAligned.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool prepareAlignment(BattleBackgroundSuppression& result) noexcept {
    const auto object = result.leaves[result.index];
    const auto front = readNativeHudGeometry();
    RearIlluminationPose replacement, repeated;
    if (!front.valid || front.mainWidth != 1920 || front.mainHeight != 1080 ||
            front.sourceWidth != 768 || front.sourceHeight != 1366 ||
            !readPose(object, result.originalGeometry) ||
            !alignRearIllumination(front.left, front.top, front.width, front.height,
                result.originalGeometry, replacement)) return false;
    result.geometryPermission = acquireSpriteGeometry(current.geometryAccess, object);
    if (!result.geometryPermission.valid || !chainMatches(result) || !readPose(object, repeated) ||
            repeated.position != result.originalGeometry.position || repeated.scale != result.originalGeometry.scale)
        return false;
    const auto frontAgain = readNativeHudGeometry();
    if (!frontAgain.valid || frontAgain.helper != front.helper || frontAgain.sprite != front.sprite ||
            frontAgain.left != front.left || frontAgain.top != front.top ||
            frontAgain.width != front.width || frontAgain.height != front.height) return false;
    result.alignment = true;
    result.attempted = true;
    committing = true;
    if (!writeSpriteGeometry(result.geometryPermission, replacement)) {
        // Both original spans are attempted even if the application failed
        // after its first byte/span. Native original still runs once upstream.
        restoreNativeBattleBackgroundCommit(result);
        failure("illumination geometry application failed");
        result = {};
        return false;
    }
    aligned.fetch_add(1, std::memory_order_relaxed);
    activeAlignedRecord=0;activeAlignedIndex=2;
    if(read(object,0x30,activeAlignedRecord))activeAlignedIndex=result.index;
    return true;
}

bool splashChainMatches(const BattleBackgroundSuppression& id) noexcept {
    std::uintptr_t manager=0,owner=0,splash=0,vt=0,leaf=0,front=0;unsigned state=0,mode=0;int rearId=-1,frontId=-1;
    if(id.splashSlot>=60||id.splashFrontSlot>=60||id.splashId<0||
       !read(moduleBase,0x13DCF78,manager)||manager!=id.splashManager||!read(manager,0,vt)||vt!=moduleBase+0x10CA298||
       !read(manager,0x48,state)||state!=3||!read(manager,0xD8,owner)||owner!=id.owner||
       !read(manager,0xE8,splash)||splash!=id.splashOwner||!read(splash,0,vt)||vt!=moduleBase+0x10CA680||
       !ownerMatches(id)||!read(owner,0x3540+id.splashSlot*8,leaf)||leaf!=id.leaves[0]||
       !read(owner,0x3720+id.splashSlot*4,rearId)||rearId!=id.splashId||
       !read(splash,0x930+id.splashFrontSlot*8,front)||front!=id.splashFront||
       !read(splash,0xB10+id.splashFrontSlot*4,frontId)||frontId!=rearId||
       !read(splash,0x478+id.splashFrontSlot*4,state)||state!=2||
       !read(splash,0xE58+id.splashFrontSlot*4,mode)||mode!=2)return false;
    ObjectBytes bytes{};char expected[32]{};
    if(id.splashSlot)std::snprintf(expected,sizeof(expected),"m_bt_splashBg%2u",id.splashSlot);
    else std::strcpy(expected,"m_bt_splashBg00");
    return named(leaf,kSpriteVtable,id.group,expected,bytes)&&field<std::uintptr_t>(bytes,0x10)==id.root&&bytes[0xC9]==1&&
        field<float>(bytes,0x90)==.5f&&field<float>(bytes,0x94)==.5f&&field<float>(bytes,0xB0)==0;
}

bool prepareSplashAlignment(std::uintptr_t object,unsigned slot,BattleBackgroundSuppression& result) noexcept {
    result=current.identity;result.leaves[0]=object;result.splashSlot=slot;result.index=0;result.splash=true;
    if(!read(moduleBase,0x13DCF78,result.splashManager)||
       !read(result.splashManager,0xE8,result.splashOwner)||
       !read(result.owner,0x3720+slot*4,result.splashId)||result.splashId<0)return false;
    std::array<int,60> ids{};if(!read(result.splashOwner,0xB10,ids))return false;
    unsigned found=60;
    for(unsigned i=0;i<60;++i)if(ids[i]==result.splashId){if(found!=60)return false;found=i;}
    if(found==60)return false;result.splashFrontSlot=found;
    if(!read(result.splashOwner,0x930+found*8,result.splashFront)||!splashChainMatches(result))return false;
    ObjectBytes front{},root{};std::uintptr_t parent=0,renderer=0;unsigned display=0;
    if(!read(result.splashFront,0,front)||field<std::uintptr_t>(front,0)!=moduleBase+kSpriteVtable||
       field<std::uintptr_t>(front,8)!=result.splashFront||front[0xC9]!=1||
       field<float>(front,0x90)!=.5f||field<float>(front,0x94)!=.5f||field<float>(front,0xB0)!=0||
       !read(result.splashFront,0x110,renderer)||!read(renderer,0x570,display)||display!=2)return false;
    parent=field<std::uintptr_t>(front,0x10);
    if(!named(parent,kRectVtable,field<unsigned>(front,0x3C),"root",root)||field<std::uintptr_t>(root,0x10)||
       field<float>(root,0x60)!=0||field<float>(root,0x64)!=0||field<float>(root,0xA0)!=1||field<float>(root,0xA4)!=1)return false;
    std::array<float,2> size{};RearIlluminationPose replacement;
    const auto geometry=readNativeHudGeometry();
    if(!geometry.valid||geometry.mainWidth!=1920||geometry.mainHeight!=1080||geometry.sourceWidth!=768||geometry.sourceHeight!=1366||
       !read(object,0x98,size)||!readPose(object,result.originalGeometry)||
       !alignSplashIllumination(field<std::array<float,2>>(front,0x60),field<std::array<float,2>>(front,0x98),
           field<std::array<float,2>>(front,0xA0),size,replacement))return false;
    const auto access=observeSpriteGeometry({object,result.splashFront},GetTickCount64());
    result.geometryPermission=acquireSpriteGeometry(access,object);
    if(!result.geometryPermission.valid||!splashChainMatches(result))return false;
    result.attempted=result.alignment=true;committing=true;
    if(!writeSpriteGeometry(result.geometryPermission,replacement)){
        restoreNativeBattleBackgroundCommit(result);failure("splash illumination application failed");result={};return false;
    }
    return true;
}

} // namespace

bool certifiedNativeBattleBackgroundRecord(std::uintptr_t record,unsigned& index) noexcept {
    if(!alignmentMode||failed.load()||!record)return false;
    // 1E5560 tail-calls adapter+8 while the aligned geometry is active.
    // Publication still requires manager completion and healthy restoration.
    if(committing&&record==activeAlignedRecord&&activeAlignedIndex<2){index=activeAlignedIndex;return true;}
    return false;
}
bool nativeBattleBackgroundHealthy() noexcept { return enabled.load()&&alignmentMode&&!failed.load(); }

BattleBackgroundSuppression beginNativeBattleBackgroundCommit(
        void* sprite, std::uintptr_t nativeCaller) noexcept {
    BattleBackgroundSuppression result;
    const auto object = reinterpret_cast<std::uintptr_t>(sprite);
    // Unrelated commits incur no RPM, timer query, ancestry walk or logging.
    if (!enabled.load(std::memory_order_acquire) || failed.load(std::memory_order_relaxed) ||
            committing || nativeCaller != moduleBase + 0x1E634D) return result;
    if(alignmentMode&&current.valid){
        for(unsigned i=0;i<splashLeaves.size();++i)if(object==splashLeaves[i]){
            if(prepareSplashAlignment(object,i,result))return result;
            return {};
        }
    }
    if (alignmentMode && currentDialogue.valid && object == currentDialogue.identity.leaves[0]) {
        if (GetTickCount64() - currentDialogue.time > 100) return result;
        result = currentDialogue.identity;
        if (prepareDialogueAlignment(result)) return result;
        dialogueRejected.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    if (!current.valid || (object != current.identity.leaves[0] &&
            object != current.identity.leaves[1])) return result;
    if (GetTickCount64() - current.time > 100) return result;
    result = current.identity;
    result.index = object == result.leaves[0] ? 0U : 1U;
    std::array<unsigned char, 2> visibility {};
    if (!chainMatches(result, &visibility) || visibility[result.index] != 1) return {};
    if (alignmentMode) {
        if (prepareAlignment(result)) return result;
        geometryRejected.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    if (object > std::numeric_limits<std::uintptr_t>::max() - 0xC9) return {};
    result.permission = acquirePrivateDataBytePermission(current.access, object + 0xC9);
    if (!result.permission.valid) return {};
    result.previous = visibility[result.index];
    result.attempted = true;
    committing = true;
    if (!writeVisibility(result, 0)) {
        restoreNativeBattleBackgroundCommit(result); // Covers an unexpected partial write.
        failure("visibility suppression write failed");
        return {};
    }
    suppressed.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void restoreNativeBattleBackgroundCommit(const BattleBackgroundSuppression& token) noexcept {
    if(token.attempted){activeAlignedRecord=0;activeAlignedIndex=2;}
    if (!token.attempted) return;
    if(token.splash){
        if(!splashChainMatches(token)||!token.geometryPermission.valid||token.geometryPermission.sprite!=token.leaves[0]||
           !writeSpriteGeometry(token.geometryPermission,token.originalGeometry))failure("splash illumination restoration failed");
        committing=false;return;
    }
    if (token.dialogue) {
        // Uses only the captured exact owner/leaf and saved permission. A slow
        // original call or a newer TLS observation cannot invalidate restoration.
        if (!dialogueChainMatches(token, nullptr, true) || !token.alignment || !token.geometryPermission.valid ||
                token.geometryPermission.sprite != token.leaves[0] ||
                !writeSpriteGeometry(token.geometryPermission, token.originalGeometry))
            failure("dialogue illumination position restoration failed");
        else dialogueRestored.fetch_add(1, std::memory_order_relaxed);
        committing = false;
        return;
    }
    std::array<unsigned char, 2> visibility {};
    if (token.index >= 2 || !chainMatches(token, &visibility)) {
        failure("owner/leaf identity changed during native commit");
    } else if (token.alignment) {
        if (!token.geometryPermission.valid || token.geometryPermission.sprite != token.leaves[token.index] ||
                !writeSpriteGeometry(token.geometryPermission, token.originalGeometry)) {
            failure("illumination geometry restoration failed");
        } else {
            restored.fetch_add(1, std::memory_order_relaxed);

        }
    } else if (visibility[token.index] != token.previous &&
            !writeVisibility(token, token.previous)) {
        failure("visibility restoration write failed");
    } else {
        restored.fetch_add(1, std::memory_order_relaxed);
    }
    committing = false;
}

void installNativeBattleBackgroundFilter(void* module, bool sharedCommitReady) noexcept {
    const bool filter = environmentEnabled(L"BONE_EATER_BATTLE_BACKGROUND_FILTER");
    const bool align = environmentEnabled(L"BONE_EATER_BATTLE_BACKGROUND_ALIGN");
    if ((!filter && !align) || originalUpdate) return;
    try {
        if (filter && align) {
            log_warning("bone-eater", "Battle background adaptation rejected: FILTER and ALIGN are mutually exclusive; neither option was installed");
            return;
        }
        if (!sharedCommitReady || !environmentEnabled(L"BONE_EATER_DESKTOP_RETICLE")) {
            log_warning("bone-eater", "Battle background adaptation requires --desktop-reticle and its installed shared commit hook");
            return;
        }
        if (align) {
            wchar_t hud[8] {};
            if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hud, 8) != 3 || std::wcscmp(hud, L"fit")) {
                log_warning("bone-eater", "Battle background alignment requires native HUD fit; no hook installed");
                return;
            }
        }
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Battle background filter disabled: {}", status);
            return;
        }
        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        alignmentMode = align;
        if (diagnostics::optionalOutputEnabled() && environmentEnabled(L"BONE_EATER_RETICLE_TIMING")) {
            LARGE_INTEGER frequency {};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
                observeTimingFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
                observeTimingEnabled = true;
            }
        }
        constexpr std::array<unsigned char, 32> updateBytes {{
            0x40,0x53,0x48,0x83,0xec,0x30,0x48,0x8b,0xd9,0x8b,0x49,0x4c,0x0f,0x29,0x74,0x24,
            0x20,0x0f,0x28,0xf1,0x85,0xc9,0x74,0x1c,0xff,0xc9,0x75,0x1b,0x38,0x4b,0x71,0x74}};
        constexpr std::array<unsigned char, 36> animationBytes {{
            0xe8,0x91,0x2a,0x00,0x00,0x48,0x8b,0xcb,0x0f,0x28,0xce,0xe8,0xf6,0x19,0x00,0x00,
            0x48,0x8b,0x8b,0x90,0x00,0x00,0x00,0x0f,0x28,0xce,0xe8,0x77,0x67,0x0c,0x00,0x0f,
            0x28,0x74,0x24,0x20}};
        constexpr std::array<unsigned char, 13> commitCallBytes {{
            0x48,0x8b,0x4c,0xdc,0x20,0x48,0x8b,0x09,0xe8,0x13,0xf2,0xff,0xff}};
        constexpr std::array<unsigned char, 95> geometryBytes {{
            0xf3,0x0f,0x10,0x4b,0x70,0xf3,0x0f,0x10,0x53,0x74,0x48,0x8b,0x43,0x30,0x48,0x8d,
            0x93,0x98,0x00,0x00,0x00,0xf3,0x0f,0x10,0x5b,0x78,0xf3,0x0f,0x5c,0x0d,0xc7,0xb4,
            0xee,0x00,0xf3,0x0f,0x5c,0x15,0xbf,0xb4,0xee,0x00,0xc6,0x83,0xc3,0x00,0x00,0x00,
            0x00,0xf3,0x0f,0x11,0x48,0x1c,0xf3,0x0f,0x11,0x50,0x20,0xf3,0x0f,0x11,0x58,0x10,
            0x48,0x8b,0x4b,0x30,0x8b,0x83,0xa8,0x00,0x00,0x00,0x89,0x41,0x2c,0x8b,0x83,0xac,
            0x00,0x00,0x00,0x89,0x41,0x30,0x48,0x8b,0x4b,0x30,0xe8,0xc6,0x73,0x00,0x00}};
        constexpr std::array<unsigned char, 32> dialogueUpdateBytes {{
            0x40,0x57,0x48,0x81,0xec,0x90,0x00,0x00,0x00,0x48,0x8b,0xf9,0x48,0x8b,0x49,0x40,
            0x48,0x85,0xc9,0x0f,0x84,0x01,0x09,0x00,0x00,0x48,0x8b,0x47,0x48,0x8b,0x91,0x24}};
        constexpr std::array<unsigned char, 15> dialogueCallBytes {{
            0x48,0x8b,0x89,0xb8,0x01,0x00,0x00,0x0f,0x28,0xf9,0x48,0x8b,0x01,0xff,0x10}};
        constexpr std::array<unsigned char, 28> dialogueResetBytes {{
            0x80,0xb8,0x73,0x13,0x00,0x00,0x00,0x74,0x13,0x49,0x8b,0x46,0x18,0x48,
            0x8b,0x88,0x28,0x04,0x00,0x00,0x48,0xc7,0x41,0x60,0x00,0x00,0x00,0x00}};
        float pixelOffset = 0;
        if (!bytesEqual(0x121CA0, updateBytes) || !bytesEqual(0x121D4A, animationBytes) ||
                !bytesEqual(0x1E6340, commitCallBytes) || (align &&
                (!bytesEqual(0x1E55BB, geometryBytes) || !bytesEqual(0x12A1F0, dialogueUpdateBytes) ||
                    !bytesEqual(0xD6412, dialogueCallBytes) || !bytesEqual(0x1299B1, dialogueResetBytes) ||
                    !read(moduleBase, 0x10D0AA4, pixelOffset) ||
                    pixelOffset != 0.1f))) {
            failure("native instruction bytes differ"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        originalUpdate = reinterpret_cast<Update>(moduleBase + 0x121CA0);
        if (!detour::trampoline_try(originalUpdate, &update, &originalUpdate)) {
            failure("BattleUIMain post-animation observer could not be installed"); return;
        }
        if (align) {
            originalDialogueUpdate = reinterpret_cast<DialogueUpdate>(moduleBase + 0x12A1F0);
            if (!detour::trampoline_try(originalDialogueUpdate, &updateDialogue, &originalDialogueUpdate)) {
                failure("CDialogueWIndows post-update observer could not be installed"); return;
            }
        }
        if (diagnostics::optionalOutputEnabled() && environmentEnabled(L"BONE_EATER_FRONT_OBSERVE")) {
            constexpr std::array<unsigned char, 17> briefingEntry {{
                0x40,0x55,0x56,0x41,0x56,0x48,0x8d,0x6c,0x24,0x80,0x48,0x81,0xec,0x80,0x01,0x00,0x00}};
            if (bytesEqual(0x127030, briefingEntry)) {
                originalBriefingUpdate = reinterpret_cast<DialogueUpdate>(moduleBase + 0x127030);
                if (!detour::trampoline_try(originalBriefingUpdate, &observeBriefing, &originalBriefingUpdate))
                    originalBriefingUpdate = nullptr;
            }
        }
        enabled.store(true, std::memory_order_release);
        log_info("bone-eater", "Battle background diagnostic {} enabled pid={} for only m_bt_UpperBg0 and m_bt_UpperBg1; exact original fields restored after each shared native commit",
            alignmentMode ? "alignment" : "filter", GetCurrentProcessId());
        if (alignmentMode) log_info("bone-eater", "Dialogue illumination alignment enabled for only the exact owned bg leaf; landscape width during shared commit, native vertical geometry and fades preserved");
    } catch (...) { failure("installation failed"); }
}

} // namespace bone_eater::render
