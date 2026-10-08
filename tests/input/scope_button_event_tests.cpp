#include "input/scope_button_event_policy.h"
#include <cassert>
#include <iostream>

using namespace bone_eater::input;
int main() {
    ScopeButtonEventPolicy events;
    ScopeControl control;
    auto poll = [&](std::uint64_t time, std::uint32_t asyncHeld = 0,
                    bool usable = true, std::uint64_t context = 1) {
        return events.consume({time, context, false, usable, 0x10, 0x20}, asyncHeld,
            [&](const ScopeControlInput& input) noexcept { return control.update(input, {250, false}); });
    };
    assert(poll(0).armed);
    // Complete tap between two game polls must not disappear.
    events.observe(0, true, 5); events.observe(0, false, 6);
    assert(poll(16).enabled && !control.read().buttonsHeld);
    events.observe(0, true, 17); events.observe(0, true, 17); // Repeat ignored.
    events.observe(0, false, 18); events.observe(0, false, 18);
    assert(!poll(19).enabled);
    // Event down owns a sustained hold; polling extends time without new edges.
    events.observe(0, true, 20);
    assert(poll(32, 1).enabled);
    assert(!poll(128, 1).higher);
    assert(!poll(256, 1).higher);
    assert(poll(272, 1).higher);
    events.observe(0, false, 280);
    assert(poll(288).enabled && !control.read().higher);
    // Two bindings are one aggregate gesture, including overlapping releases.
    events.observe(0, true, 300); poll(304, 1);
    events.observe(1, true, 305); poll(320, 3);
    events.observe(0, false, 330);
    assert(poll(336, 2).enabled && control.read().buttonsHeld);
    assert(poll(552, 2).higher);
    events.observe(1, false, 560);
    assert(poll(576).enabled && !control.read().higher);
    // Late dispatch clamps timestamps to latest native publication. A complete
    // short tap still closes once instead of triggering a backwards-clock reset.
    events.observe(0, true, 570); events.observe(0, false, 575);
    assert(!poll(592).enabled);
    // Async state alone cannot create a down or duplicate an earlier event.
    assert(!poll(608, 1).enabled);
    assert(!poll(624).enabled);
    // Stale events reject the entire gesture rather than replaying half.
    events.observe(0, true, 300); events.observe(0, false, 301);
    assert(!poll(640).enabled && !control.read().armed);
    assert(poll(656).armed);
    // A missing UP is repaired from async neutral; delayed UP is then harmless.
    events.observe(0, true, 660);
    assert(poll(672).enabled && !control.read().buttonsHeld);
    events.observe(0, false, 660);
    assert(poll(688).enabled);
    // Focus cancellation cannot carry a latch or a held gesture into recovery.
    events.invalidate();
    assert(!poll(704, 1).enabled && !control.read().armed);
    events.observe(0, false, 710); // Unarmed stream drops it; neutral rearm below.
    assert(poll(720).armed && !control.read().enabled);
    events.observe(0, true, 724); events.observe(0, false, 725);
    assert(poll(736).enabled);
    // Scene changes discard queued old-menu gestures even without focus loss.
    events.observe(0, true, 740); events.observe(0, false, 741);
    assert(!poll(752, 0, true, 2).enabled);
    // Source/eligibility invalidation also clears intent.
    events.observe(0, true, 760);
    assert(!poll(768, 1, false, 2).enabled);
    assert(!poll(784, 1, true, 2).armed);
    assert(poll(800, 0, true, 2).armed);
    // Overflow is bounded and drops all queued actions.
    for (unsigned i = 0; i < 66; ++i) events.observe(0, (i % 2) == 0, 801 + i);
    assert(!poll(880, 0, true, 2).enabled);
    // A native update stall cannot resume an old hold at raw sensitivity.
    events.observe(0, true, 890);
    assert(poll(896, 1, true, 2).enabled);
    assert(!poll(1147, 1, true, 2).enabled && !control.read().armed);
    assert(poll(1160, 0, true, 2).armed);
    // Future and out-of-order messages fail closed.
    events.observe(0, true, 1200);
    assert(!poll(1170, 1, true, 2).armed);
    poll(1180, 0, true, 2);
    events.observe(0, true, 1185); events.observe(0, false, 1184);
    assert(!poll(1190, 0, true, 2).armed);
    poll(1200, 0, true, 2);
    assert(!poll(100, 0, true, 2).armed);
    std::cout << "scope button event policy passed\n";
}
