#include "render/score_label_identity.h"
#include <cassert>
#include <map>
#include <vector>
using P=std::uintptr_t;
constexpr P b=0x180000000,scene=0x1000,battle=0x2000,mgr=0x3000,owner=0x4000,bottom=0x5000,root=0x6000,you=0x7000,local=0x8000,name=0x9000,image=0xa000,font=0xb000,wrapper=0xc000,renderer=0xd000,camera=0xe000;
struct Memory {
 std::map<P,std::vector<unsigned char>> blocks;
 template<class T> void put(P p,P o,T v){auto& a=blocks[p];if(a.size()<o+sizeof(v))a.resize(o+sizeof(v));std::memcpy(a.data()+o,&v,sizeof(v));}
 void text(P p,const char* s){auto& a=blocks[p];std::memset(a.data()+0x40,0,32);std::strcpy(reinterpret_cast<char*>(a.data()+0x40),s);}
 template<class T> bool read(P p,P o,T& v){auto it=blocks.find(p);if(it==blocks.end()||o>it->second.size()||sizeof(v)>it->second.size()-o)return false;std::memcpy(&v,it->second.data()+o,sizeof(v));return true;}
 void node(P p,P parent,const char* s,bool f=false){blocks[p].resize(0x130);put(p,0,b+(f?0x10CEAE8:0x10CEBA8));put(p,8,p);put(p,0x10,parent);put(p,0x3c,4u);put(p,0xc9,(unsigned char)1);text(p,s);}
 Memory(){
 put(b,0x13DD000,scene);put(scene,0,b+0x10C6B38);put(scene,0x38,5u);put(scene,0x3c,-1);put(scene,0x40,battle);put(battle,0,b+0x10C6E48);
 put(b,0x13DCF78,mgr);put(mgr,0,b+0x10CA298);put(mgr,0x48,3u);put(mgr,0xd0,owner);put(owner,0,b+0x10CA1D8);put(owner,0x48,3u);put(owner,0xe0,4u);put(owner,0x130,bottom);put(owner,0x190,root);
 node(bottom,0,"root_bottom");node(root,bottom,"root_ShootingRangeBtm");node(you,root,"lcd_bt_YOU");node(local,root,"lcd_bt_SRSGRoot0");node(name,local,"lcd_bt_ScoreGaugePLName0",true);
 put(root,0x18,you);put(you,0x28,local);put(local,0x18,name);put(you,0x30,image);put(you,0x110,renderer);put(name,0x30,font);put(name,0x110,wrapper);
 put(image,0,b+0x10CEC08);put(image,0x40,renderer);put(font,0,b+0x10CEC58);put(font,0x40,wrapper);put(wrapper,0x90,2u);put(wrapper,0x18,renderer);put(renderer,0,b+0x10CC0D8);put(renderer,0x570,2u);put(renderer,0x1a8,camera);put(b,0x13DB5B0,camera);
 }
 bool hide(bool f){return bone_eater::render::scoreGameplayLabel(b,f?font:image,f,[&](auto p,auto o,auto& v){return read(p,o,v);});}
};

void backplates() {
 Memory m;constexpr P mainOwner=0x11000,mainRoot=0x12000,sr=0x13000,panel=0x14000,piece=0x15000,ad=0x16000;
 m.put(mgr,0xd8,mainOwner);m.put(mainOwner,0,b+0x10CA200);m.put(mainOwner,0x48,3u);m.put(mainOwner,0x98,4u);m.put(mainOwner,0xa0,mainRoot);
 m.node(mainRoot,0,"root");m.put(mainRoot,0,b+0x10CEB48);m.node(sr,mainRoot,"root_SR");m.put(mainRoot,0x18,sr);m.put(sr,0x110,renderer);
 m.node(panel,sr,"m_bt_SRYou");m.put(panel,0,b+0x10CE9C8);m.put(panel,0x110,renderer);m.put(sr,0x18,panel);
 m.put(renderer,0x570,0u);m.put(ad,0,b+0x10CEC08);m.put(ad,0x40,renderer);
 std::array<P,9> pieces{};pieces[4]=piece;m.put(panel,0x118,pieces);
 m.node(piece,panel,"");m.put(piece,0x3c,0xffffffffu);m.put(piece,0x30,ad);m.put(piece,0x110,renderer);
 auto hide=[](Memory& x){return bone_eater::render::scoreGameplayBackplate(b,ad,[&](auto p,auto o,auto& v){return x.read(p,o,v);});};
 assert(hide(m));m.text(panel,"m_bt_SRScore0");assert(hide(m));
 auto rejects=[&](auto change){auto x=m;change(x);assert(!hide(x));};
 rejects([](auto& x){x.put(scene,0x38,4u);});rejects([](auto& x){x.put(scene,0x3c,6);});
 rejects([](auto& x){x.put(mainOwner,0x48,2u);});rejects([](auto& x){x.put(mainOwner,0x98,5u);});
 rejects([](auto& x){x.text(sr,"root_Story");});rejects([](auto& x){x.put(sr,0xc9,(unsigned char)0);});
 rejects([](auto& x){x.put(renderer,0x570,2u);});rejects([](auto& x){x.put(sr,0x110,renderer+8);});
 rejects([](auto& x){x.put(panel,0x10,mainRoot);});rejects([](auto& x){x.put(panel,0xc9,(unsigned char)0);});
 rejects([](auto& x){x.put(piece,0x10,sr);});rejects([](auto& x){x.put(piece,0x30,ad+8);});
 rejects([](auto& x){x.put(piece,0x110,renderer+8);});rejects([](auto& x){x.put(piece,0x3c,4u);});
 rejects([](auto& x){x.blocks.erase(piece);});rejects([](auto& x){x.put(panel,0x118,std::array<P,9>{});});
 for(const char* name:{"m_bt_SRScore1","m_bt_SRScore2","m_bt_SRScore3","m_bt_target","lcd_bt_ruler"})rejects([&](auto& x){x.text(panel,name);});
}

int main(){backplates();
 // The removed name's plate is a separate sprite. Suppress only that adapter.
 {Memory m;constexpr P plate=0xf000,adapter=0x10000;
  m.node(plate,local,"lcd_bt_ScoreGaugePlayers0");m.put(name,0x28,plate);
  m.put(plate,0x30,adapter);m.put(plate,0x110,renderer);
  m.put(adapter,0,b+0x10CEC08);m.put(adapter,0x40,renderer);
  auto hide=[&](){return bone_eater::render::scoreGameplayLabel(b,adapter,false,[&](auto p,auto o,auto& v){return m.read(p,o,v);});};
  assert(hide());
  for(const char* s:{"lcd_bt_ScoreGaugeEnd0","lcd_bt_ScoreGaugeJuice0",
      "lcd_bt_ScoreGaugeScore0_0","lcd_bt_ScoreGaugeScore0_1","lcd_bt_ScoreGaugeScore0_2",
      "lcd_bt_ScoreGaugeScore0_3","lcd_bt_ScoreGaugeScore0_4","lcd_bt_ScoreGaugeScore0_5"}){m.text(plate,s);assert(hide());}
  for(const char* s:{"lcd_bt_ScoreGaugePlayers1","lcd_bt_ScoreGaugeJuice1","lcd_bt_ScoreGaugeScore1_0",
      "lcd_bt_ScoreGaugeScore0_6","lcd_bt_ruler","lcd_bt_RoundScore"}){m.text(plate,s);assert(!hide());}
  m.text(plate,"lcd_bt_ScoreGaugePlayers0");m.put(plate,0x10,root);assert(!hide());
  m.put(plate,0x10,local);m.put(scene,0x38,4u);assert(!hide());
 }
 Memory good;assert(good.hide(false)&&good.hide(true));
 auto both=[&](auto change){auto bad=good;change(bad);assert(!bad.hide(false)&&!bad.hide(true));};
 both([](auto& m){m.put(scene,0x38,4u);});both([](auto& m){m.put(scene,0x3c,6);});both([](auto& m){m.put(owner,0x48,2u);});
 both([](auto& m){m.text(root,"root_Story");});both([](auto& m){m.put(root,0xc9,(unsigned char)0);});
 both([](auto& m){m.put(bottom,0xc9,(unsigned char)0);});both([](auto& m){m.put(root,0x10,you);});
 both([](auto& m){m.put(root,0x3c,5u);});both([](auto& m){m.put(renderer,0x570,1u);}); // scope camera selector
 both([](auto& m){m.put(renderer,0x1a8,camera+8);});both([](auto& m){m.blocks.erase(root);});
 for(const char* s:{"lcd_bt_ruler","lcd_bt_ScoreGaugeJuice0","lcd_bt_ScoreGaugeScore0_0","lcd_bt_PlayerName"}){auto m=good;m.text(you,s);assert(!m.hide(false)&&m.hide(true));}
 for(const char* s:{"lcd_bt_PlayerName","lcd_bt_ScoreGaugePLName1"}){auto m=good;m.text(name,s);assert(m.hide(false)&&!m.hide(true));}
 {auto m=good;m.text(local,"lcd_bt_SRSGRoot1");assert(m.hide(false)&&!m.hide(true));}
 {auto m=good;m.put(font,0x40,renderer);assert(!m.hide(true));}
 {auto m=good;m.put(you,0x30,font);assert(!m.hide(false));}
 {auto m=good;m.put(root,0x18,local);m.put(local,0x28,local);assert(!m.hide(false));} // bounded cycle
}
