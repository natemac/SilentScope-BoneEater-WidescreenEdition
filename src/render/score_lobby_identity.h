#pragma once
#include <array>
#include <cstdint>
#include <cmath>
namespace bone_eater::render {
template<class Read> bool scoreLobbyOwner(std::uintptr_t base,Read read,std::uintptr_t& owner,unsigned& phase,bool presentationTransitions=false) noexcept {
 std::uintptr_t manager=0,sceneOwner=0,messages=0,vt=0,layout=0;unsigned scene=0,key=0;int pending=0;std::array<unsigned char,2> stopped{};
 return read(base,0x13DD000,manager)&&read(manager,0,vt)&&vt==base+0x10C6B38&&
 read(manager,0x38,scene)&&scene==5&&read(manager,0x3C,pending)&&pending==-1&&
 read(manager,0x40,sceneOwner)&&read(sceneOwner,0,vt)&&vt==base+0x10C6E48&&
 read(base,0x13DCFB0,messages)&&read(messages,0,vt)&&vt==base+0x10CE298&&
 read(messages,8,owner)&&read(owner,0,vt)&&vt==base+0x10CAB18&&
 read(owner,8,stopped)&&!stopped[1]&&read(owner,0x10,phase)&&
 // Native cleanup keeps rendering its live BA0 through states19/20 even
 // with stop-request byte8 set. Input retains the original strict contract.
 (!stopped[0]||(presentationTransitions&&(phase==19||phase==20)))&&
 ((phase>=1&&phase<=7)||phase==19||phase==20||(presentationTransitions&&phase==0))&&read(owner,0x20,layout)&&
 read(layout,0,vt)&&vt==base+0x10C9FA8&&read(layout,0x64,key)&&key==0xBA0;
}
// The resident front backing outlives the lobby artwork during load/drain.
// Never dereference roots while the layout is loading or being destroyed.
template<class Read> bool scoreLobbyPresentation(std::uintptr_t base,Read read,
 std::uintptr_t& owner,std::uintptr_t& layout,float& alpha) noexcept {
 unsigned phase=0,state=0;std::uintptr_t root=0,vt=0;
 if(!scoreLobbyOwner(base,read,owner,phase,true)||!read(owner,0x20,layout)||
    !read(layout,0x18,state))return false;
 if(((phase==0||phase==1)&&state==1)||((phase==19||phase==20)&&state==3)||(phase==20&&state==0)) {
   // Phase20 waits for both layouts to reach0 before destroying them.
   // Released BA0 still owns the resident front backing for this frame.
   alpha=0;return true;
 }
 if(state!=2)return false;
 if(!read(layout,0x428,root))return false;
 if(!root) {
   alpha=0;return phase==0||phase==1;
 }
 return read(root,0,vt)&&vt==base+0x10CEB48&&read(root,0xBC,alpha)&&
   std::isfinite(alpha)&&alpha>=0&&alpha<=1;
}
}
