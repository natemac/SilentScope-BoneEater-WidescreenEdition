// Exact production module against a private fake object heap. No game module,
// device input, live process, installed hook or graphics object is involved.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fixture {
ULONGLONG now = 1000, queryDelay = 0;
ULONGLONG delayOwnerReadAfterQuery = 0, pendingOwnerReadDelay = 0;
unsigned queries = 0, nativeCalls = 0;
bool hudReady = true, originalSawInvalidated = false;
void* expectedOwner = nullptr;
void* observedCamera = nullptr;
float observedElapsed = 0;
void* makeReadonly = nullptr;
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries; now += queryDelay;
    const auto result = VirtualQuery(address, information, length);
    pendingOwnerReadDelay = delayOwnerReadAfterQuery;
    if (makeReadonly) {
        DWORD old = 0;
        VirtualProtect(makeReadonly, 0x1000, PAGE_READONLY, &old);
        makeReadonly = nullptr;
    }
    return result;
}
BOOL read(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T length, SIZE_T* copied) noexcept {
    if (address == expectedOwner && pendingOwnerReadDelay) {
        now += pendingOwnerReadDelay; pendingOwnerReadDelay = 0;
    }
    return ReadProcessMemory(process, address, buffer, length, copied);
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#define ReadProcessMemory fixture::read
#include "../../src/render/native_replay_alignment.cpp"
#undef ReadProcessMemory
#undef VirtualQuery
#undef GetTickCount64

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h;
    h.valid = fixture::hudReady; h.mainWidth = 1920; h.mainHeight = 1080;
    h.width = 768.0f * 1080.0f / 1366.0f; h.height = 1080;
    h.left = (1920.0f - h.width) / 2; h.top = 0; h.helper = 11; h.sprite = 12;
    return h;
}
}
using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
template<class T> void put(std::uintptr_t object, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<void*>(object + offset), &value, sizeof(value));
}
void __fastcall nativeUpdate(void* owner, float elapsed, void* camera) {
    ++fixture::nativeCalls;
    fixture::originalSawInvalidated = !current.valid && owner == fixture::expectedOwner;
    fixture::observedCamera = camera; fixture::observedElapsed = elapsed;
}

struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t owner = 0, config = 0;
    std::array<std::uintptr_t, 3> gui {}, sprites {};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(allocation);
        base = reinterpret_cast<std::uintptr_t>(allocation);
        owner = base + 0x1000; config = base + 0x3000;
        put(base, 0x13DCF90, owner); put(base, 0x13DCF58, config);
        put(config, 0x1373, static_cast<unsigned char>(1));
        put(owner, 0, base + 0x10C6200); put(owner, 0x170, std::uint32_t {13});
        put(owner, 0x1B8, std::uint32_t {6});
        put(owner, 0x22C, std::array<float, 2>{{768, 300}});
        gui = {{base + 0x6000, base + 0x8000, base + 0xAF70}}; // Mask spans two pages at +70/+A8.
        for (std::size_t i = 0; i < 3; ++i) {
            sprites[i] = base + 0x10000 + i * 0x2000;
            const auto material = sprites[i] + 0x1000, texture = material + 0x100;
            put(owner, 0x178 + i * 8, gui[i]);
            put(owner, std::array<std::size_t, 3>{{0x190, 0x1A0, 0x198}}[i], gui[i]);
            put(owner, std::array<std::size_t, 3>{{0x150, 0x158, 0x168}}[i], sprites[i]);
            put(gui[i], 0, base + 0x10CEBA8); put(gui[i], 8, gui[i]);
            put(gui[i], 0x3C, std::uint32_t {13});
            strcpy_s(reinterpret_cast<char*>(gui[i] + 0x40), 32, kNames[i]);
            put(gui[i], 0x110, sprites[i]);
            put(gui[i], 0xC9, static_cast<unsigned char>(i == 1 ? 0 : 1));
            put(gui[i], 0x98, i == 2 ? std::array<float, 2>{{800, 299}} : std::array<float, 2>{{768, 300}});
            put(gui[i], 0x90, std::array<float, 2>{{0.5f, 0.5f}});
            put(gui[i], 0x70, i == 2 ? std::array<float, 2>{{400, 149.5f}} : std::array<float, 2>{{384, 150}});
            put(gui[i], 0xA0, std::array<float, 2>{{1, 1}}); put(gui[i], 0xA8, std::array<float, 2>{{1, 1}});
            put(sprites[i], 0, base + 0x10CC0D8);
            put(sprites[i], 0x570, std::uint32_t {i == 2 ? 0u : 2u});
            put(sprites[i], 0x1A8, base + (i == 2 ? 0x1A000 : 0x1B000));
            put(sprites[i], 0x308, material); put(material, 0, base + 0x706570);
            put(material, 0x50, texture);
            put(texture, 0, i == 0 ? 0x7274406300001180ULL : 0x443330380005a100ULL);
            put(texture, 0x18, i == 0 ? std::array<std::uint16_t, 2>{{768, 300}} : std::array<std::uint16_t, 2>{{8, 512}});
        }
        current = {}; updating = committing = false;
        ownerThread = 0; enabled = true; failed = false;
        applied = restored = rejected = 0; nextReport = 0;
        fixture::now = 1000; fixture::queryDelay = 0; fixture::queries = fixture::nativeCalls = 0;
        fixture::delayOwnerReadAfterQuery = fixture::pendingOwnerReadDelay = 0;
        fixture::hudReady = true; fixture::makeReadonly = nullptr;
        fixture::expectedOwner = reinterpret_cast<void*>(owner);
        originalUpdate = nativeUpdate;
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); }
    void observe() { update(reinterpret_cast<void*>(owner), 0.125f, reinterpret_cast<void*>(0x1234)); }
    ReplayAlignment begin() { return beginNativeReplayAlignmentCommit(reinterpret_cast<void*>(gui[2]), base + 0x1E634D); }
    std::array<unsigned char, 0x130> bytes(unsigned index) {
        std::array<unsigned char, 0x130> result {};
        std::memcpy(result.data(), reinterpret_cast<void*>(gui[index]), result.size()); return result;
    }
};

int main() {
    unsigned cases = 0;
    try {
        { Scene s; s.observe(); CHECK(current.valid && fixture::queries == 0 && fixture::nativeCalls == 1);
          CHECK(fixture::originalSawInvalidated && fixture::observedElapsed == 0.125f && fixture::observedCamera == reinterpret_cast<void*>(0x1234));
          const auto before = s.bytes(2), image = s.bytes(0), effect = s.bytes(1);
          auto token = s.begin(); CHECK(token.attempted && applied == 1 && fixture::queries == 1);
          auto transformed = s.bytes(2); CHECK(transformed != before);
          for (std::size_t i = 0; i < before.size(); ++i)
              if (!((i >= 0x70 && i < 0x78) || (i >= 0xA8 && i < 0xB0))) CHECK(before[i] == transformed[i]);
          CHECK(s.bytes(0) == image && s.bytes(1) == effect);
          restoreNativeReplayAlignmentCommit(token); CHECK(s.bytes(2) == before && restored == 1 && !failed && !committing); ++cases; }
        { Scene s; s.observe();
          CHECK(!beginNativeReplayAlignmentCommit(reinterpret_cast<void*>(s.gui[0]), base + 0x1E634D).attempted);
          CHECK(!beginNativeReplayAlignmentCommit(reinterpret_cast<void*>(s.gui[2]), base + 1).attempted);
          fixture::hudReady = false; CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; put(s.owner, 0x224, static_cast<unsigned char>(1)); put(s.owner, 0x230, 1366.0f);
          s.observe(); CHECK(!current.valid && !s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.owner, 0x198, s.gui[0]); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; put(s.sprites[2], 0x570, std::uint32_t {2}); s.observe(); CHECK(!current.valid && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); put(s.gui[2], 0xC9, static_cast<unsigned char>(0)); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); auto t = s.begin(); CHECK(t.attempted); restoreNativeReplayAlignmentCommit(t);
          fixture::now += 50; s.observe(); t = s.begin(); CHECK(t.attempted && fixture::queries == 1 && current.access.observed == 1000);
          restoreNativeReplayAlignmentCommit(t);
          fixture::now += 1; s.observe(); t = s.begin(); CHECK(t.attempted && fixture::queries == 2 && current.access.observed == 1051);
          fixture::now += 500; current = {}; restoreNativeReplayAlignmentCommit(t); CHECK(restored == 3 && !failed); ++cases; }
        { Scene s; s.observe(); fixture::now += 90; fixture::queryDelay = 20;
          CHECK(!s.begin().attempted && applied == 0 && fixture::queries == 1); ++cases; }
        { Scene s; s.observe(); fixture::delayOwnerReadAfterQuery = 110;
          CHECK(!s.begin().attempted && applied == 0 && fixture::queries == 1 && fixture::now == 1110); ++cases; }
        { Scene s; s.observe(); const auto before = s.bytes(2);
          fixture::makeReadonly = reinterpret_cast<void*>(base + 0xB000);
          const auto t = s.begin(); CHECK(!t.attempted && failed && !committing);
          CHECK(s.bytes(2) == before); // First XY span restored despite the readonly scale span.
          DWORD old = 0; VirtualProtect(reinterpret_cast<void*>(base + 0xB000), 0x1000, PAGE_READWRITE, &old); ++cases; }
        { Scene s; s.observe(); auto t = s.begin(); CHECK(t.attempted);
          put(s.owner, 0x198, s.gui[0]); const auto after = s.bytes(2);
          restoreNativeReplayAlignmentCommit(t); CHECK(failed && s.bytes(2) == after && !committing); ++cases; }
        std::cout << cases << " replay alignment groups passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
