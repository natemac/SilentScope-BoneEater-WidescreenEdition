#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>

namespace bone_eater::render {
// Shared by the compositor and hit mapping. Native artwork is sampled with
// uniform scale, so circular controls and text retain their proportions.
struct MenuLandscapeLayout {
    static constexpr float sourceX=657, sourceY=180;
    static constexpr float width=607, height=640;
    static constexpr float left=1020, top=110, scale=1.3f;
    static bool toNative(float x,float y,float& nx,float& ny) noexcept {
        if(!std::isfinite(x)||!std::isfinite(y)||x<left||y<top||
           x>=left+width*scale||y>=top+height*scale)return false;
        nx=sourceX+(x-left)/scale;ny=sourceY+(y-top)/scale;return true;
    }
};
template<class Read> bool landscapeMenuOwner(std::uintptr_t base,Read read,
                                            std::uintptr_t& owner,unsigned& phase,
                                            bool includeMainMenu=true,
                                            bool includeMainMenuExit=false) noexcept {
    std::uintptr_t manager=0,vt=0,layout=0;unsigned scene=0,primary=0,key=0;int pending=0;
    std::array<unsigned char,2> stopped{};
    if(!read(base,0x13DD000,manager)||!read(manager,0,vt)||vt!=base+0x10C6B38||
       !read(manager,0x38,scene)||(scene!=2&&scene!=4&&scene!=6)||!read(manager,0x3C,pending)||pending!=-1||
       !read(manager,0x40,owner)||!read(owner,0,vt)||
       !read(owner,8,stopped)||stopped[0]||stopped[1]||
       !read(owner,0x10,primary)||!read(owner,0x14,phase))return false;
    if(scene==6) {
        // CSR scene owns CSRStageSelect; its state lives at +10, unlike
        // MainMenu. States 2..7 span entry/choice/exit; 19 drains the fade.
        std::uintptr_t child=0;
        if(vt!=base+0x10C7240||primary!=2||phase!=0||
           !read(owner,0x18,child)||!read(child,0,vt)||vt!=base+0x10CAAE0||
           !read(child,8,stopped)||stopped[0]||stopped[1]||
           !read(child,0x10,phase)||((phase<1||phase>7)&&phase!=19&&phase!=20)||
           !read(child,0x20,layout)||!read(layout,0,vt)||vt!=base+0x10C9FA8||
           !read(layout,0x64,key)||key!=0xBA0)return false;
        owner=child;phase+=200;return true;
    }
    // MainMenu remains the manager's live owner during its short primary-2
    // cleanup. Keep its remaining footer framed, but never remap input there.
    const bool menuExit=includeMainMenuExit&&scene==2&&primary==2&&phase==0;
    if(primary!=1&&!menuExit)return false;
    if(scene==4) {
        // CEamuScene -> CEamuFirstCredit. Its BA0 menu is separate from the
        // card-entry UI at +18. Only normal-start entry, choice and exit belong
        // to this conversion; subsequent card/name states retain their layout.
        std::uintptr_t child=0;
        if(vt!=base+0x10C6FA0||phase!=8||!read(owner,0x48,child)||
           !read(child,0,vt)||vt!=base+0x10CA7A8||
           !read(child,8,stopped)||stopped[0]||stopped[1]||
           !read(child,0x10,primary)||primary!=1||!read(child,0x14,phase)||
           (phase!=0&&phase!=1&&phase!=3&&phase!=4)||
           !read(child,0x20,layout)||!read(layout,0,vt)||vt!=base+0x10C9FA8||
           !read(layout,0x64,key)||key!=0xBA0)return false;
        owner=child;phase+=100;return true;
    }
    if(!includeMainMenu||vt!=base+0x10C7030)return false;
    // Phase 13 dispatches the difficulty confirmation sound, between the
    // selected phase-12 panel and its phase-14 exit animation (B75E1/B76A5).
    // It still owns the same BA0 menu; excluding it exposes one portrait frame.
    if(!menuExit&&phase!=1&&phase!=2&&phase!=3&&phase!=9&&phase!=10&&phase!=11&&
       phase!=12&&phase!=13&&phase!=14&&phase!=15&&phase!=16&&phase!=17&&phase!=18)return false;
    return read(owner,0x20,layout)&&read(layout,0,vt)&&vt==base+0x10C9FA8&&
        read(layout,0x64,key)&&key==0xBA0;
}
}
