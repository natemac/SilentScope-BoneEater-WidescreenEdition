#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
namespace fixture {
ULONGLONG now = 1000;
unsigned reads = 0, calls = 0;
bool nativeThrow = false, nested = false, inNested = false, failWrite = false;
void* expectedOwner = nullptr;
float expectedDelta = 0;
ULONGLONG clock() noexcept { return now; }
BOOL read(HANDLE p, LPCVOID a, LPVOID b, SIZE_T n, SIZE_T* done) noexcept {
    ++reads; return ReadProcessMemory(p,a,b,n,done);
}
size_t write(const void* b, size_t s, size_t n, FILE* f) noexcept {
    return failWrite ? 0 : fwrite(b,s,n,f);
}
}
#define GetTickCount64 fixture::clock
#define ReadProcessMemory fixture::read
#define fwrite fixture::write
#include "../../src/input/native_input_ownership.cpp"
#undef fwrite
#undef ReadProcessMemory
#undef GetTickCount64
#include "precision_stubs.h"

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
}
namespace bone_eater::input {
SelectedHidBridgeStatus readSelectedHidBridgeStatus() noexcept {
    SelectedHidBridgeStatus s;
    s.configured = s.selected = true; s.cabinetGeneration = 44;
    s.runtime.emplace(); s.runtime->input.output.state = GunSourceState::Unfocused;
    s.runtime->input.output.armed = s.runtime->input.output.sourceUsable = false;
    s.aim.valid = true; s.aim.generation = 50; s.aim.sourceIdentity.session = 2;
    s.aim.sourceIdentity.generation = 11;
    return s;
}
}
using namespace bone_eater::input;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
template<class T> void put(std::uintptr_t p, size_t off, T v) {
    memcpy(reinterpret_cast<void*>(p+off),&v,sizeof(v));
}
constexpr DWORD kException = 0xE1234567;
void __fastcall nativeScope(void* owner, float delta) {
    ++fixture::calls;
    CHECK(owner == fixture::expectedOwner && delta == fixture::expectedDelta);
    if (fixture::nested && !fixture::inNested) {
        fixture::inNested = true; invokeScope(owner,delta,base+0xAA923); fixture::inNested = false;
    }
    std::uintptr_t input = 0, wrapper = 0, camera = 0;
    read(base,0x13DCED0,input); read(base,0x13DB588,wrapper); read(wrapper,8,camera);
    put(input,0x0C,OwnershipPoint{{905.0f,478.0f}});
    observeNativeInputOwnershipRay(camera,905,478,true,true,false);
    if (fixture::nativeThrow) {
        ULONG_PTR payload = 0x1234; RaiseException(kException, EXCEPTION_NONCONTINUABLE, 1, &payload);
    }
}
int filter(EXCEPTION_POINTERS* p) {
    if (p->ExceptionRecord->ExceptionCode != kException) return EXCEPTION_CONTINUE_SEARCH;
    return p->ExceptionRecord->NumberParameters == 1 &&
        p->ExceptionRecord->ExceptionInformation[0] == 0x1234 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
bool invokeException(void* owner) {
    __try { invokeScope(owner,fixture::expectedDelta,base+0xAA923); }
    __except(filter(GetExceptionInformation())) { return true; }
    return false;
}
struct Scene {
    std::uintptr_t owner = 0, input = 0;
    Scene() {
        base = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr,0x1400000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        CHECK(base); owner=base+0x1000; input=base+0x4000;
        auto scopeCamera=base+0x6000,main=base+0xA000,wrapper=base+0xD000;
        auto config=base+0x10000,manager=base+0x13000,holder=base+0x15000,ui=base+0x16000;
        put(base,0x13DCF88,owner); put(owner,0,base+0x10C7350);
        put(owner,8,scopeCamera); put(owner,0x60,scopeCamera); put(scopeCamera,0,base+0x6FDF48);
        put(base,0x13DCED0,input); put(base,0x13DCF58,config); put(config,0,base+0x10C5FA8);
        put(config,0x136A,std::uint8_t{1}); put(config,0x12D8,OwnershipPoint{{.5f,.5f}});
        put(base,0x13DB588,wrapper); put(wrapper,8,main); put(main,0,base+0x6FDF48);
        put(main,0xEB4,OwnershipPoint{{1920,1080}}); put(main,0xE8E,std::uint8_t{0});
        put(base,0x12E1930,manager); put(manager,0x340,main);
        put(base,0x13DCF48,holder); put(holder,0,ui); put(ui,0,base+0x10CA5E0);
        put(owner,0x174,.2f); put(owner,0x1C2,std::uint8_t{1});
        fixture::expectedOwner=reinterpret_cast<void*>(owner); fixture::expectedDelta=.016f;
        shared=new Shared; shared->enabled=true; CHECK(tmpfile_s(&shared->file)==0);
        originalScope=&nativeScope; reset();
    }
    ~Scene() {
        if(shared->file) fclose(shared->file);
        delete shared; shared=nullptr; VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE); base=0;
    }
    void reset() {
        ownership={}; sourceContext={}; activeScope=nullptr; scopeDepth=0;
        fixture::calls=0; fixture::nativeThrow=fixture::nested=fixture::inNested=fixture::failWrite=false;
        shared->nextSample=0; shared->enabled=true; fixture::now+=1000;
        put(input,0x0C,OwnershipPoint{{915,478}});
        const auto id=beginNativeInputOwnership(input,true);
        convertedNativeInputOwnership(id,{{915,478}},true);
        finishNativeInputOwnership(id,true,true,{{915,478}});
    }
};
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out; std::istringstream stream(line); std::string part;
    while(std::getline(stream,part,',')) out.push_back(part); return out;
}
void beginSample(Observation& row,std::uintptr_t owner) {
    // The shared Scope boundary now owns counting, before either consumer.
    ownership.scopeInvocation(); beginScope(row,owner,.016f,base+0xAA923,false);
}
int main(int argc,char**) {
    if(argc>1) {
        SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",L"1");
        SetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT_OWNERSHIP_OBSERVE",L"1");
        installNativeInputOwnershipObserver(reinterpret_cast<void*>(1));
        CHECK(!shared && !attempted && fixture::reads==0);
        CHECK(!beginNativeInputOwnership(1,true));
        observeNativeInputOwnershipRay(1,2,3,true,true,false);
        CHECK(fixture::reads==0); std::cout<<"quiet no-install/no-read passed\n"; return 0;
    }
    unsigned groups=0;
    CHECK(beginNativeInputOwnership(1,true)==0 && fixture::reads==0); ++groups;
    Scene scene;
    ScopeState snap; CHECK(repeated(scene.owner,snap) && snap.point==OwnershipPoint({915,478})); ++groups;
    Observation row {}; beginSample(row,scene.owner);
    CHECK(row.sampled && row.beforeValid && row.match==OwnershipMatch::Completed);
    CHECK(row.source.selected && row.source.runtime && !row.source.usable && !row.source.armed &&
        row.source.state==static_cast<int>(GunSourceState::Unfocused)); ++groups;
    activeScope=&row; nativeScope(reinterpret_cast<void*>(scene.owner),.016f); activeScope=nullptr;
    finishScope(row,true);
    CHECK(row.rayCalls==1 && row.rayVerified && row.rayPoint==OwnershipPoint({905,478}) &&
        row.rayInputRead && row.rayInput==row.rayPoint && row.afterValid);
    CHECK(row.serialAtRay==row.input.serial && std::string(rowStatus(row))=="completed_same_update");
    auto encoded=formatRow(row); CHECK(encoded.valid);
    CHECK(fields(kHeader).size()==75 && fields(encoded.bytes).size()==fields(kHeader).size()); ++groups;
    scene.reset(); invokeScope(reinterpret_cast<void*>(scene.owner),.016f,base+0xAA923);
    CHECK(fixture::calls==1 && !activeScope && !scopeDepth); ++groups;
    scene.reset(); fixture::nativeThrow=true;
    CHECK(invokeException(reinterpret_cast<void*>(scene.owner)) && fixture::calls==1 && !activeScope && !scopeDepth); ++groups;
    scene.reset(); fixture::nested=true;
    invokeScope(reinterpret_cast<void*>(scene.owner),.016f,base+0xAA923);
    CHECK(fixture::calls==2 && ownership.scopeCalls==2 && !activeScope && !scopeDepth); ++groups;
    scene.reset(); Observation first{}, second{};
    beginSample(first,scene.owner);
    beginSample(second,scene.owner);
    CHECK(first.sampled && !second.sampled && ownership.scopeCalls==2); ++groups;
    scene.reset(); put(scene.owner,0,std::uintptr_t{0});
    invokeScope(reinterpret_cast<void*>(scene.owner),.016f,base+0xAA923);
    CHECK(fixture::calls==1 && !activeScope); put(scene.owner,0,base+0x10C7350); ++groups;
    scene.reset(); Observation changed{}; beginSample(changed,scene.owner);
    changed.returned=true; changed.afterValid=true; changed.after=changed.before;
    beginNativeInputOwnership(scene.input,true);
    CHECK(std::string(rowStatus(changed))=="update_changed_during_scope"); ++groups;
    scene.reset(); fixture::failWrite=true;
    invokeScope(reinterpret_cast<void*>(scene.owner),.016f,base+0xAA923);
    CHECK(!shared->enabled && !shared->file && fixture::calls==1);
    scopeObserved(reinterpret_cast<void*>(scene.owner),.016f); CHECK(fixture::calls==2); ++groups;
    // Functional certificates survive optional output failure/quiet; source
    // diagnostics are no longer sampled once their own sink is disabled.
    precision_fixture::ready=true;
    shared->enabled=false; fixture::failWrite=false;
    const auto ticket=beginNativeInputOwnership(scene.input,true); CHECK(ticket);
    convertedNativeInputOwnership(ticket,{{915,478}},true);
    finishNativeInputOwnership(ticket,true,true,{{915,478}});
    CHECK(ownership.completed && !shared->file);
    put(scene.input,0x0C,OwnershipPoint{{915,478}});
    const auto enters=precision_fixture::enters,leaves=precision_fixture::leaves;
    invokeScope(reinterpret_cast<void*>(scene.owner),.016f,base+0xAA923);
    CHECK(precision_fixture::enters==enters+1 && precision_fixture::leaves==leaves+1 && ownership.scopeCalls==1); ++groups;
    fixture::nativeThrow=true;
    CHECK(invokeException(reinterpret_cast<void*>(scene.owner)) && !scopeDepth && !activeScope);
    CHECK(precision_fixture::enters==enters+2 && precision_fixture::leaves==leaves+2); ++groups;
    precision_fixture::ready=false;
    std::cout<<groups<<" native ownership groups passed\n";
}
