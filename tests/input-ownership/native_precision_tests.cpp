#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <limits>
namespace fixture {
ULONGLONG now=1000;
bool focus=true,visible=true,minimized=false,known=true,selected=false,failCsv=false;
unsigned nativeX=0,nativeY=0,reads=0;
unsigned delayAtRead=0,delayMs=0;
unsigned focusDelayMs=0;
HWND window=reinterpret_cast<HWND>(0x1234);
ULONGLONG clock() noexcept { return now; }
BOOL isWindow(HWND w) { return w==window; }
BOOL isVisible(HWND w) { return w==window && visible; }
BOOL isIconic(HWND w) { return w==window && minimized; }
HWND foreground() { now+=focusDelayMs; focusDelayMs=0; return focus?window:nullptr; }
HWND ancestor(HWND w,UINT) { return w==window?window:nullptr; }
DWORD owner(HWND w,LPDWORD p) { if (w!=window) return 0; *p=GetCurrentProcessId(); return 123; }
BOOL rpm(HANDLE h,LPCVOID a,LPVOID b,SIZE_T n,SIZE_T* done) {
    if(++reads==delayAtRead) now+=delayMs;
    return ReadProcessMemory(h,a,b,n,done);
}
}
#define GetTickCount64 fixture::clock
#define IsWindow fixture::isWindow
#define IsWindowVisible fixture::isVisible
#define IsIconic fixture::isIconic
#define GetForegroundWindow fixture::foreground
#define GetAncestor fixture::ancestor
#define GetWindowThreadProcessId fixture::owner
#define ReadProcessMemory fixture::rpm
#include "../../src/input/native_precision_bypass.cpp"
#include "input/native_input_ownership.h"
#undef GetTickCount64
#undef IsWindow
#undef IsWindowVisible
#undef IsIconic
#undef GetForegroundWindow
#undef GetAncestor
#undef GetWindowThreadProcessId
#undef ReadProcessMemory

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "verified"; }
const char* registeredD3D11WindowRole(void* p) noexcept { return p==fixture::window?"main":"unknown"; }
NativeHudGeometry readNativeHudGeometry() noexcept {
    NativeHudGeometry h; h.valid=true; h.mainWidth=1920; h.mainHeight=1080;
    h.width=768.0f*1080/1366; h.height=1080; h.left=(1920-h.width)/2;
    h.helper=1; h.sprite=2; return h;
}
}
namespace bone_eater::input {
SelectedHidBridgeStatus readSelectedHidBridgeStatus() noexcept {
    SelectedHidBridgeStatus s; s.configured=fixture::known; s.selected=fixture::selected; return s;
}
}
using namespace bone_eater::input;
extern "C" ULONGLONG precisionFixtureClock() { return fixture::now; }
extern "C" bool precisionOwnershipSetup(std::uintptr_t,void(__fastcall*)(void*,float));
extern "C" void precisionOwnershipCleanup();
extern "C" void precisionOwnershipUpdate(std::uintptr_t,std::uintptr_t);
extern "C" void precisionOwnershipInvoke(std::uintptr_t);
extern "C" bool precisionOwnershipSinkClosed();
extern "C" void precisionOwnershipCloseSink(std::uintptr_t,bool);
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); } while(false)
template<class T> void put(std::uintptr_t p,size_t offset,T value) { memcpy(reinterpret_cast<void*>(p+offset),&value,sizeof(value)); }
void __fastcall nativeX(float x) { ++fixture::nativeX; std::uintptr_t p=0; memcpy(&p,reinterpret_cast<void*>(base+0x13DCED0),8); put(p,0x0C,x); }
void __fastcall nativeY(float y) { ++fixture::nativeY; std::uintptr_t p=0; memcpy(&p,reinterpret_cast<void*>(base+0x13DCED0),8); put(p,0x10,y); }
struct Scene {
    std::uintptr_t owner=0,input=0,main=0,config=0;
    InputOwnershipState ledger;
    Scene() {
        base=reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr,0x1400000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE)); CHECK(base);
        owner=base+0x1000; input=base+0x4000; const auto scope=base+0x6000; main=base+0xA000;
        const auto wrapper=base+0xD000; config=base+0x10000;
        const auto manager=base+0x13000,holder=base+0x15000,ui=base+0x16000,battle=base+0x18000,window=base+0x19000;
        put(base,0x13DCF88,owner); put(owner,0,base+0x10C7350); put(owner,8,scope); put(owner,0x60,scope);
        put(scope,0,base+0x6FDF48); put(base,0x13DCED0,input); put(base,0x13DCF58,config);
        put(config,0,base+0x10C5FA8); put(config,0x136A,std::uint8_t{1}); put(config,0x12D8,OwnershipPoint{{.5f,.5f}});
        put(base,0x13DB588,wrapper); put(wrapper,8,main); put(main,0,base+0x6FDF48);
        put(main,0xEB4,OwnershipPoint{{1920,1080}}); put(main,0xE8E,std::uint8_t{0});
        put(base,0x12E1930,manager); put(manager,0x340,main);
        put(base,0x13DCF48,holder); put(holder,0,ui); put(ui,0,base+0x10CA5E0);
        put(base,0x13DCF78,battle); put(battle,0,base+0x10CA298); put(battle,0x48,unsigned{3});
        put(base,0x12E17A0,window); put(window,0x28,reinterpret_cast<std::uintptr_t>(fixture::window));
        put(owner,0x174,1.0f); put(owner,0x1C2,std::uint8_t{1}); put(owner,0x190,OwnershipPoint{{895,478}});
        shared=new Shared; shared->setters=shared->scope=shared->aim=shared->legacy=true; shared->ready=true;
        originalX=&nativeX; originalY=&nativeY; reset();
    }
    ~Scene() { if(shared->output) fclose(shared->output); delete shared; shared=nullptr;
        VirtualFree(reinterpret_cast<void*>(base),0,MEM_RELEASE); base=0; }
    void reset() {
        if(shared->output) { fclose(shared->output); shared->output=nullptr; }
        shared->ready=true; shared->failed=false; shared->thread=0; shared->nextReport=0; shared->rows=0;
        transaction={}; active=nullptr; warmup={}; domain={}; ledger={};
        fixture::focus=fixture::visible=fixture::known=true; fixture::minimized=fixture::selected=false;
        fixture::nativeX=fixture::nativeY=fixture::reads=0; fixture::now+=1000;
        fixture::delayAtRead=fixture::delayMs=0;
        fixture::focusDelayMs=0;
        put(base,0x13DCED0,input); put(base,0x13D9F7C,unsigned{0});
        put(owner,0x1C1,std::uint8_t{0}); put(owner,0x1C2,std::uint8_t{1});
        put(owner,0x1B8,OwnershipPoint{}); put(config,0x12D8,OwnershipPoint{{.5f,.5f}});
        put(input,0,0xABCu); put(input,8,0xDEFu); put(input,0x24,OwnershipPoint{{77,88}});
    }
    NativePrecisionScopeToken begin(unsigned state=0,float delta=.02f) {
        put(owner,0x1C4,state); put(input,0x0C,OwnershipPoint{{915,478}});
        const auto ticket=ledger.begin(input,true); ledger.conversion(ticket,{{915,478}},true);
        ledger.finish(ticket,true,true,{{915,478}},fixture::now); ledger.scopeInvocation();
        recordNativePrecisionDomain(ticket,config,bone_eater::render::readNativeHudGeometry());
        return beginNativePrecisionScope(owner,base+0xAA923,delta,ledger,false);
    }
    void complete(NativePrecisionScopeToken token,bool loseFocus=false,bool dropOutput=false) {
        const auto x=transaction.pair.xCaller(),y=transaction.pair.yCaller();
        setter(false,905,base+x);
        if(loseFocus) fixture::focus=false;
        if(dropOutput && shared->output) { fclose(shared->output); shared->output=nullptr; }
        setter(true,474,base+y);
        float native=0; memcpy(&native,reinterpret_cast<void*>(input+0x10),4);
        setter(true,native+3,base+0xCD179);
        OwnershipPoint point{}; memcpy(point.data(),reinterpret_cast<void*>(input+0x0C),8);
        observeNativePrecisionRay(main,point[0],point[1],true,true,false);
        finishNativePrecisionScope(token,true);
    }
    OwnershipPoint point() const { OwnershipPoint p{}; memcpy(p.data(),reinterpret_cast<void*>(input+0x0C),8); return p; }
};
constexpr DWORD exceptionCode=0xE1287654;
Scene* integratedScene=nullptr;
int closeSink=0;
void __fastcall integratedNativeScope(void* owner,float delta) {
    CHECK(integratedScene && reinterpret_cast<std::uintptr_t>(owner)==integratedScene->owner && delta==.02f);
    setter(false,905,base+transaction.pair.xCaller());
    if(closeSink) precisionOwnershipCloseSink(integratedScene->owner,closeSink==2);
    setter(true,474,base+transaction.pair.yCaller());
    const auto p=integratedScene->point(); setter(true,p[1]+3,base+0xCD179);
    const auto final=integratedScene->point();
    observeNativePrecisionRay(integratedScene->main,final[0],final[1],true,true,false);
    observeNativeInputOwnershipRay(integratedScene->main,final[0],final[1],true,true,false);
}
void invokeException(NativePrecisionScopeToken token) {
    bool returned=false;
    __try { setter(false,905,base+transaction.pair.xCaller()); RaiseException(exceptionCode,EXCEPTION_NONCONTINUABLE,0,nullptr); returned=true; }
    __finally { finishNativePrecisionScope(token,returned); }
}
bool catchException(NativePrecisionScopeToken token) {
    __try { invokeException(token); }
    __except(GetExceptionCode()==exceptionCode?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { return true; }
    return false;
}
int main(int argc,char** argv) {
    if(argc>1 && std::string(argv[1])=="--disabled") {
        SetEnvironmentVariableW(L"BONE_EATER_NATIVE_PRECISION_BYPASS",nullptr);
        CHECK(!nativePrecisionRequested() && !prepareNativePrecisionBypass(reinterpret_cast<void*>(1)));
        InputOwnershipState empty;
        CHECK(!beginNativePrecisionScope(1,2,.02f,empty,false).owns && !fixture::reads && !shared);
        std::cout<<"disabled precision has no hook preparation/native reads\n"; return 0;
    }
    const bool quiet=argc>1 && std::string(argv[1])=="--quiet";
    if(quiet) {
        SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",L"1");
        CHECK(!bone_eater::diagnostics::optionalOutputEnabled());
        CHECK(!bone_eater::diagnostics::openOptionalCsv(L"never-created-precision-fixture.csv",[](FILE*){}));
    }
    SetEnvironmentVariableW(L"BONE_EATER_NATIVE_PRECISION_BYPASS",L"1");
    Scene s; unsigned groups=0;
    shared->ready=false; auto token=s.begin(); CHECK(!token.owns && !active); setter(false,42,base+0xCCF5B); CHECK(s.point()[0]==42); ++groups;
    s.reset(); shared->ready=false; shared->scope=false; shared->aim=false;
    nativePrecisionScopeInstalled(reinterpret_cast<void*>(base)); CHECK(!nativePrecisionReady());
    nativePrecisionAimInstalled(reinterpret_cast<void*>(base)); CHECK(nativePrecisionReady()); ++groups;
    s.reset(); token=s.begin(); CHECK(transaction.eligible && !transaction.pair.substitute); s.complete(token);
    CHECK(warmup.anchor && !shared->failed && sameOwnershipPoint(s.point(),{{905,477}}));
    token=s.begin(); CHECK(transaction.pair.substitute); s.complete(token);
    CHECK(sameOwnershipPoint(s.point(),{{915,481}}) && !active && !shared->failed);
    CHECK(fixture::nativeX==2 && fixture::nativeY==4 && transaction.reads<=96 && transaction.bytes<=8192); ++groups;
    unsigned value=0; memcpy(&value,reinterpret_cast<void*>(s.input),4); CHECK(value==0xABC);
    memcpy(&value,reinterpret_cast<void*>(s.input+8),4); CHECK(value==0xDEF);
    OwnershipPoint ui{}; memcpy(ui.data(),reinterpret_cast<void*>(s.input+0x24),8); CHECK((ui==OwnershipPoint{{77,88}})); ++groups;
    token=s.begin(3); CHECK(!transaction.pair.substitute); s.complete(token); CHECK(warmup.returning);
    token=s.begin(3); CHECK(transaction.pair.substitute); s.complete(token); CHECK(sameOwnershipPoint(s.point(),{{915,481}})); ++groups;
    token=s.begin(); s.complete(token,true); CHECK(sameOwnershipPoint(s.point(),{{915,481}}) && !shared->failed);
    token=s.begin(); CHECK(!transaction.eligible && !warmup.anchor); finishNativePrecisionScope(token,true);
    fixture::focus=true; token=s.begin(); CHECK(!transaction.pair.substitute); s.complete(token); ++groups;
    token=s.begin(); CHECK(tmpfile_s(&shared->output)==0); s.complete(token,false,true);
    CHECK(!shared->failed && sameOwnershipPoint(s.point(),{{915,481}})); ++groups;
    token=s.begin(); setter(false,905,base+transaction.pair.xCaller()); setter(true,474,base+0xDEADB);
    CHECK(shared->failed && transaction.pair.fault); finishNativePrecisionScope(token,true); CHECK(!active); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin();
    CHECK(catchException(token) && shared->failed && !active); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin();
    auto nested=beginNativePrecisionScope(s.owner,base+0xAA923,.02f,s.ledger,true);
    CHECK(!active && !transaction.eligible); finishNativePrecisionScope(nested,true); CHECK(active==&transaction);
    finishNativePrecisionScope(token,true); CHECK(!active); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin(); setter(false,905,base+transaction.pair.xCaller());
    ++s.ledger.serial; setter(true,474,base+transaction.pair.yCaller()); CHECK(shared->failed);
    finishNativePrecisionScope(token,true); ++groups;
    for(const auto state:{1u,2u,4u}) { s.reset(); token=s.begin(state); CHECK(!transaction.eligible); finishNativePrecisionScope(token,true); } ++groups;
    s.reset(); token=s.begin(0,std::numeric_limits<float>::quiet_NaN()); CHECK(!transaction.eligible); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); put(s.owner,0x1C1,std::uint8_t{1}); token=s.begin(); CHECK(!transaction.eligible); finishNativePrecisionScope(token,true);
    s.reset(); put(s.owner,0x1B8,OwnershipPoint{{1,0}}); token=s.begin(); CHECK(!transaction.eligible); finishNativePrecisionScope(token,true);
    s.reset(); put(base,0x13D9F7C,1u); token=s.begin(); CHECK(!transaction.eligible); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); setter(false,905,base+transaction.pair.xCaller()); setter(true,474,base+transaction.pair.yCaller());
    setter(true,477,base+0xCD179); observeNativePrecisionRay(s.main,906,477,true,true,false);
    CHECK(shared->failed); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin();
    put(base,0x13DCED0,s.input+0x40); setter(false,905,base+transaction.pair.xCaller());
    CHECK(!transaction.eligible && !shared->failed); put(base,0x13DCED0,s.input); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); s.complete(token); shared->thread=GetCurrentThreadId()+1; token=s.begin();
    CHECK(shared->failed); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin();
    setter(false,905,base+transaction.pair.xCaller()); setter(true,std::numeric_limits<float>::quiet_NaN(),base+transaction.pair.yCaller());
    CHECK(shared->failed); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); fixture::known=false; attempted=false; CHECK(!prepareNativePrecisionBypass(reinterpret_cast<void*>(base)));
    fixture::known=true; fixture::selected=true; attempted=false; CHECK(!prepareNativePrecisionBypass(reinterpret_cast<void*>(base))); ++groups;
    s.reset(); fixture::delayAtRead=40; fixture::delayMs=110; token=s.begin();
    CHECK(!transaction.eligible && !warmup.anchor); finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin(); fixture::now+=101;
    setter(false,905,base+transaction.pair.xCaller()); CHECK(!transaction.eligible && !shared->failed && s.point()[0]==905);
    finishNativePrecisionScope(token,true); ++groups;
    s.reset(); token=s.begin(); s.complete(token); token=s.begin(); put(s.input,0x0C,OwnershipPoint{{910,478}});
    setter(false,905,base+transaction.pair.xCaller()); CHECK(!transaction.eligible && !shared->failed && s.point()[0]==905);
    finishNativePrecisionScope(token,true); ++groups;
    for(int cause:{1,2}) {
        if(quiet) continue; // The combined sink fixture intentionally enables CSV.
        s.reset(); integratedScene=&s; closeSink=0;
        CHECK(precisionOwnershipSetup(base,&integratedNativeScope));
        put(s.owner,0x1C4,0u); put(s.input,0x0C,OwnershipPoint{{915,478}});
        precisionOwnershipUpdate(s.input,s.config); precisionOwnershipInvoke(s.owner);
        CHECK(warmup.anchor && !shared->failed);
        put(s.input,0x0C,OwnershipPoint{{915,478}}); precisionOwnershipUpdate(s.input,s.config);
        closeSink=cause; precisionOwnershipInvoke(s.owner);
        CHECK(precisionOwnershipSinkClosed() && nativePrecisionReady() && !shared->failed &&
            sameOwnershipPoint(s.point(),{{915,481}}));
        closeSink=0; put(s.input,0x0C,OwnershipPoint{{915,478}});
        precisionOwnershipUpdate(s.input,s.config); precisionOwnershipInvoke(s.owner);
        CHECK(nativePrecisionReady() && sameOwnershipPoint(s.point(),{{915,481}}));
        precisionOwnershipCleanup(); ++groups;
    }
    s.reset(); token=s.begin(); s.complete(token); token=s.begin(); fixture::focusDelayMs=110;
    setter(false,905,base+transaction.pair.xCaller());
    CHECK(!transaction.eligible && !shared->failed && s.point()[0]==905);
    finishNativePrecisionScope(token,true); ++groups;
    std::cout<<groups<<" native precision fixture groups passed\n";
}
