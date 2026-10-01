#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace bone_eater::render {
// Called only for the certified left/right rear illumination pair. Read the
// uploaded positions: nominal layout coordinates lose the native rounding gap.
inline bool hudSeamTranslation(std::span<const unsigned char> vertices,
    std::uint32_t left, std::uint32_t right, float scale, float& translation) noexcept {
    constexpr std::size_t stride=112;
    if(left==right || vertices.size()%stride || left>=vertices.size()/stride ||
        right>=vertices.size()/stride || !std::isfinite(scale) || scale<=0)return false;
    float a=0,b=0;
    std::memcpy(&a,vertices.data()+std::size_t(left)*stride+56,4);
    std::memcpy(&b,vertices.data()+std::size_t(right)*stride,4);
    const float gap=b-a;
    if(!std::isfinite(gap)||gap<0||gap>=.1f)return false;
    translation=-gap*scale;
    return std::isfinite(translation);
}
}
