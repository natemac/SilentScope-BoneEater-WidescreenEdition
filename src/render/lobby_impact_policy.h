#pragma once
#include "render/gui_font_identity.h"
#include "render/score_lobby_identity.h"
#include <cstdio>
namespace bone_eater::render {
struct LobbyImpact { float x=0,y=0,width=0,height=0,angle=0,alpha=0; };
// Undo ONLY the cabinet conversion in native 139920. Keep the original sprite
// dimensions in portrait pixels so the artwork retains its apparent size.
inline bool lobbyImpactPose(float x,float y,float sx,float sy,float angle,float alpha,LobbyImpact& out) {
    for(float v:{x,y,sx,sy,angle,alpha})if(!std::isfinite(v))return false;
    if(sx<=0||sy<=0||sx>4||sy>4||alpha<0||alpha>1||std::abs(angle)>100)return false;
    out={x*(800.f/768.f),y*(1280.f/1366.f),320.f*sx*(1080.f/1366.f),280.f*sy*(1080.f/1366.f),angle,alpha};
    return out.x>=-320&&out.x<=2240&&out.y>=-280&&out.y<=1360;
}
template<class Read> bool lobbyImpactNode(std::uintptr_t base,std::uintptr_t gun,unsigned index,
    std::uintptr_t adapter,Read read,LobbyImpact& pose) noexcept {
    using gui_font_detail::field;
    using Node=std::array<unsigned char,0x118>;
    std::uintptr_t vt=0,p=0,linked=0,camera=0;unsigned group=0,selector=0;
    if(index>=10||!gun||!read(gun,0,vt)||vt!=base+0x10CA460||!read(gun,0xE0,group)||!group||group>=32||
        !read(gun,0x1A0+8*index,p))return false;
    Node n{};if(!p||!read(p,0,n)||field<std::uintptr_t>(n,0)!=base+0x10CEBA8||
        field<std::uintptr_t>(n,8)!=p||field<unsigned>(n,0x3C)!=group||n[0xC9]!=1||
        field<std::uintptr_t>(n,0x30)!=adapter)return false;
    char name[32]{};std::snprintf(name,sizeof(name),"lcd_m_hole_%u",index);
    if(!gui_font_detail::named(n,name)||field<float>(n,0x98)!=320||field<float>(n,0x9C)!=280||
        field<float>(n,0x90)!=.5f||field<float>(n,0x94)!=.5f)return false;
    auto parent=field<std::uintptr_t>(n,0x10);
    for(const char* expected:{"root_hole","root"}) {
        Node ancestor{};
        if(!parent||!read(parent,0,ancestor)||field<std::uintptr_t>(ancestor,8)!=parent||
            field<unsigned>(ancestor,0x3C)!=group||ancestor[0xC9]!=1||!gui_font_detail::named(ancestor,expected))return false;
        auto type=field<std::uintptr_t>(ancestor,0);
        if(type!=base+0x10CEBA8&&type!=base+0x10CEB48)return false;
        parent=field<std::uintptr_t>(ancestor,0x10);
    }
    if(parent)return false;
    auto renderer=field<std::uintptr_t>(n,0x110);
    if(!read(adapter,0,vt)||vt!=base+0x10CEC08||!read(adapter,0x40,linked)||linked!=renderer||
        !read(renderer,0,vt)||vt!=base+0x10CC0D8||!read(renderer,0x570,selector)||selector!=2||
        !read(renderer,0x1A8,camera)||!camera||!read(base,0x13DB5B0,linked)||camera!=linked)return false;
    return lobbyImpactPose(field<float>(n,0x70),field<float>(n,0x74),field<float>(n,0xA8),field<float>(n,0xAC),
        field<float>(n,0xB4),field<float>(n,0xBC),pose);
}
}
