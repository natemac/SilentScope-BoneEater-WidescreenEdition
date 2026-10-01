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
bool raiseUpdate = false, recurseUpdate = false, recursing = false;
constexpr DWORD updateException = 0xE0535343;
bool hudReady = true, invalidateHudOnQuery = false, originalSawInvalid = false;
void* readonlyOnQuery = nullptr;
void (*mutateOnQuery)() = nullptr;
std::uintptr_t mutationAddress = 0;
std::uint32_t mutationValue = 0;
void (*mutateDuringUpdate)() = nullptr;
void mutateWord() { std::memcpy(reinterpret_cast<void*>(mutationAddress), &mutationValue, sizeof(mutationValue)); }
ULONGLONG clock() noexcept { return now; }
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T length) noexcept {
    ++queries; now += queryDelay;
    const auto result = VirtualQuery(address, information, length);
    if (invalidateHudOnQuery) hudReady = false;
    if (mutateOnQuery) { const auto callback = mutateOnQuery; mutateOnQuery = nullptr; callback(); }
    if (readonlyOnQuery) {
        DWORD old = 0; VirtualProtect(readonlyOnQuery, 0x1000, PAGE_READONLY, &old);
        readonlyOnQuery = nullptr;
    }
    return result;
}
}
#define GetTickCount64 fixture::clock
#define VirtualQuery fixture::query
#include "../../src/render/native_options_backing.cpp"
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
void __fastcall nativeUpdate(void* object) {
    ++fixture::nativeCalls; fixture::originalSawInvalid = !current.valid;
    if (fixture::recurseUpdate && !fixture::recursing) {
        fixture::recursing = true; updateObserved(object, base + 0xCC478); fixture::recursing = false;
    }
    if (fixture::mutateDuringUpdate) fixture::mutateDuringUpdate();
    if (fixture::raiseUpdate) RaiseException(fixture::updateException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
bool exceptionalUpdate(void* object) {
    __try { updateObserved(object, base + 0xCC478); }
    __except (GetExceptionCode() == fixture::updateException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t manager, parent, options, resident, config, frame, layout, root, leaf, sprite, material, texture, record;
    RearIlluminationPose original {{{564.68518f, -45.06589f}}, {{0.7906295657f, 0.7906295657f}}};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(allocation); base = reinterpret_cast<std::uintptr_t>(allocation);
        manager = base + 0x1000; parent = base + 0x2000; options = base + 0x3000; resident = base + 0x4000;
        config = base + 0x5000; frame = base + 0x8000; layout = base + 0x10000; root = base + 0x20000;
        leaf = base + 0x2FF70; sprite = base + 0x40000; material = base + 0x41000;
        texture = base + 0x42000; record = base + 0x43000;
        put(base, 0x13DD000, manager); put(manager, 0, base + 0x10C6B38);
        put(manager, 0x38, std::uint32_t {3}); put(manager, 0x3C, std::int32_t {-1}); put(manager, 0x40, parent);
        put(parent, 0, base + 0x10C7308); put(parent, 0x10, std::uint32_t {2});
        put(parent, 0x18, options); put(options, 0, base + 0x10CA6F0);
        put(options, 0x10, std::uint32_t {76});
        put(base, 0x13DCF58, config); put(config, 0, base + 0x10C5FA8); put(config, 0x1373, static_cast<unsigned char>(1));
        put(base, 0x12E24E8, frame); put(frame, 0, base + 0x703E78);
        put(frame, 0x28, std::array<std::uint16_t, 2>{{1920, 1080}});
        put(base, 0x13DCF48, resident); put(resident, 0x128, layout); put(layout, 0, base + 0x10C9FA8);
        put(layout, 0x18, std::uint32_t {2}); put(layout, 0x1C, std::uint32_t {1});
        put(layout, 0x58, 1.0f); put(layout, 0x64, std::uint32_t {0xBAA}); put(layout, 0x3C98, std::uint32_t {2});
        strcpy_s(reinterpret_cast<char*>(layout + 0x368), 32, "Root");
        put(layout, 0x3F8, std::uint32_t {4}); put(layout, 0x428, root);
        for (const auto object : {root, leaf}) {
            put(object, 0, base + (object == root ? 0x10CEB48 : 0x10CEBA8)); put(object, 8, object);
            put(object, 0x3C, std::uint32_t {4}); put(object, 0xC9, static_cast<unsigned char>(1));
            put(object, 0xB8, std::array<float, 2>{{1, 1}}); put(object, 0xC0, std::uint32_t {0xFFFFFF});
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
        put(record, 8, std::uint32_t {0xFFFFFFFF});
        put(root, 0x60, std::array<float, 2>{{643.748168945f,33.9970703125f}});
        put(root, 0x70, std::array<float, 2>{{643.748168945f,33.9970703125f}});
        put(root, 0xA0, original.scale); put(root, 0xA8, original.scale);
        put(base, 0x13DB5A0, base + 0x45000); put(base + 0x45000, 0, base + 0x6FDF48);
        addMasks(); addFront();
        current = {}; updating = committing = nestedUpdate = false; enabled = true; failed = false; ownerThread = 0;
        applied = restored = rejected = 0; nextReport = ~ULONGLONG {0}; originalOptionsUpdate = &nativeUpdate;
        fixture::now = 1000; fixture::queryDelay = fixture::queries = fixture::nativeCalls = 0;
        fixture::hudReady = true; fixture::invalidateHudOnQuery = fixture::originalSawInvalid = false;
        fixture::readonlyOnQuery = nullptr; fixture::mutateOnQuery = nullptr; fixture::mutationAddress = 0; fixture::mutateDuringUpdate = nullptr;
        fixture::raiseUpdate = fixture::recurseUpdate = fixture::recursing = false;
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
        put(mr, 0xB8, std::array<float, 2>{{0,1}}); put(mr, 0xC9, static_cast<unsigned char>(1)); put(mr, 0x18, mask(reverse ? 1 : 0));
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
            put(object, 0xB8, std::array<float, 2>{{1, 0}}); put(object, 0xC0, std::uint32_t {0xA0B0C0});
            put(maskRecord, 0, base + 0x10CEC08); put(maskRecord, 0x40, source);
            put(maskRecord, 0x58, std::array<float, 8>{{0, i ? 0.0f : 1.0f, 1, i ? 0.0f : 1.0f, 0, i ? 1.0f : 0.0f, 1, i ? 1.0f : 0.0f}});
        }
    }
    OptionsBackingAlignment beginMask(unsigned index) { return beginNativeOptionsBackingCommit(reinterpret_cast<void*>(mask(index)), base + 0x1E634D); }
    RearIlluminationPose maskPose(unsigned index) const {
        RearIlluminationPose result;
        std::memcpy(result.position.data(), reinterpret_cast<void*>(mask(index) + 0x70), 8);
        std::memcpy(result.scale.data(), reinterpret_cast<void*>(mask(index) + 0xA8), 8); return result;
    }
    bool maskRestoredExactly(unsigned index) const {
        auto p = maskPose(index), o = maskOriginal(index); return p.position == o.position && p.scale == o.scale;
    }
    std::uintptr_t frontLayout() const { return base + 0x80000; }
    std::uintptr_t frontRoot() const { return base + 0x90000; }
    std::uintptr_t frontLeaf() const { return base + 0x91000; }
    void addFront() {
        const auto l = frontLayout(), r = frontRoot(), f = frontLeaf(), source = base + 0x92000;
        const auto mat = base + 0x93000, tex = base + 0x94000, rec = base + 0x95000, cam = base + 0x96000;
        put(resident, 0x120, l); put(l, 0, base + 0x10C9FA8); put(l, 0x18, 2u); put(l, 0x1C, 1u);
        put(l, 0x58, 1.0f); put(l, 0x6C, 0xB8Cu); put(l, 0x3C90, 1u); put(l, 0x3C98, 3u);
        strcpy_s(reinterpret_cast<char*>(l + 0x368), 32, "Root"); put(l, 0x400, 3u); put(l, 0x438, r);
        for (auto object : {r,f}) {
            put(object, 0, base + (object == r ? 0x10CEB48 : 0x10CEBA8)); put(object, 8, object);
            put(object, 0x3C, 3u); put(object, 0xC9, static_cast<unsigned char>(1)); put(object, 0xB8, std::array<float,2>{{1,1}});
            strcpy_s(reinterpret_cast<char*>(object + 0x40), 32, object == r ? "Root" : "white");
        }
        put(r, 0x18, f); put(r, 0x98, std::array<float,2>{{5,5}});
        put(f, 0x10, r); put(f, 0x98, std::array<float,2>{{968,1566}}); put(f, 0x110, source); put(f, 0x30, rec);
        put(source, 0, base + 0x10CC0D8); put(source, 0x570, 2u); put(source, 0x1A8, cam); put(source, 0x308, mat);
        put(base, 0x13DB5B0, cam); put(cam, 0, base + 0x6FDF48);
        put(mat, 0, base + 0x706570); put(mat, 0x50, tex); put(tex, 0, std::uint64_t{0x5553455200052B00ULL});
        put(tex, 0x18, std::array<std::uint16_t,2>{{64,64}}); put(rec, 0, base + 0x10CEC08); put(rec, 0x40, source); put(rec, 8, 0xFF000000u);
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); current = {}; base = 0; }
    void observe() { updateObserved(reinterpret_cast<void*>(options), base + 0xCC478); }
    OptionsBackingAlignment begin() { return beginNativeOptionsBackingCommit(reinterpret_cast<void*>(leaf), base + 0x1E634D); }
    RearIlluminationPose pose() const {
        RearIlluminationPose p; std::memcpy(p.position.data(), reinterpret_cast<void*>(leaf + 0x70), 8);
        std::memcpy(p.scale.data(), reinterpret_cast<void*>(leaf + 0xA8), 8); return p;
    }
    bool isOriginal() const { const auto p = pose(); return p.position == original.position && p.scale == original.scale; }
};

int main() {
    unsigned cases = 0;
    try {
        for (unsigned phase : {75u,76u,77u}) for (float fade : {0.0f,.25f,.5f,1.0f}) {
            Scene s; put(s.layout,0x58,fade); put(s.leaf,0xBC,fade);
            put(s.options,0x10,phase);
            put(s.root,0xB8,std::array<float,2>{{fade,fade}});
            GuiBytes rootBefore {}; std::memcpy(rootBefore.data(),reinterpret_cast<void*>(s.root),rootBefore.size());
            put(s.record,8,(static_cast<unsigned>(fade*255)<<24)|0xFFFFFFu);
            s.observe(); CHECK(current.valid);
            const auto token=s.begin(); CHECK(token.attempted);
            float retained=0; std::memcpy(&retained,reinterpret_cast<void*>(s.leaf+0xBC),4); CHECK(retained==fade);
            restoreNativeOptionsBackingCommit(token); CHECK(!failed && s.isOriginal());
            CHECK(!std::memcmp(rootBefore.data(),reinterpret_cast<void*>(s.root),rootBefore.size())); ++cases;
        }
        { Scene s; put(s.layout,0x58,.5f); put(s.root,0xB8,std::array<float,2>{{.5f,1}});
          s.observe(); CHECK(current.valid); const auto token=s.begin(); CHECK(token.attempted);
          restoreNativeOptionsBackingCommit(token); CHECK(!failed && s.isOriginal()); ++cases; }
        for (float invalid : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
          Scene s; put(s.root,0xBC,invalid); s.observe(); CHECK(!current.valid && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); CHECK(current.valid && fixture::nativeCalls == 1 && fixture::originalSawInvalid && !fixture::queries);
          std::array<unsigned char,0x130> before {}; std::memcpy(before.data(), reinterpret_cast<void*>(s.leaf), before.size());
          std::array<unsigned char,0x130> front {}, top {}, bottom {};
          std::memcpy(front.data(), reinterpret_cast<void*>(s.frontLeaf()), front.size());
          std::memcpy(top.data(), reinterpret_cast<void*>(s.mask(0)), top.size());
          std::memcpy(bottom.data(), reinterpret_cast<void*>(s.mask(1)), bottom.size());
          const auto token = s.begin(); CHECK(token.attempted && fixture::queries == 1);
          const auto p = s.pose(); const auto h = readNativeHudGeometry();
          CHECK(std::fabs(p.position[0] - .1f - h.left) < .001 && std::fabs(p.scale[0]*1000 - h.width) < .001);
          const auto* bytes = reinterpret_cast<const unsigned char*>(s.leaf);
          for (std::size_t i=0;i<before.size();++i) if (!((i>=0x70 && i<0x74)||(i>=0xA8 && i<0xAC))) CHECK(bytes[i]==before[i]);
          CHECK(!std::memcmp(front.data(),reinterpret_cast<void*>(s.frontLeaf()),front.size()));
          CHECK(!std::memcmp(top.data(),reinterpret_cast<void*>(s.mask(0)),top.size()));
          CHECK(!std::memcmp(bottom.data(),reinterpret_cast<void*>(s.mask(1)),bottom.size()));
          restoreNativeOptionsBackingCommit(token); CHECK(s.isOriginal() && !failed && !committing && restored == 1); ++cases; }
        { Scene s; for (unsigned scene : {1u,2u,4u,5u}) { put(s.manager,0x38,scene); s.observe(); CHECK(!current.valid); }
          put(s.manager,0x38,3u); for (unsigned phase : {0u,1u,3u,99u}) { put(s.parent,0x10,phase); s.observe(); CHECK(!current.valid); }
          put(s.parent,0x10,2u); for (unsigned phase : {0u,1u,50u,51u,52u,74u,78u,100u,101u,99u}) { put(s.options,0x10,phase); s.observe(); CHECK(!current.valid); }
          CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.options,0,base+0x10CA7E0); CHECK(!s.begin().attempted);
          put(s.options,0,base+0x10CA6F0); put(s.parent,0,base+0x10C7278); CHECK(!s.begin().attempted);
          put(s.parent,0,base+0x10C7308); put(s.parent,0x18,s.options+8); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.manager,0x3C,1); CHECK(!s.begin().attempted); put(s.manager,0x3C,-1);
          for (auto object : {s.parent,s.options}) for (unsigned offset : {8u,9u}) {
              put(object,offset,static_cast<unsigned char>(1)); CHECK(!s.begin().attempted); put(object,offset,static_cast<unsigned char>(0)); }
          CHECK(!fixture::queries); ++cases; }
        { Scene s; s.observe(); for (auto object : {s.frontLeaf(),s.mask(0),s.mask(1)}) CHECK(!beginNativeOptionsBackingCommit(reinterpret_cast<void*>(object),base+0x1E634D).attempted);
          CHECK(!beginNativeOptionsBackingCommit(reinterpret_cast<void*>(s.leaf),base+0x1E634E).attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(base,0x13DB5A0,base+0x45008); CHECK(!s.begin().attempted); put(base,0x13DB5A0,base+0x45000);
          put(base,0x13DB5B0,base+0x96008); CHECK(!s.begin().attempted); put(base,0x13DB5B0,base+0x96000);
          put(base+0x96000,0,base+0x10CC0D8); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.texture,0,std::uint64_t{0x5553455200055180ULL}); CHECK(!s.begin().attempted);
          put(s.texture,0,std::uint64_t{0x5553455200052B00ULL}); put(s.record,0x40,s.sprite+8); CHECK(!s.begin().attempted);
          put(s.record,0x40,s.sprite); put(s.sprite,0x570,2u); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); for (auto object : {s.frontLeaf()}) { put(object,0xBC,.5f); CHECK(!s.begin().attempted); put(object,0xBC,1.0f); }
          put(s.layout,0x58,NAN); CHECK(!s.begin().attempted); put(s.layout,0x58,1.0f);
          put(s.record,8,0x7FFFFF00u); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); for (auto object : {s.mask(0),s.mask(1)}) { put(object,0xBC,.01f); CHECK(!s.begin().attempted); put(object,0xBC,0.0f); }
          put(base+0x73000,8,0x01000000u); CHECK(!s.begin().attempted); put(base+0x73000,8,0u);
          put(s.mask(1),0x20,std::uintptr_t{0}); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.root,0x90,1.0f); CHECK(!s.begin().attempted); put(s.root,0x90,0.0f);
          put(s.leaf,0xB0,1.0f); CHECK(!s.begin().attempted); put(s.leaf,0xB0,0.0f);
          put(s.leaf,0x60,-99.0f); CHECK(!s.begin().attempted); put(s.leaf,0x60,-100.0f);
          put(s.leaf,0x70,600.0f); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); auto token=s.begin(); CHECK(token.attempted); restoreNativeOptionsBackingCommit(token);
          fixture::now+=50; s.observe(); token=s.begin(); CHECK(token.attempted && fixture::queries==1 && current.access.observed==1000); restoreNativeOptionsBackingCommit(token);
          ++fixture::now; s.observe(); token=s.begin(); CHECK(token.attempted && fixture::queries==2 && current.access.observed==1051); restoreNativeOptionsBackingCommit(token); ++cases; }
        { Scene s; s.observe(); fixture::now+=90; fixture::queryDelay=20; CHECK(!s.begin().attempted && fixture::queries==1 && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::invalidateHudOnQuery=true; CHECK(!s.begin().attempted && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::mutationAddress=s.options+0x10; fixture::mutationValue=4;
          fixture::mutateOnQuery=&fixture::mutateWord; CHECK(!s.begin().attempted && fixture::queries==1 && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::mutationAddress=s.record+8; fixture::mutationValue=0x7FFFFF00;
          fixture::mutateOnQuery=&fixture::mutateWord; CHECK(!s.begin().attempted && fixture::queries==1 && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::mutationAddress=base+0x96000; fixture::mutationValue=0;
          fixture::mutateOnQuery=&fixture::mutateWord; CHECK(!s.begin().attempted && fixture::queries==1 && s.isOriginal()); ++cases; }
        { Scene s; enabled=false; s.observe(); CHECK(!current.valid && fixture::nativeCalls==1 && !updating);
          CHECK(!s.begin().attempted && !fixture::queries && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::now+=101; CHECK(!s.begin().attempted && !fixture::queries);
          s.observe(); ownerThread=GetCurrentThreadId()+1; CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); const auto token=s.begin(); CHECK(token.attempted);
          current={}; fixture::now+=1000; fixture::hudReady=false; put(s.manager,0x38,1u); put(s.manager,0x3C,3);
          put(s.parent,0,base+0x10C7278); put(s.parent,0x18,std::uintptr_t{0}); put(s.options,0,base+0xA5710);
          put(s.config,0x1373,static_cast<unsigned char>(0)); put(s.frame,0x28,std::array<std::uint16_t,2>{{800,1366}});
          put(s.layout,0x18,3u); put(s.layout,0x58,.3f); put(s.leaf,0xBC,.4f); put(s.record,8,0x22FFFFFFu);
          put(s.leaf,0x74,27.0f); put(s.leaf,0xAC,.61f); put(base,0x13DB5A0,base+0x45008);
          restoreNativeOptionsBackingCommit(token); const auto p=s.pose();
          CHECK(p.position[0]==s.original.position[0] && p.scale[0]==s.original.scale[0] && p.position[1]==27 && p.scale[1]==.61f && !failed && !committing); ++cases; }
        { Scene s; s.observe(); DWORD old=0; CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READONLY,&old));
          CHECK(!s.begin().attempted && s.isOriginal()); CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; s.observe(); fixture::readonlyOnQuery=reinterpret_cast<void*>(base+0x30000);
          CHECK(!s.begin().attempted && failed && !committing && s.isOriginal()); DWORD old=0;
          CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; s.observe(); const auto token=s.begin(); CHECK(token.attempted); put(s.resident,0x128,s.layout+8);
          restoreNativeOptionsBackingCommit(token); CHECK(failed && !committing); put(s.resident,0x128,s.layout); CHECK(writeHorizontal(token.permission,token.original)); ++cases; }
        { Scene s; s.observe(); updateObserved(reinterpret_cast<void*>(s.options),base+0xCC479);
          CHECK(!current.valid && !updating && fixture::nativeCalls==2 && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); fixture::raiseUpdate=true; CHECK(exceptionalUpdate(reinterpret_cast<void*>(s.options)));
          CHECK(!current.valid && !updating && fixture::nativeCalls==2 && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); fixture::recurseUpdate=true; s.observe();
          CHECK(!current.valid && !updating && fixture::nativeCalls==3 && !s.begin().attempted); ++cases; }
        { Scene s; const auto newRoot=base+0x24F30; std::memcpy(reinterpret_cast<void*>(newRoot),reinterpret_cast<void*>(s.root),0xD0);
          s.root=newRoot; put(s.root,8,s.root); put(s.layout,0x428,s.root); put(s.leaf,0x10,s.root);
          DWORD old=0; CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x25000),0x1000,PAGE_NOACCESS,&old));
          s.observe(); const auto token=s.begin(); CHECK(token.attempted); restoreNativeOptionsBackingCommit(token); CHECK(s.isOriginal() && !failed);
          CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x25000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; const auto h=readNativeHudGeometry(); RearIlluminationPose after {{{9,9}},{{9,9}}}; auto before=s.original;
          before.scale[0]=.1f; CHECK(!fitNormalStartWhiteHorizontal(h.left,h.top,h.width,h.height,before,after));
          before=s.original; before.position[1]=NAN; CHECK(!fitNormalStartWhiteHorizontal(h.left,h.top,h.width,h.height,before,after));
          CHECK(after.position[0]==9 && after.scale[1]==9); ++cases; }
        { Scene s; put(s.options,0x10,74u);fixture::mutationAddress=s.options+0x10;fixture::mutationValue=76;
          fixture::mutateDuringUpdate=&fixture::mutateWord;s.observe();CHECK(!current.valid && fixture::nativeCalls==1 && !s.begin().attempted);
          fixture::mutateDuringUpdate=nullptr;s.observe();CHECK(current.valid && fixture::nativeCalls==2);const auto token=s.begin();CHECK(token.attempted);restoreNativeOptionsBackingCommit(token);++cases; }
        { Scene s; s.observe();fixture::mutationAddress=s.options+0x10;fixture::mutationValue=78;fixture::mutateDuringUpdate=&fixture::mutateWord;
          s.observe();CHECK(!current.valid && fixture::nativeCalls==2 && !s.begin().attempted);++cases; }
        { Scene s;s.observe();fixture::mutationAddress=s.parent+0x18;fixture::mutationValue=0;fixture::mutateDuringUpdate=&fixture::mutateWord;
          s.observe();CHECK(!current.valid && !s.begin().attempted && fixture::nativeCalls==2);++cases; }
        { Scene s;s.observe();updateObserved(reinterpret_cast<void*>(s.options),base+0xCC416);
          CHECK(!current.valid && fixture::nativeCalls==2 && !s.begin().attempted);++cases; }
        for (unsigned phase : {1u, 11u, 2u, 0u, 22u}) {
            Scene s;
            put(s.manager,0x38,5u); put(s.parent,0,base+0x10C6E48);
            put(s.parent,0x1190,phase); put(s.frontLayout(),0x58,0.0f);
            put(s.frontRoot(),0xB8,0.0f); put(s.frontLeaf(),0xBC,0.0f);
            put(base+0x95000,8,0u); originalBattleUpdate=&nativeUpdate;
            updateBattle(reinterpret_cast<void*>(s.parent));
            CHECK(current.valid == (phase==1 || phase==11));
            const auto token=s.begin(); CHECK(token.attempted == current.valid);
            if(token.attempted) {
                float x=0,scale=0;read(s.leaf,0x70,x);read(s.leaf,0xA8,scale);
                CHECK(x==0.1f && scale==1920.f/1000.f);
            }
            restoreNativeOptionsBackingCommit(token); CHECK(s.isOriginal() && !failed); ++cases;
        }
        { Scene s; put(s.manager,0x38,5u);put(s.parent,0,base+0x10C6E48);put(s.parent,0x1190,1u);
          put(s.frontLayout(),0x58,0.0f);put(s.frontRoot(),0xB8,0.0f);put(s.frontLeaf(),0xBC,0.0f);put(base+0x95000,8,0u);
          const auto ui=base+0xB0000,splash=base+0xC0000,textures=base+0xD0000,builder=base+0xD1000;
          put(base,0x13DCF78,ui);put(ui,0,base+0x10CA298);put(ui,0x38,s.manager);put(ui,0xE8,splash);
          put(splash,0,base+0x10CA680);put(splash,0x48,2u);put(splash,0x4C,1u);
          put(splash,0x80,textures);put(splash,0x88,builder);put(textures,0,base+0x10C9D70);put(builder,0,base+0x10CEC80);
          put(s.layout,0x58,0.0f);put(s.root,0xB8,0.0f);
          originalBattleUpdate=&nativeUpdate;fixture::mutationAddress=s.parent+0x1190;fixture::mutationValue=2;
          fixture::mutateDuringUpdate=&fixture::mutateWord;
          updateBattle(reinterpret_cast<void*>(s.parent));CHECK(current.valid);
          const auto token=s.begin();CHECK(token.attempted);
          float x=0,scale=0;read(s.leaf,0x70,x);read(s.leaf,0xA8,scale);CHECK(x==0.1f&&scale==1920.f/1000.f);
          restoreNativeOptionsBackingCommit(token);CHECK(s.isOriginal()&&!failed);
          put(s.leaf,0xBC,0.0f);put(s.record,8,0x00FFFFFFu);
          updateBattle(reinterpret_cast<void*>(s.parent));CHECK(!current.valid&&!s.begin().attempted);++cases;
        }
        { Scene s; put(s.manager,0x38,5u); put(s.parent,0,base+0x10C6E48);
          put(s.parent,0x1190,11u); originalBattleUpdate=&nativeUpdate;
          updateBattle(reinterpret_cast<void*>(s.parent)); CHECK(!current.valid); ++cases; }
        std::cout << "Passed " << cases << " Options backing groups\n";
    } catch (const std::exception& error) { std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1; }
}

