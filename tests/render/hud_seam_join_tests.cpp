#include "render/hud_seam_join.h"
#include <array>
#include <cassert>
#include <limits>
using namespace bone_eater::render;
int main(){
    std::array<unsigned char,336> bytes{};
    auto put=[&](std::size_t offset,float value){std::memcpy(bytes.data()+offset,&value,4);};
    // Captured native coordinates. Draw order is intentionally reversed.
    const float left=973.04553f,right=973.0547f,scale=3.04f;
    put(2*112+56,left);put(0,right);
    float delta=123;
    assert(hudSeamTranslation(bytes,2,0,scale,delta));
    assert(std::abs((right*scale+delta)-left*scale)<.0005f);
    assert(delta<0 && delta>-.1f);
    // A seam at a pixel center must neither leave a hole nor double blend.
    const double edge=960.5, gap=double(right-left)*scale;
    const double unjoined=edge+gap;
    assert(edge<=960.5 && unjoined>960.5);
    assert(std::abs(unjoined+delta-edge)<1e-6);
    put(0,left);assert(hudSeamTranslation(bytes,2,0,scale,delta)&&delta==0);
    put(0,left-.01f);assert(!hudSeamTranslation(bytes,2,0,scale,delta));
    put(0,left+.2f);assert(!hudSeamTranslation(bytes,2,0,scale,delta));
    put(0,std::numeric_limits<float>::quiet_NaN());assert(!hudSeamTranslation(bytes,2,0,scale,delta));
    put(0,right);
    assert(!hudSeamTranslation(bytes,2,2,scale,delta));
    assert(!hudSeamTranslation(bytes,2,3,scale,delta));
    assert(!hudSeamTranslation(bytes,UINT32_MAX,0,scale,delta));
    assert(!hudSeamTranslation(std::span(bytes).first(335),2,0,scale,delta));
    assert(!hudSeamTranslation(bytes,2,0,0,delta));
    assert(!hudSeamTranslation(bytes,2,0,std::numeric_limits<float>::infinity(),delta));
}
