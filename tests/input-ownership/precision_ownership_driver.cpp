// Separate translation unit: executes the real shared Scope/CSV implementation
// against the same private-memory scene as the real precision module fixture.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
extern "C" ULONGLONG precisionFixtureClock();
namespace sink_fixture {
bool fail=false;
size_t write(const void* p,size_t s,size_t n,FILE* f) { return fail?0:fwrite(p,s,n,f); }
}
#define GetTickCount64 precisionFixtureClock
#define fwrite sink_fixture::write
#include "../../src/input/native_input_ownership.cpp"
#undef fwrite
#undef GetTickCount64
extern "C" bool precisionOwnershipSetup(std::uintptr_t module,void(__fastcall* original)(void*,float)) {
    using namespace bone_eater::input;
    if(shared) { if(shared->file) fclose(shared->file); delete shared; }
    base=module; shared=new Shared; shared->enabled=true;
    if(tmpfile_s(&shared->file)) return false;
    ownership={}; sourceContext={}; activeScope=nullptr; scopeDepth=0;
    originalScope=original; sink_fixture::fail=false;
    return true;
}
extern "C" void precisionOwnershipCleanup() {
    using namespace bone_eater::input;
    if(shared) { if(shared->file) fclose(shared->file); delete shared; shared=nullptr; }
    activeScope=nullptr; scopeDepth=0; ownership={}; base=0;
}
extern "C" void precisionOwnershipUpdate(std::uintptr_t input,std::uintptr_t config) {
    using namespace bone_eater::input;
    const auto ticket=beginNativeInputOwnership(input,true);
    convertedNativeInputOwnership(ticket,{{915,478}},true);
    recordNativePrecisionDomain(ticket,config,bone_eater::render::readNativeHudGeometry());
    finishNativeInputOwnership(ticket,true,true,{{915,478}});
}
extern "C" void precisionOwnershipInvoke(std::uintptr_t owner) {
    using namespace bone_eater::input;
    invokeScope(reinterpret_cast<void*>(owner),.02f,base+0xAA923);
}
extern "C" bool precisionOwnershipSinkClosed() {
    using namespace bone_eater::input;
    return shared && !shared->enabled && !shared->file;
}
extern "C" void precisionOwnershipCloseSink(std::uintptr_t owner,bool rowLimit) {
    using namespace bone_eater::input;
    // Simulate another optional observation exhausting/failing its sink while
    // the functional caller is between setters. Use the actual finish/output
    // routine, not an assignment to its enabled flag or a mocked file close.
    if(rowLimit) shared->rows=35999; else sink_fixture::fail=true;
    Observation row{}; row.sampled=true; row.owner=owner;
    row.beforeValid=repeated(owner,row.before); row.input=ownership;
    finishScope(row,true);
}
