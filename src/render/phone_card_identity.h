#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace bone_eater::render {
inline float phoneCardAlpha(float elapsed,float duration) noexcept {
    const float fade=std::min(0.4f,duration*0.5f);
    return std::clamp(std::min(elapsed,duration-elapsed)/fade,0.f,1.f);
}
// CSRFinalResult phase127 owns the guest e-AMUSEMENT card. Other termination
// variants contain account/save information and must never be covered.
template<class Read> bool scorePhoneCard(std::uintptr_t base,Read read,
    std::uintptr_t& owner,std::uintptr_t& layout,float& alpha) noexcept {
    owner=layout=0;alpha=0;
    std::uintptr_t manager=0,sceneOwner=0,messages=0,vt=0;
    unsigned scene=0,count=0;int pending=0;
    if(!read(base,0x13DD000,manager)||!read(manager,0,vt)||vt!=base+0x10C6B38||
       !read(manager,0x38,scene)||scene!=5||!read(manager,0x3C,pending)||pending!=-1||
       !read(manager,0x40,sceneOwner)||!read(sceneOwner,0,vt)||vt!=base+0x10C6E48||
       !read(base,0x13DCFB0,messages)||!read(messages,0,vt)||vt!=base+0x10CE298||
       !read(messages,0x808,count)||!count||count>256)return false;
    // Native registration scans count entries from +8 (constructor175170).
    // Do not assume the final result always occupies the first two slots.
    for(unsigned index=0;index<count;++index) {
        const unsigned slot=8+index*8;
        std::uintptr_t result=0,child=0,card=0;unsigned phase=0,variant=0,state=0,key=0;
        std::array<unsigned char,2> flags{};
        if(!read(messages,slot,result)||!read(result,0,vt)||vt!=base+0x10CA968||
           !read(result,8,flags)||flags[1]||!read(result,0x10,phase)||phase!=127||
           !read(result,0x30,child)||!read(child,0,vt)||vt!=base+0x10CA818||
           !read(child,8,flags)||flags[1]||!read(child,0x2C,variant)||variant!=2||
           !read(child,0x10,phase)||phase>2||!read(child,0x18,card)||
           // Constructor107540 stores the INFO resource at +64, not the AIF.
           !read(card,0,vt)||vt!=base+0x10C9FA8||!read(card,0x64,key)||key!=0xB67||
           !read(card,0x18,state)||state>3)continue;
        float elapsed=0,duration=0;
        if(!read(child,0x20,duration)||!read(child,0x24,elapsed)||
           !std::isfinite(duration)||duration<=0||!std::isfinite(elapsed)||elapsed<0)continue;
        // Only the active card exposes art. Loading and the native two-frame
        // release display black; no released root is dereferenced.
        if((phase==0&&(state==1||state==2))||
           (phase==1&&state==2)||(phase==2&&(state==3||state==0))) {
            owner=child;layout=card;
            alpha=phase==1?phoneCardAlpha(elapsed,duration):0;
            return true;
        }
    }
    return false;
}
}
