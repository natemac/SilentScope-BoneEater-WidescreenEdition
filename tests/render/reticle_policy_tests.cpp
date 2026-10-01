#include "render/reticle_policy.h"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace bone_eater::render;
static void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    const auto menu = chooseReticle(true, false, true, false, 1000, 1001,
        1700, 900, false, false, true, 0, 0, false);
    require(menu.kind == ReticleKind::Menu && menu.x == 1700 && menu.y == 900,
        "menu uses desktop point without ray or scope texture");
    require(chooseReticle(true, true, true, false, 1000, 1001, 0, 1080,
        true, true, false, 960, 540, true).kind == ReticleKind::Menu,
        "scope button cannot turn menu marker into lens");
    const auto game = chooseReticle(true, false, true, true, 1000, 1001,
        1700, 900, true, true, false, 1200, 700, false);
    require(game.kind == ReticleKind::Gameplay && game.x == 1200 && game.y == 700,
        "game uses final ray rather than raw menu point, without texture");
    require(chooseReticle(true, true, true, true, 1000, 1001, 1700, 900,
        true, true, false, 1200, 700, true).kind == ReticleKind::Lens,
        "fresh native scope source permits lens");
    require(chooseReticle(true, true, true, true, 1000, 1001, 1700, 900,
        true, true, false, 1200, 700, false).kind == ReticleKind::Gameplay,
        "source loss falls back to accurate gameplay reticle");
    for (int failure = 0; failure < 7; ++failure) {
        require(chooseReticle(true, true, failure != 0, true,
            failure == 1 ? 700 : 1000, failure == 2 ? 999 : 1001,
            1700, 900, failure != 3, failure != 4, failure == 5,
            failure == 6 ? std::numeric_limits<float>::quiet_NaN() : 1200, 700, true).kind == ReticleKind::None,
            "invalid/stale state fails closed");
    }
    require(chooseReticle(false, false, true, false, 1000, 1001, 960, 540,
        false, false, true, 0, 0, false).kind == ReticleKind::None, "disabled marker stays disabled");
    const auto outside = chooseReticle(true, false, true, true, 1000, 1001,
        1920, 1080, true, true, false, 1930, -10, false);
    require(outside.x == 1930 && outside.y == -10, "offscreen ray must not clamp and misrepresent shots");
    for (auto kind : {ReticleKind::Gameplay, ReticleKind::Lens}) {
        const auto meters = chooseSideMeters(true, kind, 3, 0,0,1920,1080,1920,1080,1920,1080);
        require(meters.mask == 3 && meters.left == 0 && meters.right == 1920 && meters.bottom == 1080,
            "side rulers use both widescreen edges in scoped and unscoped play");
        const auto scaled = chooseSideMeters(true, kind, 1, 100,50,900,500,1000,600,2000,1200);
        require(scaled.mask == 1 && scaled.left == 200 && scaled.right == 1800 && scaled.top == 100 && scaled.bottom == 1000,
            "content offsets and OS-to-buffer scale apply once");
        require(std::abs(scaled.scale - 900.0f/1366.0f) < 0.0001f, "ruler proportions depend on content height");
    }
    require(!chooseSideMeters(true, ReticleKind::Menu, 3,0,0,1920,1080,1920,1080,1920,1080).mask,
        "menus never gain battle rulers");
    require(!chooseSideMeters(true, ReticleKind::Gameplay, 0,0,0,1920,1080,1920,1080,1920,1080).mask,
        "hidden native meters remain hidden");
    require(!chooseSideMeters(true, ReticleKind::Gameplay, 3,0,0,1920,1080,0,1080,1920,1080).mask,
        "invalid content geometry fails closed");
    std::cout << "reticle policy passed\n";
}
