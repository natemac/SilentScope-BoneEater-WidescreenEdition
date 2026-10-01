#pragma once
#include "render/gui_font_identity.h"

namespace bone_eater::render {
// Local Score Attack identity, lower score digits and gauge, at native submission.
// No material suppression: YOU shares its image batch with unrelated HUD art.
template<class Read>
bool scoreGameplayLabel(std::uintptr_t base,std::uintptr_t record,bool font,Read read) noexcept {
    using gui_font_detail::field;
    std::uintptr_t vt=0,scene=0,battle=0,manager=0,owner=0,bottom=0,root=0;
    unsigned state=0,group=0;int pending=0;
    if(!record||!read(record,0,vt)||vt!=base+(font?0x10CEC58:0x10CEC08)||
       !read(base,0x13DD000,scene)||!read(scene,0,vt)||vt!=base+0x10C6B38||
       !read(scene,0x38,state)||state!=5||!read(scene,0x3C,pending)||pending!=-1||
       !read(scene,0x40,battle)||!read(battle,0,vt)||vt!=base+0x10C6E48||
       !read(base,0x13DCF78,manager)||!read(manager,0,vt)||vt!=base+0x10CA298||
       !read(manager,0x48,state)||state!=3||!read(manager,0xD0,owner)||
       !read(owner,0,vt)||vt!=base+0x10CA1D8||!read(owner,0x48,state)||state!=3||
       !read(owner,0xE0,group)||!group||group>=32||
       !read(owner,0x130,bottom)||!bottom||!read(owner,0x190,root)||!root)return false;
    using Node=std::array<unsigned char,0x118>;
    auto node=[&](std::uintptr_t p,std::uintptr_t parent,bool isFont,Node& bytes) {
        return p&&read(p,0,bytes)&&field<std::uintptr_t>(bytes,0)==base+(isFont?0x10CEAE8:0x10CEBA8)&&
            field<std::uintptr_t>(bytes,8)==p&&(!parent||field<std::uintptr_t>(bytes,0x10)==parent)&&
            field<unsigned>(bytes,0x3C)==group;
    };
    Node bytes{};
    if(!node(bottom,0,false,bytes)||!gui_font_detail::named(bytes,"root_bottom")||bytes[0xC9]!=1||
       !node(root,bottom,false,bytes)||!gui_font_detail::named(bytes,"root_ShootingRangeBtm")||bytes[0xC9]!=1)return false;
    auto find=[&](std::uintptr_t parent,const char* name,bool isFont,std::uintptr_t& found,Node& result) {
        std::uintptr_t child=0;if(!read(parent,0x18,child))return false;
        for(unsigned i=0;child&&i<32;++i) {
            Node candidate{};
            if(!read(child,0,candidate)||field<std::uintptr_t>(candidate,8)!=child||
               field<std::uintptr_t>(candidate,0x10)!=parent||field<unsigned>(candidate,0x3C)!=group)return false;
            if(name?gui_font_detail::named(candidate,name):field<std::uintptr_t>(candidate,0x30)==record) {
                if(field<std::uintptr_t>(candidate,0)!=base+(isFont?0x10CEAE8:0x10CEBA8)||candidate[0xC9]!=1)return false;
                found=child;result=candidate;return true;
            }
            child=field<std::uintptr_t>(candidate,0x28);
        }
        return false;
    };
    std::uintptr_t label=0,parent=root;
    bool matched=false;
    if(!font&&find(root,"lcd_bt_YOU",false,label,bytes))
        matched=field<std::uintptr_t>(bytes,0x30)==record;
    if(!matched) {
        if(!find(root,"lcd_bt_SRSGRoot0",false,parent,bytes))return false;
        if(font) {
            matched=find(parent,"lcd_bt_ScoreGaugePLName0",true,label,bytes)&&
                field<std::uintptr_t>(bytes,0x30)==record;
        } else if(find(parent,nullptr,false,label,bytes)) {
            for(const char* name:{"lcd_bt_ScoreGaugePlayers0","lcd_bt_ScoreGaugeEnd0",
                "lcd_bt_ScoreGaugeJuice0","lcd_bt_ScoreGaugeScore0_0","lcd_bt_ScoreGaugeScore0_1",
                "lcd_bt_ScoreGaugeScore0_2","lcd_bt_ScoreGaugeScore0_3",
                "lcd_bt_ScoreGaugeScore0_4","lcd_bt_ScoreGaugeScore0_5"}) {
                if(gui_font_detail::named(bytes,name)) {
                    matched=true;break;
                }
            }
        }
        if(!matched)return false;
    }
    std::uintptr_t linked=0,renderer=field<std::uintptr_t>(bytes,0x110),camera=0;
    if(!renderer||!read(record,0x40,linked)||linked!=renderer)return false;
    if(font) {
        unsigned selector=0;
        if(!read(renderer,0x90,selector)||selector!=2||!read(renderer,0x18,renderer))return false;
    }
    unsigned selector=0;
    return read(renderer,0,vt)&&vt==base+0x10CC0D8&&read(renderer,0x570,selector)&&selector==2&&
        read(renderer,0x1A8,camera)&&camera&&read(base,0x13DB5B0,linked)&&camera==linked;
}
// The rear Score Attack row is made of two Win9 panels, each owning nine
// anonymous sprites. Suppress those exact submissions, never their shared atlas.
template<class Read>
bool scoreGameplayBackplate(std::uintptr_t base,std::uintptr_t record,Read read) noexcept {
    using gui_font_detail::field;
    std::uintptr_t vt=0,renderer=0,scene=0,battle=0,manager=0,owner=0,root=0;
    unsigned state=0,group=0,selector=0;int pending=0;
    if(!record||!read(record,0,vt)||vt!=base+0x10CEC08||
       !read(record,0x40,renderer)||!renderer||!read(renderer,0,vt)||vt!=base+0x10CC0D8||
       !read(renderer,0x570,selector)||selector!=0||
       !read(base,0x13DD000,scene)||!read(scene,0,vt)||vt!=base+0x10C6B38||
       !read(scene,0x38,state)||state!=5||!read(scene,0x3C,pending)||pending!=-1||
       !read(scene,0x40,battle)||!read(battle,0,vt)||vt!=base+0x10C6E48||
       !read(base,0x13DCF78,manager)||!read(manager,0,vt)||vt!=base+0x10CA298||
       !read(manager,0x48,state)||state!=3||!read(manager,0xD8,owner)||
       !read(owner,0,vt)||vt!=base+0x10CA200||!read(owner,0x48,state)||state!=3||
       !read(owner,0x98,group)||!group||group>=32||!read(owner,0xA0,root))return false;
    using Node=std::array<unsigned char,0x118>;
    auto node=[&](std::uintptr_t p,std::uintptr_t parent,std::uintptr_t type,unsigned expectedGroup,Node& n) {
        return p&&read(p,0,n)&&field<std::uintptr_t>(n,0)==base+type&&
            field<std::uintptr_t>(n,8)==p&&field<std::uintptr_t>(n,0x10)==parent&&
            field<unsigned>(n,0x3C)==expectedGroup&&n[0xC9]==1;
    };
    Node bytes{};
    if(!node(root,0,0x10CEB48,group,bytes)||!gui_font_detail::named(bytes,"root"))return false;
    auto find=[&](std::uintptr_t parent,const char* name,std::uintptr_t type,std::uintptr_t& found,Node& result) {
        std::uintptr_t child=0;if(!read(parent,0x18,child))return false;
        for(unsigned i=0;child&&i<32;++i) {
            Node n{};
            if(!read(child,0,n)||field<std::uintptr_t>(n,8)!=child||
               field<std::uintptr_t>(n,0x10)!=parent||field<unsigned>(n,0x3C)!=group)return false;
            if(gui_font_detail::named(n,name)) {
                if(!node(child,parent,type,group,result))return false;
                found=child;return true;
            }
            child=field<std::uintptr_t>(n,0x28);
        }
        return false;
    };
    std::uintptr_t sr=0;
    if(!find(root,"root_SR",0x10CEBA8,sr,bytes)||field<std::uintptr_t>(bytes,0x110)!=renderer)return false;
    for(const char* name:{"m_bt_SRYou","m_bt_SRScore0"}) {
        std::uintptr_t panel=0;
        if(!find(sr,name,0x10CE9C8,panel,bytes)||field<std::uintptr_t>(bytes,0x110)!=renderer)continue;
        std::array<std::uintptr_t,9> pieces{};
        if(!read(panel,0x118,pieces))continue;
        for(auto piece:pieces) {
            if(node(piece,panel,0x10CEBA8,0xffffffffu,bytes)&&
               field<std::uintptr_t>(bytes,0x30)==record&&field<std::uintptr_t>(bytes,0x110)==renderer)return true;
        }
    }
    return false;
}

}
