#include "render/dialogue_illumination_math.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace bone_eater::render;
namespace {
void require(bool value, int line) { if (!value) throw std::runtime_error("line " + std::to_string(line)); }
#define CHECK(v) require((v), __LINE__)
constexpr float scale = 1080.0f / 1366.0f;
constexpr float width = 768.0f * scale, left = (1920.0f - width) * 0.5f;
const RearIlluminationPose measured {{{0, 270.0f * scale}}, {{scale, scale}}};
bool fit(const RearIlluminationPose& input, RearIlluminationPose& output) {
    return alignDialogueIllumination(left, 0, width, 1080, input, output);
}
void translatedBeamCoversFrontDialogue() {
    RearIlluminationPose result;
    CHECK(fit(measured, result));
    CHECK(std::fabs(result.position[0] - .1f) < .001f);
    CHECK(std::fabs(result.scale[0] * 800 - 1920) < .001f);
    CHECK(std::fabs(result.position[1] - 213.46999f) < .001f);
    const float textX = left + 235.0f * scale, textY = 327.0f * scale;
    CHECK(textX > result.position[0] && textX < result.position[0] + 800 * result.scale[0]);
    CHECK(textY > result.position[1] && textY < result.position[1] + 211 * result.scale[1]);
}
void scaleAndSourceRemainBitExact() {
    const auto before = measured;
    RearIlluminationPose result, repeated;
    CHECK(fit(before, result) && fit(before, repeated));
    CHECK(result.scale[1] == before.scale[1]);
    CHECK(result.position[1] == before.position[1]);
    CHECK(!std::memcmp(&before, &measured, sizeof(before)));
    CHECK(result.position == repeated.position && result.scale == repeated.scale);
}
void translatedOrUnfittedSourceFailsWithoutPublication() {
    RearIlluminationPose translated;
    CHECK(fit(measured, translated));
    const RearIlluminationPose sentinel {{{9,8}},{{7,6}}};
    for (auto bad : {translated, RearIlluminationPose{{{0,270}},{{1,1}}},
            RearIlluminationPose{{{643.74817f,247.46706f}},{{scale,scale}}},
            RearIlluminationPose{{{0,213.46999f}},{{scale,scale*.5f}}}}) {
        auto result = sentinel;
        CHECK(!fit(bad, result));
        CHECK(!std::memcmp(&result, &sentinel, sizeof(result)));
    }
}
void malformedOrUnexpectedGeometryFails() {
    for (unsigned i=0;i<4;++i) for (const float bad : {
            std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f}) {
        auto value = measured;
        (i<2?value.position[i]:value.scale[i-2]) = bad;
        RearIlluminationPose result;
        CHECK(!fit(value,result));
    }
    auto shifted = measured; shifted.position[0] = 1;
    RearIlluminationPose result;
    CHECK(!fit(shifted,result));
    CHECK(!alignDialogueIllumination(0,0,1920,1080,measured,result));
    CHECK(!alignDialogueIllumination(left,1,width,1080,measured,result));
    CHECK(!alignDialogueIllumination(left,0,width,1366,measured,result));
}
}
int main() {
    try { translatedBeamCoversFrontDialogue(); scaleAndSourceRemainBitExact();
        translatedOrUnfittedSourceFailsWithoutPublication(); malformedOrUnexpectedGeometryFails(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Passed 4 dialogue registration and rejection cases\n";
}
