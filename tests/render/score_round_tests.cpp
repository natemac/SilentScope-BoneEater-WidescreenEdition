#include "render/score_round_identity.h"
#include <cassert>
#include <cstring>
#include <unordered_map>
int main(){using P=std::uintptr_t;constexpr P b=0x180000000,m=0x1000,o=0x2000,msg=0x3000,w=0x4000;
 std::unordered_map<P,std::uint64_t> mem{{b+0x13DD000,m},{m,b+0x10C6B38},{m+0x38,5},{m+0x3C,0xFFFFFFFF},{m+0x40,o},{o,b+0x10C6E48},{b+0x13DCFB0,msg},{msg,b+0x10CE298},{msg+8,w},{msg+16,0},{w,b+0x10CAB50},{w+8,0},{w+0x10,20}};
 auto read=[&](P p,P off,auto& v){auto it=mem.find(p+off);if(it==mem.end()||sizeof(v)>8)return false;std::memcpy(&v,&it->second,sizeof(v));return true;};
 P owner=0;unsigned state=0;auto yes=[&]{return bone_eater::render::scoreRoundInfoOwner(b,read,owner,state);};
 for(unsigned n=3;n<=22;++n){mem[w+0x10]=n;assert(yes()&&owner==w&&state==n);}
 for(unsigned n:{0,1,2,23,24,100,101}){mem[w+0x10]=n;assert(!yes());}
 mem[w+0x10]=20;
 for(P p:{m,m+0x38,m+0x3C,o,msg,w,w+8}){auto save=mem[p];mem[p]=0xBAD;assert(!yes());mem[p]=save;assert(yes());}
 mem[msg+8]=0;mem[msg+16]=w;assert(yes());mem[w+8]=0x100;assert(!yes());
}
