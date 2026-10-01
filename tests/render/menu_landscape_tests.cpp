#include "render/score_lobby_identity.h"
#include "render/menu_landscape_layout.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <unordered_map>
int main() {
    using L=bone_eater::render::MenuLandscapeLayout;
    float x=0,y=0;
    // Round-trip points across the selectable content, including its edges.
    for(float sx:{657.f,800.f,960.f,1100.f,1263.f})
        for(float sy:{180.f,350.f,486.f,700.f,819.f}) {
            assert(L::toNative(L::left+(sx-L::sourceX)*L::scale,
                L::top+(sy-L::sourceY)*L::scale,x,y));
            assert(std::abs(x-sx)<.001f&&std::abs(y-sy)<.001f);
        }
    assert(!L::toNative(80,300,x,y)); // Instructions must not activate a choice.
    assert(!L::toNative(L::left-1,L::top,x,y));
    assert(!L::toNative(L::left+L::width*L::scale,L::top,x,y));
    assert(!L::toNative(L::left,L::top+L::height*L::scale,x,y));
    assert(!L::toNative(std::numeric_limits<float>::quiet_NaN(),400,x,y));
    // Never remap gameplay or a changing/stopped menu owner.
    constexpr std::uintptr_t base=0x180000000, manager=0x1000, menu=0x2000, layout=0x3000;
    std::unordered_map<std::uintptr_t,std::uint64_t> memory{
        {base+0x13DD000,manager},{manager,base+0x10C6B38},
        {manager+0x38,2},{manager+0x3C,0xFFFFFFFF},{manager+0x40,menu},
        {menu,base+0x10C7030},{menu+8,0},{menu+0x10,1},{menu+0x14,2},
        {menu+0x20,layout},{layout,base+0x10C9FA8},{layout+0x64,0xBA0}};
    auto read=[&](std::uintptr_t object,std::size_t offset,auto& value) {
        const auto found=memory.find(object+offset);
        if(found==memory.end()||sizeof(value)>sizeof(found->second))return false;
        std::memcpy(&value,&found->second,sizeof(value));return true;
    };
    std::uintptr_t owner=0;unsigned phase=0;
    auto admitted=[&]{return bone_eater::render::landscapeMenuOwner(base,read,owner,phase);};
    assert(admitted()&&owner==menu&&phase==2);
    memory[manager+0x38]=5;assert(!admitted());memory[manager+0x38]=2;
    memory[manager+0x3C]=3;assert(!admitted());memory[manager+0x3C]=0xFFFFFFFF;
    memory[menu+8]=1;assert(!admitted());memory[menu+8]=0x100;assert(!admitted());memory[menu+8]=0;
    memory[layout+0x64]=0xBA1;assert(!admitted());memory[layout+0x64]=0xBA0;
    memory[menu+0x10]=2;assert(!admitted());memory[menu+0x10]=1;
    for(unsigned valid:{1,2,3,9,10,11,12,13,14,15,16,17,18}) {
        memory[menu+0x14]=valid;assert(admitted()&&phase==valid);
    }
    for(unsigned invalid:{0u,4u,8u,19u,0xFFFFFFFFu}) {
        memory[menu+0x14]=invalid;assert(!admitted());
    }
    // The photographed tutorial-exit footer must not jump back to portrait.
    // Presentation may cover the live primary-2 tail; input must remain off.
    auto presented=[&]{return bone_eater::render::landscapeMenuOwner(base,read,owner,phase,true,true);};
    memory[menu+0x10]=2;memory[menu+0x14]=0;
    assert(presented()&&!admitted());
    memory[menu+0x14]=18;assert(!presented());memory[menu+0x14]=0;
    memory[menu+0x10]=3;assert(!presented());memory[menu+0x10]=2;
    memory[manager+0x3C]=5;assert(!presented());memory[manager+0x3C]=0xFFFFFFFF;
    memory[menu+8]=1;assert(!presented());memory[menu+8]=0;
    memory[layout+0x64]=0xBA1;assert(!presented());memory[layout+0x64]=0xBA0;
    memory[manager+0x38]=5;assert(!presented());memory[manager+0x38]=2;
    memory[menu+0x10]=1;
    // Normal Start has a separate child owner and is enabled independently of
    // the still-experimental MainMenu conversion.
    constexpr std::uintptr_t credit=0x4000;
    memory[manager+0x38]=4;memory[menu]=base+0x10C6FA0;memory[menu+0x14]=8;
    memory[menu+0x48]=credit;memory[credit]=base+0x10CA7A8;
    memory[credit+8]=0;memory[credit+0x10]=1;memory[credit+0x20]=layout;
    auto normal=[&]{return bone_eater::render::landscapeMenuOwner(base,read,owner,phase,false);};
    for(unsigned valid:{0,1,3,4}) {
        memory[credit+0x14]=valid;assert(normal()&&owner==credit&&phase==100+valid);
    }
    for(unsigned invalid:{2,5,6,7,8}) {memory[credit+0x14]=invalid;assert(!normal());}
    memory[credit+0x14]=3;
    memory[credit+8]=1;assert(!normal());memory[credit+8]=0x100;assert(!normal());memory[credit+8]=0;
    memory[credit+0x10]=2;assert(!normal());memory[credit+0x10]=1;
    memory[credit]=base+0x10CA770;assert(!normal());memory[credit]=base+0x10CA7A8;
    memory[layout+0x64]=0xB85;assert(!normal());memory[layout+0x64]=0xBA0;
    memory[menu+0x14]=7;assert(!normal());memory[menu+0x14]=8;
    memory[manager+0x3C]=2;assert(!normal());memory[manager+0x3C]=0xFFFFFFFF;
    assert(normal());

    // Score selector must never admit Story's battle scene or a stale child.
    constexpr std::uintptr_t selector=0x5000;
    memory[manager+0x38]=6;memory[menu]=base+0x10C7240;
    memory[menu+0x10]=2;memory[menu+0x14]=0;memory[menu+0x18]=selector;
    memory[selector]=base+0x10CAAE0;memory[selector+8]=0;
    memory[selector+0x20]=layout;
    for(unsigned valid:{1,2,3,4,5,6,7,19,20}) {
        memory[selector+0x10]=valid;
        assert(admitted()&&owner==selector&&phase==200+valid);
    }
    for(unsigned invalid:{0u,8u,18u,21u,0xFFFFFFFFu}) {
        memory[selector+0x10]=invalid;assert(!admitted());
    }
    memory[selector+0x10]=4;
    for(auto address:{manager+0x38,manager+0x3C,menu,menu+8,menu+0x10,
                      menu+0x14,selector,selector+8,layout,layout+0x64}) {
        const auto saved=memory[address];memory[address]=0xBAD;assert(!admitted());
        memory[address]=saved;assert(admitted());
    }
    memory[manager+0x38]=5;assert(!presented()&&!admitted());

    constexpr std::uintptr_t messages=0x6000;
    memory[menu]=base+0x10C6E48;memory[base+0x13DCFB0]=messages;
    memory[messages]=base+0x10CE298;memory[messages+8]=selector;
    memory[selector]=base+0x10CAB18;
    auto lobby=[&]{return bone_eater::render::scoreLobbyOwner(base,read,owner,phase);};
    for(unsigned v:{1,2,3,4,5,6,7,19,20}) {memory[selector+0x10]=v;assert(lobby());assert(!admitted());}
    for(unsigned v:{0,8,18,21}) {memory[selector+0x10]=v;assert(!lobby());}
    memory[selector+0x10]=4;
    for(auto address:{manager+0x38,manager+0x3C,menu,messages,selector,selector+8,layout,layout+0x64}) {
      auto saved=memory[address];memory[address]=0xBAD;assert(!lobby());memory[address]=saved;assert(lobby());
    }
    // Presentation alone covers native startup and stop-request fade tail.
    auto lobbyPresentation=[&]{return bone_eater::render::scoreLobbyOwner(base,read,owner,phase,true);};
    memory[selector+0x10]=0; assert(!lobby()&&lobbyPresentation());
    memory[selector+8]=1; assert(!lobbyPresentation());
    for(unsigned v:{19,20}) {
      memory[selector+0x10]=v;assert(!lobby()&&lobbyPresentation());
      memory[selector+8]=0x100;assert(!lobbyPresentation());memory[selector+8]=1;
    }
    for(unsigned v:{0,1,4,7,8,18,21}) {memory[selector+0x10]=v;assert(!lobbyPresentation());}
    memory[selector+0x10]=20;
    for(auto address:{manager+0x38,manager+0x3C,menu,messages,selector,layout,layout+0x64,selector+0x20}) {
      auto saved=memory[address];memory[address]=0xBAD;assert(!lobbyPresentation());memory[address]=saved;assert(lobbyPresentation());
    }

    // No artwork root exists during observed load; cleanup must not follow
    // the root of a state3 layout. Neither exception admits an active frame.
    std::uintptr_t presentedLayout=0;float alpha=-1;
    auto backing=[&]{return bone_eater::render::scoreLobbyPresentation(base,read,owner,presentedLayout,alpha);};
    memory[selector+8]=0;memory[selector+0x10]=1;memory[layout+0x18]=1;
    assert(backing()&&alpha==0&&presentedLayout==layout);
    memory[selector+0x10]=4;assert(!backing());
    memory[selector+0x10]=20;memory[layout+0x18]=3;assert(backing()&&alpha==0);
    memory[layout+0x18]=0;memory[layout+0x428]=0xBAD;
    assert(backing()&&alpha==0); // Native phase20 waits on released layouts.
    for(auto address:{layout+0x64,layout,selector,menu,messages,manager+0x38,manager+0x3C}) {
      auto saved=memory[address];memory[address]=0xBAD;assert(!backing());memory[address]=saved;
    }
    memory[selector+8]=0x100;assert(!backing());memory[selector+8]=0;
    for(unsigned v:{1,4,7,19,21}){memory[selector+0x10]=v;assert(!backing());}
    memory[selector+0x10]=4;
    constexpr std::uintptr_t root=0x7000;
    memory[layout+0x18]=2;memory[layout+0x428]=root;memory[root]=base+0x10CEB48;
    memory[root+0xBC]=0x3F000000;assert(backing()&&alpha==.5f);
    memory[root+0xBC]=0x7FC00000;assert(!backing());memory[root+0xBC]=0x3F000000;
    memory[manager+0x38]=2;assert(!backing());memory[manager+0x38]=5;
    memory[messages+8]=0;assert(!backing());memory[messages+8]=selector;
    memory[selector+0x10]=21;assert(!backing());
}
