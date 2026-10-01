#include "render/checked_sprite_geometry.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace bone_eater::render;
namespace {
void require(bool v,int line) { if (!v) throw std::runtime_error("line "+std::to_string(line)); }
#define CHECK(v) require((v),__LINE__)
struct Memory {
    unsigned char* data=nullptr; std::size_t page=0;
    Memory() { SYSTEM_INFO info{}; GetSystemInfo(&info);page=info.dwPageSize;
        data=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        CHECK(data);std::memset(data,0x5a,page*2); }
    ~Memory(){if(data)VirtualFree(data,0,MEM_RELEASE);}
    std::uintptr_t object(unsigned i=0)const{return reinterpret_cast<std::uintptr_t>(data)+64+i*512;}
    void protect(DWORD flags, bool second=false){DWORD old=0;CHECK(VirtualProtect(data+(second?page:0),second?page:page*2,flags,&old));}
};
const RearIlluminationPose before {{{1,2}},{{3,4}}}, after {{{5,6}},{{7,8}}};
SpriteGeometryAccess observe(const Memory& m){return observeSpriteGeometry({{m.object(),m.object(1)}},GetTickCount64());}
bool same(std::uintptr_t object,const RearIlluminationPose& value) {
    return !std::memcmp(reinterpret_cast<void*>(object+0x70),value.position.data(),8)&&
        !std::memcmp(reinterpret_cast<void*>(object+0xA8),value.scale.data(),8);
}
void exactTwoSpansAndNoInterveningWrites(){Memory m;const auto p=acquireSpriteGeometry(observe(m),m.object());
    CHECK(p.valid&&writeSpriteGeometry(p,after)&&same(m.object(),after));
    for(unsigned i=0;i<0xB8;++i)if(!(i>=0x70&&i<0x78)&&!(i>=0xA8&&i<0xB0))CHECK(m.data[64+i]==0x5a);
    CHECK(!writeSpriteGeometrySpan(p,0x78,after.position));}
void exactAllowlistAndInvalidAddresses(){Memory m;const auto a=observe(m);
    CHECK(!acquireSpriteGeometry(a,m.object()+8).valid);
    CHECK(!observeSpriteGeometry({{0,m.object()}},GetTickCount64()).valid);
    CHECK(!observeSpriteGeometry({{m.object(),m.object()}},GetTickCount64()).valid);
    CHECK(!observeSpriteGeometry({{std::numeric_limits<std::uintptr_t>::max()-8,m.object()}},GetTickCount64()).valid);}
void staleAcquireSavedRestore(){Memory m;auto a=observe(m);const auto p=acquireSpriteGeometry(a,m.object());
    CHECK(writeSpriteGeometry(p,after));a.observed-=101;CHECK(!acquireSpriteGeometry(a,m.object()).valid);
    a={};CHECK(writeSpriteGeometry(p,before)&&same(m.object(),before));}
void shortLeaseRetainsOriginalTick(){Memory m;auto a=observe(m);a.observed=GetTickCount64()-40;
    const auto b=observeSpriteGeometry(a.sprites,GetTickCount64(),&a);CHECK(b.valid&&b.observed==a.observed);
    a.observed=GetTickCount64()-51;m.protect(PAGE_READONLY);
    CHECK(!observeSpriteGeometry(a.sprites,GetTickCount64(),&a).valid);}
void identityChangeCannotInheritOldPageCheck(){Memory m;auto a=observe(m);m.protect(PAGE_READONLY);
    auto changed=a.sprites;changed[1]+=8;
    CHECK(!observeSpriteGeometry(changed,GetTickCount64(),&a).valid);
    CHECK(!writeSpriteGeometry(acquireSpriteGeometry(a,m.object()),after));}
void forbiddenProtections(){for(DWORD flags:{PAGE_READONLY,PAGE_NOACCESS,PAGE_EXECUTE_READWRITE,PAGE_READWRITE|PAGE_GUARD}){
    Memory m;m.protect(flags);CHECK(!observe(m).valid);}}
void fullSpanBoundaryGuard(){Memory m;
    const auto crossing=reinterpret_cast<std::uintptr_t>(m.data)+m.page-0xA8-4;
    m.protect(PAGE_READONLY,true);
    CHECK(!observeSpriteGeometry({{crossing,m.object()}},GetTickCount64()).valid);}
void partialSecondSpanAndBaselinePrefixRestore(){Memory m;
    const auto crossing=reinterpret_cast<std::uintptr_t>(m.data)+m.page-0xA8-4;
    const auto a=observeSpriteGeometry({{crossing,m.object()}},GetTickCount64());
    const auto p=acquireSpriteGeometry(a,crossing);CHECK(p.valid&&writeSpriteGeometry(p,before));
    m.protect(PAGE_READONLY,true);CHECK(!writeSpriteGeometry(p,after));
    CHECK(!writeSpriteGeometry(p,before)); // Fails conservatively, but restores every writable prefix.
    CHECK(same(crossing,before));
    m.protect(PAGE_READWRITE,true);CHECK(writeSpriteGeometry(p,before));}
void secondRestoreAttemptedAfterFirstSpanFails(){Memory m;
    const auto split=reinterpret_cast<std::uintptr_t>(m.data)+m.page-0x90;
    const auto a=observeSpriteGeometry({{split,m.object()}},GetTickCount64());
    const auto p=acquireSpriteGeometry(a,split);CHECK(p.valid&&writeSpriteGeometry(p,before));
    DWORD old=0;CHECK(VirtualProtect(m.data,m.page,PAGE_READONLY,&old));
    CHECK(!writeSpriteGeometry(p,after));
    CHECK(!writeSpriteGeometry(p,before));
    CHECK(same(split,before));}
void futureTimestampRejects(){Memory m;auto a=observe(m);a.observed+=60000;
    CHECK(!acquireSpriteGeometry(a,m.object()).valid);
    CHECK(!observeSpriteGeometry(a.sprites,a.observed).valid);}
void singleLeafPermissions(){Memory m;
    const auto a=observeSingleSpriteGeometry(m.object(),GetTickCount64());
    const auto p=acquireSpriteGeometry(a,m.object());CHECK(p.valid);
    CHECK(!acquireSpriteGeometry(a,0).valid && !acquireSpriteGeometry(a,m.object(1)).valid);
    CHECK(writeSpriteGeometry(p,after) && writeSpriteGeometry(p,before) && same(m.object(),before));
    auto stale=a;stale.observed-=101;CHECK(!acquireSpriteGeometry(stale,m.object()).valid);
    m.protect(PAGE_READONLY);CHECK(!observeSingleSpriteGeometry(m.object(),GetTickCount64(),&stale).valid);
    CHECK(!observeSingleSpriteGeometry(0,GetTickCount64()).valid);}
}
int main(){try{exactTwoSpansAndNoInterveningWrites();exactAllowlistAndInvalidAddresses();staleAcquireSavedRestore();
    shortLeaseRetainsOriginalTick();identityChangeCannotInheritOldPageCheck();forbiddenProtections();fullSpanBoundaryGuard();
    partialSecondSpanAndBaselinePrefixRestore();secondRestoreAttemptedAfterFirstSpanFails();futureTimestampRejects();singleLeafPermissions();}
    catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
    std::cout<<"Passed 11 sprite geometry permission/restoration cases\n";}
