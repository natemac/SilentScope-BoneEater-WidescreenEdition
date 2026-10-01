#include "render/loading_screen_identity.h"
#include <cassert>
#include <map>
#include <vector>
using namespace bone_eater::render;
constexpr std::uintptr_t base=0x10000000,manager=0x20000000,scene=0x20001000,
    ui=0x20003000,splash=0x20004000,textures=0x20005000,builder=0x20006000;
struct Memory {
 std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
 template<class T> void put(std::uintptr_t p,std::size_t o,T v){auto& b=blocks.at(p);assert(o+sizeof(v)<=b.size());std::memcpy(b.data()+o,&v,sizeof(v));}
 template<class T> bool read(std::uintptr_t p,std::size_t o,T& v){auto i=blocks.find(p);if(i==blocks.end()||o>i->second.size()||sizeof(v)>i->second.size()-o)return false;std::memcpy(&v,i->second.data()+o,sizeof(v));return true;}
 Memory(){
  blocks[base].resize(0x13dd008);blocks[manager].resize(0x48);blocks[scene].resize(0x1194);
  blocks[ui].resize(0xf0);blocks[splash].resize(0xa8);blocks[textures].resize(8);blocks[builder].resize(8);
  put(base,0x13dd000,manager);put(manager,0,base+0x10c6b38);put(manager,0x38,5u);put(manager,0x3c,~0u);put(manager,0x40,scene);
  put(scene,0,base+0x10c6e48);put(scene,0x1190,1u);put(base,0x13dcf78,ui);put(ui,0,base+0x10ca298);put(ui,0x38,manager);put(ui,0xe8,splash);
  put(splash,0,base+0x10ca680);put(splash,0x80,textures);put(splash,0x88,builder);put(textures,0,base+0x10c9d70);put(builder,0,base+0x10cec80);
 }
 bool capture(LoadingScreenIdentity& out){return readLoadingScreen(base,[&](auto p,auto o,auto& v){return read(p,o,v);},out);}
};
int main(){
 Memory good;LoadingScreenIdentity id;assert(good.capture(id));assert(id.phase==1&&id.splash==splash);
 good.put(scene,0x1190,11u);assert(good.capture(id));
 auto reject=[&](auto change){auto bad=good;change(bad);assert(!bad.capture(id));assert(!id.scene&&!id.splash&&!id.phase);};
 reject([](auto& m){m.put(scene,0x1190,2u);}); // actual gameplay must never be reframed
 reject([](auto& m){m.put(manager,0x38,1u);});
 reject([](auto& m){m.put(manager,0x3c,5u);});
 reject([](auto& m){m.put(scene,0,base+0x10c7278);});
 reject([](auto& m){m.put(ui,0x38,manager+8);});
 reject([](auto& m){m.put(splash,0,base+0x10ca688);});
 reject([](auto& m){m.blocks.erase(textures);});
 reject([](auto& m){m.put(builder,0,base+0x10cec88);});
 // Initial visible card while the builder is still loading.
 auto early=good;early.put(splash,0x88,std::uintptr_t{});
 early.put(splash,0x48,1u);early.put(splash,0x4c,1u);
 assert(early.capture(id)&&id.phase==11);
 early.put(scene,0x1190,1u);assert(!early.capture(id));
 early.put(scene,0x1190,2u);assert(!early.capture(id));
 reject([](auto& m){m.put(splash,0x88,std::uintptr_t{});});
 // Real phase-2 handoff: local fade cleared, cached white still opaque.
 constexpr std::uintptr_t resident=0x30000000,layout=0x30001000,root=0x30002000,
     leaf=0x30003000,record=0x30004000;
 auto tail=good;tail.put(scene,0x1190,2u);tail.put(splash,0x48,2u);tail.put(splash,0x4c,1u);
 tail.blocks[resident].resize(0x130);tail.blocks[layout].resize(0x430);
 tail.blocks[root].resize(0xd0);tail.blocks[leaf].resize(0xd0);tail.blocks[record].resize(0x10);
 tail.put(base,0x13dcf48,resident);tail.put(resident,0x128,layout);
 tail.put(layout,0,base+0x10c9fa8);tail.put(layout,0x64,0xbaau);tail.put(layout,0x428,root);
 tail.put(root,0,base+0x10ceb48);tail.put(root,8,root);tail.put(root,0x18,leaf);
 tail.put(root,0xbc,1.f);tail.put(root,0xc9,static_cast<unsigned char>(1));
 tail.put(leaf,0,base+0x10ceba8);tail.put(leaf,8,leaf);tail.put(leaf,0x10,root);tail.put(leaf,0x30,record);
 std::memcpy(tail.blocks[leaf].data()+0x40,"white",6);
 tail.put(leaf,0xb8,1.f);tail.put(leaf,0xbc,1.f);tail.put(leaf,0xc9,static_cast<unsigned char>(1));
 tail.put(record,0,base+0x10cec08);tail.put(record,8,0xffffffffu);
 assert(tail.capture(id)&&id.phase==2);
 auto rejectTail=[&](auto change){auto bad=tail;change(bad);assert(!bad.capture(id));assert(id==LoadingScreenIdentity{});};
 rejectTail([&](auto& m){m.put(leaf,0xbc,0.f);m.put(record,8,0x00ffffffu);}); // normal gameplay
 rejectTail([&](auto& m){m.put(leaf,0xbc,0.99f);}); // no translucent gameplay crop
 rejectTail([&](auto& m){m.put(record,8,0xff000000u);});
 rejectTail([&](auto& m){m.put(layout,0x58,1.f);}); // only cleared local fade
 rejectTail([&](auto& m){m.put(root,0xb8,1.f);});
 rejectTail([&](auto& m){m.put(root,0xc9,static_cast<unsigned char>(0));});
 rejectTail([&](auto& m){m.put(leaf,0x10,root+8);});
 rejectTail([&](auto& m){m.put(leaf,0x40,'x');});
 rejectTail([&](auto& m){m.blocks.erase(record);});
 rejectTail([&](auto& m){m.put(splash,0xa0,root);});
 rejectTail([&](auto& m){m.put(splash,0x4c,0u);});
 rejectTail([&](auto& m){m.put(scene,0x1190,3u);});
}
