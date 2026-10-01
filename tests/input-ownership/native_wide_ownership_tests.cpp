#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include "../../src/input/native_wide_input.cpp"
#include "precision_stubs.h"
namespace fixture {
bone_eater::input::InputOwnershipState ledger;
bool diagnosticEnabled = true, throwNative = false, recurse = false, nested = false;
unsigned calls = 0;
bone_eater::input::OwnershipPoint converted {};
}
namespace bone_eater::input {
void installNativeInputOwnershipObserver(void*) noexcept {}
bool scopeControlNativeReady() noexcept { return false; }
bool scopeStartRoutingReady() noexcept { return false; }
std::uint64_t beginNativeInputOwnership(std::uintptr_t p,bool allowed) noexcept {
    return fixture::diagnosticEnabled ? fixture::ledger.begin(p,allowed) : 0;
}
void convertedNativeInputOwnership(std::uint64_t t,const OwnershipPoint& p,bool ok) noexcept {
    fixture::ledger.conversion(t,p,ok); fixture::converted=p;
}
void finishNativeInputOwnership(std::uint64_t t,bool returned,bool valid,const OwnershipPoint& p) noexcept {
    fixture::ledger.finish(t,returned,valid,p,1000);
}
}
namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h; h.valid=true; h.mainWidth=1920; h.mainHeight=1080;
    h.width=768.0f*1080/1366; h.height=1080; h.left=(1920-h.width)/2;
    h.sourceWidth=768; h.sourceHeight=1366; h.helper=1; h.sprite=2; return h;
}
}
using namespace bone_eater::input;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
template<class T> void put(std::uintptr_t p,size_t off,T x) { memcpy(reinterpret_cast<void*>(p+off),&x,sizeof(x)); }
constexpr DWORD kCode=0xE1245678;
void __fastcall nativeUpdate(void* raw,float delta) {
    ++fixture::calls; CHECK(delta==.02f);
    if(fixture::recurse && !fixture::nested) {
        fixture::nested=true;
        const auto ticket=beginNativeInputOwnership(reinterpret_cast<std::uintptr_t>(raw),false);
        invokeUpdate(raw,delta,nullptr,ticket);
        finishNativeInputOwnership(ticket,true,false,{}); fixture::nested=false;
    }
    std::array<std::uint8_t,52> original{};
    const float x=32+736*(915.0f/1920),y=132+896*(478.0f/1080);
    const std::int32_t rawX=2000,rawY=2000;
    memcpy(original.data()+0x24,&x,4); memcpy(original.data()+0x28,&y,4);
    memcpy(original.data()+0x2C,&rawX,4); memcpy(original.data()+0x30,&rawY,4);
    auto result=original;
    convertNativeWideInput(result.data(),original,800,1280);
    if(!currentUpdate) CHECK(result==original); // Excluded/nested calls cannot borrow outer context.
    OwnershipPoint p{}; memcpy(p.data(),result.data()+0x24,8);
    put(reinterpret_cast<std::uintptr_t>(raw),0x0C,p);
    put(reinterpret_cast<std::uintptr_t>(raw),0x24,p);
    if(fixture::throwNative) RaiseException(kCode,EXCEPTION_NONCONTINUABLE,0,nullptr);
}
bool catchNative(void* raw,UpdateContext* c) {
    __try { invokeUpdate(raw,.02f,c,c->ownershipTicket); }
    __except(GetExceptionCode()==kCode ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
int main() {
    CHECK(SetEnvironmentVariableA("BONE_EATER_SCOPE_MODE", "toggle_hold"));
    CHECK(SetEnvironmentVariableA("BONE_EATER_SCOPE_HOLD_MS", "250"));
    initializeScopeControlFromEnvironment();
    auto allocation=VirtualAlloc(nullptr,0x1400000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(allocation);
    moduleBase=reinterpret_cast<std::uintptr_t>(allocation); arkBase=moduleBase+0x800000;
    const auto input=moduleBase+0x2000,config=moduleBase+0x10000,io=arkBase+0x10000;
    put(moduleBase,0x13DCED0,input); put(moduleBase,0x13DCF58,config); put(config,0x136A,std::uint8_t{1});
    put(arkBase,0x18BD38,io); put(io,0,arkBase+0xCFCB8);
    put(arkBase+0xCFCB8,0x360,arkBase+0xA440); put(io,0x78,io+0x408); put(io,0x408,arkBase+0xD04E0);
    shared=new Shared; shared->enabled=true; shared->mapping=WideInputMapping::Desktop;
    originalUpdate=&nativeUpdate;
    auto context=[&] {
        UpdateContext c; c.input=input; c.config=config; c.supported=true;
        c.geometry=bone_eater::render::readNativeHudGeometry();
        c.ownershipTicket=beginNativeInputOwnership(input,true); return c;
    };
    unsigned groups=0;
    auto c=context(); invokeUpdate(reinterpret_cast<void*>(input),.02f,&c,c.ownershipTicket);
    CHECK(fixture::calls==1 && !currentUpdate && fixture::ledger.convertedOk);
    finishUiPair(c); finishOwnership(c);
    CHECK(fixture::ledger.completed && sameOwnershipPoint(fixture::ledger.converted,fixture::converted));
    OwnershipPoint ui{}; read(input,0x24,ui);
    const auto mapped=wideToHud(fixture::converted[0],fixture::converted[1],c.geometry);
    CHECK(sameOwnershipPoint(ui,{{mapped.x,mapped.y}})); ++groups;
    c=context(); invokeUpdate(reinterpret_cast<void*>(input),.02f,&c,c.ownershipTicket);
    put(input,0x0C,OwnershipPoint{{1,2}}); finishOwnership(c); CHECK(!fixture::ledger.completed); ++groups;
    c=context(); fixture::throwNative=true; UpdateContext previous; currentUpdate=&previous;
    CHECK(catchNative(reinterpret_cast<void*>(input),&c));
    CHECK(currentUpdate==&previous && !fixture::ledger.completed && !fixture::ledger.updating);
    currentUpdate=nullptr; fixture::throwNative=false; ++groups;
    c=context(); fixture::recurse=true; const auto oldCalls=fixture::calls;
    invokeUpdate(reinterpret_cast<void*>(input),.02f,&c,c.ownershipTicket); finishOwnership(c);
    CHECK(fixture::calls==oldCalls+2 && !currentUpdate && !fixture::ledger.completed && !fixture::ledger.updating);
    fixture::recurse=false; ++groups;
    c=context(); invokeUpdate(reinterpret_cast<void*>(input),.02f,&c,c.ownershipTicket); finishOwnership(c);
    CHECK(fixture::ledger.completed); ++groups;
    fixture::diagnosticEnabled=false; c=context(); CHECK(!c.ownershipTicket);
    invokeUpdate(reinterpret_cast<void*>(input),.02f,&c,c.ownershipTicket); finishUiPair(c); finishOwnership(c);
    OwnershipPoint main{}; read(input,0x0C,main); read(input,0x24,ui);
    CHECK(main[0]>914 && main[0]<916 && ui[0]!=main[0]); ++groups;
    // Exercise source-reset handling through the real ARK conversion adapter.
    // Native zoom stays unavailable in this fixture; no HWND/focus is invented.
    auto latch = [&] {
        resetScopeControl();
        CHECK(publishScopeControl({1000, 1, false, true}).armed);
        CHECK(publishScopeControl({1010, 1, true, true}).enabled);
        CHECK(publishScopeControl({1020, 1, false, true}).enabled);
    };
    auto convert = [&] {
        auto current = context();
        invokeUpdate(reinterpret_cast<void*>(input), .02f, &current, current.ownershipTicket);
    };
    auto selected = [&](std::uint64_t session, std::uint64_t generation) {
        GunSourceOutput sample;
        sample.mode = sample.aim.source = GunSourceMode::SelectedHid;
        sample.state = GunSourceState::Active;
        sample.aim.known = sample.armed = sample.sourceUsable = true;
        const auto now = GunSourceClock::now();
        sample.aim.identity = {session, generation, now};
        publishSelectedAimSample(sample, std::chrono::milliseconds(250), now, generation);
    };
    latch(); selected(3, 10); convert();
    CHECK(!readScopeControlSnapshot().enabled && shared->lastSourceMode == GunSourceMode::SelectedHid);
    latch(); selected(3, 11); convert();
    CHECK(readScopeControlSnapshot().enabled && shared->lastSourceGeneration == 11);
    selected(3, 11); convert(); CHECK(readScopeControlSnapshot().enabled);
    selected(3, 9); convert(); CHECK(!readScopeControlSnapshot().enabled);
    latch(); selected(4, 10); convert(); CHECK(!readScopeControlSnapshot().enabled);
    latch(); publishAimSample(2000, 2000, false, false, false); convert();
    CHECK(!readScopeControlSnapshot().enabled && shared->lastSourceMode == GunSourceMode::Legacy);
    ++groups;
    // Source metadata cannot make an unfocused/unknown scene a menu reticle or
    // scope certificate. The original calibrated coordinate conversion survives.
    CHECK(!readDesktopInputSnapshot().valid);
    read(input, 0x0C, main); CHECK(main[0] > 914 && main[0] < 916);
    ++groups;
    delete shared; shared=nullptr; VirtualFree(allocation,0,MEM_RELEASE);
    std::cout<<groups<<" wide-update ownership/SEH groups passed\n";
}
