#include "render/phone_card_identity.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <unordered_map>
int main() {
    using P=std::uintptr_t;using namespace bone_eater::render;
    constexpr P b=0x180000000,m=0x1000,s=0x2000,msg=0x3000,r=0x4000,c=0x5000,l=0x6000;
    std::unordered_map<P,std::uint64_t> mem{
        {b+0x13DD000,m},{m,b+0x10C6B38},{m+0x38,5},{m+0x3C,0xFFFFFFFF},{m+0x40,s},{s,b+0x10C6E48},
        {b+0x13DCFB0,msg},{msg,b+0x10CE298},{msg+0x808,2},{msg+8,r},{msg+16,0},
        {r,b+0x10CA968},{r+8,0},{r+0x10,127},{r+0x30,c},
        {c,b+0x10CA818},{c+8,0},{c+0x10,1},{c+0x2C,2},{c+0x18,l},
        {l,b+0x10C9FA8},{l+0x64,0xB67},{l+0x18,2}};
    auto number=[&](P p,float f){mem[p]=0;std::memcpy(&mem[p],&f,sizeof(f));};
    number(c+0x20,5);number(c+0x24,2);
    auto read=[&](P p,P off,auto& v){auto it=mem.find(p+off);if(it==mem.end()||sizeof(v)>8)return false;std::memcpy(&v,&it->second,sizeof(v));return true;};
    P owner=0,layout=0;float alpha=0;auto yes=[&]{return scorePhoneCard(b,read,owner,layout,alpha);};
    assert(yes()&&owner==c&&layout==l&&alpha==1);
    // Native1653FC supplies INFO B67; constructor10780B stores it at +64.
    // B65 identifies the artwork and was the failed85ECADE4 gate.
    mem[l+0x64]=0xB65;assert(!yes());mem[l+0x64]=0xB67;assert(yes());
    for(P p:{m,m+0x38,m+0x3C,s,msg,r,r+0x10,c,c+0x2C,l,l+0x64}) {
        auto save=mem[p];mem[p]=0xBAD;assert(!yes());mem[p]=save;assert(yes());
    }
    for(auto variant:{0u,1u,3u}){mem[c+0x2C]=variant;assert(!yes());}mem[c+0x2C]=2;
    for(P p:{r+8,c+8}){mem[p]=0x100;assert(!yes());mem[p]=0;}
    mem[msg+8]=0;mem[msg+16]=r;assert(yes());
    mem[msg+16]=0;mem[msg+24]=r;mem[msg+0x808]=3;assert(yes());
    for(unsigned n:{0u,2u,257u}){mem[msg+0x808]=n;assert(!yes());}mem[msg+0x808]=3;
    for(unsigned phase:{0u,2u}) {
        mem[c+0x10]=phase;
        for(unsigned state=0;state<4;++state){mem[l+0x18]=state;
            bool valid=phase==0?(state==1||state==2):(state==0||state==3);
            assert(yes()==valid);if(valid)assert(alpha==0);
        }
    }
    mem[c+0x10]=3;assert(!yes());mem[c+0x10]=1;mem[l+0x18]=2;
    for(float f:{-1.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        number(c+0x24,f);assert(!yes());
    }
    number(c+0x24,0);assert(yes()&&alpha==0);
    number(c+0x24,.2f);assert(yes()&&std::abs(alpha-.5f)<.0001f);
    number(c+0x24,4.8f);assert(yes()&&std::abs(alpha-.5f)<.0001f);
    number(c+0x24,5.1f);assert(yes()&&alpha==0);
    number(c+0x20,0);assert(!yes());
    for(int i=0;i<=50;++i){float a=phoneCardAlpha(i/10.f,5);assert(a>=0&&a<=1);}
}
