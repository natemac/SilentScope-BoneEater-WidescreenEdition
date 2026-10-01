#include "render/lobby_impact_policy.h"
#include <cassert>
#include <map>
#include <vector>
#include <limits>
using namespace bone_eater::render;
using P=std::uintptr_t;
constexpr P b=0x180000000,gun=0x1000,hole=0x2000,parent=0x3000,root=0x4000,adapter=0x5000,renderer=0x6000,camera=0x7000;
struct Memory {
 std::map<P,std::vector<unsigned char>> blocks;
 template<class T> void put(P p,P o,T v){auto& a=blocks[p];if(a.size()<o+sizeof(v))a.resize(o+sizeof(v));std::memcpy(a.data()+o,&v,sizeof(v));}
 void text(P p,const char* s){auto& a=blocks[p];std::memset(a.data()+0x40,0,32);std::strcpy(reinterpret_cast<char*>(a.data()+0x40),s);}
 template<class T> bool read(P p,P o,T& v){auto it=blocks.find(p);if(it==blocks.end()||o>it->second.size()||sizeof(v)>it->second.size()-o)return false;std::memcpy(&v,it->second.data()+o,sizeof(v));return true;}
 void node(P p,P parent,const char* s,bool f=false){blocks[p].resize(0x130);put(p,0,b+(f?0x10CEAE8:0x10CEBA8));put(p,8,p);put(p,0x10,parent);put(p,0x3c,4u);put(p,0xc9,(unsigned char)1);text(p,s);}
 Memory(){
 put(gun,0,b+0x10CA460);put(gun,0xe0,4u);put(gun,0x1a0,hole);
 node(root,0,"root");node(parent,root,"root_hole");node(hole,parent,"lcd_m_hole_0");
 put(hole,0x30,adapter);put(hole,0x110,renderer);put(hole,0x90,.5f);put(hole,0x94,.5f);put(hole,0x98,320.f);put(hole,0x9c,280.f);
 put(hole,0x70,1218.744995f);put(hole,0x74,602.450317f);put(hole,0xa8,1.f);put(hole,0xac,1.f);put(hole,0xb4,1.5f);put(hole,0xbc,.6f);
 put(adapter,0,b+0x10CEC08);put(adapter,0x40,renderer);put(renderer,0,b+0x10CC0D8);put(renderer,0x570,2u);put(renderer,0x1a8,camera);put(b,0x13DB5B0,camera);
 }
 bool match(LobbyImpact& q){return lobbyImpactNode(b,gun,0,adapter,[&](auto p,auto o,auto& v){return read(p,o,v);},q);}
};
int main(){
 Memory good;LobbyImpact q{};assert(good.match(q));
 // Captured native shot independently supplies these main coordinates.
 assert(std::abs(q.x-1269.526123f)<.01f&&std::abs(q.y-564.521484f)<.01f);
 assert(q.alpha==.6f&&q.angle==1.5f);
 auto reject=[&](auto mutate){auto m=good;mutate(m);assert(!m.match(q));};
 reject([](auto& m){m.text(hole,"lcd_m_line_v_0");});
 reject([](auto& m){m.text(parent,"root_AimCrossChair");});
 reject([](auto& m){m.put(hole,0xc9,(unsigned char)0);});
 reject([](auto& m){m.put(parent,0xc9,(unsigned char)0);});
 reject([](auto& m){m.put(root,0x10,parent);});
 reject([](auto& m){m.put(renderer,0x570,1u);});
 reject([](auto& m){m.put(renderer,0x1a8,camera+8);});
 reject([](auto& m){m.put(hole,0x3c,5u);});
 reject([](auto& m){m.put(hole,0x30,adapter+8);});
 reject([](auto& m){m.put(hole,0xbc,std::numeric_limits<float>::quiet_NaN());});
 reject([](auto& m){m.blocks.erase(parent);});
 for(auto point: {std::pair{0.f,0.f},std::pair{960.f,540.f},std::pair{1920.f,1080.f},std::pair{200.f,450.f}}){
 assert(lobbyImpactPose(point.first*.96f,point.second*(1366.f/1280),1,1,0,1,q));
 assert(std::abs(q.x-point.first)<.001f&&std::abs(q.y-point.second)<.001f);
 }
 assert(!lobbyImpactPose(0,0,0,1,0,1,q));assert(!lobbyImpactPose(0,0,1,1,0,2,q));
}
