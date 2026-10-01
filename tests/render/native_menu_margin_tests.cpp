// Exact production module over a private inert heap. No native game, window,
// device or detour is created. RPM, VQ and checked writes use real Win32 APIs.
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace fixture {
ULONGLONG now = 1000, delay = 0;
unsigned queries = 0, nativeCalls = 0, verifyCalls = 0;
bool hudReady = true, loseHud = false, raiseUpdate = false, recurseUpdate = false, recursing = false;
bool sawInvalid = false;
void* protectOnQuery = nullptr;
constexpr DWORD nativeException = 0xE0431818;
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries; now += delay;
    const auto result = VirtualQuery(address, information, length);
    if (loseHud) hudReady = false;
    if (protectOnQuery) {
        DWORD old = 0; VirtualProtect(protectOnQuery, 0x1000, PAGE_READONLY, &old); protectOnQuery = nullptr;
    }
    return result;
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#include "../../src/render/native_menu_margin.cpp"
#undef VirtualQuery
#undef GetTickCount64

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { ++fixture::verifyCalls; return "fixture_only"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h; h.valid = fixture::hudReady; h.helper = 1; h.sprite = 2;
    h.mainWidth = 1920; h.mainHeight = 1080; h.width = 768.0f * 1080 / 1366;
    h.height = 1080; h.left = (1920 - h.width) / 2; h.top = 0; return h;
}
}
using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
template<class T> void put(std::uintptr_t object, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<void*>(object + offset), &value, sizeof(value));
}
void __fastcall nativeUpdate(void* object) {
    ++fixture::nativeCalls;
    fixture::sawInvalid = !current.valid;
    if (fixture::recurseUpdate && !fixture::recursing) {
        fixture::recursing = true; updateObserved(object, base + 0xA55B8); fixture::recursing = false;
    }
    if (fixture::raiseUpdate) RaiseException(fixture::nativeException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
bool exceptionalUpdate(void* object) {
    __try { updateObserved(object, base + 0xA55B8); }
    __except (GetExceptionCode() == fixture::nativeException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t manager, menu, config, frame, layout, root, sprite, material, texture, camera, record;
    std::array<std::uintptr_t, 4> leaves;
    RearIlluminationPose original {{{643.7481689453125f, -45.065887451171875f}}, {{.7906295657157898f, .7906295657157898f}}};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(allocation); base = reinterpret_cast<std::uintptr_t>(allocation);
        manager = base + 0x1000; menu = base + 0x2000; config = base + 0x4000; frame = base + 0x8000;
        layout = base + 0x10000; root = base + 0x20000;
        leaves = {{base + 0x21000, base + 0x22000, base + 0x23000, base + 0x2FF70}};
        sprite = base + 0x40000; material = base + 0x41000; texture = base + 0x42000; camera = base + 0x43000; record = base + 0x44000;
        put(base, 0x13DD000, manager); put(manager, 0, base + 0x10C6B38); put(manager, 0x38, std::uint32_t {2});
        put(manager, 0x3C, std::int32_t {-1}); put(manager, 0x40, menu);
        put(menu, 0, base + 0x10C7030); put(menu, 0x10, std::uint32_t {1}); put(menu, 0x14, std::uint32_t {2}); put(menu, 0x20, layout);
        put(base, 0x13DCF58, config); put(config, 0, base + 0x10C5FA8); put(config, 0x1373, static_cast<unsigned char>(1));
        put(base, 0x12E24E8, frame); put(frame, 0, base + 0x703E78); put(frame, 0x28, std::array<std::uint16_t, 2>{{1920, 1080}});
        put(layout, 0, base + 0x10C9FA8); put(layout, 0x18, std::uint32_t {2}); put(layout, 0x1C, std::uint32_t {1});
        put(layout, 0x64, std::uint32_t {0xBA0}); put(layout, 0x3C98, std::uint32_t {1}); put(layout, 0x3F8, std::uint32_t {13});
        strcpy_s(reinterpret_cast<char*>(layout + 0x368), 32, "Root"); put(layout, 0x428, root);
        put(root, 0, base + 0x10CEB48); put(root, 8, root); put(root, 0x18, leaves[0]); put(root, 0x3C, std::uint32_t {13});
        strcpy_s(reinterpret_cast<char*>(root + 0x40), 32, "Root"); put(root, 0x98, std::array<float, 2>{{5, 5}});
        put(root, 0x60, std::array<float, 2>{{643.7481689453125f,33.9970703125f}});
        put(root, 0x70, std::array<float, 2>{{643.7481689453125f,33.9970703125f}});
        put(root, 0xA0, original.scale); put(root, 0xA8, original.scale); put(root, 0xC9, static_cast<unsigned char>(1));
        put(root, 0xB8, std::array<float, 2>{{1, 1}});
        constexpr std::array<const char*, 4> names {{"BG", "BG2", "BG3", "White"}};
        for (unsigned i = 0; i < 4; ++i) {
            const auto leaf = leaves[i];
            put(leaf, 0, base + 0x10CEBA8); put(leaf, 8, leaf); put(leaf, 0x10, root);
            put(leaf, 0x20, i ? leaves[i - 1] : std::uintptr_t {0}); put(leaf, 0x28, i == 3 ? std::uintptr_t {0} : leaves[i + 1]);
            put(leaf, 0x3C, std::uint32_t {13}); strcpy_s(reinterpret_cast<char*>(leaf + 0x40), 32, names[i]);
            put(leaf, 0x60, std::array<float, 2>{{0, i == 3 ? -100.0f : 88.0f}});
            put(leaf, 0x98, std::array<float, 2>{{800, i == 3 ? 1480.0f : 969.0f}}); put(leaf, 0xA0, std::array<float, 2>{{1, 1}});
            put(leaf, 0x70, original.position); put(leaf, 0xA8, original.scale); put(leaf, 0xC9, static_cast<unsigned char>(1));
            put(leaf, 0xB8, std::array<float, 2>{{.63f, .41f}}); put(leaf, 0xC0, std::uint32_t {0x2478AC});
            put(leaf, 0x110, sprite); // Deliberately shared with BG; only exact White may change.
        }
        put(leaves[3], 0x30, record);
        put(sprite, 0, base + 0x10CC0D8); put(sprite, 0x1A8, camera); put(sprite, 0x308, material);
        put(base, 0x13DB5A0, camera); put(camera, 0, base + 0x6FDF48);
        put(material, 0, base + 0x706570); put(material, 0x50, texture);
        put(texture, 0, std::uint64_t {0x5553455200061280ULL}); put(texture, 0x18, std::array<std::uint16_t, 2>{{2048, 2048}});
        put(record, 0, base + 0x10CEC08); put(record, 0x40, sprite); put(record, 0x58, whiteUv); put(record, 8, std::uint32_t {0x682478AC});
        current = {}; updating = committing = nestedUpdate = false; enabled = true; failed = false; ownerThread = 0;
        applied = restored = rejected = 0; nextReport = ~ULONGLONG {0}; originalMenuUpdate = &nativeUpdate;
        fixture::now = 1000; fixture::delay = fixture::queries = fixture::nativeCalls = fixture::verifyCalls = 0;
        fixture::hudReady = true; fixture::loseHud = fixture::raiseUpdate = fixture::recurseUpdate = fixture::recursing = fixture::sawInvalid = false;
        fixture::protectOnQuery = nullptr;
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); current = {}; base = 0; }
    void observe() { updateObserved(reinterpret_cast<void*>(menu), base + 0xA55B8); }
    MenuMarginAlignment begin() { return beginNativeMenuMarginCommit(reinterpret_cast<void*>(leaves[3]), base + 0x1E634D); }
    RearIlluminationPose readPose() const {
        RearIlluminationPose result;
        std::memcpy(result.position.data(), reinterpret_cast<void*>(leaves[3] + 0x70), 8);
        std::memcpy(result.scale.data(), reinterpret_cast<void*>(leaves[3] + 0xA8), 8); return result;
    }
    bool horizontalRestored() const { const auto p = readPose(); return p.position[0] == original.position[0] && p.scale[0] == original.scale[0]; }
    void addBackground(unsigned role) {
        const auto source = base + 0x60000 + role * 0x4000;
        const auto mat = source + 0x1000, tex = source + 0x2000, rec = source + 0x3000;
        put(leaves[role], 0x110, source); put(leaves[role], 0x30, rec);
        put(leaves[role], 0x74, 33.9970703125f + 88 * original.scale[1]);
        put(source, 0, base + 0x10CC0D8); put(source, 0x1A8, camera); put(source, 0x308, mat);
        put(mat, 0, base + 0x706570); put(mat, 0x50, tex);
        put(tex, 0, std::uint64_t{0x5553455200061280ULL + role * 0x80ULL});
        put(tex, 0x18, std::array<std::uint16_t,2>{{2048,2048}});
        put(rec, 0, base + 0x10CEC08); put(rec, 0x40, source); put(rec, 0x58, backgroundUv[role]);
    }
};

int main() {
    unsigned cases = 0;
    try {
        // Each named artwork role follows the same panel bounds through fades,
        // while every byte except borrowed X/scale-X remains untouched.
        for (unsigned phase : {1u,2u,3u,9u,10u,11u,12u,14u,15u,16u,17u,18u}) {
            for (unsigned variant = 0; variant < 2; ++variant)
            for (unsigned role = 0; role < 3; ++role) {
                Scene s; s.addBackground(role);
                put(base + 0x63000 + role * 0x4000, 0x58, backgroundUv[variant]);
                put(s.menu,0x14,phase); s.observe();
                std::array<unsigned char,0x130> before {}, during {};
                std::memcpy(before.data(),reinterpret_cast<void*>(s.leaves[role]),before.size());
                auto token = beginNativeMenuMarginCommit(reinterpret_cast<void*>(s.leaves[role]),base+0x1E634D);
                CHECK(token.attempted && token.role == role);
                std::memcpy(during.data(),reinterpret_cast<void*>(s.leaves[role]),during.size());
                for(unsigned i=0;i<before.size();++i)
                    if(!((i>=0x70 && i<0x74)||(i>=0xA8 && i<0xAC))) CHECK(before[i]==during[i]);
                const auto x = field<float>(during,0x70), scale = field<float>(during,0xA8);
                CHECK(std::fabs(x-.1f-readNativeHudGeometry().left)<.001f);
                CHECK(std::fabs(scale*800-readNativeHudGeometry().width)<.001f);
                put(s.menu,0x14,0u); // Restoration does not depend on eligibility.
                restoreNativeMenuMarginCommit(token);
                CHECK(!failed && std::memcmp(before.data(),reinterpret_cast<void*>(s.leaves[role]),before.size())==0);
                ++cases;
            }
        }
        { Scene s; s.addBackground(1); s.observe(); put(base+0x67000,0x58,.7f);
          CHECK(!beginNativeMenuMarginCommit(reinterpret_cast<void*>(s.leaves[1]),base+0x1E634D).attempted);
          CHECK(!failed); ++cases; }
        { Scene s; s.observe(); CHECK(current.valid && fixture::queries == 0 && fixture::nativeCalls == 1 && fixture::sawInvalid);
          std::array<unsigned char, 0x130> before {}, during {}, bg {};
          std::array<unsigned char, 0x78> record {};
          std::memcpy(before.data(), reinterpret_cast<void*>(s.leaves[3]), before.size());
          std::memcpy(bg.data(), reinterpret_cast<void*>(s.leaves[0]), bg.size()); std::memcpy(record.data(), reinterpret_cast<void*>(s.record), record.size());
          const auto token = s.begin(); CHECK(token.attempted && fixture::queries == 1); const auto p = s.readPose(); const auto h = readNativeHudGeometry();
          CHECK(std::fabs(p.position[0] - .1f - h.left) < .001 && std::fabs(p.scale[0] * 800 - h.width) < .001);
          std::memcpy(during.data(), reinterpret_cast<void*>(s.leaves[3]), during.size());
          for (std::size_t i = 0; i < before.size(); ++i) if (!((i >= 0x70 && i < 0x74) || (i >= 0xA8 && i < 0xAC))) CHECK(before[i] == during[i]);
          CHECK(std::memcmp(bg.data(), reinterpret_cast<void*>(s.leaves[0]), bg.size()) == 0);
          CHECK(std::memcmp(record.data(), reinterpret_cast<void*>(s.record), record.size()) == 0);
          restoreNativeMenuMarginCommit(token); CHECK(std::memcmp(before.data(), reinterpret_cast<void*>(s.leaves[3]), before.size()) == 0); ++cases; }
        { Scene s; for (unsigned state : {0u, 4u, 5u, 6u, 7u, 8u, 13u, 19u, ~0u}) {
              put(s.menu, 0x14, state); s.observe(); CHECK(!current.valid && !s.begin().attempted); }
          put(s.menu, 0x14, 2u); for (unsigned scene : {1u, 3u, 4u, 5u}) { put(s.manager, 0x38, scene); s.observe(); CHECK(!current.valid); }
          put(s.manager, 0x38, 2u); put(s.manager, 0x3C, 3); s.observe(); CHECK(!current.valid && !fixture::queries); ++cases; }
        { Scene s; s.observe(); for (unsigned i = 0; i < 3; ++i) CHECK(!beginNativeMenuMarginCommit(reinterpret_cast<void*>(s.leaves[i]), base + 0x1E634D).attempted);
          CHECK(!beginNativeMenuMarginCommit(reinterpret_cast<void*>(s.leaves[3]), base + 0x1E634E).attempted && !fixture::queries); ++cases; }
        { Scene s; put(s.layout, 0x64, std::uint32_t {0xBAA}); s.observe(); CHECK(!current.valid);
          put(s.layout, 0x64, std::uint32_t {0xBA0}); put(s.layout, 0x3C90, std::uint32_t {1}); s.observe(); CHECK(!current.valid);
          put(s.layout, 0x3C90, std::uint32_t {0}); put(s.layout, 0x3C98, std::uint32_t {2}); s.observe(); CHECK(!current.valid && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.leaves[1], 0x20, std::uintptr_t {0}); CHECK(!s.begin().attempted);
          put(s.leaves[1], 0x20, s.leaves[0]); put(s.leaves[3], 0x28, s.leaves[0]); CHECK(!s.begin().attempted);
          put(s.leaves[3], 0x28, std::uintptr_t {0}); put(s.leaves[3], 0x18, s.leaves[0]); CHECK(!s.begin().attempted);
          put(s.leaves[3], 0x18, std::uintptr_t {0}); put(s.leaves[3], 0x3C, std::uint32_t {12}); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.sprite, 0x570, 2u); CHECK(!s.begin().attempted); put(s.sprite, 0x570, 0u);
          put(s.texture, 0, std::uint64_t {0x5553455200052B00}); CHECK(!s.begin().attempted); put(s.texture, 0, std::uint64_t {0x5553455200061280});
          put(s.texture, 0x18, std::array<std::uint16_t, 2>{{64, 64}}); CHECK(!s.begin().attempted); put(s.texture, 0x18, std::array<std::uint16_t, 2>{{2048, 2048}});
          put(s.record, 0x40, s.sprite + 8); CHECK(!s.begin().attempted); put(s.record, 0x40, s.sprite);
          put(s.camera, 0, base + 0x10CC0D8); CHECK(!s.begin().attempted); put(s.camera, 0, base + 0x6FDF48);
          put(s.record, 0x58, 0.0f); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.root, 0x90, 1.0f); CHECK(!s.begin().attempted); put(s.root, 0x90, 0.0f);
          put(s.leaves[3], 0xB0, 1.0f); CHECK(!s.begin().attempted); put(s.leaves[3], 0xB0, 0.0f);
          put(s.leaves[3], 0xC9, static_cast<unsigned char>(0)); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.leaves[3], 0xBC, NAN); CHECK(!s.begin().attempted); put(s.leaves[3], 0xBC, .25f);
          auto token = s.begin(); CHECK(token.attempted); restoreNativeMenuMarginCommit(token);
          put(s.leaves[3], 0xBC, 0.0f); token = s.begin(); CHECK(token.attempted); restoreNativeMenuMarginCommit(token); CHECK(s.horizontalRestored()); ++cases; }
        { Scene s; s.observe(); auto token = s.begin(); restoreNativeMenuMarginCommit(token);
          fixture::now += 50; s.observe(); token = s.begin(); CHECK(token.attempted && fixture::queries == 1 && current.access.observed == 1000); restoreNativeMenuMarginCommit(token);
          ++fixture::now; s.observe(); token = s.begin(); CHECK(token.attempted && fixture::queries == 2 && current.access.observed == 1051); restoreNativeMenuMarginCommit(token); ++cases; }
        { Scene s; s.observe(); fixture::now += 90; fixture::delay = 20;
          CHECK(!s.begin().attempted && fixture::queries == 1 && s.horizontalRestored()); ++cases; }
        { Scene s; s.observe(); fixture::loseHud = true; CHECK(!s.begin().attempted && s.horizontalRestored()); ++cases; }
        { Scene s; s.observe(); fixture::now += 101; CHECK(!s.begin().attempted && !fixture::queries);
          s.observe(); ownerThread = GetCurrentThreadId() + 1; CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); const auto token = s.begin(); CHECK(token.attempted);
          current = {}; fixture::now += 1000; fixture::hudReady = false; put(s.manager, 0x38, 3u); put(s.manager, 0x3C, 5);
          put(s.menu, 0x10, 2u); put(s.menu, 0x14, 9u); put(s.menu, 8, static_cast<unsigned char>(1));
          put(s.config, 0x1373, static_cast<unsigned char>(0)); put(s.frame, 0x28, std::array<std::uint16_t, 2>{{800, 1366}}); put(s.layout, 0x18, 3u);
          put(s.leaves[3], 0x74, 9.0f); put(s.leaves[3], 0xAC, .88f); put(s.leaves[3], 0xC9, static_cast<unsigned char>(0));
          put(s.leaves[3], 0xBC, .7f); put(s.record, 0x58, .1f);
          restoreNativeMenuMarginCommit(token); const auto p = s.readPose(); CHECK(s.horizontalRestored() && !failed && !committing);
          CHECK(p.position[1] == 9 && p.scale[1] == .88f); float uv = 0; std::memcpy(&uv, reinterpret_cast<void*>(s.record + 0x58), 4); CHECK(uv == .1f); ++cases; }
        { Scene s; s.observe(); DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READONLY, &old));
          CHECK(!s.begin().attempted && s.horizontalRestored()); CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; s.observe(); DWORD old = 0;
          // The actual VQ result authorizes a private RW region. Page two
          // loses access just after that snapshot, before the actual copy.
          fixture::protectOnQuery = reinterpret_cast<void*>(base + 0x30000);
          CHECK(!s.begin().attempted && failed && !committing && s.horizontalRestored());
          CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x30000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; s.observe(); const auto token = s.begin(); CHECK(token.attempted); put(s.menu, 0x20, s.layout + 8);
          restoreNativeMenuMarginCommit(token); CHECK(failed && !committing); put(s.menu, 0x20, s.layout); CHECK(writeHorizontal(token.permission, token.original)); ++cases; }
        { Scene s; s.observe(); CHECK(current.valid); updateObserved(reinterpret_cast<void*>(s.menu), base + 0xA55B9);
          CHECK(!current.valid && !updating && fixture::nativeCalls == 2 && !s.begin().attempted); ++cases; }
        { Scene s; const auto boundedRoot = base + 0x24F30;
          std::memcpy(reinterpret_cast<void*>(boundedRoot), reinterpret_cast<void*>(s.root), 0xD0);
          s.root = boundedRoot; put(s.root, 8, s.root); put(s.layout, 0x428, s.root);
          for (const auto leaf : s.leaves) put(leaf, 0x10, s.root);
          DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x25000), 0x1000, PAGE_NOACCESS, &old));
          s.observe(); CHECK(current.valid); const auto token = s.begin(); CHECK(token.attempted);
          restoreNativeMenuMarginCommit(token); CHECK(s.horizontalRestored() && !failed);
          CHECK(VirtualProtect(reinterpret_cast<void*>(base + 0x25000), 0x1000, PAGE_READWRITE, &old)); ++cases; }
        { Scene s; s.observe(); fixture::raiseUpdate = true; CHECK(exceptionalUpdate(reinterpret_cast<void*>(s.menu)));
          CHECK(!current.valid && !updating && fixture::nativeCalls == 2 && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); fixture::recurseUpdate = true; s.observe();
          CHECK(!current.valid && !updating && fixture::nativeCalls == 3 && !s.begin().attempted); ++cases; }
        { Scene s; const auto h = readNativeHudGeometry(); RearIlluminationPose result {{{9, 9}}, {{9, 9}}}; auto bad = s.original;
          bad.position[0] = NAN; CHECK(!fitMenuWhiteHorizontal(h.left,h.top,h.width,h.height,bad,result));
          bad = s.original; bad.scale[0] = .1f; CHECK(!fitMenuWhiteHorizontal(h.left,h.top,h.width,h.height,bad,result));
          CHECK(!fitMenuWhiteHorizontal(h.left,h.top,h.width + 1,h.height,s.original,result));
          CHECK(result.position[0] == 9 && result.scale[0] == 9); ++cases; }
        { Scene s; originalMenuUpdate = nullptr; enabled = false;
          CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_RANKING_BACKING", L"1")); CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_MENU_MARGIN", nullptr));
          installNativeMenuMargin(reinterpret_cast<void*>(base), true); CHECK(fixture::verifyCalls == 0 && !failed && !enabled);
          CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_RANKING_BACKING", nullptr)); CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_MENU_MARGIN", L"1"));
          CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", L"fit")); installNativeMenuMargin(reinterpret_cast<void*>(base), true);
          CHECK(fixture::verifyCalls == 1 && failed && !enabled); // Fixture verifier intentionally refuses hooks.
          CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_MENU_MARGIN", nullptr)); CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", nullptr)); ++cases; }
        for (const unsigned phase : {1u, 3u, 9u, 10u, 11u, 12u, 14u, 15u, 16u, 17u, 18u}) { Scene s; put(s.menu, 0x14, phase);
          // Paired observations: Stage Select uses BG2, Rules uses BG,
          // Tutorial Choice uses BG3. None is itself a correction target.
          const unsigned visibleBg = phase == 9 ? 1 : phase == 12 ? 0 : 2;
          for (unsigned i = 0; i < 3; ++i) put(s.leaves[i], 0xC9, static_cast<unsigned char>(i == visibleBg));
          std::array<std::array<unsigned char, 0x130>, 4> before {};
          std::array<unsigned char, 0x78> record {};
          for (unsigned i = 0; i < 4; ++i) std::memcpy(before[i].data(), reinterpret_cast<void*>(s.leaves[i]), before[i].size());
          std::memcpy(record.data(), reinterpret_cast<void*>(s.record), record.size());
          s.observe(); CHECK(current.valid && fixture::queries == 0);
          for (unsigned i = 0; i < 3; ++i) CHECK(!beginNativeMenuMarginCommit(reinterpret_cast<void*>(s.leaves[i]), base + 0x1E634D).attempted);
          const auto token = s.begin(); CHECK(token.attempted && fixture::queries == 1);
          const auto p = s.readPose(); const auto h = readNativeHudGeometry();
          CHECK(std::fabs(p.position[0] - .1f - h.left) < .001 && std::fabs(p.scale[0] * 800 - h.width) < .001);
          const auto* during = reinterpret_cast<const unsigned char*>(s.leaves[3]);
          for (std::size_t i = 0; i < before[3].size(); ++i) if (!((i >= 0x70 && i < 0x74) || (i >= 0xA8 && i < 0xAC))) CHECK(before[3][i] == during[i]);
          for (unsigned i = 0; i < 3; ++i) CHECK(std::memcmp(before[i].data(), reinterpret_cast<void*>(s.leaves[i]), before[i].size()) == 0);
          CHECK(std::memcmp(record.data(), reinterpret_cast<void*>(s.record), record.size()) == 0);
          restoreNativeMenuMarginCommit(token); CHECK(!failed && !committing);
          for (unsigned i = 0; i < 4; ++i) CHECK(std::memcmp(before[i].data(), reinterpret_cast<void*>(s.leaves[i]), before[i].size()) == 0); ++cases; }
        for (const unsigned phase : {1u, 3u, 9u, 10u, 11u, 12u, 14u, 15u, 16u, 17u, 18u}) { Scene s; put(s.menu, 0x14, phase); s.observe(); CHECK(current.valid);
          put(s.menu, 0x14, 0u); CHECK(!s.begin().attempted && s.horizontalRestored() && !fixture::queries);
          put(s.menu, 0x14, phase); s.observe(); put(s.manager, 0x3C, 3);
          CHECK(!s.begin().attempted && s.horizontalRestored() && !fixture::queries); ++cases; }
        for (const unsigned phase : {1u, 3u, 9u, 10u, 11u, 12u, 14u, 15u, 16u, 17u, 18u}) { Scene s; put(s.menu, 0x14, phase); s.observe(); const auto token = s.begin(); CHECK(token.attempted);
          // A native state/vertical animation change cannot cancel repayment
          // of the two borrowed horizontal fields on the same owned leaf.
          put(s.menu, 0x14, 0u); put(s.manager, 0x3C, 3); put(s.leaves[3], 0x74, 27.0f); put(s.leaves[3], 0xAC, .61f);
          current = {}; fixture::now += 1000; fixture::hudReady = false;
          restoreNativeMenuMarginCommit(token); const auto p = s.readPose();
          CHECK(s.horizontalRestored() && p.position[1] == 27 && p.scale[1] == .61f && !failed && !committing); ++cases; }
        std::cout << "Passed " << cases << " Main_Quarter margin groups\n";
    } catch (const std::exception& error) { std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1; }
}
