#pragma once
#include "render/gui_font_identity.h"
namespace bone_eater::render {
struct RankingNoticeIdentity {std::uintptr_t background=0, font=0, material=0;};
template<class Reader>
bool readRankingNotice(std::uintptr_t module,Reader&& read,RankingNoticeIdentity& out) noexcept {
    using gui_font_detail::field;
    out={};std::uintptr_t manager=0,title=0,layout=0,root=0,vt=0,node=0;
    unsigned value=0;int pending=0;
    if(!read(module,0x13DD000,manager)||!read(manager,0,vt)||vt!=module+0x10C6B38||
       !read(manager,0x38,value)||value!=1||!read(manager,0x3C,pending)||pending!=-1||
       !read(manager,0x40,title)||!read(title,0,vt)||vt!=module+0x10C7278||
       !read(title,0x14,value)||value!=12||!read(manager,0x10,layout)||
       !read(layout,0,vt)||vt!=module+0x10C9FA8||!read(layout,0x68,value)||value!=2993||
       !read(layout,0x3C98,value)||value!=2||!read(layout,0x430,root))return false;
    std::array<unsigned char,0xD0> rb{};
    if(!read(root,0,rb)||field<std::uintptr_t>(rb,0)!=module+0x10CEB48||
       field<std::uintptr_t>(rb,8)!=root||field<std::uintptr_t>(rb,0x10)||!gui_font_detail::named(rb,"Root"))return false;
    const auto group=field<unsigned>(rb,0x3C);
    if(!group||group>=32)return false;
    node=field<std::uintptr_t>(rb,0x18);RankingNoticeIdentity id;
    for(unsigned i=0;i<6&&node;++i){
        std::array<unsigned char,0xD0> b{};
        if(!read(node,0,b)||field<std::uintptr_t>(b,8)!=node||field<std::uintptr_t>(b,0x10)!=root||field<unsigned>(b,0x3C)!=group)return false;
        if(gui_font_detail::named(b,"Message")&&field<std::uintptr_t>(b,0)==module+0x10CEAE8){
            if(id.font)return false;id.font=node;
        }
        if(gui_font_detail::named(b,"Message_BG")&&field<std::uintptr_t>(b,0)==module+0x10CEBA8){
            if(id.background)return false;id.background=node;
        }
        node=field<std::uintptr_t>(b,0x28);
    }
    if(node||!id.font||!id.background)return false;
    std::uintptr_t adapter=0,wrapper=0,linked=0,renderer=0;
    if(!read(id.font,0x30,adapter)||!read(adapter,0,vt)||vt!=module+0x10CEC58||
       !read(id.font,0x110,wrapper)||!read(adapter,0x40,linked)||linked!=wrapper||
       !read(wrapper,0x18,renderer)||!read(renderer,0,vt)||vt!=module+0x10CC0D8||
       !read(renderer,0x570,value)||value!=2||!read(renderer,0x308,id.material)||
       !read(id.material,0,vt)||vt!=module+0x706570)return false;
    out=id;return true;
}
}
