#include "input/scope_control.h"
#include <cassert>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#endif

using namespace bone_eater::input;

int main() {
    ScopeControl scope;
    auto step = [&](std::uint64_t tick, bool held, bool usable = true,
                    std::uint64_t context = 1) {
        return scope.update({tick, context, held, usable, 0x1000, 0x2000});
    };
    // Held-at-start is ignored until a neutral sample arrives.
    assert(!step(0, true).armed);
    assert(!step(1000, true).enabled);
    assert(step(1001, false).armed);
    assert(step(1010, true).enabled);
    assert(!scope.read().higher);
    assert(step(1020, false).enabled); // Opening tap stays latched.
    assert(step(1030, true).enabled); // Closing press cannot hide before classification.
    assert(!step(1040, false).enabled);

    // Hold from off -> higher -> release to lower -> tap off.
    assert(step(1100, true).enabled);
    assert(!step(1349, true).higher);
    assert(step(1350, true).higher);
    assert(step(1400, true).higher); // Keyboard repeats are the same held state.
    assert(step(1410, false).enabled && !scope.read().higher);
    step(1420, true);
    assert(!step(1430, false).enabled);

    // Hold while already latched must not produce a closing tap on release.
    step(1500, true); step(1510, false);
    step(1600, true);
    assert(step(1850, true).higher);
    assert(step(1860, false).enabled && !scope.read().higher);
    // A delayed release still classifies as a hold, without a high frame.
    step(1900, true);
    step(2140, true);
    assert(step(2200, false).enabled);

    // Caller aggregates two bindings. Releasing the first keeps second held.
    bool enter = true, mouse = false;
    step(2300, enter || mouse);
    mouse = true; step(2350, enter || mouse);
    enter = false; assert(step(2400, enter || mouse).enabled);
    assert(step(2550, enter || mouse).higher);
    mouse = false; assert(step(2560, enter || mouse).enabled && !scope.read().higher);

    // Focus/source/script invalidation closes and prevents held reactivation.
    assert(!step(2600, true, false).enabled);
    assert(!step(2610, true).armed);
    assert(!step(3000, true).enabled);
    assert(step(3010, false).armed);
    assert(step(3020, true).enabled);
    // Context replacement cannot carry a latch or a queued held action.
    assert(!step(3030, true, true, 2).enabled);
    assert(!step(3300, true, true, 2).armed);
    assert(step(3310, false, true, 2).armed);
    assert(step(3320, true, true, 2).enabled);
    // Invalid context and backwards clocks fail closed and require neutral.
    assert(!step(3330, true, true, 0).enabled);
    step(3340, false); step(3350, true);
    assert(!step(100, true).enabled);
    assert(!step(101, true).armed);
    assert(step(102, false).armed);
    step(103, true);
    assert(!scope.update({104, 1, true, true, 0x1001, 0x2000}).enabled);
    assert(!scope.update({105, 1, true, true, 0x1001, 0x2001}).armed);
    scope.reset();
    assert(step(1000, false).armed);
    step(1001, true); step(1010, false);
    assert(step(1260, false).enabled); // Exact250ms remains continuous.
    assert(!step(1511, true).enabled && !scope.read().armed);
    assert(!step(1520, true).armed);
    assert(step(1530, false).armed);

    // Exact lower and upper timing limits; invalid settings cannot latch.
    ScopeControl custom;
    custom.update({0, 1, false, true}, {100});
    custom.update({1, 1, true, true}, {100});
    assert(!custom.update({100, 1, true, true}, {100}).higher);
    assert(custom.update({101, 1, true, true}, {100}).higher);
    assert(!custom.update({102, 1, true, true}, {99}).enabled);
    assert(!custom.update({103, 1, false, true}, {1001}).armed);
    custom.update({104, 1, false, true}, {1000});
    custom.update({105, 1, true, true}, {1000});
    custom.update({355, 1, true, true}, {1000});
    custom.update({605, 1, true, true}, {1000});
    custom.update({855, 1, true, true}, {1000});
    assert(custom.update({1105, 1, true, true}, {1000}).higher);
    custom.reset();
    assert(!custom.read().enabled && !custom.read().armed);

    // Physical playtest establishes native0=stronger, native1=lower. The native
    // optical scalar is not a magnification label. Logical gains stay unchanged.
    assert(scopeZoomNeedsEdge(false, 0));
    assert(!scopeZoomNeedsEdge(true, 0));
    assert(!scopeZoomNeedsEdge(false, 1));
    assert(scopeZoomNeedsEdge(true, 1));
    for (const auto mode : {2u, 3u, 4u, 99u}) {
        assert(!scopeZoomNeedsEdge(false, mode));
        assert(!scopeZoomNeedsEdge(true, mode));
    }
    // Released during zoom-in: wait through3, request lower after stable0.
    assert(!scopeZoomNeedsEdge(false, 2));
    assert(!scopeZoomNeedsEdge(false, 1));
    assert(!scopeZoomNeedsEdge(false, 3));
    assert(scopeZoomNeedsEdge(false, 0));
    // Exercise actual gesture output through the optical adapter. Starting in
    // either stable native mode, a tap settles at lower; a hold requests higher,
    // release returns lower, and another tap still closes the logical scope.
    for (const auto initialNativeMode : {0u, 1u}) {
        ScopeControl gestures;
        auto input = [&](std::uint64_t at, bool held) {
            return gestures.update({at, 1, held, true, 0x1000, 0x2000});
        };
        input(1000, false);
        auto intent = input(1010, true);
        assert(intent.enabled && !intent.higher);
        assert(scopeZoomNeedsEdge(intent.higher, initialNativeMode) == (initialNativeMode == 0));
        intent = input(1020, false);
        assert(intent.enabled && !scopeZoomNeedsEdge(intent.higher, 1));
        input(1100, true);
        intent = input(1350, true);
        assert(intent.higher && scopeZoomNeedsEdge(intent.higher, 1));
        assert(!scopeZoomNeedsEdge(intent.higher, 3)); // Native animation runs.
        assert(!scopeZoomNeedsEdge(intent.higher, 0)); // Stronger reached.
        intent = input(1360, false);
        assert(intent.enabled && !intent.higher && scopeZoomNeedsEdge(intent.higher, 0));
        assert(!scopeZoomNeedsEdge(intent.higher, 2)); // Native zoom-out runs.
        assert(!scopeZoomNeedsEdge(intent.higher, 1)); // Lower reached.
        input(1370, true);
        assert(!input(1380, false).enabled);
    }
#ifdef _WIN32
    // Launcher/runtime flags use the Win32 environment, not the CRT snapshot.
    assert(SetEnvironmentVariableA("BONE_EATER_SCOPE_MODE", "toggle_hold"));
    assert(SetEnvironmentVariableA("BONE_EATER_SCOPE_HOLD_MS", "100"));
#else
    assert(setenv("BONE_EATER_SCOPE_MODE", "toggle_hold", 1) == 0);
    assert(setenv("BONE_EATER_SCOPE_HOLD_MS", "100", 1) == 0);
#endif
    initializeScopeControlFromEnvironment();
    assert(scopeControlRequested());
    assert(publishScopeControl({0, 1, false, true, 0x10, 0x20}).armed);
    assert(publishScopeControl({1, 1, true, true, 0x10, 0x20}).enabled);
    assert(publishScopeControl({101, 1, true, true, 0x10, 0x20}).higher);
    assert(readScopeControlSnapshot().nativeScopeOwner == 0x10);
    resetScopeControl();
    assert(!readScopeControlSnapshot().enabled && !readScopeControlSnapshot().armed);
    std::cout << "scope control transitions passed\n";
}
