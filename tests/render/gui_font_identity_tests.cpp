#include "render/gui_font_identity.h"
#include <cassert>
#include <map>
#include <vector>
using namespace bone_eater::render;
namespace {
constexpr std::uintptr_t module=0x10000000, manager=0x20000000, owner=0x20001000;
constexpr std::uintptr_t gui=0x20002000, parent=0x20003000, adapter=0x20004000;
constexpr std::uintptr_t wrapper=0x20005000, renderer=0x20006000, material=0x20007000, camera=0x20008000;
struct Memory {
    std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
    template<class T> void put(std::uintptr_t base, std::size_t off, T value) {
        auto& bytes=blocks.at(base); assert(off+sizeof(T)<=bytes.size());
        std::memcpy(bytes.data()+off,&value,sizeof(T));
    }
    template<class T> bool read(std::uintptr_t base,std::size_t off,T& value) const {
        const auto found=blocks.find(base);
        if(found==blocks.end() || off>found->second.size() || sizeof(T)>found->second.size()-off)return false;
        std::memcpy(&value,found->second.data()+off,sizeof(T)); return true;
    }
    Memory() {
        blocks[module].resize(0x13dd000); blocks[manager].resize(0xe0); blocks[owner].resize(0x200);
        blocks[gui].resize(0x130); blocks[parent].resize(0x60); blocks[adapter].resize(0x80);
        blocks[wrapper].resize(0xa0); blocks[renderer].resize(0x580); blocks[material].resize(0x70);
        put(module,0x13dcf78,manager); put(module,0x13db5b0,camera);
        put(manager,0,module+0x10ca298); put(manager,0x48,3u); put(manager,0xd0,owner);
        put(owner,0,module+0x10ca1d8); put(owner,0x48,3u); put(owner,0xe0,4u);
        put(owner,0x140,parent); put(owner,0x1b8,gui);
        put(gui,0,module+0x10ceae8); put(gui,8,gui); put(gui,0x10,parent); put(gui,0x3c,4u);
        put(gui,0x30,adapter); put(gui,0x110,wrapper); put(gui,0x12c,2u);
        std::memcpy(blocks[gui].data()+0x40,"lcd_bt_PlayerName",18);
        put(parent,0,module+0x10ceba8); put(parent,8,parent); put(parent,0x3c,4u);
        std::memcpy(blocks[parent].data()+0x40,"root_upper",11);
        put(adapter,0,module+0x10cec58); put(adapter,0x40,wrapper);
        put(wrapper,0x18,renderer); put(wrapper,0x90,2u);
        put(renderer,0,module+0x10cc0d8); put(renderer,0x570,2u);
        put(renderer,0x1a0,0x800000u); put(renderer,0x1a8,camera); put(renderer,0x308,material);
        put(material,0,module+0x706570);
    }
    bool capture(GuiFontIdentity& result,std::uint64_t frame=9) const {
        return readPlayerNameIdentity(module,owner,gui,frame,
            [this](auto base,auto off,auto& value) { return read(base,off,value); },result);
    }
};
}
int main() {
    Memory good;
    GuiFontIdentity first, second;
    assert(good.capture(first)); assert(good.capture(second)); assert(sameGuiFontIdentity(first,second));
    assert(first.renderer==renderer && first.material==material && first.wrapper==wrapper);
    assert(good.capture(second,10)); assert(!sameGuiFontIdentity(first,second));
    assert(!good.capture(second,0) && !second.frame);
    // Every case starts from the real owner chain. Reject swapped wrappers,
    // ordinary CSprites, wrong selection/camera, foreign/reparented fonts,
    // and unreadable/destroyed objects without returning stale identities.
    auto reject=[&](auto mutate) { Memory bad=good; mutate(bad); second=first;
        assert(!bad.capture(second)); assert(!second.frame && !second.gui && !second.material); };
    reject([](Memory& m){m.put(adapter,0x40,wrapper+8);});
    reject([](Memory& m){m.put(renderer,0,module+0x10cd1e8);});
    reject([](Memory& m){m.put(gui,0,module+0x10ceba8);});
    reject([](Memory& m){m.put(gui,0x10,parent+8);});
    reject([](Memory& m){m.put(parent,0x3c,5u);});
    reject([](Memory& m){m.blocks[gui][0x40]='X';});
    reject([](Memory& m){m.put(gui,0x12c,0u);});
    reject([](Memory& m){m.put(wrapper,0x90,0u);});
    reject([](Memory& m){m.put(renderer,0x570,0u);});
    reject([](Memory& m){m.put(renderer,0x1a0,0u);});
    reject([](Memory& m){m.put(renderer,0x1a8,camera+8);});
    reject([](Memory& m){m.put(manager,0x48,2u);});
    reject([](Memory& m){m.put(owner,0x48,2u);});
    reject([](Memory& m){m.blocks.erase(wrapper);});
    reject([](Memory& m){m.blocks[renderer].resize(0x308);});
}
