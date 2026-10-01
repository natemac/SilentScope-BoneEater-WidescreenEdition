// Includes each exact production module in a separate executable. The private
// fake module supplies only validated instruction bytes: this is an output /
// functional-path test, not an independent instruction-signature proof.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <share.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fixture {
std::string mode;
unsigned directories=0, opens=0, seeks=0, flushes=0, hookCalls=0, infoCalls=0, warningCalls=0;
unsigned nativeCalls=0, beginCalls=0, endCalls=0, suspended=0, resumed=0, experimentInstalls=0;
unsigned setterCalls=0;
std::uintptr_t nativeTargetObject=0;
bool throwWarning=false, validHookSeed=false, recurse=false;
void* expectedObject=nullptr;
void* expectedDescriptor=nullptr;
BOOL directory(LPCWSTR, LPSECURITY_ATTRIBUTES) {
    ++directories;
    if (mode=="directory") { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return TRUE;
}
FILE* open(const wchar_t*, const wchar_t*, int) {
    ++opens;
    if (mode=="open") return nullptr;
    FILE* file=nullptr;
    if (tmpfile_s(&file)) return nullptr;
    return file;
}
int seek(FILE* file, __int64 offset, int origin) {
    ++seeks; return mode=="seek" ? -1 : _fseeki64(file,offset,origin);
}
__int64 tell(FILE* file) { return mode=="tell" ? -1 : _ftelli64(file); }
int flush(FILE* file) { ++flushes; return mode=="header" ? EOF : std::fflush(file); }
BOOL pin(DWORD, LPCWSTR module, HMODULE* output) { *output=(HMODULE)module; return TRUE; }
}
#define CreateDirectoryW fixture::directory
#define _wfsopen fixture::open
#define _fseeki64 fixture::seek
#define _ftelli64 fixture::tell
// fflush lives in std in production; a namespace shim preserves call syntax.
namespace std { inline int fixture_flush(FILE* f) { return fixture::flush(f); } }
#define fflush fixture_flush
#define GetModuleHandleExW fixture::pin
#if defined(TEST_CAMERA)
#include "../../src/camera/native_camera_hook.cpp"
#elif defined(TEST_FRAMING)
#include "../../src/camera/native_framing_observer.cpp"
#elif defined(TEST_DOF)
#include "../../src/render/native_dof_probe.cpp"
#endif
#undef GetModuleHandleExW
#undef fflush
#undef _ftelli64
#undef _fseeki64
#undef _wfsopen
#undef CreateDirectoryW

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "verified"; }
#if defined(TEST_CAMERA)
float originalNativeMainAspect() noexcept { return 800.0f/1280; }
void installNativeDofProbe(void*) noexcept {}
#elif defined(TEST_FRAMING)
NativeHudGeometry readNativeHudGeometry() noexcept { return {}; }
#elif defined(TEST_DOF)
void installNativeDofExperiment(void*) noexcept { ++fixture::experimentInstalls; }
void beginNativeDofExperiment(void*,void*,std::uintptr_t) noexcept { ++fixture::beginCalls; }
void endNativeDofExperiment(bool) noexcept { ++fixture::endCalls; }
std::uint64_t suspendNativeDofExperiment() noexcept { ++fixture::suspended; return 17; }
void resumeNativeDofExperiment(std::uint64_t n) noexcept { if(n==17) ++fixture::resumed; }
#endif
}
#if defined(TEST_CAMERA)
namespace bone_eater::camera { void installNativeFramingObserver(void*) noexcept {} }
#elif defined(TEST_FRAMING)
namespace bone_eater::input {
AimSnapshot readAimSnapshot() { return {}; }
NativeAimSnapshot readNativeAimSnapshot() { return {}; }
NativeRaySnapshot readNativeRaySnapshot() { return {}; }
bool aimUsableAt(const AimSnapshot&, GunSourceClock::time_point, std::chrono::milliseconds) noexcept { return false; }
}
#endif

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
template<class T> void put(std::uintptr_t p,std::size_t offset,const T& v) {
    std::memcpy(reinterpret_cast<void*>(p+offset),&v,sizeof(v));
}
#include "fixture_guards.h"

#if defined(TEST_CAMERA)
using namespace bone_eater::camera;
void __fastcall native(void* object) { CHECK(object==fixture::expectedObject); ++fixture::nativeCalls; }
bool __fastcall setter(void* camera,std::uint64_t property,const void* data) {
    CHECK(property==0x15); ++fixture::setterCalls;
    std::uintptr_t parameter=0;
    std::memcpy(&parameter,static_cast<unsigned char*>(camera)+0xDF0,8);
    std::memcpy(reinterpret_cast<void*>(parameter+0x18),data,4);
    ++*reinterpret_cast<std::uint32_t*>(parameter+0x70);
    return true;
}
#elif defined(TEST_FRAMING)
using namespace bone_eater::camera;
void __fastcall native(void* object) {
    CHECK(object==fixture::expectedObject); ++fixture::nativeCalls;
    if(fixture::nativeTargetObject) std::memcpy(reinterpret_cast<void*>(fixture::nativeTargetObject+0x80),
        static_cast<unsigned char*>(object)+0x20,16); // Model the native commit of the proposed target.
}
#elif defined(TEST_DOF)
using namespace bone_eater::render;
bool __fastcall native(void* object,void* descriptor) {
    CHECK(object==fixture::expectedObject && descriptor==fixture::expectedDescriptor); ++fixture::nativeCalls;
    if(fixture::recurse) { fixture::recurse=false; CHECK(!prepareHook(object,descriptor)); }
    return false;
}
#endif

int main(int argc,char** argv) {
    try {
        CHECK(argc==2); fixture::mode=argv[1];
        const bool quiet=fixture::mode=="quiet";
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",quiet ? L"1" : nullptr));
        fixture::throwWarning=!quiet && fixture::mode!="normal";
        auto* storage=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x1511000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        CHECK(storage);
        const auto module=reinterpret_cast<std::uintptr_t>(storage);
        seedGuards(module);
#if defined(TEST_CAMERA)
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_VERTICAL_FOV",L"1"));
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_NATIVE_FOV_ZOOM",L"1"));
        installNativeCameraObserver(storage);
        CHECK(observer && originalRecalculate && preserveVertical && setProperty && !opticsFailed);
        CHECK((observer->output!=nullptr)==(fixture::mode=="normal"));
        if(quiet || fixture::mode!="normal") CHECK(!beginSample(0,0).sample);
        originalRecalculate=native;
        fixture::expectedObject=storage+0x10000;
        recalculate(fixture::expectedObject);
        CHECK(fixture::nativeCalls==1 && !insideObserver && !opticsFailed);
        // The production RAII setter/restorer remains operational without any
        // output sink. Exact active/scope eligibility is supplied synthetically.
        Snapshot s; s.status="resolved"; s.camera=module+0x10000; s.parameter=module+0x20000;
        s.parameterVtable=module+0x6FDEE8; s.references=1; s.slot=0; s.width=1920; s.height=1080;
        s.managerCamera=s.camera; s.active.valid=true; s.active.target=s.camera;
        s.active.vtable=module+0x10C4B60; s.scope.valid=true; s.scope.target=module+0x30000;
        s.scope.owned=module+0x30000; s.scope.targetParameter=module+0x40000;
        s.scope.ownedParameter=module+0x40000; s.systemZoom=2; s.viewportRatio=1920.0f/1080; s.generation=10;
        put(s.camera,0,module+0x6FDF48); put(s.camera,0xDF0,s.parameter);
        put(s.parameter,8,std::uint32_t(1)); put(s.parameter,0x70,std::uint32_t(10));
        put(s.parameter,0x18,s.systemZoom); setProperty=setter;
        Ticket change;
        { OpticalOverride temporary(s,change); CHECK(temporary.active && temporary.applied!=s.systemZoom); }
        float restored=0; CHECK(read(s.parameter,0x18,restored));
        CHECK(restored==s.systemZoom && fixture::setterCalls==2 && change.opticsRestored && !opticsFailed);
#elif defined(TEST_FRAMING)
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_AIM_FRAMING",L"1"));
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_FRAMING_TIMING",L"1"));
        installNativeFramingObserver(storage);
        CHECK(observer && pan && originalCommit && !pan->failed);
        CHECK((observer->output!=nullptr)==(fixture::mode=="normal"));
        CHECK((pan->output!=nullptr)==(fixture::mode=="normal"));
        if(quiet) CHECK(!writeTiming);
        if(quiet || fixture::mode!="normal") CHECK(!begin(0,0).sample);
        originalCommit=native;
        fixture::expectedObject=storage+0x10000;
        commit(fixture::expectedObject);
        CHECK(fixture::nativeCalls==1 && !inside && !pan->failed);
        // Exercise the production synchronous restore/final certificate path
        // with a private synthetic wrapper/native target pair, independent of CSV.
        PanOverride change; change.wrapper=module+0x10000; change.camera=module+0x20000;
        change.targetObject=module+0x30000; change.baseline={0,0,10,1}; change.pitched={0,1,10,1};
        change.caller=0x4518A; change.serial=1; change.offset=.01;
        put(change.wrapper,0,module+0x10C4B60); put(change.wrapper,8,change.camera);
        put(change.wrapper,0x60,change.camera); put(change.wrapper,0x20,change.pitched);
        put(change.camera,0,module+0x6FDF48); put(change.camera,0x1B0,change.targetObject);
        put(change.targetObject,0,module+0x6FDC58);
        change.permission=acquireNativeTargetPermission(observeNativeTargetAccess(change.wrapper,GetTickCount64()),change.wrapper);
        CHECK(change.permission.valid); change.attempted=true; change.applied=true;
        fixture::nativeTargetObject=change.targetObject;
        pan->wrapper=change.wrapper; pan->earlySerial=1; pan->framing.neutralEarlyRequired=false;
        inside=true;
        invokeOriginal(fixture::expectedObject,&change);
        certifyEarlyPan(change);
        Vector restored {}; CHECK(read(change.wrapper,0x20,restored));
        CHECK(fixture::nativeCalls==2 && change.restored && restored==change.baseline && !inside && !pan->failed);
        CHECK(pan->framing.earlyCertified && pan->framing.certifiedOffset==.01);
#elif defined(TEST_DOF)
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_DOF_OBSERVE",L"1"));
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_DOF_MAIN_OFF",L"1"));
        installNativeDofProbe(storage);
        CHECK(output && originalPrepare && enabled && fixture::experimentInstalls==1);
        CHECK((output->file!=nullptr)==(fixture::mode=="normal"));
        if(quiet || fixture::mode!="normal") CHECK(!begin(nullptr,nullptr,0).sample);
        originalPrepare=native;
        fixture::expectedObject=storage+0x10000; fixture::expectedDescriptor=storage+0x20000;
        fixture::recurse=true;
        CHECK(!prepareHook(fixture::expectedObject,fixture::expectedDescriptor));
        CHECK(fixture::nativeCalls==2 && fixture::beginCalls==1 && fixture::endCalls==1);
        CHECK(fixture::suspended==1 && fixture::resumed==1 && !threadState.inside);
#endif
        CHECK(fixture::hookCalls==1 && fixture::validHookSeed);
        if(quiet) CHECK(!fixture::directories && !fixture::opens && !fixture::seeks && !fixture::flushes);
        if(fixture::mode=="directory") CHECK(fixture::directories>0 && !fixture::opens);
        if(fixture::throwWarning) CHECK(fixture::warningCalls>0);
        CHECK(bone_eater::diagnostics::optionalOutputEnabled()==!quiet);
        CHECK(SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS",quiet ? nullptr : L"1"));
        CHECK(bone_eater::diagnostics::optionalOutputEnabled()==!quiet); // Startup policy is immutable.
        VirtualFree(storage,0,MEM_RELEASE);
        std::cout << "functional hook / optional output: " << fixture::mode << " passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
