#pragma once
#include "render/rear_illumination_math.h"
#include <algorithm>

namespace bone_eater::render {
// Match the active front card, including its opening/closing scale, instead of
// the cabinet backing's hard-coded 3x horizontal stretch. The front pass clips
// the card at the portrait canvas, so its rear illumination must stop there too.
inline bool alignSplashIllumination(const std::array<float,2>& position,
        const std::array<float,2>& size,const std::array<float,2>& scale,
        const std::array<float,2>& backingSize,RearIlluminationPose& result) noexcept {
    for(unsigned i=0;i<2;++i)if(!std::isfinite(position[i])||std::fabs(position[i])>16384||
        !std::isfinite(size[i])||size[i]<=0||size[i]>4096||
        !std::isfinite(scale[i])||scale[i]<=0||scale[i]>4||
        !std::isfinite(backingSize[i])||backingSize[i]<=0||backingSize[i]>4096)return false;
    constexpr float fit=1080.f/1366.f,left=(1920.f-768.f*fit)*.5f;
    const float cx=position[0]+size[0]*.5f,cy=position[1]+size[1]*.5f;
    const float l=std::max(0.f,cx-size[0]*scale[0]*.5f);
    const float r=std::min(768.f,cx+size[0]*scale[0]*.5f);
    if(r<=l)return false;
    RearIlluminationPose after;
    after.position={left+(l+r)*.5f*fit+.1f,cy*fit+.1f};
    after.scale={(r-l)*fit/backingSize[0],(size[1]+128.f)*.8f*scale[1]*fit/backingSize[1]};
    result=after;return true;
}
}
