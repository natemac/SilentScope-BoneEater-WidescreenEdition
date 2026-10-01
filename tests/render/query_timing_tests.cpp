#include "render/checked_data_write.h"
#include "render/checked_sprite_geometry.h"
#include <iostream>
#include <stdexcept>
using namespace bone_eater::render;
#define CHECK(v) do { if (!(v)) throw std::runtime_error(#v); } while (false)
int main() {
    SYSTEM_INFO info {}; GetSystemInfo(&info);
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,info.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!memory) return 2;
    try {
        const auto base=reinterpret_cast<std::uintptr_t>(memory);
        const std::uintptr_t addresses[] {base+16,base+32};
        PrivateDataObservationTiming metrics;
        privateDataObservationTiming=&metrics;
        const auto a=observePrivateDataBytes(addresses,2,GetTickCount64());
        CHECK(a.valid && metrics.queries.calls==1 && metrics.reads.calls==0);
        const auto reused=observePrivateDataBytes(addresses,2,GetTickCount64(),&a);
        CHECK(reused.valid && reused.observed==a.observed && metrics.queries.calls==1);
        const std::uintptr_t changed[] {base+16,base+48};
        CHECK(observePrivateDataBytes(changed,2,GetTickCount64(),&a).valid);
        CHECK(metrics.queries.calls==2);
        CHECK(observeSpriteGeometry({{base+128,base+512}},GetTickCount64()).valid);
        CHECK(metrics.queries.calls==3); // Geometry spans share the same instrumented query boundary.
        DWORD old=0; CHECK(VirtualProtect(memory,info.dwPageSize,PAGE_READONLY,&old));
        CHECK(!observePrivateDataBytes(addresses,2,GetTickCount64()).valid);
        CHECK(metrics.queries.calls==4 && metrics.queries.timedCalls==4);
        privateDataObservationTiming=nullptr;
        CHECK(!observePrivateDataBytes(addresses,2,GetTickCount64()).valid);
        CHECK(metrics.queries.calls==4); // Disabled context cannot update metrics.
        CHECK(VirtualProtect(memory,info.dwPageSize,PAGE_READWRITE,&old));
        CHECK(observePrivateDataBytes(addresses,2,GetTickCount64()).valid);
        std::cout<<"Passed 6 optional query-timing probe checks; actual queries="<<metrics.queries.calls<<'\n';
    } catch(const std::exception& e) {
        privateDataObservationTiming=nullptr; VirtualFree(memory,0,MEM_RELEASE);
        std::cerr<<e.what()<<'\n'; return 1;
    }
    VirtualFree(memory,0,MEM_RELEASE);
}
