// Include the exact native implementation; only detour/log sinks, the clock and
// a query-counting/delay wrapper are substituted. No native game is loaded and
// no hooks are installed. Reads, page queries and checked writes use real Win32
// APIs against this fixture's private allocation.
// Shared-commit integration cases additionally supply an inert original and
// replay/background callbacks; the reticle's actual restore remains unchanged.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fixture {
ULONGLONG now = 1000, queryDelay = 0;
std::size_t queries = 0;
bool ready = true;
bool meterReady = true;
bool loseReadinessOnQuery = false;
constexpr DWORD commitException = 0xE0424242;
constexpr ULONG_PTR commitExceptionPayload = 0xF17E;
struct CommitTrace {
    bool enabled = false, raise = false, valid = true, returnedNormally = false;
    std::uintptr_t object = 0, caller = 0;
    unsigned originalCalls = 0, backgroundBegins = 0, replayBegins = 0, rankingBegins = 0, achievementBegins = 0, marginBegins = 0, normalBegins = 0, optionsBegins = 0;
    unsigned backgroundRestores = 0, replayRestores = 0, rankingRestores = 0, achievementRestores = 0, marginRestores = 0, normalRestores = 0, optionsRestores = 0, eventCount = 0;
    unsigned events[8] {}; // original, Options, Normal Start, margin, achievement, ranking, replay, background
    DWORD caughtCode = 0, caughtFlags = 0;
    ULONG caughtParameterCount = 0;
    ULONG_PTR caughtPayload = 0;
} commitTrace;
struct UpdateTrace {
    bool enabled = false, raise = false, recurse = false, recursing = false, valid = true;
    bool completed = false, caught = false;
    void* object = nullptr;
    unsigned starts = 0, ends = 0, nativeCalls = 0;
} updateTrace;
void recordCommitEvent(unsigned event) noexcept {
    if (commitTrace.eventCount < std::size(commitTrace.events))
        commitTrace.events[commitTrace.eventCount] = event;
    else commitTrace.valid = false;
    ++commitTrace.eventCount;
}
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries;
    now += queryDelay;
    if (loseReadinessOnQuery) ready = false;
    return VirtualQuery(address, information, length);
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#include "../../src/render/native_reticle.cpp"
#undef VirtualQuery
#undef GetTickCount64

namespace bone_eater::render {
bool desktopMetersReady(unsigned) noexcept { return fixture::meterReady; }
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
void installNativeOptionsBacking(void*, bool) noexcept {}
OptionsBackingAlignment beginNativeOptionsBackingCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& t = fixture::commitTrace; ++t.optionsBegins;
    t.valid = t.valid && reinterpret_cast<std::uintptr_t>(object) == t.object && caller == t.caller;
    OptionsBackingAlignment token; token.identity.leaf = t.object; token.identity.group = 0xF55; token.attempted = true;
    return token;
}
void restoreNativeOptionsBackingCommit(const OptionsBackingAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& t = fixture::commitTrace; ++t.optionsRestores; fixture::recordCommitEvent(8);
    t.valid = t.valid && token.attempted && token.identity.leaf == t.object && token.identity.group == 0xF55 &&
        t.normalRestores == 0 && t.marginRestores == 0 && t.achievementRestores == 0 && t.rankingRestores == 0 &&
        t.replayRestores == 0 && t.backgroundRestores == 0 && committing &&
        *reinterpret_cast<const unsigned char*>(t.object + 0xC9) == 0;
}
void installNativeNormalStartBacking(void*, bool) noexcept {}
NormalStartBackingAlignment beginNativeNormalStartBackingCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& t = fixture::commitTrace; ++t.normalBegins;
    t.valid = t.valid && reinterpret_cast<std::uintptr_t>(object) == t.object && caller == t.caller;
    NormalStartBackingAlignment token; token.identity.leaf = t.object; token.identity.group = 0xE55; token.attempted = true;
    return token;
}
void restoreNativeNormalStartBackingCommit(const NormalStartBackingAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& t = fixture::commitTrace; ++t.normalRestores; fixture::recordCommitEvent(7);
    t.valid = t.valid && token.attempted && token.identity.leaf == t.object && token.identity.group == 0xE55 &&
        t.optionsRestores == 1 && t.marginRestores == 0 && t.achievementRestores == 0 && t.rankingRestores == 0 &&
        t.replayRestores == 0 && t.backgroundRestores == 0 && committing &&
        *reinterpret_cast<const unsigned char*>(t.object + 0xC9) == 0;
}
void installNativeMenuMargin(void*, bool) noexcept {}
MenuMarginAlignment beginNativeMenuMarginCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& t = fixture::commitTrace; ++t.marginBegins;
    t.valid = t.valid && reinterpret_cast<std::uintptr_t>(object) == t.object && caller == t.caller;
    MenuMarginAlignment token; token.identity.leaves[3] = t.object; token.identity.group = 0xD55; token.attempted = true;
    return token;
}
void restoreNativeMenuMarginCommit(const MenuMarginAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& t = fixture::commitTrace; ++t.marginRestores; fixture::recordCommitEvent(6);
    t.valid = t.valid && token.attempted && token.identity.leaves[3] == t.object && token.identity.group == 0xD55 &&
        t.normalRestores == 1 && t.achievementRestores == 0 && t.rankingRestores == 0 && t.replayRestores == 0 && t.backgroundRestores == 0 && committing &&
        *reinterpret_cast<const unsigned char*>(t.object + 0xC9) == 0;
}
void installNativeAchievementAlignment(void*, bool) noexcept {}
bool beginNativeAchievementUpdate() noexcept {
    if (!fixture::updateTrace.enabled) return false;
    auto& t = fixture::updateTrace; ++t.starts;
    t.valid = t.valid && updatingBattle && !currentBattleRoots.valid;
    return true;
}
void endNativeAchievementUpdate(void* object, bool completed) noexcept {
    auto& t = fixture::updateTrace; ++t.ends; t.completed = completed;
    t.valid = t.valid && updatingBattle && object == t.object;
}
AchievementAlignment beginNativeAchievementAlignmentCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& t = fixture::commitTrace; ++t.achievementBegins;
    t.valid = t.valid && reinterpret_cast<std::uintptr_t>(object) == t.object && caller == t.caller;
    AchievementAlignment token; token.identity.main = t.object; token.identity.groups[0] = 0xC55; token.attempted = true;
    return token;
}
void restoreNativeAchievementAlignmentCommit(const AchievementAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& t = fixture::commitTrace; ++t.achievementRestores; fixture::recordCommitEvent(5);
    t.valid = t.valid && token.attempted && token.identity.main == t.object && token.identity.groups[0] == 0xC55 &&
        t.marginRestores == 1 && t.rankingRestores == 0 && t.replayRestores == 0 && t.backgroundRestores == 0 && committing &&
        *reinterpret_cast<const unsigned char*>(t.object + 0xC9) == 0;
}
void installNativeRankingBacking(void*, bool) noexcept {}
RankingBackingAlignment beginNativeRankingBackingCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& trace = fixture::commitTrace;
    ++trace.rankingBegins;
    trace.valid = trace.valid && reinterpret_cast<std::uintptr_t>(object) == trace.object && caller == trace.caller;
    RankingBackingAlignment token;
    token.identity.leaf = trace.object; token.identity.group = 0xB55; token.attempted = true;
    return token;
}
void restoreNativeRankingBackingCommit(const RankingBackingAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& trace = fixture::commitTrace;
    ++trace.rankingRestores;
    fixture::recordCommitEvent(4);
    trace.valid = trace.valid && token.attempted && token.identity.leaf == trace.object && token.identity.group == 0xB55 &&
        trace.achievementRestores == 1 && trace.replayRestores == 0 && trace.backgroundRestores == 0 && committing &&
        *reinterpret_cast<const unsigned char*>(trace.object + 0xC9) == 0;
}
void installNativeBattleBackgroundFilter(void*, bool) noexcept {}
BattleBackgroundSuppression beginNativeBattleBackgroundCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& trace = fixture::commitTrace;
    ++trace.backgroundBegins;
    trace.valid = trace.valid && reinterpret_cast<std::uintptr_t>(object) == trace.object && caller == trace.caller;
    BattleBackgroundSuppression token;
    token.owner = trace.object;
    token.group = 0xA55;
    token.attempted = true;
    return token;
}
void restoreNativeBattleBackgroundCommit(const BattleBackgroundSuppression& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& trace = fixture::commitTrace;
    ++trace.backgroundRestores;
    fixture::recordCommitEvent(3);
    trace.valid = trace.valid && token.attempted && token.owner == trace.object && token.group == 0xA55 &&
        trace.replayRestores == 1 && committing &&
        *reinterpret_cast<const unsigned char*>(trace.object + 0xC9) == 0;
}
void installNativeReplayAlignment(void*, bool) noexcept {}
ReplayAlignment beginNativeReplayAlignmentCommit(void* object, std::uintptr_t caller) noexcept {
    if (!fixture::commitTrace.enabled) return {};
    auto& trace = fixture::commitTrace;
    ++trace.replayBegins;
    trace.valid = trace.valid && reinterpret_cast<std::uintptr_t>(object) == trace.object && caller == trace.caller;
    ReplayAlignment token;
    token.identity.owner = trace.object;
    token.identity.group = 0x5AA;
    token.attempted = true;
    return token;
}
void restoreNativeReplayAlignmentCommit(const ReplayAlignment& token) noexcept {
    if (!fixture::commitTrace.enabled) return;
    auto& trace = fixture::commitTrace;
    ++trace.replayRestores;
    fixture::recordCommitEvent(2);
    trace.valid = trace.valid && token.attempted && token.identity.owner == trace.object && token.identity.group == 0x5AA &&
        trace.backgroundRestores == 0 && trace.rankingRestores == 1 && committing &&
        *reinterpret_cast<const unsigned char*>(trace.object + 0xC9) == 0;
}
}

using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

template<class Value> void put(std::uintptr_t address, std::size_t offset, Value value) {
    std::memcpy(reinterpret_cast<void*>(address + offset), &value, sizeof(value));
}
unsigned char visibility(std::uintptr_t object) { return *reinterpret_cast<unsigned char*>(object + 0xC9); }

void __fastcall fixtureOriginalCommit(void* object) {
    auto& trace = fixture::commitTrace;
    ++trace.originalCalls;
    fixture::recordCommitEvent(1);
    trace.valid = trace.valid && reinterpret_cast<std::uintptr_t>(object) == trace.object &&
        trace.backgroundBegins == 1 && trace.replayBegins == 1 && trace.rankingBegins == 1 && trace.achievementBegins == 1 && committing && visibility(trace.object) == 0;
    if (trace.raise) {
        const ULONG_PTR payload = fixture::commitExceptionPayload;
        RaiseException(fixture::commitException, EXCEPTION_NONCONTINUABLE, 1, &payload);
    }
}

void __fastcall fixtureOriginalBattleUpdate(void* object, float elapsed) {
    auto& t = fixture::updateTrace; ++t.nativeCalls;
    t.valid = t.valid && object == t.object && elapsed == 0.5f && updatingBattle && t.starts == 1 && t.ends == 0;
    if (t.recurse && !t.recursing) {
        t.recursing = true; updateBattle(object, elapsed); t.recursing = false;
    }
    if (t.raise) RaiseException(fixture::commitException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
void invokeBattleWithSehCapture(void* object) {
    __try { updateBattle(object, 0.5f); }
    __except (GetExceptionCode() == fixture::commitException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        fixture::updateTrace.caught = true;
    }
}

int captureCommitException(EXCEPTION_POINTERS* exception) noexcept {
    if (exception->ExceptionRecord->ExceptionCode != fixture::commitException)
        return EXCEPTION_CONTINUE_SEARCH;
    auto& trace = fixture::commitTrace;
    trace.caughtCode = exception->ExceptionRecord->ExceptionCode;
    trace.caughtFlags = exception->ExceptionRecord->ExceptionFlags;
    trace.caughtParameterCount = exception->ExceptionRecord->NumberParameters;
    if (trace.caughtParameterCount) trace.caughtPayload = exception->ExceptionRecord->ExceptionInformation[0];
    return EXCEPTION_EXECUTE_HANDLER;
}

// No C++ unwinding objects in this outer SEH boundary. Only our explicit
// native exception is caught; an access violation or other error is not hidden.
void invokeObservedWithSehCapture(void* object, std::uintptr_t caller) {
    __try {
        commitObserved(object, caller);
        fixture::commitTrace.returnedNormally = true;
    } __except(captureCommitException(GetExceptionInformation())) {
    }
}

struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t owner = 0;
    std::array<std::uintptr_t, 10> objects {};
    OwnerKind kind;
    explicit Scene(OwnerKind selected = OwnerKind::Gun) : kind(selected) {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x10000,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!allocation) throw std::runtime_error("fixture allocation failed");
        owner = reinterpret_cast<std::uintptr_t>(allocation);
        moduleBase = 0x180000000ULL;
        put(owner, 0, moduleBase + (kind == OwnerKind::Battle ? kBattleVtable : kGunVtable));
        put(owner, 0x48, kind == OwnerKind::Battle ? 3U : 2U);
        put(owner, 0xE0, std::uint32_t {7});
        const auto amount = kind == OwnerKind::Battle ? kBattleRootNames.size() : kRootNames.size();
        for (std::size_t index = 0; index < amount; ++index) {
            objects[index] = owner + 0x2000 + index * 0x400;
            put(owner, kind == OwnerKind::Battle ? kBattleOffsets[index] : kGunOffsets[index], objects[index]);
            put(objects[index], 0, moduleBase + kSpriteVtable);
            put(objects[index], 8, objects[index]);
            put(objects[index], 0x3C, std::uint32_t {7});
            strcpy_s(reinterpret_cast<char*>(objects[index] + 0x40), 32,
                kind == OwnerKind::Battle ? kBattleRootNames[index] : kRootNames[index]);
            put(objects[index], 0xC9, static_cast<unsigned char>(1));
        }
        fixture::now = 1000;
        fixture::queryDelay = 0;
        fixture::queries = 0;
        fixture::ready = true;
        fixture::meterReady = true;
        fixture::loseReadinessOnQuery = false;
        currentRoots = {}; currentBattleRoots = {};
        failed.store(false);
        replacementReady = [] { return fixture::ready; };
        timingEnabled = false; timingDepth = 0; timingWindow = {};
        privateDataObservationTiming = nullptr;
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); }
    Roots& roots() { return kind == OwnerKind::Battle ? currentBattleRoots : currentRoots; }
    std::uintptr_t sideMeter(bool right, unsigned index) {
        const auto root = owner + 0x5000;
        put(owner, 0x1338, root);
        put(root, 0, moduleBase + kSpriteVtable); put(root, 8, root);
        put(root, 0x3C, std::uint32_t {7});
        strcpy_s(reinterpret_cast<char*>(root + 0x40), 32, "lcd_bt_CCRoot");
        put(root, 0xC9, static_cast<unsigned char>(1));
        const auto leaf = owner + 0x6000 + ((right ? 64 : 0) + index) * 0x130;
        put(owner, (right ? 0x19B0 : 0x17B0) + index * 8, leaf);
        put(leaf, 0, moduleBase + kSpriteVtable); put(leaf, 8, leaf); put(leaf, 0x10, root);
        put(leaf, 0x3C, std::uint32_t {7}); put(leaf, 0xC9, static_cast<unsigned char>(1));
        char name[32] {};
        if (index) sprintf_s(name, "lcd_bt_meter%s_%u", right ? "Right" : "Left", index);
        else sprintf_s(name, "lcd_bt_meter%s", right ? "Right" : "Left");
        strcpy_s(reinterpret_cast<char*>(leaf + 0x40), 32, name);
        return leaf;
    }
    void observe() {
        const auto previous = roots();
        roots().valid = false;
        observeRoots(reinterpret_cast<void*>(owner), kind, &previous);
    }
};

int main() {
    unsigned cases = 0;
    try {
        { Scene scene(OwnerKind::Battle);
          // All64slots, independently instantiated with certified offsets.
          for (bool right : {false, true}) for (unsigned index = 0; index < 64; ++index) scene.sideMeter(right, index);
          scene.observe(); CHECK(!nativeSideMetersVisible());
          for (bool right : {false, true}) for (unsigned index = 0; index < 64; ++index) {
              const auto leaf = *reinterpret_cast<std::uintptr_t*>(scene.owner + (right ? 0x19B0 : 0x17B0) + index * 8);
              const auto token = prepare(leaf);
              CHECK(token.attempted && token.meter && token.root == scene.owner + 0x5000);
              CHECK(visibility(leaf) == 1 && visibility(token.root) == 0);
              restore(token); CHECK(visibility(token.root) == 1);
          }
          CHECK(fixture::queries == 1 && nativeSideMetersVisible() == 3); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(false, 1); scene.observe();
          // Generic CCRoot commit and unowned same-name sibling are untouched.
          CHECK(!prepare(scene.owner + 0x5000).attempted);
          put(scene.owner, 0x17B8, std::uintptr_t {0}); CHECK(!prepare(leaf).attempted);
          put(scene.owner, 0x17B8, leaf); put(leaf, 0x10, scene.objects[0]);
          CHECK(!ownedSideMeter(scene.roots(), leaf)); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(true, 63); scene.observe();
          for (const char* name : {"lcd_bt_meterRight_64", "lcd_bt_meterRight_0", "lcd_bt_meterRight_01",
                  "lcd_bt_meterRight_63extra", "lcd_bt_meterUp_63", "lcd_bt_meterBtm_63"}) {
              strcpy_s(reinterpret_cast<char*>(leaf + 0x40), 32, name);
              CHECK(!prepare(leaf).attempted);
          } ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(false, 10); scene.observe();
          fixture::meterReady = false; CHECK(!prepare(leaf).attempted && fixture::queries == 0);
          fixture::meterReady = true; fixture::now += 101;
          CHECK(!nativeSideMetersVisible() && !prepare(leaf).attempted); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(true, 5);
          put(scene.owner + 0x5000, 0xC9, static_cast<unsigned char>(0)); scene.observe();
          CHECK(!nativeSideMetersVisible() && !prepare(leaf).attempted); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(true, 5); scene.observe();
          fixture::queryDelay = 101; CHECK(!prepare(leaf).attempted);
          CHECK(visibility(scene.owner + 0x5000) == 1 && !failed.load()); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto left = scene.sideMeter(false, 1);
          const auto right = scene.sideMeter(true, 1); put(right, 0xC9, static_cast<unsigned char>(0));
          scene.observe(); fixture::meterReady = false;
          CHECK(!prepare(left).attempted && nativeSideMetersVisible() == 1);
          CHECK(!prepare(right).attempted && nativeSideMetersVisible() == 1);
          scene.observe(); CHECK(!nativeSideMetersVisible()); ++cases; }
        { Scene scene(OwnerKind::Battle); const auto leaf = scene.sideMeter(false, 1); scene.observe();
          fixture::loseReadinessOnQuery = true; CHECK(!prepare(leaf).attempted);
          CHECK(visibility(scene.owner + 0x5000) == 1); ++cases; }
        // These expected offsets/names are independent of the implementation
        // tables: loader138C62 uses table12C0E90 and stores at Gun+130..148.
        { Scene scene;
          constexpr std::array<std::size_t, 4> offsets {{0x130,0x138,0x140,0x148}};
          constexpr std::array<const char*, 4> names {{"lcd_m_line_v_0", "lcd_m_line_h_1",
              "lcd_m_line_v_1", "lcd_m_line_h_0"}};
          const auto parent = scene.owner + 0x6000;
          put(parent, 0, moduleBase + kSpriteVtable); put(parent, 8, parent);
          put(parent, 0x3C, std::uint32_t {7});
          strcpy_s(reinterpret_cast<char*>(parent + 0x40), 32, "root");
          put(parent, 0xC9, static_cast<unsigned char>(1));
          for (std::size_t i = 0; i < offsets.size(); ++i) {
              const auto object = scene.objects[i + 2];
              CHECK(*reinterpret_cast<std::uintptr_t*>(scene.owner + offsets[i]) == object);
              CHECK(!std::strcmp(reinterpret_cast<const char*>(object + 0x40), names[i]));
              put(object, 0x10, parent); // Sibling, deliberately outside both reticle roots.
          }
          scene.observe(); CHECK(scene.roots().valid);
          for (std::size_t i = 2; i < 6; ++i) {
              const auto token = prepare(scene.objects[i]);
              CHECK(token.attempted && token.root == scene.objects[i] && visibility(token.root) == 0);
              CHECK(visibility(parent) == 1 && visibility(scene.objects[0]) == 1);
              restore(token); CHECK(visibility(scene.objects[i]) == 1);
          }
          CHECK(scene.roots().writable.count == 6);
          CHECK(!prepare(parent).attempted && visibility(parent) == 1);
          for (const char* name : {"root_hole", "root_Cursor", "lcd_m_hole_0", "lcd_m_Cursor", "lcd_m_line_h_0"}) {
              const auto sibling = scene.owner + 0x6400;
              put(sibling, 0, moduleBase + kSpriteVtable); put(sibling, 8, sibling);
              put(sibling, 0x3C, std::uint32_t {7}); put(sibling, 0x10, parent);
              strcpy_s(reinterpret_cast<char*>(sibling + 0x40), 32, name);
              put(sibling, 0xC9, static_cast<unsigned char>(1));
              CHECK(!prepare(sibling).attempted && visibility(sibling) == 1);
          }
          ++cases;
        }
        { Scene scene; scene.observe();
          put(scene.owner, 0x138, scene.objects[4]);
          CHECK(!prepare(scene.objects[3]).attempted); // Changed owned leaf identity.
          CHECK(visibility(scene.objects[3]) == 1); ++cases;
        }
        { Scene scene;
          strcpy_s(reinterpret_cast<char*>(scene.objects[3] + 0x40), 32, "unrelated_line");
          scene.observe(); CHECK(!scene.roots().valid);
          CHECK(!prepare(scene.objects[3]).attempted); ++cases;
        }
        { Scene scene; scene.observe();
          CHECK(scene.roots().valid && !scene.roots().writable.valid && fixture::queries == 0);
          for (unsigned update = 0; update < 10; ++update) { fixture::now += 30; scene.observe(); }
          CHECK(scene.roots().valid && fixture::queries == 0); ++cases; }
        { Scene scene; scene.observe(); fixture::ready = false;
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 0);
          fixture::ready = true; put(scene.objects[0], 0xC9, static_cast<unsigned char>(0));
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 0); ++cases; }
        { Scene scene; scene.observe();
          put(scene.objects[0], 0x3C, std::uint32_t {8});
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 0); ++cases; }
        { Scene scene; scene.observe(); const auto token = prepare(scene.objects[0]);
          CHECK(token.attempted && fixture::queries == 1 && visibility(scene.objects[0]) == 0);
          CHECK(scene.roots().writable.observed == 1000);
          restore(token); CHECK(visibility(scene.objects[0]) == 1); ++cases; }
        { Scene scene; scene.observe(); auto token = prepare(scene.objects[0]); restore(token);
          fixture::now += 50; scene.observe(); CHECK(fixture::queries == 1);
          token = prepare(scene.objects[0]); CHECK(token.attempted && fixture::queries == 1);
          CHECK(scene.roots().observed == 1050 && scene.roots().writable.observed == 1000); restore(token);
          ++fixture::now; scene.observe(); CHECK(fixture::queries == 1);
          token = prepare(scene.objects[0]); CHECK(token.attempted && fixture::queries == 2);
          CHECK(scene.roots().writable.observed == 1051); restore(token); ++cases; }
        { Scene scene; scene.observe(); auto token = prepare(scene.objects[0]); restore(token);
          put(scene.owner, 0xE0, std::uint32_t {9});
          for (auto object : scene.objects) if (object) put(object, 0x3C, std::uint32_t {9});
          ++fixture::now; scene.observe(); CHECK(scene.roots().valid && !scene.roots().writable.valid);
          CHECK(fixture::queries == 1); token = prepare(scene.objects[0]);
          CHECK(token.attempted && fixture::queries == 2); restore(token); ++cases; }
        { Scene scene; scene.observe(); fixture::now += 90; fixture::queryDelay = 20;
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 1);
          CHECK(visibility(scene.objects[0]) == 1 && scene.roots().observed == 1000);
          CHECK(!scene.roots().writable.valid); ++cases; }
        { Scene scene; scene.observe(); fixture::now += 101;
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 0); ++cases; }
        { Scene scene; scene.observe(); fixture::loseReadinessOnQuery = true;
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 1);
          CHECK(visibility(scene.objects[0]) == 1 && !failed.load()); ++cases; }
        { Scene scene; scene.observe(); const auto token = prepare(scene.objects[0]);
          CHECK(token.attempted); fixture::now += 1000; scene.roots() = {};
          restore(token); CHECK(visibility(scene.objects[0]) == 1 && !failed.load()); ++cases; }
        { Scene scene(OwnerKind::Battle); scene.observe();
          CHECK(scene.roots().valid && fixture::queries == 0);
          const auto token = prepare(scene.objects[8]);
          CHECK(token.attempted && fixture::queries == 1 && token.kind == OwnerKind::Battle);
          CHECK(scene.roots().writable.count == 10); restore(token); ++cases; }
        { Scene scene; scene.observe(); DWORD old = 0;
          CHECK(VirtualProtect(scene.allocation, 0x10000, PAGE_READONLY, &old));
          CHECK(!prepare(scene.objects[0]).attempted && fixture::queries == 1);
          CHECK(visibility(scene.objects[0]) == 1 && !failed.load());
          CHECK(VirtualProtect(scene.allocation, 0x10000, PAGE_READWRITE, &old)); ++cases; }
        { Scene scene; scene.observe(); LARGE_INTEGER frequency {}; CHECK(QueryPerformanceFrequency(&frequency));
          timingEnabled = true; timingFrequency = frequency.QuadPart; timingDepth = 1;
          PrivateDataObservationTiming outer;
          privateDataObservationTiming = &outer;
          const auto token = prepare(scene.objects[0]);
          CHECK(token.attempted && timingWindow.permissions[0].operations.queries.calls == 1);
          CHECK(timingWindow.successfulHides[0] == 1 && timingWindow.successfulHides[1] == 0);
          CHECK(privateDataObservationTiming == &outer && outer.queries.calls == 0);
          restore(token); privateDataObservationTiming = nullptr; timingEnabled = false; ++cases; }
        // Exercise the shared production integration, not just the standalone
        // permission helper. Stub replay/background callbacks see the actual
        // reticle byte still hidden; its final visible value proves real
        // reticle restoration runs after both callbacks and before completion.
        for (const bool raise : {false, true}) {
          Scene scene; scene.observe();
          fixture::commitTrace = {};
          auto& trace = fixture::commitTrace;
          trace.enabled = true; trace.raise = raise;
          trace.object = scene.objects[0]; trace.caller = moduleBase + 0x1E634D;
          committing = false;
          originalCommit = &fixtureOriginalCommit;
          const auto suppressedBefore = counts[static_cast<std::size_t>(Count::Suppressed)].load();
          invokeObservedWithSehCapture(reinterpret_cast<void*>(trace.object), trace.caller);
          CHECK(trace.valid && trace.originalCalls == 1 && trace.backgroundBegins == 1 && trace.replayBegins == 1);
          CHECK(trace.replayRestores == 1 && trace.backgroundRestores == 1 && trace.rankingRestores == 1 && trace.achievementRestores == 1 && trace.marginRestores == 1 && trace.marginBegins == 1 && trace.normalBegins == 1 && trace.normalRestores == 1 && trace.optionsBegins == 1 && trace.optionsRestores == 1 && trace.eventCount == 8);
          CHECK(trace.events[0] == 1 && trace.events[1] == 8 && trace.events[2] == 7 && trace.events[3] == 6 && trace.events[4] == 5 && trace.events[5] == 4 && trace.events[6] == 2 && trace.events[7] == 3);
          CHECK(visibility(trace.object) == 1 && !committing && !failed.load());
          CHECK(counts[static_cast<std::size_t>(Count::Suppressed)].load() == suppressedBefore + 1);
          CHECK(trace.returnedNormally == !raise);
          if (raise) {
            CHECK(trace.caughtCode == fixture::commitException &&
                (trace.caughtFlags & EXCEPTION_NONCONTINUABLE) != 0 && trace.caughtParameterCount == 1 &&
                trace.caughtPayload == fixture::commitExceptionPayload);
          } else CHECK(trace.caughtCode == 0 && trace.caughtParameterCount == 0);
          fixture::commitTrace.enabled = false;
          originalCommit = nullptr;
          ++cases;
        }
        // The actual shared LCD-update wrapper brackets one complete native
        // invocation. Native recursion forwards without reopening a certificate;
        // native exceptions still notify failure before propagating.
        for (unsigned mode = 0; mode < 3; ++mode) {
            Scene scene(OwnerKind::Battle); scene.observe();
            fixture::updateTrace = {}; auto& t = fixture::updateTrace;
            t.enabled = true; t.raise = mode == 1; t.recurse = mode == 2;
            t.object = reinterpret_cast<void*>(scene.owner);
            updatingBattle = false; originalBattleUpdate = &fixtureOriginalBattleUpdate;
            invokeBattleWithSehCapture(t.object);
            CHECK(t.valid && t.starts == 1 && t.ends == 1 && t.completed == !t.raise);
            CHECK(t.nativeCalls == (t.recurse ? 2u : 1u) && t.caught == t.raise && !updatingBattle);
            CHECK(currentBattleRoots.valid == !t.raise);
            t.enabled = false; originalBattleUpdate = nullptr; ++cases;
        }
        std::cout << "Passed " << cases << " native reticle lazy-permission groups\n";
    } catch (const std::exception& error) {
        privateDataObservationTiming = nullptr;
        std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1;
    }
}
