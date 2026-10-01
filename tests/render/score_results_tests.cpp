#include "render/score_results_identity.h"
#include <cassert>
#include <cstring>
#include <map>
int main() {
 using P=std::uintptr_t;constexpr P b=0x180000000,s=0x1000,bt=0x2000,m=0x3000,o=0x4000,l=0x5000;
 std::map<P,std::uint64_t> v{{b+0x13DD000,s},{s,b+0x10C6B38},{s+0x38,5},{s+0x3C,0xffffffff},
  {s+0x40,bt},{bt,b+0x10C6E48},{b+0x13DCFB0,m},{m,b+0x10CE298},{m+0x808,2},{m+8,0},{m+16,o},
  {o,b+0x10CA968},{o+8,0},{o+0x10,108},{o+0x50,l},{l,b+0x10C9FA8},{l+0x64,0xBC6},{l+0x18,2}};
 auto yes=[&]{return bone_eater::render::scoreResultsReticle(b,[&](P p,P off,auto& out){
  auto i=v.find(p+off);if(i==v.end()||sizeof(out)>8)return false;std::memcpy(&out,&i->second,sizeof(out));return true;});};
 assert(yes());v[o+0x10]=110;assert(yes());
 for(unsigned phase=0;phase<134;++phase){v[o+0x10]=phase;assert(yes()==(phase==108||phase==110));}
 v[o+0x10]=108;
 for(P p:{s,s+0x38,s+0x3C,bt,m,o,o+0x50,l,l+0x64,l+0x18}){auto saved=v[p];v[p]=0xbad;assert(!yes());v[p]=saved;assert(yes());}
 for(auto flags:{1u,0x100u}){v[o+8]=flags;assert(!yes());}v[o+8]=0;
 for(auto n:{0u,1u,257u}){v[m+0x808]=n;assert(!yes());}v[m+0x808]=2;assert(yes());
 v.erase(o+0x50);assert(!yes());
}
