#include "camera/main_fov_zoom.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace bone_eater::camera;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, "numeric mismatch");
}
constexpr float originalAspect = 800.0f / 1366.0f;
constexpr float wideAspect = 1920.0f / 1080.0f;

void parserCases() {
    for (const auto text : {"1", "1.0", "1.25", "1.5", "125e-2", "01.25"})
        require(parseMainFovZoom(text).has_value(), "valid decimal rejected");
    near(*parseMainFovZoom("1.25"), 1.25, 0);
    for (const auto text : {"", " ", " 1.25", "1.25 ", "+1.25", "-1", "0.99999",
            "1.50000001", "2", "1e999", "nan", "NaN", "inf", "-inf", "0x1.4p0",
            "1,25", "1.25extra", ".", "1e"})
        require(!parseMainFovZoom(text), "invalid decimal accepted");
    require(!parseMainFovZoom(std::string_view("1.25\0extra", 10)), "embedded NUL accepted");
}

void existingBehaviorAndScriptBaseline() {
    // Default must preserve the existing float operations, not subtly alter
    // the authored view while adding a configuration option.
    for (const float baseline : {0.1f, 1.0f, 2.75f, 1000.0f}) {
        const auto result = mainFovZoom(baseline, originalAspect, wideAspect, 1.0f);
        const float old = baseline * (originalAspect / wideAspect);
        require(result.valid && std::memcmp(&old, &result.temporaryZoom, sizeof(old)) == 0,
            "default changed existing arithmetic");
        const auto cropped = mainFovZoom(baseline, originalAspect, wideAspect, 1.25f);
        require(cropped.valid, "authored baseline rejected");
        near(cropped.temporaryZoom / baseline, cropped.factor, 1e-7);
        near(cropped.factor / result.factor, 1.25, 1e-6);
    }
}

void angularCoverage() {
    constexpr double pi = 3.14159265358979323846;
    const double originalP00 = 1.0 / std::tan(pi / 8.0); // Authored H45 degrees.
    const auto full = mainFovZoom(1, originalAspect, wideAspect, 1);
    const auto cropped = mainFovZoom(1, originalAspect, wideAspect, 1.25f);
    require(full.valid && cropped.valid, "measured geometry rejected");
    const double horizontal = 2 * std::atan(1 / (originalP00 * cropped.factor)) * 180 / pi;
    const double vertical = 2 * std::atan(1 / (originalP00 * cropped.factor * wideAspect)) * 180 / pi;
    near(horizontal, 90.3367, 1e-4);
    near(vertical, 59.0037, 1e-4);
    const double preservedVertical = 2 * std::atan(1 / (originalP00 * full.factor * wideAspect)) * 180 / pi;
    require(vertical + 12 > preservedVertical, "six-degree pan no longer spans example height");
    const auto maximum = mainFovZoom(1, originalAspect, wideAspect, 1.5f);
    require(maximum.valid && maximum.factor > cropped.factor && cropped.factor > full.factor,
        "zoom is not bounded monotonic composition");
}

void rejectedInputs() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {0.0f, -1.0f, nan, inf}) {
        require(!mainFovZoom(bad, originalAspect, wideAspect, 1.25f).valid, "bad baseline accepted");
        require(!mainFovZoom(1, bad, wideAspect, 1.25f).valid, "bad original aspect accepted");
        require(!mainFovZoom(1, originalAspect, bad, 1.25f).valid, "bad native aspect accepted");
    }
    for (const float bad : {0.99f, 1.5001f, nan, inf})
        require(!mainFovZoom(1, originalAspect, wideAspect, bad).valid, "bad multiplier accepted");
    require(!mainFovZoom(1, 0.1f, 2, 1).valid, "old lower factor guard lost");
    require(!mainFovZoom(1, 2, 2, 1).valid, "old upper factor guard lost");
    require(!mainFovZoom(std::numeric_limits<float>::denorm_min(), originalAspect, wideAspect, 1).valid,
        "underflow accepted");
    require(mainFovZoom(std::numeric_limits<float>::max(), originalAspect, wideAspect, 1.5f).valid,
        "finite scaled maximum rejected");
    // The original safety bound guards the height correction, while the
    // explicit multiplier bounds the additional zoom. Both remain independent.
    require(mainFovZoom(1, 0.625f, wideAspect, 1.5f).valid, "valid old800x1280 aspect rejected");
}
} // namespace

int main() {
    try {
        parserCases();
        existingBehaviorAndScriptBaseline();
        angularCoverage();
        rejectedInputs();
        std::cout << "Main FOV zoom: parsing, baseline, angular coverage and rejection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
