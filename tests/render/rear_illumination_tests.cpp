#include "render/splash_illumination_math.h"
#include "render/rear_illumination_math.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace bone_eater::render;
namespace {
void require(bool v, int line) { if (!v) throw std::runtime_error("line " + std::to_string(line)); }
#define CHECK(v) require((v), __LINE__)
constexpr float width = 768.0f * 1080.0f / 1366.0f;
constexpr float left = (1920.0f - width) * 0.5f;
bool near(double a, double b) { return std::fabs(a-b) < 0.0002; }
RearIlluminationPose fit(RearIlluminationPose p) {
    RearIlluminationPose result;
    CHECK(alignRearIllumination(left, 0, width, 1080, p, result));
    return result;
}
void settledPairPreservesBaselineRegistration() {
    const auto a=fit({{{0,-43}},{{1,1}}}), b=fit({{{400,-43}},{{1,1}}});
    CHECK(near(a.position[0]-.1,656.7140836));
    CHECK(near(a.position[1]-.1,-33.6561814));
    CHECK(near(b.position[0]-a.position[0],400.0*a.scale[0]));
    CHECK(near(b.position[0]-.1+400*b.scale[0],1288.3952424));
    CHECK(near(a.position[1]-.1+481*a.scale[1],346.3584492));
}
void originalQuadEdgesMapToNewQuad() {
    const auto a=fit({{{-.4f,-.4f}},{{1,1}}});
    CHECK(near(a.position[0]-.1,left) && near(a.position[1]-.1,0));
    CHECK(near(a.position[0]-.1+769*a.scale[0],left+width));
    CHECK(near(a.position[1]-.1+1367*a.scale[1],1080));
}
void nativeAnimationDeltaAndAncestorScalePreserved() {
    const auto settled=fit({{{0,-43}},{{2,.5f}}});
    const auto hidden=fit({{{-800,-443}},{{2,.5f}}});
    CHECK(near(settled.position[0]-hidden.position[0],800.0*width/769));
    CHECK(near(settled.position[1]-hidden.position[1],400.0*1080/1367));
    CHECK(near(settled.scale[0],2.0*width/769) && near(settled.scale[1],.5*1080/1367));
}
void freshPoseNeverCompoundsAndInputUnchanged() {
    const RearIlluminationPose original {{{400,-43}},{{1,1}}};
    const auto a=fit(original), b=fit(original);
    CHECK(a.position==b.position && a.scale==b.scale);
    CHECK(original.position[0]==400 && original.position[1]==-43 && original.scale[0]==1);
}
void invalidPoseNeverPublishesPartialResult() {
    for (unsigned field=0;field<4;++field) for (float bad : {
            std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        RearIlluminationPose input {{{1,2}},{{1,1}}}, output {{{99,98}},{{97,96}}};
        (field<2?input.position[field]:input.scale[field-2])=bad;
        CHECK(!alignRearIllumination(left,0,width,1080,input,output));
        CHECK(output.position[0]==99 && output.scale[1]==96);
    }
    for (float bad : {0.0f,-1.0f,17.0f}) {
        RearIlluminationPose output;
        CHECK(!alignRearIllumination(left,0,width,1080,{{{0,0}},{{bad,1}}},output));
    }
    RearIlluminationPose output;
    CHECK(!alignRearIllumination(left,0,width,1080,{{{20000,0}},{{1,1}}},output));
}
void wrongFrontCanvasRejected() {
    RearIlluminationPose output;
    const RearIlluminationPose input {{{0,0}},{{1,1}}};
    CHECK(!alignRearIllumination(0,0,1920,1080,input,output));
    CHECK(!alignRearIllumination(left,1,width,1080,input,output));
    CHECK(!alignRearIllumination(left,0,width,1079,input,output));
    CHECK(!alignRearIllumination(left,0,std::numeric_limits<float>::quiet_NaN(),1080,input,output));
}
}
void splashCardBounds() {
    RearIlluminationPose full,opening,unchanged{{{99,98}},{{97,96}}};
    CHECK(alignSplashIllumination({-16,862},{800,342},{1,1},{749,532},full));
    constexpr float fit=1080.f/1366.f;
    CHECK(near(full.position[0]-.1f,960));
    CHECK(near(full.position[1]-.1f,1033*fit));
    CHECK(near(full.scale[0]*749,768*fit));
    CHECK(near(full.scale[1]*532,470*.8f*fit));
    CHECK(alignSplashIllumination({-16,862},{800,342},{1,.375f},{749,532},opening));
    CHECK(near(opening.scale[1],full.scale[1]*.375f));
    CHECK(opening.position==full.position);
    CHECK(!alignSplashIllumination({-16,862},{800,342},{1,0},{749,532},unchanged));
    CHECK(unchanged.position[0]==99 && unchanged.scale[0]==97);
    CHECK(!alignSplashIllumination({2000,862},{800,342},{1,1},{749,532},unchanged));
}
int main() {
    try { splashCardBounds(); settledPairPreservesBaselineRegistration(); originalQuadEdgesMapToNewQuad();
        nativeAnimationDeltaAndAncestorScalePreserved(); freshPoseNeverCompoundsAndInputUnchanged();
        invalidPoseNeverPublishesPartialResult(); wrongFrontCanvasRejected(); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    std::cout<<"Passed 7 rear illumination affine cases\n";
}
