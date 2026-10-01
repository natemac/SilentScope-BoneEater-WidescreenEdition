#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace bone_eater::render {
struct LoadingScreenIdentity {
    std::uintptr_t manager=0, scene=0, ui=0, splash=0;
    unsigned phase=0;
    bool operator==(const LoadingScreenIdentity&) const = default;
};
// Pinned loading phases, plus the opaque resident white left in the first
// battle update. Never admit an ordinary phase-2 gameplay frame.
template<class Read> bool readLoadingScreen(std::uintptr_t base, Read read,
                                            LoadingScreenIdentity& out) noexcept {
    out={}; LoadingScreenIdentity id;
    std::array<unsigned char,0x48> manager{};
    std::array<unsigned char,0xf0> ui{};
    std::array<unsigned char,0xa8> splash{};
    auto ptr=[](const auto& bytes,std::size_t offset) {
        std::uintptr_t value=0;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
    };
    auto number=[](const auto& bytes,std::size_t offset) {
        unsigned value=0;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
    };
    std::uintptr_t vt=0, textures=0, builder=0;
    if(!base||!read(base,0x13DD000,id.manager)||!read(id.manager,0,manager)||
       ptr(manager,0)!=base+0x10c6b38||number(manager,0x38)!=5||number(manager,0x3c)!=~0u)return false;
    id.scene=ptr(manager,0x40);
    if(!read(id.scene,0,vt)||vt!=base+0x10c6e48||!read(id.scene,0x1190,id.phase)||
       (id.phase!=1&&id.phase!=11&&id.phase!=2)||!read(base,0x13DCF78,id.ui)||!read(id.ui,0,ui)||
       ptr(ui,0)!=base+0x10ca298||ptr(ui,0x38)!=id.manager)return false;
    id.splash=ptr(ui,0xe8);
    if(!read(id.splash,0,splash)||ptr(splash,0)!=base+0x10ca680)return false;
    textures=ptr(splash,0x80);builder=ptr(splash,0x88);
    if(!read(textures,0,vt)||vt!=base+0x10c9d70)return false;
    // The loading card is already drawn during the initial phase-11 wait,
    // before the splash builder is allocated. This exact state was observed
    // through the transition; waiting for the builder exposes portrait frames.
    if(!builder) {
        if(id.phase!=11||number(splash,0x48)!=1||number(splash,0x4c)!=1)return false;
    } else if(!read(builder,0,vt)||vt!=base+0x10cec80)return false;
    if(id.phase==2) {
        // The sampled handoff has cleared local fade/root alpha to zero, but
        // GUI propagation has not yet cleared the cached leaf or draw color.
        // Require that exact opaque white, not a timer or phase-2 allowance.
        if(number(splash,0x48)!=2||number(splash,0x4c)!=1||ptr(splash,0xa0))return false;
        std::uintptr_t resident=0,layout=0,root=0,leaf=0,record=0;
        std::array<unsigned char,0x70> l{};
        std::array<unsigned char,0xd0> r{},f{};
        auto scalar=[](const auto& bytes,std::size_t offset) {
            float value=0;std::memcpy(&value,bytes.data()+offset,sizeof(value));return value;
        };
        if(!read(base,0x13DCF48,resident)||!read(resident,0x128,layout)||!read(layout,0,l)||
           ptr(l,0)!=base+0x10c9fa8||number(l,0x64)!=0xbaa||scalar(l,0x58)!=0||
           !read(layout,0x428,root)||!read(root,0,r)||ptr(r,0)!=base+0x10ceb48||
           ptr(r,8)!=root||ptr(r,0x10)||scalar(r,0xb8)!=0||scalar(r,0xbc)!=1||r[0xc9]!=1)return false;
        leaf=ptr(r,0x18);
        if(!read(leaf,0,f)||ptr(f,0)!=base+0x10ceba8||ptr(f,8)!=leaf||ptr(f,0x10)!=root||
           ptr(f,0x18)||ptr(f,0x20)||ptr(f,0x28)||std::memcmp(f.data()+0x40,"white",6)||
           scalar(f,0xb8)!=1||scalar(f,0xbc)!=1||f[0xc9]!=1)return false;
        record=ptr(f,0x30);unsigned color=0;
        if(!read(record,0,vt)||vt!=base+0x10cec08||!read(record,8,color)||color!=0xffffffffu)return false;
    }
    out=id;return true;
}
}
