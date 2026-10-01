// Production module, private inert object heap, real RPM/VQ/checked writes.
// No game, graphics device, native hook installation or live process control.
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fixture {
ULONGLONG now = 1000, queryDelay = 0;
unsigned queries = 0, nativeCalls = 0;
bool hudReady = true, invalidateHudOnQuery = false, originalSawInvalid = false;
void* readonlyOnQuery = nullptr;
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries; now += queryDelay;
    const auto result = VirtualQuery(address, information, length);
    if (invalidateHudOnQuery) hudReady = false;
    if (readonlyOnQuery) {
        DWORD old = 0; VirtualProtect(readonlyOnQuery, 0x1000, PAGE_READONLY, &old);
        readonlyOnQuery = nullptr;
    }
    return result;
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#include "../../src/render/native_menu_backing.cpp"
#undef VirtualQuery
#undef GetTickCount64

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h; h.valid = fixture::hudReady; h.helper = 5; h.sprite = 6;
    h.mainWidth = 1920; h.mainHeight = 1080; h.width = 768.0f * 1080 / 1366;
    h.height = 1080; h.left = (1920 - h.width) / 2; h.top = 0;
    return h;
}
}
using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
template<class T> void put(std::uintptr_t object, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<void*>(object + offset), &value, sizeof(value));
}
void __fastcall nativeUpdate(void*) { ++fixture::nativeCalls; fixture::originalSawInvalid = !current.valid; }
struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t manager, title, rank, resident, config, frame, layout, root, leaf, sprite, material, texture, record;
    RearIlluminationPose original {{{564.68518f, -45.06589f}}, {{0.7906295657f, 0.7906295657f}}};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(allocation); base = reinterpret_cast<std::uintptr_t>(allocation);
        manager = base + 0x1000; title = base + 0x2000; rank = base + 0x3000; resident = base + 0x4000;
        config = base + 0x5000; frame = base + 0x8000; layout = base + 0x10000; root = base + 0x20000;
        leaf = base + 0x2FF70; sprite = base + 0x40000; material = base + 0x41000;
        texture = base + 0x42000; record = base + 0x43000;
        put(base, 0x13DD000, manager); put(manager, 0, base + 0x10C6B38);
        put(manager, 0x38, std::uint32_t {1}); put(manager, 0x3C, std::int32_t {-1}); put(manager, 0x40, title);
        put(title, 0, base + 0x10C7278); put(title, 0x10, std::uint32_t {1});
        put(title, 0x14, std::uint32_t {12}); put(title, 0x38, rank); put(rank, 0, base + 0x10C6FF0);
        put(rank, 8, std::uint32_t {1});
        put(base, 0x13DCF58, config); put(config, 0, base + 0x10C5FA8); put(config, 0x1373, static_cast<unsigned char>(1));
        put(base, 0x12E24E8, frame); put(frame, 0, base + 0x703E78);
        put(frame, 0x28, std::array<std::uint16_t, 2>{{1920, 1080}});
        put(base, 0x13DCF48, resident); put(resident, 0x128, layout); put(layout, 0, base + 0x10C9FA8);
        put(layout, 0x18, std::uint32_t {2}); put(layout, 0x1C, std::uint32_t {1});
        put(layout, 0x64, std::uint32_t {0xBAA}); put(layout, 0x3C98, std::uint32_t {2});
        strcpy_s(reinterpret_cast<char*>(layout + 0x368), 32, "Root");
        put(layout, 0x3F8, std::uint32_t {4}); put(layout, 0x428, root);
        for (const auto object : {root, leaf}) {
            put(object, 0, base + (object == root ? 0x10CEB48 : 0x10CEBA8)); put(object, 8, object);
            put(object, 0x3C, std::uint32_t {4}); put(object, 0xC9, static_cast<unsigned char>(1));
            put(object, 0xB8, std::array<float, 2>{{0.35f, 0.27f}}); put(object, 0xC0, std::uint32_t {0x2468AC});
            strcpy_s(reinterpret_cast<char*>(object + 0x40), 32, object == root ? "Root" : "white");
        }
        put(root, 0x18, leaf); put(root, 0x98, std::array<float, 2>{{20, 20}});
        put(leaf, 0x10, root); put(leaf, 0x30, record); put(leaf, 0x110, sprite);
        put(leaf, 0x60, std::array<float, 2>{{-100, -100}}); put(leaf, 0x98, std::array<float, 2>{{1000, 1480}});
        put(leaf, 0xA0, std::array<float, 2>{{1, 1}}); put(leaf, 0x70, original.position); put(leaf, 0xA8, original.scale);
        put(sprite, 0, base + 0x10CC0D8); put(sprite, 0x1A8, base + 0x45000); put(sprite, 0x308, material);
        put(material, 0, base + 0x706570); put(material, 0x50, texture);
        put(texture, 0, std::uint64_t {0x5553455200052B00ULL}); put(texture, 0x18, std::array<std::uint16_t, 2>{{64, 64}});
        put(record, 0, base + 0x10CEC08); put(record, 0x40, sprite);
        current = {}; updating = committing = false; enabled = true; failed = false; ownerThread = 0;
        applied = restored = rejected = 0; for (auto& n : maskApplied) n = 0; for (auto& n : maskRestored) n = 0; nextReport = ~ULONGLONG {0}; originalSceneUpdate = &nativeUpdate;
        fixture::now = 1000; fixture::queryDelay = fixture::queries = fixture::nativeCalls = 0;
        fixture::hudReady = true; fixture::invalidateHudOnQuery = fixture::originalSawInvalid = false;
        fixture::readonlyOnQuery = nullptr;
    }
    std::uintptr_t maskRoot() const { return base + 0x50000; }
    std::uintptr_t mask(unsigned index) const { return base + (index == 0 ? 0x5FF70 : 0x62000); }
    RearIlluminationPose maskOriginal(unsigned index) const {
        return {{{643.74817f, index == 0 ? -1.5812607f : 1030.98096f}}, {{0.7906295657f, 0.7906295657f}}};
    }
    void addMasks(bool reverse = false) {
        put(layout, 0x68, std::uint32_t {0xBF6}); put(layout, 0x3FC, std::uint32_t {6}); put(layout, 0x430, maskRoot());
        const auto mr = maskRoot();
        put(mr, 0, base + 0x10CEB48); put(mr, 8, mr); put(mr, 0x3C, std::uint32_t {6});
        strcpy_s(reinterpret_cast<char*>(mr + 0x40), 32, "Root"); put(mr, 0x98, std::array<float, 2>{{5, 5}});
        put(mr, 0xC9, static_cast<unsigned char>(1)); put(mr, 0x18, mask(reverse ? 1 : 0));
        // Both native records share one maskMaterial/source, as observed live.
        const auto source = base + 0x70000, maskMaterial = base + 0x71000, maskTexture = base + 0x72000;
        put(source, 0, base + 0x10CC0D8); put(source, 0x1A8, base + 0x45000); put(source, 0x308, maskMaterial);
        put(maskMaterial, 0, base + 0x706570); put(maskMaterial, 0x50, maskTexture);
        put(maskTexture, 0, std::uint64_t {0x5553455200055180ULL}); put(maskTexture, 0x18, std::array<std::uint16_t, 2>{{64, 64}});
        for (unsigned i = 0; i < 2; ++i) {
            const auto object = mask(i), maskRecord = base + 0x73000 + 0x1000 * i;
            put(object, 0, base + 0x10CEBA8); put(object, 8, object); put(object, 0x10, mr);
            put(object, 0x20, i == (reverse ? 1u : 0u) ? std::uintptr_t {0} : mask(1 - i));
            put(object, 0x28, i == (reverse ? 1u : 0u) ? mask(1 - i) : std::uintptr_t {0});
            put(object, 0x3C, std::uint32_t {6}); strcpy_s(reinterpret_cast<char*>(object + 0x40), 32, i ? "Btm" : "Top");
            put(object, 0xC9, static_cast<unsigned char>(1)); put(object, 0x30, maskRecord); put(object, 0x110, source);
            put(object, 0x60, std::array<float, 2>{{0, i ? 1261.0f : -45.0f}});
            put(object, 0x98, std::array<float, 2>{{850, 64}}); put(object, 0xA0, std::array<float, 2>{{1, 1}});
            const auto pose = maskOriginal(i); put(object, 0x70, pose.position); put(object, 0xA8, pose.scale);
            put(object, 0xB8, std::array<float, 2>{{0.45f, 0.25f}}); put(object, 0xC0, std::uint32_t {0xA0B0C0});
            put(maskRecord, 0, base + 0x10CEC08); put(maskRecord, 0x40, source);
            put(maskRecord, 0x58, std::array<float, 8>{{0, i ? 0.0f : 1.0f, 1, i ? 0.0f : 1.0f, 0, i ? 1.0f : 0.0f, 1, i ? 1.0f : 0.0f}});
        }
    }
    RankingBackingAlignment beginMask(unsigned index) { return beginNativeRankingBackingCommit(reinterpret_cast<void*>(mask(index)), base + 0x1E634D); }
    RearIlluminationPose maskPose(unsigned index) const {
        RearIlluminationPose result;
        std::memcpy(result.position.data(), reinterpret_cast<void*>(mask(index) + 0x70), 8);
        std::memcpy(result.scale.data(), reinterpret_cast<void*>(mask(index) + 0xA8), 8); return result;
    }
    bool maskRestoredExactly(unsigned index) const {
        auto p = maskPose(index), o = maskOriginal(index); return p.position == o.position && p.scale == o.scale;
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); current = {}; base = 0; }
    void observe() { update(reinterpret_cast<void*>(manager)); }
    RankingBackingAlignment begin() { return beginNativeRankingBackingCommit(reinterpret_cast<void*>(leaf), base + 0x1E634D); }
    RearIlluminationPose pose() const {
        RearIlluminationPose p; std::memcpy(p.position.data(), reinterpret_cast<void*>(leaf + 0x70), 8);
        std::memcpy(p.scale.data(), reinterpret_cast<void*>(leaf + 0xA8), 8); return p;
    }
    bool isOriginal() const { const auto p = pose(); return p.position == original.position && p.scale == original.scale; }
};

int main() {
    unsigned cases = 0;
    try {
        { Scene s; s.observe(); CHECK(current.valid && fixture::nativeCalls == 1 && fixture::originalSawInvalid && fixture::queries == 0);
          auto token = s.begin(); CHECK(token.attempted && !s.isOriginal() && fixture::queries == 1);
          const auto p = s.pose(); const auto h = readNativeHudGeometry();
          CHECK(std::fabs(p.position[0] - .1 - h.left) < .001 && std::fabs(p.scale[0] * 1000 - h.width) < .001);
          CHECK(std::fabs(p.position[1] - .1) < .001 && std::fabs(p.scale[1] * 1480 - 1080) < .001);
          restoreNativeRankingBackingCommit(token); CHECK(s.isOriginal() && !committing && restored == 1); ++cases; }
        { Scene s; s.observe(); std::array<unsigned char, 0x130> before {}; std::memcpy(before.data(), reinterpret_cast<void*>(s.leaf), before.size());
          auto token = s.begin(); CHECK(token.attempted); std::array<unsigned char, 0x130> during {};
          std::memcpy(during.data(), reinterpret_cast<void*>(s.leaf), during.size());
          for (std::size_t i = 0; i < before.size(); ++i) if (!((i >= 0x70 && i < 0x78) || (i >= 0xA8 && i < 0xB0))) CHECK(before[i] == during[i]);
          restoreNativeRankingBackingCommit(token); CHECK(std::memcmp(before.data(), reinterpret_cast<void*>(s.leaf), before.size()) == 0); ++cases; }
        { Scene s; for (const std::uint32_t state : {0u, 1u, 3u, 5u, 13u}) { put(s.title, 0x14, state); s.observe(); CHECK(!current.valid && !s.begin().attempted); }
          put(s.title, 0x14, std::uint32_t {12}); put(s.manager, 0x3C, std::int32_t {2}); s.observe(); CHECK(!current.valid);
          CHECK(fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.rank, 0, base + 0x10C7030); CHECK(!s.begin().attempted && s.isOriginal());
          put(s.rank, 0, base + 0x10C6FF0); put(s.layout, 0x64, std::uint32_t {0xB8C}); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.sprite, 0x570, std::uint32_t {2}); CHECK(!s.begin().attempted);
          put(s.sprite, 0x570, std::uint32_t {0}); put(s.record, 0x40, s.sprite + 8); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.leaf, 0x10, s.root + 8); CHECK(!s.begin().attempted);
          put(s.leaf, 0x10, s.root); put(s.leaf, 0xB0, 1.0f); CHECK(!s.begin().attempted);
          put(s.leaf, 0xB0, 0.0f); put(s.leaf, 0xC9, static_cast<unsigned char>(0)); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); restoreNativeRankingBackingCommit(token);
          fixture::now += 50; s.observe(); token = s.begin(); CHECK(token.attempted && fixture::queries == 1 && current.access.observed == 1000);
          restoreNativeRankingBackingCommit(token); ++fixture::now; s.observe(); token = s.begin();
          CHECK(token.attempted && fixture::queries == 2 && current.access.observed == 1051); restoreNativeRankingBackingCommit(token); ++cases; }
        { Scene s; s.observe(); fixture::now += 90; fixture::queryDelay = 20;
          CHECK(!s.begin().attempted && fixture::queries == 1 && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::invalidateHudOnQuery = true; CHECK(!s.begin().attempted && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); CHECK(token.attempted);
          fixture::now += 1000; current = {}; put(s.title, 0x14, std::uint32_t {13}); put(s.title, 0x38, std::uintptr_t {0});
          restoreNativeRankingBackingCommit(token); CHECK(s.isOriginal() && !failed && !committing); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); CHECK(token.attempted);
          put(s.config, 0x1373, static_cast<unsigned char>(0));
          put(s.frame, 0x28, std::array<std::uint16_t, 2>{{800, 1366}}); put(s.layout, 0x18, std::uint32_t {3});
          restoreNativeRankingBackingCommit(token); CHECK(s.isOriginal() && !failed && !committing); ++cases; }
        { Scene s; s.observe(); DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READONLY, &old));
          CHECK(!s.begin().attempted && s.isOriginal());
          CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; s.observe(); fixture::readonlyOnQuery = reinterpret_cast<void*>(base + 0x30000);
          CHECK(!s.begin().attempted && failed && !committing && s.isOriginal());
          DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; s.observe(); RearIlluminationPose after {{{9, 9}}, {{9, 9}}}; const auto h = readNativeHudGeometry();
          auto bad = s.original; bad.scale[0] = 0.1f; CHECK(!clipRankingBacking(h.left, h.top, h.width, h.height, bad, after));
          CHECK(after.position[0] == 9 && after.scale[0] == 9); bad = s.original; bad.position[1] = NAN;
          CHECK(!clipRankingBacking(h.left, h.top, h.width, h.height, bad, after));
          CHECK(!clipRankingBacking(h.left + 1, h.top, h.width, h.height, s.original, after)); ++cases; }
        { Scene s; s.observe(); fixture::now += 101; CHECK(!s.begin().attempted && fixture::queries == 0);
          s.observe(); ownerThread = GetCurrentThreadId() + 1; CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.addMasks(); s.observe(); CHECK(current.masksValid && fixture::queries == 0);
          for (unsigned i = 0; i < 2; ++i) {
            const auto before = s.maskPose(i); auto token = s.beginMask(i); CHECK(token.attempted && token.identity.kind == i + 1);
            const auto during = s.maskPose(i), h = s.maskOriginal(i); const auto hud = readNativeHudGeometry();
            CHECK(std::fabs(during.position[0] - .1f - hud.left) < .001 && std::fabs(during.scale[0] * 850 - hud.width) < .001);
            CHECK(during.position[1] == h.position[1] && during.scale[1] == h.scale[1]);
            restoreNativeRankingBackingCommit(token); CHECK(s.maskRestoredExactly(i) && maskApplied[i] == 1 && maskRestored[i] == 1);
          } CHECK(applied == 2 && restored == 2); ++cases; }
        { Scene s; s.addMasks(true); s.observe(); CHECK(current.masksValid);
          auto token = s.beginMask(0); CHECK(token.attempted && token.identity.leaf == s.mask(0));
          restoreNativeRankingBackingCommit(token); CHECK(s.maskRestoredExactly(0)); ++cases; }
        { Scene s; s.addMasks(); s.observe(); std::array<unsigned char, 0x130> before {}, during {};
          std::array<unsigned char, 0x78> record {}; const auto recordPointer = base + 0x73000;
          std::memcpy(before.data(), reinterpret_cast<void*>(s.mask(0)), before.size());
          std::memcpy(record.data(), reinterpret_cast<void*>(recordPointer), record.size());
          auto token = s.beginMask(0); CHECK(token.attempted); std::memcpy(during.data(), reinterpret_cast<void*>(s.mask(0)), during.size());
          for (std::size_t i = 0; i < before.size(); ++i) if (!((i >= 0x70 && i < 0x74) || (i >= 0xA8 && i < 0xAC))) CHECK(before[i] == during[i]);
          CHECK(std::memcmp(record.data(), reinterpret_cast<void*>(recordPointer), record.size()) == 0);
          restoreNativeRankingBackingCommit(token); CHECK(std::memcmp(before.data(), reinterpret_cast<void*>(s.mask(0)), before.size()) == 0); ++cases; }
        { Scene s; s.addMasks(); put(s.layout, 0x68, std::uint32_t {0xBAA}); s.observe();
          CHECK(current.valid && !current.masksValid && !s.beginMask(0).attempted && fixture::queries == 0);
          auto white = s.begin(); CHECK(white.attempted); restoreNativeRankingBackingCommit(white); ++cases; }
        { Scene s; s.addMasks(); s.observe(); put(s.mask(1), 0x28, s.mask(0)); CHECK(!s.beginMask(0).attempted);
          put(s.mask(1), 0x28, std::uintptr_t {0}); put(s.mask(1), 0x20, std::uintptr_t {0}); CHECK(!s.beginMask(0).attempted);
          CHECK(fixture::queries == 0 && s.maskRestoredExactly(0)); ++cases; }
        { Scene s; s.addMasks(); s.observe(); put(s.mask(0), 0x9C, -64.0f); CHECK(!s.beginMask(0).attempted);
          put(s.mask(0), 0x9C, 64.0f); put(base + 0x72000, 0, std::uint64_t {0x5553455200052B00ULL}); CHECK(!s.beginMask(0).attempted);
          CHECK(fixture::queries == 0); ++cases; }
        { Scene s; s.addMasks(); s.observe(); put(s.mask(0), 0x64, -46.0f); CHECK(!s.beginMask(0).attempted);
          put(s.mask(0), 0x64, -45.0f); put(s.maskRoot(), 0xB0, 1.0f); CHECK(!s.beginMask(0).attempted);
          put(s.maskRoot(), 0xB0, 0.0f); put(s.mask(0), 0xC9, static_cast<unsigned char>(0)); CHECK(!s.beginMask(0).attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.addMasks(); s.observe(); auto token = s.beginMask(1); CHECK(token.attempted); restoreNativeRankingBackingCommit(token);
          fixture::now += 50; s.observe(); token = s.beginMask(1); CHECK(token.attempted && fixture::queries == 1 && current.maskAccess[1].observed == 1000);
          restoreNativeRankingBackingCommit(token); ++fixture::now; s.observe(); token = s.beginMask(1); CHECK(token.attempted && fixture::queries == 2);
          restoreNativeRankingBackingCommit(token); ++cases; }
        { Scene s; s.addMasks(); s.observe(); fixture::now += 90; fixture::queryDelay = 20;
          CHECK(!s.beginMask(0).attempted && s.maskRestoredExactly(0)); ++cases; }
        { Scene s; s.addMasks(); s.observe(); auto token = s.beginMask(0); CHECK(token.attempted);
          fixture::now += 1000; current = {}; put(s.title, 0x14, std::uint32_t {13}); put(s.config, 0x1373, static_cast<unsigned char>(0));
          put(s.frame, 0x28, std::array<std::uint16_t, 2>{{800, 1366}}); put(s.layout, 0x18, std::uint32_t {3});
          // Only X was borrowed. An unrelated native Y update must survive.
          put(s.mask(0), 0x74, 19.0f); put(s.mask(0), 0xAC, .9f);
          restoreNativeRankingBackingCommit(token); const auto p = s.maskPose(0), o = s.maskOriginal(0);
          CHECK(!failed && !committing && p.position[0] == o.position[0] && p.scale[0] == o.scale[0] && p.position[1] == 19 && p.scale[1] == .9f); ++cases; }
        { Scene s; s.addMasks(); s.observe(); fixture::readonlyOnQuery = reinterpret_cast<void*>(base + 0x60000);
          CHECK(!s.beginMask(0).attempted && failed && !committing && s.maskRestoredExactly(0));
          DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x60000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; RearIlluminationPose result {{{9, 9}}, {{9, 9}}}; const auto hud = readNativeHudGeometry(); auto original = s.maskOriginal(0);
          CHECK(fitRankingMaskHorizontal(hud.left, hud.top, hud.width, hud.height, original, result));
          CHECK(result.position[1] == original.position[1] && result.scale[1] == original.scale[1]);
          original.scale[0] = .1f; CHECK(!fitRankingMaskHorizontal(hud.left, hud.top, hud.width, hud.height, original, result));
          original = s.maskOriginal(1); original.position[1] = NAN; CHECK(!fitRankingMaskHorizontal(hud.left, hud.top, hud.width, hud.height, original, result));
          original = s.maskOriginal(1); CHECK(!fitRankingMaskHorizontal(hud.left, hud.top, hud.width + 1, hud.height, original, result)); ++cases; }
        std::cout << "Passed " << cases << " ranking backing groups\n";
    } catch (const std::exception& error) { std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1; }
}
