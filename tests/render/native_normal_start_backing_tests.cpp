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
#include "../../src/render/native_normal_start_backing.cpp"
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
        fixture::recursing = true; updateObserved(object, base + 0xB07DC); fixture::recursing = false;
    }
    if (fixture::raiseUpdate) RaiseException(fixture::updateException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}
bool exceptionalUpdate(void* object) {
    __try { updateObserved(object, base + 0xB07DC); }
    __except (GetExceptionCode() == fixture::updateException ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
struct Scene {
    unsigned char* allocation = nullptr;
    std::uintptr_t manager, parent, credit, resident, config, frame, layout, root, leaf, sprite, material, texture, record;
    RearIlluminationPose original {{{564.68518f, -45.06589f}}, {{0.7906295657f, 0.7906295657f}}};
    Scene() {
        allocation = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(allocation); base = reinterpret_cast<std::uintptr_t>(allocation);
        manager = base + 0x1000; parent = base + 0x2000; credit = base + 0x3000; resident = base + 0x4000;
        config = base + 0x5000; frame = base + 0x8000; layout = base + 0x10000; root = base + 0x20000;
        leaf = base + 0x2FF70; sprite = base + 0x40000; material = base + 0x41000;
        texture = base + 0x42000; record = base + 0x43000;
        put(base, 0x13DD000, manager); put(manager, 0, base + 0x10C6B38);
        put(manager, 0x38, std::uint32_t {4}); put(manager, 0x3C, std::int32_t {-1}); put(manager, 0x40, parent);
        put(parent, 0, base + 0x10C6FA0); put(parent, 0x10, std::uint32_t {1});
        put(parent, 0x14, std::uint32_t {8}); put(parent, 0x48, credit); put(credit, 0, base + 0x10CA7A8);
        put(credit, 0x10, std::uint32_t {1}); put(credit, 0x14, std::uint32_t {3});
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
        applied = restored = rejected = quarterApplied = quarterRestored = 0; nextReport = ~ULONGLONG {0}; originalCreditUpdate = &nativeUpdate;
        fixture::now = 1000; fixture::queryDelay = fixture::queries = fixture::nativeCalls = 0;
        fixture::hudReady = true; fixture::invalidateHudOnQuery = fixture::originalSawInvalid = false;
        fixture::readonlyOnQuery = nullptr; fixture::mutateOnQuery = nullptr; fixture::mutationAddress = 0;
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
    NormalStartBackingAlignment beginMask(unsigned index) { return beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(mask(index)), base + 0x1E634D); }
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
    std::uintptr_t quarterLayout() const { return base+0xA0000; }
    std::uintptr_t quarterRoot() const { return base+0xA8000; }
    std::uintptr_t quarterLeaf(unsigned i) const { return base+(i==2 ? 0xBFF70 : 0xB0000+i*0x1000); }
    RearIlluminationPose quarterOriginal() const { return {{{643.748168945f,-45.06588745f}},original.scale}; }
    RearIlluminationPose quarterPoseValue() const {
        RearIlluminationPose p; std::memcpy(p.position.data(),reinterpret_cast<void*>(quarterLeaf(2)+0x70),8);
        std::memcpy(p.scale.data(),reinterpret_cast<void*>(quarterLeaf(2)+0xA8),8); return p;
    }
    NormalStartBackingAlignment beginQuarter() { return beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(quarterLeaf(2)),base+0x1E634D); }
    bool quarterOriginalExactly() const { const auto a=quarterPoseValue(),b=quarterOriginal(); return a.position==b.position && a.scale==b.scale; }
    void addQuarter() {
        const auto l=quarterLayout(),r=quarterRoot(),src=base+0xD0000,mat=base+0xD1000,tex=base+0xD2000,rec=base+0xD3000;
        put(credit,0x20,l);put(l,0,base+0x10C9FA8);put(l,0x18,2u);put(l,0x1C,1u);put(l,0x64,0xBA0u);
        put(l,0x3C94,2u);put(l,0x3C98,1u);put(l,0x3F8,12u);put(l,0x428,r);
        strcpy_s(reinterpret_cast<char*>(l+0x368),32,"Root");
        put(r,0,base+0x10CEB48);put(r,8,r);put(r,0x3C,12u);put(r,0x18,quarterLeaf(0));
        strcpy_s(reinterpret_cast<char*>(r+0x40),32,"Root");put(r,0x98,std::array<float,2>{{5,5}});
        put(r,0x60,std::array<float,2>{{643.748168945f,33.9970703125f}});put(r,0x70,std::array<float,2>{{643.748168945f,33.9970703125f}});
        put(r,0xA0,original.scale);put(r,0xA8,original.scale);put(r,0xB8,std::array<float,2>{{1,1}});put(r,0xC9,static_cast<unsigned char>(1));
        const char* names[]={"BG","BG2","White"};
        for(unsigned i=0;i<3;++i) {
            const auto object=quarterLeaf(i);put(object,0,base+0x10CEBA8);put(object,8,object);put(object,0x10,r);
            put(object,0x20,i ? quarterLeaf(i-1) : std::uintptr_t{0});put(object,0x28,i<2 ? quarterLeaf(i+1) : std::uintptr_t{0});
            put(object,0x3C,12u);strcpy_s(reinterpret_cast<char*>(object+0x40),32,names[i]);
            put(object,0x60,std::array<float,2>{{0,i==2 ? -100.0f : 88.0f}});
            put(object,0x98,std::array<float,2>{{800,i==2 ? 1480.0f : 969.0f}});put(object,0xA0,std::array<float,2>{{1,1}});
            put(object,0xB8,std::array<float,2>{{1,1}});put(object,0xC9,static_cast<unsigned char>(i ? 1 : 0));
            put(object,0x110,i==1 ? src+0x800 : src);
        }
        const auto o=quarterOriginal();put(quarterLeaf(2),0x70,o.position);put(quarterLeaf(2),0xA8,o.scale);put(quarterLeaf(2),0x30,rec);
        put(src,0,base+0x10CC0D8);put(src,0x1A8,base+0x45000);put(src,0x308,mat);put(mat,0,base+0x706570);put(mat,0x50,tex);
        put(tex,0,std::uint64_t{0x5553455200061280ULL});put(tex,0x18,std::array<std::uint16_t,2>{{2048,2048}});
        put(rec,0,base+0x10CEC08);put(rec,0x40,src);put(rec,8,0xFFFFFFFFu);put(rec,0x58,whiteUv);
    }
    ~Scene() { VirtualFree(allocation, 0, MEM_RELEASE); current = {}; base = 0; }
    void addArtwork() {
        addQuarter();
        const auto src=base+0xD0800,mat=base+0xD4000,tex=base+0xD5000,rec=base+0xD6000;
        put(quarterLeaf(1),0x70,std::array<float,2>{{643.748168945f,33.9970703125f+88*original.scale[1]}});
        put(quarterLeaf(1),0xA8,original.scale);put(quarterLeaf(1),0x30,rec);
        put(src,0,base+0x10CC0D8);put(src,0x1A8,base+0x45000);put(src,0x308,mat);
        put(mat,0,base+0x706570);put(mat,0x50,tex);
        put(tex,0,std::uint64_t{0x5553455200061300ULL});put(tex,0x18,std::array<std::uint16_t,2>{{2048,2048}});
        put(rec,0,base+0x10CEC08);put(rec,0x40,src);put(rec,8,0xFFFFFFFFu);put(rec,0x58,normalArtworkUv);
    }
    void observe() { updateObserved(reinterpret_cast<void*>(credit), base + 0xB07DC); }
    NormalStartBackingAlignment begin() { return beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(leaf), base + 0x1E634D); }
    RearIlluminationPose pose() const {
        RearIlluminationPose p; std::memcpy(p.position.data(), reinterpret_cast<void*>(leaf + 0x70), 8);
        std::memcpy(p.scale.data(), reinterpret_cast<void*>(leaf + 0xA8), 8); return p;
    }
    bool isOriginal() const { const auto p = pose(); return p.position == original.position && p.scale == original.scale; }
};

int main() {
    unsigned cases = 0;
    try {
        for (unsigned phase : {0u,1u,3u,4u,5u}) for (float fade : {0.0f,.5f,1.0f}) {
            Scene s;s.addArtwork();put(s.credit,0x14,phase);
            for (auto object : {s.quarterRoot(),s.quarterLeaf(1),s.quarterLeaf(2)}) put(object,0xBC,fade);
            put(base+0xD3000,8,(static_cast<unsigned>(fade*255)<<24)|0xFFFFFFu);
            put(base+0xD6000,8,(static_cast<unsigned>(fade*255)<<24)|0xFFFFFFu);
            s.observe();CHECK(current.artworkValid && current.quarterValid);
            for (unsigned role : {1u,2u}) {
                std::array<unsigned char,0x130> before{},after{};
                std::memcpy(before.data(),reinterpret_cast<void*>(s.quarterLeaf(role)),before.size());
                auto token=beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(s.quarterLeaf(role)),base+0x1E634D);
                CHECK(token.attempted && token.quarterRole==role);
                std::memcpy(after.data(),reinterpret_cast<void*>(s.quarterLeaf(role)),after.size());
                CHECK(std::fabs(field<float>(after,0x70)-.1f-readNativeHudGeometry().left)<.001f);
                CHECK(std::fabs(field<float>(after,0xA8)*800-readNativeHudGeometry().width)<.001f);
                for(unsigned i=0;i<before.size();++i) if(!((i>=0x70&&i<0x74)||(i>=0xA8&&i<0xAC))) CHECK(before[i]==after[i]);
                restoreNativeNormalStartBackingCommit(token);
                CHECK(!failed && !std::memcmp(before.data(),reinterpret_cast<void*>(s.quarterLeaf(role)),before.size()));
            }
            ++cases;
        }
        for (float fade : {0.0f,.25f,.5f,1.0f}) {
            Scene s; put(s.layout,0x58,fade); put(s.leaf,0xBC,fade);
            put(s.root,0xB8,std::array<float,2>{{fade,fade}});
            GuiBytes rootBefore {}; std::memcpy(rootBefore.data(),reinterpret_cast<void*>(s.root),rootBefore.size());
            put(s.record,8,(static_cast<unsigned>(fade*255)<<24)|0xFFFFFFu);
            s.observe(); CHECK(current.valid);
            const auto token=s.begin(); CHECK(token.attempted);
            float retained=0; std::memcpy(&retained,reinterpret_cast<void*>(s.leaf+0xBC),4); CHECK(retained==fade);
            CHECK(!std::memcmp(rootBefore.data(),reinterpret_cast<void*>(s.root),rootBefore.size()));
            restoreNativeNormalStartBackingCommit(token); CHECK(!failed && s.isOriginal()); ++cases;
        }
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
          restoreNativeNormalStartBackingCommit(token); CHECK(s.isOriginal() && !failed && !committing && restored == 1); ++cases; }
        { Scene s; for (unsigned scene : {1u,2u,3u,5u}) { put(s.manager,0x38,scene); s.observe(); CHECK(!current.valid); }
          put(s.manager,0x38,4u); for (unsigned phase : {0u,1u,7u,9u}) { put(s.parent,0x14,phase); s.observe(); CHECK(!current.valid); }
          put(s.parent,0x14,8u); for (unsigned phase : {2u,6u,7u,99u}) { put(s.credit,0x14,phase); s.observe(); CHECK(!current.valid); }
          CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.credit,0,base+0x10CA7E0); CHECK(!s.begin().attempted);
          put(s.credit,0,base+0x10CA7A8); put(s.parent,0,base+0x10C7278); CHECK(!s.begin().attempted);
          put(s.parent,0,base+0x10C6FA0); put(s.parent,0x48,s.credit+8); CHECK(!s.begin().attempted && !fixture::queries); ++cases; }
        { Scene s; s.observe(); put(s.manager,0x3C,1); CHECK(!s.begin().attempted); put(s.manager,0x3C,-1);
          for (auto object : {s.parent,s.credit}) for (unsigned offset : {8u,9u}) {
              put(object,offset,static_cast<unsigned char>(1)); CHECK(!s.begin().attempted); put(object,offset,static_cast<unsigned char>(0)); }
          CHECK(!fixture::queries); ++cases; }
        { Scene s; s.observe(); for (auto object : {s.frontLeaf(),s.mask(0),s.mask(1)}) CHECK(!beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(object),base+0x1E634D).attempted);
          CHECK(!beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(s.leaf),base+0x1E634E).attempted && !fixture::queries); ++cases; }
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
        { Scene s; s.observe(); auto token=s.begin(); CHECK(token.attempted); restoreNativeNormalStartBackingCommit(token);
          fixture::now+=50; s.observe(); token=s.begin(); CHECK(token.attempted && fixture::queries==1 && current.access.observed==1000); restoreNativeNormalStartBackingCommit(token);
          ++fixture::now; s.observe(); token=s.begin(); CHECK(token.attempted && fixture::queries==2 && current.access.observed==1051); restoreNativeNormalStartBackingCommit(token); ++cases; }
        { Scene s; s.observe(); fixture::now+=90; fixture::queryDelay=20; CHECK(!s.begin().attempted && fixture::queries==1 && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::invalidateHudOnQuery=true; CHECK(!s.begin().attempted && s.isOriginal()); ++cases; }
        { Scene s; s.observe(); fixture::mutationAddress=s.credit+0x14; fixture::mutationValue=6;
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
          put(s.parent,0,base+0x10C7278); put(s.parent,0x48,std::uintptr_t{0}); put(s.credit,0,base+0xA5710);
          put(s.config,0x1373,static_cast<unsigned char>(0)); put(s.frame,0x28,std::array<std::uint16_t,2>{{800,1366}});
          put(s.layout,0x18,3u); put(s.layout,0x58,.3f); put(s.leaf,0xBC,.4f); put(s.record,8,0x22FFFFFFu);
          put(s.leaf,0x74,27.0f); put(s.leaf,0xAC,.61f); put(base,0x13DB5A0,base+0x45008);
          restoreNativeNormalStartBackingCommit(token); const auto p=s.pose();
          CHECK(p.position[0]==s.original.position[0] && p.scale[0]==s.original.scale[0] && p.position[1]==27 && p.scale[1]==.61f && !failed && !committing); ++cases; }
        { Scene s; s.observe(); DWORD old=0; CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READONLY,&old));
          CHECK(!s.begin().attempted && s.isOriginal()); CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; s.observe(); fixture::readonlyOnQuery=reinterpret_cast<void*>(base+0x30000);
          CHECK(!s.begin().attempted && failed && !committing && s.isOriginal()); DWORD old=0;
          CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x30000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; s.observe(); const auto token=s.begin(); CHECK(token.attempted); put(s.resident,0x128,s.layout+8);
          restoreNativeNormalStartBackingCommit(token); CHECK(failed && !committing); put(s.resident,0x128,s.layout); CHECK(writeHorizontal(token.permission,token.original)); ++cases; }
        { Scene s; s.observe(); updateObserved(reinterpret_cast<void*>(s.credit),base+0xB07DD);
          CHECK(!current.valid && !updating && fixture::nativeCalls==2 && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); fixture::raiseUpdate=true; CHECK(exceptionalUpdate(reinterpret_cast<void*>(s.credit)));
          CHECK(!current.valid && !updating && fixture::nativeCalls==2 && !s.begin().attempted); ++cases; }
        { Scene s; s.observe(); fixture::recurseUpdate=true; s.observe();
          CHECK(!current.valid && !updating && fixture::nativeCalls==3 && !s.begin().attempted); ++cases; }
        { Scene s; const auto newRoot=base+0x24F30; std::memcpy(reinterpret_cast<void*>(newRoot),reinterpret_cast<void*>(s.root),0xD0);
          s.root=newRoot; put(s.root,8,s.root); put(s.layout,0x428,s.root); put(s.leaf,0x10,s.root);
          DWORD old=0; CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x25000),0x1000,PAGE_NOACCESS,&old));
          s.observe(); const auto token=s.begin(); CHECK(token.attempted); restoreNativeNormalStartBackingCommit(token); CHECK(s.isOriginal() && !failed);
          CHECK(VirtualProtect(reinterpret_cast<void*>(base+0x25000),0x1000,PAGE_READWRITE,&old)); ++cases; }
        { Scene s; const auto h=readNativeHudGeometry(); RearIlluminationPose after {{{9,9}},{{9,9}}}; auto before=s.original;
          before.scale[0]=.1f; CHECK(!fitNormalStartWhiteHorizontal(h.left,h.top,h.width,h.height,before,after));
          before=s.original; before.position[1]=NAN; CHECK(!fitNormalStartWhiteHorizontal(h.left,h.top,h.width,h.height,before,after));
          CHECK(after.position[0]==9 && after.scale[1]==9); ++cases; }
        { Scene s; s.addQuarter(); s.observe(); CHECK(current.valid && current.quarterValid && !fixture::queries);
          std::array<unsigned char,0x130> white {},bg {},bg2 {};std::array<unsigned char,0x78> record {};
          std::memcpy(white.data(),reinterpret_cast<void*>(s.quarterLeaf(2)),white.size());
          std::memcpy(bg.data(),reinterpret_cast<void*>(s.quarterLeaf(0)),bg.size());std::memcpy(bg2.data(),reinterpret_cast<void*>(s.quarterLeaf(1)),bg2.size());
          std::memcpy(record.data(),reinterpret_cast<void*>(base+0xD3000),record.size());
          const auto token=s.beginQuarter();CHECK(token.attempted && token.quarter && fixture::queries==1 && quarterApplied==1);
          const auto h=readNativeHudGeometry(); const auto p=s.quarterPoseValue();
          CHECK(std::fabs(p.position[0]-.1f-h.left)<.001 && std::fabs(p.scale[0]*800-h.width)<.001);
          const auto* bytes=reinterpret_cast<const unsigned char*>(s.quarterLeaf(2));
          for(unsigned i=0;i<white.size();++i) if(!((i>=0x70 && i<0x74)||(i>=0xA8 && i<0xAC))) CHECK(bytes[i]==white[i]);
          CHECK(!std::memcmp(bg.data(),reinterpret_cast<void*>(s.quarterLeaf(0)),bg.size()));CHECK(!std::memcmp(bg2.data(),reinterpret_cast<void*>(s.quarterLeaf(1)),bg2.size()));
          CHECK(!std::memcmp(record.data(),reinterpret_cast<void*>(base+0xD3000),record.size()));
          restoreNativeNormalStartBackingCommit(token);CHECK(s.quarterOriginalExactly() && s.isOriginal() && !failed && !committing && quarterRestored==1);++cases; }
        { Scene s; s.addQuarter();put(s.quarterLeaf(2),0x40,std::uint32_t{0});s.observe();CHECK(current.valid && !current.quarterValid);
          const auto t=s.begin();CHECK(t.attempted && !t.quarter);restoreNativeNormalStartBackingCommit(t);CHECK(s.isOriginal() && !s.beginQuarter().attempted);++cases; }
        { Scene s;s.addQuarter();s.observe();for(unsigned i=0;i<2;++i)CHECK(!beginNativeNormalStartBackingCommit(reinterpret_cast<void*>(s.quarterLeaf(i)),base+0x1E634D).attempted);
          CHECK(!fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();for(unsigned phase : {2u,6u,99u}) {put(s.credit,0x14,phase);CHECK(!s.beginQuarter().attempted);}
          CHECK(!fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();put(s.quarterLeaf(1),0x28,std::uintptr_t{0});CHECK(!s.beginQuarter().attempted);put(s.quarterLeaf(1),0x28,s.quarterLeaf(2));
          put(s.quarterLeaf(2),0x28,s.quarterLeaf(0));CHECK(!s.beginQuarter().attempted);put(s.quarterLeaf(2),0x28,std::uintptr_t{0});
          put(s.quarterLeaf(1),0x20,std::uintptr_t{0});CHECK(!s.beginQuarter().attempted && !fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();for(auto address : {s.quarterRoot(),s.quarterLeaf(2)}) {put(address,0xBC,NAN);CHECK(!s.beginQuarter().attempted);put(address,0xBC,1.0f);}
          put(base+0xD3000,8,0x7FFFFF00u);CHECK(!s.beginQuarter().attempted);put(base+0xD3000,8,0xFFFFFFFFu);
          put(base+0xD3000,0x58,.5f);CHECK(!s.beginQuarter().attempted && !fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();put(base+0xD2000,0,std::uint64_t{0x5553455200052B00ULL});CHECK(!s.beginQuarter().attempted);
          put(base+0xD2000,0,std::uint64_t{0x5553455200061280ULL});put(base+0xD3000,0x40,base+0xD0008);CHECK(!s.beginQuarter().attempted);
          put(base+0xD3000,0x40,base+0xD0000);put(base+0xD0000,0x1A8,base+0x96000);CHECK(!s.beginQuarter().attempted && !fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();put(s.quarterRoot(),0xB0,1.0f);CHECK(!s.beginQuarter().attempted);put(s.quarterRoot(),0xB0,0.0f);
          put(s.quarterLeaf(2),0x90,1.0f);CHECK(!s.beginQuarter().attempted);put(s.quarterLeaf(2),0x90,0.0f);
          put(s.quarterLeaf(2),0x70,620.0f);CHECK(!s.beginQuarter().attempted && !fixture::queries);++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::mutationAddress=s.credit+0x14;fixture::mutationValue=6;fixture::mutateOnQuery=&fixture::mutateWord;
          CHECK(!s.beginQuarter().attempted && fixture::queries==1 && s.quarterOriginalExactly());++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::invalidateHudOnQuery=true;CHECK(!s.beginQuarter().attempted && s.quarterOriginalExactly());++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::now+=90;fixture::queryDelay=20;CHECK(!s.beginQuarter().attempted && s.quarterOriginalExactly());++cases; }
        { Scene s;s.addQuarter();s.observe();auto t=s.beginQuarter();CHECK(t.attempted);restoreNativeNormalStartBackingCommit(t);
          fixture::now+=50;s.observe();t=s.beginQuarter();CHECK(t.attempted && fixture::queries==1 && current.quarterAccess.observed==1000);restoreNativeNormalStartBackingCommit(t);
          ++fixture::now;s.observe();t=s.beginQuarter();CHECK(t.attempted && fixture::queries==2);restoreNativeNormalStartBackingCommit(t);++cases; }
        { Scene s;s.addQuarter();s.observe();const auto t=s.beginQuarter();CHECK(t.attempted);
          current={};fixture::now+=1000;fixture::hudReady=false;put(base,0x13DD000,std::uintptr_t{0});
          put(s.parent,0x10,2u);put(s.parent,0x14,9u);put(s.credit,0x14,4u);put(s.credit,8,static_cast<unsigned char>(1));
          put(s.config,0x1373,static_cast<unsigned char>(0));put(s.frame,0x28,std::array<std::uint16_t,2>{{800,1366}});
          put(s.quarterLayout(),0x18,3u);put(s.quarterLayout(),0x58,NAN);put(s.quarterLeaf(2),0xBC,.3f);put(s.quarterRoot(),0xC9,static_cast<unsigned char>(0));
          put(base+0xD3000,8,0x12FFFFFFu);put(base+0xD3000,0x58,.2f);put(base,0x13DB5A0,base+0x45008);
          put(s.quarterLeaf(2),0x74,29.0f);put(s.quarterLeaf(2),0xAC,.62f);
          restoreNativeNormalStartBackingCommit(t);const auto p=s.quarterPoseValue(),o=s.quarterOriginal();
          CHECK(p.position[0]==o.position[0] && p.scale[0]==o.scale[0] && p.position[1]==29 && p.scale[1]==.62f && !failed && !committing);++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::readonlyOnQuery=reinterpret_cast<void*>(base+0xC0000);
          CHECK(!s.beginQuarter().attempted && failed && !committing && s.quarterOriginalExactly());DWORD old=0;
          CHECK(VirtualProtect(reinterpret_cast<void*>(base+0xC0000),0x1000,PAGE_READWRITE,&old));++cases; }
        { Scene s;s.addQuarter();s.observe();const auto t=s.beginQuarter();CHECK(t.attempted);put(s.credit,0x20,s.quarterLayout()+8);
          restoreNativeNormalStartBackingCommit(t);CHECK(failed && !committing);put(s.credit,0x20,s.quarterLayout());CHECK(writeHorizontal(t.permission,t.original));++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::raiseUpdate=true;CHECK(exceptionalUpdate(reinterpret_cast<void*>(s.credit)));
          CHECK(!current.valid && !current.quarterValid && !s.beginQuarter().attempted && !updating);++cases; }
        { Scene s;s.addQuarter();s.observe();fixture::recurseUpdate=true;s.observe();CHECK(!current.valid && !current.quarterValid && !s.beginQuarter().attempted);++cases; }
        std::cout << "Passed " << cases << " Normal Start backing groups\n";
    } catch (const std::exception& error) { std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1; }
}

