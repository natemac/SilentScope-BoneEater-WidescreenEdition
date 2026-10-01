// Exact production module on a private fake object heap. No native module,
// input, graphics device, hook installation or game process is involved.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fixture {
ULONGLONG now = 1000, queryDelay = 0, readDelay = 0;
unsigned queries = 0, nativeCalls = 0;
bool hudReady = true, loseHud = false;
void* readonlyPage = nullptr;
std::uintptr_t expectedManager = 0;
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries; now += queryDelay;
    const auto result = VirtualQuery(address, information, length);
    if (loseHud) hudReady = false;
    if (readonlyPage) { DWORD old = 0; VirtualProtect(readonlyPage, 4096, PAGE_READONLY, &old); readonlyPage = nullptr; }
    return result;
}
BOOL read(HANDLE process, LPCVOID address, LPVOID buffer, SIZE_T length, SIZE_T* copied) noexcept {
    if (reinterpret_cast<std::uintptr_t>(address) == expectedManager && queries && readDelay) {
        now += readDelay; readDelay = 0;
    }
    return ReadProcessMemory(process, address, buffer, length, copied);
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#define ReadProcessMemory fixture::read
#include "../../src/render/native_achievement_alignment.cpp"
#undef ReadProcessMemory
#undef VirtualQuery
#undef GetTickCount64

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h; h.valid = fixture::hudReady;
    h.mainWidth = 1920; h.mainHeight = 1080;
    h.width = 768.0f * 1080 / 1366; h.height = 1080; h.left = (1920 - h.width) / 2;
    h.helper = 11; h.sprite = 12; return h;
}
}
using namespace bone_eater::render;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
template<class T> void put(std::uintptr_t object, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<void*>(object + offset), &value, sizeof(value));
}

struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t manager = 0, main = 0, lcd = 0, config = 0;
    std::array<std::uintptr_t, 7> gui {};
    std::array<std::uintptr_t, 2> sprites {}, records {}, textures {};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(allocation); base = reinterpret_cast<std::uintptr_t>(allocation);
        manager = base + 0x1000; main = base + 0x2000; lcd = base + 0x3000; config = base + 0x4000;
        put(base, 0x13DCF78, manager); put(base, 0x13DCF58, config); put(config, 0x1373, static_cast<unsigned char>(1));
        put(manager, 0, base + 0x10CA298); put(main, 0, base + 0x10CA200); put(lcd, 0, base + 0x10CA1D8);
        put(manager, 0xD8, main); put(manager, 0xD0, lcd);
        for (const auto object : {manager, main, lcd}) put(object, 0x48, std::uint32_t {3});
        put(main, 0x98, std::uint32_t {9}); put(lcd, 0xE0, std::uint32_t {10});
        // +70 spans a page boundary. A forced partial application must restore
        // its accessible prefix and independently attempt the scale span.
        gui = {{base + 0x6F8C, base + 0x8000, base + 0x9000, base + 0xA000,
            base + 0xB000, base + 0xC000, base + 0xD000}};
        put(main, 0xF0, gui[0]); put(main, 0xA0, gui[2]); put(lcd, 0x478, gui[3]); put(lcd, 0x4C0, gui[6]);
        put(lcd, 0x4A4, static_cast<unsigned char>(1)); put(lcd, 0x4A0, 20.0f);
        for (std::size_t i = 0; i < gui.size(); ++i) {
            const bool root = i == 2 || i == 5;
            put(gui[i], 0, base + (root ? 0x10CEB48 : 0x10CEBA8)); put(gui[i], 8, gui[i]);
            put(gui[i], 0x3C, std::uint32_t {i < 3 ? 9u : 10u});
            strcpy_s(reinterpret_cast<char*>(gui[i] + 0x40), 32, names[i]);
            put(gui[i], 0x10, root ? std::uintptr_t(0) : gui[i == 6 ? 5 : i + 1]);
            put(gui[i], 0xC9, static_cast<unsigned char>(1));
            put(gui[i], 0xA0, std::array<float, 2>{{1, 1}}); put(gui[i], 0xA8, std::array<float, 2>{{1, 1}});
            put(gui[i], 0xB8, 1.0f); put(gui[i], 0xBC, 1.0f); put(gui[i], 0xC0, std::uint32_t {0xFFFFFF});
        }
        put(gui[0], 0x98, std::array<float, 2>{{800, 82}}); put(gui[3], 0x98, std::array<float, 2>{{768, 85}});
        put(gui[3], 0x60, 80.0f); put(gui[4], 0x70, -80.0f); put(gui[5], 0x60, -80.0f); put(gui[5], 0x70, -80.0f);
        for (std::size_t i = 0; i < 2; ++i) {
            sprites[i] = base + 0x10000 + i * 0x4000; records[i] = sprites[i] + 0x2000;
            const auto material = sprites[i] + 0x1000, camera = sprites[i] + 0x3000;
            textures[i] = material + 0x100;
            put(gui[i * 3], 0x110, sprites[i]); put(gui[i * 3], 0x30, records[i]);
            put(sprites[i], 0, base + 0x10CC0D8); put(sprites[i], 0x570, std::uint32_t(i * 2));
            put(sprites[i], 0x1A8, camera); put(base, 0x13DB5A0 + i * 16, camera); put(camera, 0, base + 0x6FDF48);
            put(sprites[i], 0x308, material); put(material, 0, base + 0x706570); put(material, 0x50, textures[i]);
            put(textures[i], 0, i ? 0x555345520004F880ULL : 0x555345520004F780ULL);
            put(textures[i], 0x18, i ? std::array<std::uint16_t, 2>{{1024, 512}} : std::array<std::uint16_t, 2>{{512, 1024}});
            put(records[i], 0, base + 0x10CEC08); put(records[i], 0x40, sprites[i]);
            put(records[i], 0x58, std::array<float, 4>{{.2f, .3f, .4f, .5f}});
        }
        current = {}; previousUpdate = {}; updating = committing = false;
        enabled = true; failed = false; ownerThread = 0; applied = restored = rejected = nextReport = 0;
        fixture::now = 1000; fixture::queries = fixture::nativeCalls = 0; fixture::queryDelay = fixture::readDelay = 0;
        fixture::hudReady = true; fixture::loseHud = false; fixture::readonlyPage = nullptr; fixture::expectedManager = manager;
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); }
    void observe() { CHECK(beginNativeAchievementUpdate()); endNativeAchievementUpdate(reinterpret_cast<void*>(lcd), true); }
    AchievementAlignment begin() { return beginNativeAchievementAlignmentCommit(reinterpret_cast<void*>(gui[0]), base + 0x1E634D); }
    void y(float value) {
        put(gui[0], 0x64, value); put(gui[0], 0x74, value); put(gui[3], 0x64, value); put(gui[3], 0x74, value);
    }
    GuiBytes bytes(unsigned i) { GuiBytes result {}; std::memcpy(result.data(), reinterpret_cast<void*>(gui[i]), result.size()); return result; }
};

constexpr DWORD kFixtureException = 0xE0421234;
void inertCommit(bool raises) {
    ++fixture::nativeCalls;
    if (raises) RaiseException(kFixtureException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
void scopedCommit(const AchievementAlignment* token, bool raises) {
    __try { inertCommit(raises); }
    __finally { restoreNativeAchievementAlignmentCommit(*token); }
}
bool exerciseException(const AchievementAlignment* token) {
    __try { scopedCommit(token, true); }
    __except (GetExceptionCode() == kFixtureException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}

int main() {
    unsigned cases = 0;
    try {
        { Scene s; s.observe(); CHECK(current.valid && fixture::queries == 0);
          const auto before = s.bytes(0), title = s.bytes(3); auto token = s.begin(); CHECK(token.attempted && applied == 1);
          const auto changed = s.bytes(0);
          for (std::size_t i = 0; i < before.size(); ++i)
              if (!((i >= 0x70 && i < 0x78) || (i >= 0xA8 && i < 0xB0))) CHECK(changed[i] == before[i]);
          CHECK(s.bytes(3) == title); CHECK(field<float>(changed, 0x70) > 656.7f);
          CHECK(field<float>(changed, 0xA8) * 800 > 631.6f); // No silent 800-to-768 crop.
          scopedCommit(&token, false); CHECK(fixture::nativeCalls == 1 && s.bytes(0) == before && restored == 1 && !committing); ++cases; }
        { Scene s; s.y(-42.5f); s.observe(); const auto before = s.bytes(0); auto token = s.begin(); CHECK(token.attempted);
          CHECK(field<float>(s.bytes(0), 0x74) < 0); restoreNativeAchievementAlignmentCommit(token); CHECK(s.bytes(0) == before); ++cases; }
        { Scene s; s.y(-85); s.observe(); CHECK(!s.begin().attempted && fixture::queries == 0);
          s.y(-82); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.gui[0], 0x74, -85.0f); CHECK(!s.begin().attempted);
          s.y(0); put(s.gui[3], 0x64, -1.0f); CHECK(!s.begin().attempted); ++cases; }
        { Scene s; s.observe(); put(s.lcd, 0x4A4, static_cast<unsigned char>(0)); CHECK(!s.begin().attempted);
          put(s.lcd, 0x4A4, static_cast<unsigned char>(1)); put(s.gui[3], 0xB8, 0.0f); CHECK(!s.begin().attempted); ++cases; }
        { Scene s; s.observe(); CHECK(!beginNativeAchievementAlignmentCommit(reinterpret_cast<void*>(s.gui[3]), base + 0x1E634D).attempted);
          CHECK(!beginNativeAchievementAlignmentCommit(reinterpret_cast<void*>(s.gui[0]), base + 1).attempted); CHECK(fixture::queries == 0); ++cases; }
        { Scene s; put(s.sprites[0], 0x570, std::uint32_t {2}); s.observe(); CHECK(!current.valid); ++cases; }
        { Scene s; put(s.records[0], 0x40, s.sprites[1]); s.observe(); CHECK(!current.valid); ++cases; }
        { Scene s; put(s.gui[6], 0x10, s.gui[2]); s.observe(); CHECK(!current.valid); ++cases; }
        { Scene s; s.observe(); put(s.textures[0], 0, std::uint64_t {1}); CHECK(!s.begin().attempted && fixture::queries == 0); ++cases; }
        { Scene s; s.observe(); put(s.gui[0], 0xB0, 1.0f); CHECK(!s.begin().attempted);
          put(s.gui[0], 0xB0, 0.0f); put(s.gui[0], 0xA8, 2.0f); CHECK(!s.begin().attempted); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); CHECK(token.attempted); restoreNativeAchievementAlignmentCommit(token);
          fixture::now += 50; s.observe(); token = s.begin(); CHECK(token.attempted && fixture::queries == 1 && current.access.observed == 1000);
          restoreNativeAchievementAlignmentCommit(token); fixture::now += 1; s.observe(); token = s.begin();
          CHECK(token.attempted && fixture::queries == 2 && current.access.observed == 1051); restoreNativeAchievementAlignmentCommit(token); ++cases; }
        { Scene s; s.observe(); fixture::now += 90; fixture::queryDelay = 20; CHECK(!s.begin().attempted && applied == 0); ++cases; }
        { Scene s; s.observe(); fixture::readDelay = 110; CHECK(!s.begin().attempted && applied == 0); ++cases; }
        { Scene s; s.observe(); fixture::loseHud = true; CHECK(!s.begin().attempted && applied == 0); ++cases; }
        { Scene s; s.observe(); const auto before = s.bytes(0); auto token = s.begin(); CHECK(token.attempted);
          fixture::now += 500; current = {}; fixture::hudReady = false;
          put(s.config, 0x1373, static_cast<unsigned char>(0)); put(s.main, 0x48, std::uint32_t {4}); put(s.lcd, 0x4A4, static_cast<unsigned char>(0));
          restoreNativeAchievementAlignmentCommit(token); CHECK(s.bytes(0) == before && !failed && !committing); ++cases; }
        { Scene s; s.observe(); const auto before = s.bytes(0); auto token = s.begin(); CHECK(token.attempted);
          CHECK(exerciseException(&token)); CHECK(fixture::nativeCalls == 1 && s.bytes(0) == before && restored == 1 && !committing); ++cases; }
        { Scene s; s.observe(); CHECK(beginNativeAchievementUpdate() && !current.valid && !s.begin().attempted);
          CHECK(!beginNativeAchievementUpdate()); endNativeAchievementUpdate(reinterpret_cast<void*>(s.lcd), false);
          CHECK(!current.valid && !updating); s.observe(); CHECK(current.valid); ++cases; }
        { Scene s; s.observe(); const auto before = s.bytes(0);
          fixture::readonlyPage = reinterpret_cast<void*>(base + 0x7000); CHECK(!s.begin().attempted && failed && !committing);
          CHECK(s.bytes(0) == before); DWORD old = 0; VirtualProtect(reinterpret_cast<void*>(base + 0x7000), 4096, PAGE_READWRITE, &old); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); CHECK(token.attempted); const auto changed = s.bytes(0);
          put(s.main, 0xF0, s.gui[3]); restoreNativeAchievementAlignmentCommit(token); CHECK(failed && !committing && s.bytes(0) == changed); ++cases; }
        std::cout << cases << " achievement alignment groups passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
