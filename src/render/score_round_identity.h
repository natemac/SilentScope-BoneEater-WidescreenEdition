#pragma once
#include <array>
#include <cstdint>
namespace bone_eater::render {
template<class Read> bool scoreRoundInfoOwner(std::uintptr_t base,Read read,std::uintptr_t& owner,unsigned& state) noexcept {
 std::uintptr_t manager=0,messages=0,sceneOwner=0,vt=0;unsigned scene=0;int pending=0;
 if(!read(base,0x13DD000,manager)||!read(manager,0,vt)||vt!=base+0x10C6B38||
 !read(manager,0x38,scene)||scene!=5||!read(manager,0x3C,pending)||pending!=-1||
 !read(manager,0x40,sceneOwner)||!read(sceneOwner,0,vt)||vt!=base+0x10C6E48||
 !read(base,0x13DCFB0,messages)||!read(messages,0,vt)||vt!=base+0x10CE298)return false;
 for(unsigned offset:{8u,16u}) {std::array<unsigned char,2> stopped{};
  if(read(messages,offset,owner)&&read(owner,0,vt)&&vt==base+0x10CAB50&&
   read(owner,8,stopped)&&!stopped[0]&&!stopped[1]&&read(owner,0x10,state)&&state>=3&&state<=22)return true;
 }return false;
}
}
