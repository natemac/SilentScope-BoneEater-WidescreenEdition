#pragma once
#include <array>
#include <cstdint>
namespace bone_eater::render {
// Pinned CSRFinalResult update177200:110 updates total results (+38),
// 108 updates earned titles (+40). Exclude gameplay, teardown and phone127.
template<class Read> bool scoreResultsReticle(std::uintptr_t base,Read read) noexcept {
    std::uintptr_t scene=0,battle=0,messages=0,vt=0;
    unsigned state=0,count=0;int pending=0;
    if(!read(base,0x13DD000,scene)||!read(scene,0,vt)||vt!=base+0x10C6B38||
       !read(scene,0x38,state)||state!=5||!read(scene,0x3C,pending)||pending!=-1||
       !read(scene,0x40,battle)||!read(battle,0,vt)||vt!=base+0x10C6E48||
       !read(base,0x13DCFB0,messages)||!read(messages,0,vt)||vt!=base+0x10CE298||
       !read(messages,0x808,count)||!count||count>256)return false;
    for(unsigned i=0;i<count;++i) {
        std::uintptr_t owner=0,layout=0;unsigned phase=0,key=0;
        std::array<unsigned char,2> flags{};
        if(!read(messages,8+i*8,owner)||!read(owner,0,vt)||vt!=base+0x10CA968||
           !read(owner,8,flags)||flags[0]||flags[1]||!read(owner,0x10,phase)||
           (phase!=108&&phase!=110)||!read(owner,0x50,layout)||
           !read(layout,0,vt)||vt!=base+0x10C9FA8||
           !read(layout,0x64,key)||key!=0xBC6||!read(layout,0x18,state)||state!=2)continue;
        return true;
    }
    return false;
}
}
