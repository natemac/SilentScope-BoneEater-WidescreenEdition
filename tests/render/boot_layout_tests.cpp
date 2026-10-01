#include "render/boot_layout_policy.h"
#include <cassert>
#include <limits>
using namespace bone_eater::render;
int main() {
    const std::array<float,8> nativeUv {0,0,0,.625f,.78125f,.625f,.78125f,0};
    std::array<float,8> fitted {};
    assert(fittedBootCanvasUv({1920,1280},nativeUv,fitted));
    // One source texel remains one display pixel despite power-of-two padding.
    assert(1920.f / (2048.f*fitted[4]) == 1.f);
    assert(fitted[5] == nativeUv[5] && fitted[1] == nativeUv[1]);
    assert(!fittedBootCanvasUv({800,1280},nativeUv,fitted));
    assert(!fittedBootCanvasUv({1920,1080},nativeUv,fitted));
    auto changed=nativeUv; changed[4]=1;
    assert(!fittedBootCanvasUv({1920,1280},changed,fitted));
    changed=nativeUv; changed[1]=std::numeric_limits<float>::quiet_NaN();
    assert(!fittedBootCanvasUv({1920,1280},changed,fitted));
    assert(bootHorizontalOffset(true, 1920, 1080) == 70);
    assert(bootHorizontalOffset(false, 1920, 1080) == 0);
    assert(bootHorizontalOffset(true, 800, 1280) == 0);
    assert(bootHorizontalOffset(true, 1920, 1280) == 0);
    BootFontContext slot;
    assert(translatedBootX(slot, 2, 50, 10) == 50);
    {
        ScopedBootFontContext scope(slot, {1, 2, 3, 70});
        assert(translatedBootX(slot, 2, 50, 10) * 8 == 960);
        // Relative spacing remains native: label/status spacing and text scale
        // are unchanged. Version/title/row origins share one translation.
        assert(translatedBootX(slot, 3, 60, 42) - translatedBootX(slot, 3, 40, 42) == 20);
        assert(translatedBootX(slot, 2, 5, 5) == 75);
        assert(translatedBootX(slot, 4, 50, 10) == 50);
        assert(translatedBootX(slot, 2, 50, std::numeric_limits<float>::infinity()) == 50);
        try {
            ScopedBootFontContext nested(slot, {});
            assert(translatedBootX(slot, 2, 50, 10) == 50);
            throw 1;
        } catch (...) {}
        assert(translatedBootX(slot, 2, 50, 10) == 120);
    }
    assert(translatedBootX(slot, 2, 50, 10) == 50);
}
