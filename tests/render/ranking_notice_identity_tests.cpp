#include "render/ranking_notice_identity.h"
#include <cassert>
#include <map>
#include <vector>
using namespace bone_eater::render;
constexpr std::uintptr_t base=0x10000000,manager=0x20000000,title=0x20001000,layout=0x20002000,root=0x20008000,font=0x20009000,bg=0x2000A000,adapter=0x2000B000,wrapper=0x2000C000,renderer=0x2000D000,material=0x2000E000;
struct Memory {
 std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
 template<class T>void put(std::uintptr_t p,std::size_t o,T v){auto& b=blocks.at(p);assert(o+sizeof(v)<=b.size());std::memcpy(b.data()+o,&v,sizeof(v));}
 template<class T>bool read(std::uintptr_t p,std::size_t o,T& v){auto i=blocks.find(p);if(i==blocks.end()||o>i->second.size()||sizeof(v)>i->second.size()-o)return false;std::memcpy(&v,i->second.data()+o,sizeof(v));return true;}
 Memory(){
  blocks[base].resize(0x13DD008);blocks[manager].resize(0x48);blocks[title].resize(0x20);blocks[layout].resize(0x3CA0);
  blocks[root].resize(0xD0);blocks[font].resize(0x130);blocks[bg].resize(0xD0);blocks[adapter].resize(0x48);blocks[wrapper].resize(0x20);blocks[renderer].resize(0x580);blocks[material].resize(8);
  put(base,0x13DD000,manager);put(manager,0,base+0x10C6B38);put(manager,0x38,1u);put(manager,0x3C,-1);put(manager,0x40,title);put(manager,0x10,layout);
  put(title,0,base+0x10C7278);put(title,0x14,12u);put(layout,0,base+0x10C9FA8);put(layout,0x68,2993u);put(layout,0x3C98,2u);put(layout,0x430,root);
  for(auto n:{root,font,bg}){put(n,8,n);put(n,0x3C,10u);}
  put(root,0,base+0x10CEB48);put(root,0x18,font);std::memcpy(blocks[root].data()+0x40,"Root",5);
  put(font,0,base+0x10CEAE8);put(font,0x10,root);put(font,0x28,bg);std::memcpy(blocks[font].data()+0x40,"Message",8);
  put(bg,0,base+0x10CEBA8);put(bg,0x10,root);std::memcpy(blocks[bg].data()+0x40,"Message_BG",11);
  put(font,0x30,adapter);put(font,0x110,wrapper);put(adapter,0,base+0x10CEC58);put(adapter,0x40,wrapper);put(wrapper,0x18,renderer);
  put(renderer,0,base+0x10CC0D8);put(renderer,0x570,2u);put(renderer,0x308,material);put(material,0,base+0x706570);
 }
 bool capture(RankingNoticeIdentity& out){return readRankingNotice(base,[&](auto p,auto o,auto& v){return read(p,o,v);},out);}
};
int main(){Memory good;RankingNoticeIdentity id;assert(good.capture(id));assert(id.material==material&&id.background==bg);
 auto reject=[&](auto mutate){auto bad=good;mutate(bad);assert(!bad.capture(id));assert(!id.font&&!id.background&&!id.material);};
 reject([](auto& m){m.put(manager,0x38,5u);});
 reject([](auto& m){m.put(manager,0x3C,2);});
 reject([](auto& m){m.put(title,0x14,7u);});
 reject([](auto& m){m.put(layout,0x68,2994u);});
 reject([](auto& m){m.put(bg,0x10,root+8);});
 reject([](auto& m){m.put(font,0x3C,11u);});
 reject([](auto& m){m.put(bg,0x28,font);});
 reject([](auto& m){m.blocks[font][0x40]='X';});
 reject([](auto& m){m.put(adapter,0x40,wrapper+8);});
 reject([](auto& m){m.put(renderer,0x570,0u);});
 reject([](auto& m){m.blocks.erase(material);});
}
