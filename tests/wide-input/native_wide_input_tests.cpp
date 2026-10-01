#include "input/native_wide_input.h"
#include <cassert>
#include <limits>

using namespace bone_eater;

bool near(float actual, float expected) { return std::fabs(actual - expected) < 0.001f; }

int main() {
    render::NativeHudGeometry g;
    g.valid = true;
    g.mainWidth = 1920;
    g.mainHeight = 1080;
    g.width = 768.0f * (1080.0f / 1366.0f);
    g.height = 1080;
    g.left = (1920 - g.width) * 0.5f;
    const auto center = input::cabinetToWide(400, 580, 800, 1280, g);
    assert(center.valid && near(center.x, 960) && near(center.y, 489.375f));
    const auto low = input::cabinetToWide(32, 132, 800, 1280, g);
    const auto high = input::cabinetToWide(768, 1028, 800, 1280, g);
    assert(low.valid && high.valid && near(low.x, 76.8f) && near(high.x, 1843.2f));
    assert(near(low.y, 111.375f) && near(high.y, 867.375f));

    // Live B85 btn_L_BG2: cached center (226,615), pivot (.5,.5), size256,
    // triangle vertices (12,231),(396,615),(12,999). BA0 has no hit objects.
    const auto inStart = [](const input::WideInputPoint& p) {
        return p.valid && p.x >= 12 && p.x <= 396 &&
            std::fabs(p.y - 615) <= 396 - p.x;
    };
    const float startX = g.left + 226 * (g.width / 768);
    const float startY = 615 * (g.height / 1366);
    const auto lobbyStart = input::wideToLobbyUi(startX,startY,g);
    assert(near(lobbyStart.x,226) && near(lobbyStart.y,615) && inStart(lobbyStart));
    // The former identity mapping activated Start from this far-left point.
    assert(inStart({true,200,615}));
    assert(!inStart(input::wideToLobbyUi(200,615,g)));
    assert(!inStart(input::wideToLobbyUi(1190,503,g)));
    assert(input::wideToLobbyUi(-20,503,g).x < 0);
    assert(input::wideToLobbyUi(1950,503,g).x > 768);
    assert(!input::wideToLobbyUi(std::numeric_limits<float>::infinity(),503,g).valid);
    auto badLobbyGeometry=g; badLobbyGeometry.valid=false;
    assert(!input::wideToLobbyUi(840,486,badLobbyGeometry).valid);

    // Desktop mode removes only the post-calibration viewport margins.
    const auto desktopCenter = input::cabinetToDesktop(400, 580, 800, 1280, g);
    assert(desktopCenter.valid && near(desktopCenter.x, 960) && near(desktopCenter.y, 540));
    for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            // Hardware A6E8..A734: horizontal span uses width - width*.08f,
            // rather than assuming a literal .92f is the original multiply.
            const float arkX = (800.0f - 800.0f * 0.08f) * u + 800.0f * 0.04f;
            const float arkY = 132.0f + 896.0f * v;
            const auto point = input::cabinetToDesktop(arkX, arkY, 800, 1280, g);
            assert(point.valid && near(point.x, u * 1920) && near(point.y, v * 1080));
            assert(near((point.x - 960) / 1920, u - 0.5f));
            assert(near((point.y - 540) / 1080, v - 0.5f));
            const auto menu = input::wideToHud(point.x, point.y, g);
            assert(menu.valid);
            assert(near(g.left + menu.x / 768 * g.width, point.x));
            assert(near(g.top + menu.y / 1366 * g.height, point.y));
        }
    }
    // Native calibration's X already descends as raw cabinet X increases.
    // Conversion must preserve that result instead of adding a second flip.
    const auto rawLeft = input::cabinetToDesktop(768, 132, 800, 1280, g); // raw X=0
    const auto rawRight = input::cabinetToDesktop(32, 1028, 800, 1280, g); // raw X=4095
    assert(rawLeft.x == 1920 && rawLeft.y == 0);
    assert(rawRight.x == 0 && rawRight.y == 1080);
    // One count around factory neutral2047 produces asymmetric native half
    // ranges. Their subpixel differences are retained, not snapped to center.
    const auto below = input::cabinetToDesktop(400 + 736.0f / 4094,
        580 - 896.0f / 4094, 800, 1280, g);
    const auto above = input::cabinetToDesktop(400 - 736.0f / 4096,
        580 + 896.0f / 4096, 800, 1280, g);
    assert(near(below.x, 960 + 1920.0f / 4094) && near(below.y, 540 - 1080.0f / 4094));
    assert(near(above.x, 960 - 1920.0f / 4096) && near(above.y, 540 + 1080.0f / 4096));
    const auto floatLow = input::cabinetToDesktop(std::nextafter(32.0f, 0.0f),
        std::nextafter(132.0f, 0.0f), 800, 1280, g);
    const auto floatHigh = input::cabinetToDesktop(std::nextafter(768.0f, 1000.0f),
        std::nextafter(1028.0f, 2000.0f), 800, 1280, g);
    assert(floatLow.valid && floatLow.x == 0 && floatLow.y == 0);
    assert(floatHigh.valid && floatHigh.x == 1920 && floatHigh.y == 1080);
    for (const auto point : {std::array<float, 2>{31.9f, 580}, {768.1f, 580},
            {400, 131.9f}, {400, 1028.1f}, {0, 0}, {800, 1280}})
        assert(!input::cabinetToDesktop(point[0], point[1], 800, 1280, g).valid);

    // World-ray normalization sees the same logical fraction after conversion.
    for (float x : {32.0f, 200.0f, 400.0f, 600.0f, 768.0f}) {
        for (float y : {132.0f, 320.0f, 580.0f, 960.0f, 1028.0f}) {
            const auto point = input::cabinetToWide(x, y, 800, 1280, g);
            assert(near((point.x - 960) / 1920, (x - 400) / 800));
            assert(near((point.y - 540) / 1080, (y - 640) / 1280));
        }
    }

    // Artwork-space menu locations survive fit -> inverse-fit. Name-entry's
    // native multipliers yield exactly the same LCD point as ordinary menus.
    for (float x : {0.0f, 128.0f, 384.0f, 640.0f, 768.0f}) {
        for (float y : {0.0f, 100.0f, 683.0f, 1200.0f, 1366.0f}) {
            const float mainX = g.left + x / 768 * g.width;
            const float mainY = g.top + y / 1366 * g.height;
            const auto menu = input::wideToHud(mainX, mainY, g);
            const auto widget = input::wideToWidgetGetter(mainX, mainY, g);
            assert(menu.valid && near(menu.x, x) && near(menu.y, y));
            assert(widget.valid && near(widget.x * (768.0f / 800), x));
            assert(near(widget.y * (1366.0f / 1280), y));
        }
    }
    assert(input::wideToHud(0, 540, g).x < 0);
    assert(input::wideToHud(1920, 540, g).x > 768);
    assert(input::wideToHud(960, -1, g).y < 0);
    assert(input::wideToHud(960, 1081, g).y > 1366);

    std::array<std::uint8_t, 52> bytes;
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i + 13);
    const float x = 400, y = 580;
    const std::int32_t rawX = 2047, rawY = 2048;
    std::memcpy(bytes.data() + 0x24, &x, 4);
    std::memcpy(bytes.data() + 0x28, &y, 4);
    std::memcpy(bytes.data() + 0x2C, &rawX, 4);
    std::memcpy(bytes.data() + 0x30, &rawY, 4);
    const auto original = bytes;
    assert(input::rewriteWideArkOutput(bytes, 800, 1280, g));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (i < 0x24 || i >= 0x2C) assert(bytes[i] == original[i]);
    float mappedX = 0, mappedY = 0;
    std::memcpy(&mappedX, bytes.data() + 0x24, 4);
    std::memcpy(&mappedY, bytes.data() + 0x28, 4);
    assert(near(mappedX, 960) && near(mappedY, 489.375f));

    bytes = original;
    assert(input::rewriteWideArkOutput(bytes, 800, 1280, g, input::WideInputMapping::Desktop));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (i < 0x24 || i >= 0x2C) assert(bytes[i] == original[i]);
    std::memcpy(&mappedX, bytes.data() + 0x24, 4);
    std::memcpy(&mappedY, bytes.data() + 0x28, 4);
    assert(near(mappedX, 960) && near(mappedY, 540));

    for (const auto dimensions : {std::array<int, 2>{768, 1280}, {800, 1080}, {1920, 1080}, {0, 0}}) {
        bytes = original;
        assert(!input::rewriteWideArkOutput(bytes, dimensions[0], dimensions[1], g));
        assert(bytes == original);
        assert(!input::rewriteWideArkOutput(bytes, dimensions[0], dimensions[1], g,
            input::WideInputMapping::Desktop) && bytes == original);
    }
    auto invalid = g;
    invalid.valid = false;
    bytes = original;
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, invalid) && bytes == original);
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, invalid,
        input::WideInputMapping::Desktop) && bytes == original);
    invalid = g;
    invalid.width = 0;
    assert(!input::wideToHud(400, 580, invalid).valid);
    invalid = g;
    invalid.left = std::numeric_limits<float>::quiet_NaN();
    assert(!input::wideToWidgetGetter(400, 580, invalid).valid);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bytes = original;
    std::memcpy(bytes.data() + 0x28, &nan, 4);
    auto before = bytes;
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, g) && bytes == before);
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, g,
        input::WideInputMapping::Desktop) && bytes == before);
    bytes = original;
    const std::int32_t badRaw = 4096;
    std::memcpy(bytes.data() + 0x30, &badRaw, 4);
    before = bytes;
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, g) && bytes == before);
    assert(!input::rewriteWideArkOutput(bytes, 800, 1280, g,
        input::WideInputMapping::Desktop) && bytes == before);
    for (float malformed : {-1.0f, 0.0f, 31.9f, 768.1f, 800.0f,
            std::numeric_limits<float>::infinity()}) {
        bytes = original;
        std::memcpy(bytes.data() + 0x24, &malformed, 4);
        before = bytes;
        assert(!input::rewriteWideArkOutput(bytes, 800, 1280, g,
            input::WideInputMapping::Desktop) && bytes == before);
    }
}
