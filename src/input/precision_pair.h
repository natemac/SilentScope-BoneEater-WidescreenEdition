#pragma once
#include "input/input_ownership_state.h"

namespace bone_eater::input {

// One native Scope invocation. No clocks, native calls, input polls or storage writes.
struct PrecisionPair {
    enum class Step { X, Y, AuthoredY, Ray, Complete };
    Step step = Step::X;
    OwnershipPoint replacement {}, nativePair {}, sentPair {};
    float authoredY = 0;
    bool substitute = false, changed = false, fault = false;
    unsigned branch = 0;

    void begin(unsigned state, const OwnershipPoint& point, bool warmed) noexcept {
        *this = {}; branch = state; replacement = point; substitute = warmed;
        fault = (state != 0 && state != 3) || !validOwnershipPoint(point);
    }
    std::uintptr_t xCaller() const noexcept { return branch == 0 ? 0xCCF5B : 0xCD0B6; }
    std::uintptr_t yCaller() const noexcept { return branch == 0 ? 0xCCF63 : 0xCD0BF; }
    float argument(bool y, std::uintptr_t caller, float native) noexcept {
        if (fault || !std::isfinite(native)) { fault = true; return native; }
        if (!y && caller == xCaller() && step == Step::X) {
            nativePair[0] = native; sentPair[0] = substitute ? replacement[0] : native;
            step = Step::Y; changed = substitute; return sentPair[0];
        }
        if (y && caller == yCaller() && step == Step::Y) {
            nativePair[1] = native; sentPair[1] = substitute ? replacement[1] : native;
            step = Step::AuthoredY; return sentPair[1];
        }
        if (y && caller == 0xCD179 && step == Step::AuthoredY) {
            authoredY = native; step = Step::Ray; return native;
        }
        fault = true; return native;
    }
    void ray(const OwnershipPoint& point, bool valid) noexcept {
        if (fault || step != Step::Ray || !valid ||
                !sameOwnershipPoint(point, {{sentPair[0], authoredY}})) { fault = true; return; }
        step = Step::Complete;
    }
    bool complete() const noexcept { return !fault && step == Step::Complete; }
};
} // namespace bone_eater::input
