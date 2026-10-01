#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace bone_eater::render {
class NewsPresentationPolicy {
    std::uintptr_t owner_=0;
    std::uint64_t entered_=0;
public:
    void reset() noexcept {owner_=0;entered_=0;}
    float alpha(std::uintptr_t owner,std::uint64_t now,float nativeAlpha) noexcept {
        if(!owner||!std::isfinite(nativeAlpha)||nativeAlpha<0||nativeAlpha>1){reset();return 0;}
        if(owner!=owner_||now<entered_){owner_=owner;entered_=now;}
        const float t=std::min(1.f,float(now-entered_)/800.f);
        return nativeAlpha*t*t*(3.f-2.f*t);
    }
};
}
