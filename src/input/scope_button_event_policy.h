#pragma once
#include "input/scope_control.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace bone_eater::input {

// One producer/consumer lock is supplied by the runtime adapter. Only events
// create down edges; asynchronous state can rearm or repair a missing release.
class ScopeButtonEventPolicy {
public:
    static constexpr std::size_t capacity = 64;
    void invalidate() noexcept {
        active_ = armed_ = false; observed_ = 0; count_ = 0;
    }
    void observe(unsigned binding, bool down, std::uint64_t time) noexcept {
        if (!active_ || !armed_ || binding >= 8) return;
        const auto bit = std::uint32_t{1} << binding;
        const auto next = down ? observed_ | bit : observed_ & ~bit;
        if (next == observed_) return; // Repeats and duplicate releases.
        if (count_ == capacity) { invalidate(); return; }
        observed_ = next;
        queue_[count_++] = {time, observed_};
    }

    template<class Publish>
    ScopeControlSnapshot consume(ScopeControlInput request, std::uint32_t asyncHeld,
            Publish publish) noexcept {
        const bool same = active_ && context_.contextIdentity == request.contextIdentity &&
            context_.nativeScopeOwner == request.nativeScopeOwner &&
            context_.nativeInputOwner == request.nativeInputOwner;
        const bool backwards = request.nowMs < lastPublished_;
        const bool gap = !backwards && request.nowMs - lastPublished_ > 250;
        if (!request.usable || !request.contextIdentity || !same || backwards || gap) {
            invalidate();
            context_ = request;
            lastPublished_ = request.nowMs;
            auto cleared = request; cleared.usable = false; cleared.buttonsHeld = false;
            auto result = publish(cleared);
            if (!request.usable || !request.contextIdentity || backwards) return result;
            active_ = true;
            // No queued press crosses a context, source, focus or time gap.
            // A physically held binding must release before its next gesture.
            if (asyncHeld) return result;
            armed_ = true;
            request.buttonsHeld = false;
            return publish(request);
        }
        if (!armed_) {
            count_ = 0; observed_ = 0; lastPublished_ = request.nowMs;
            if (asyncHeld) { request.usable = false; request.buttonsHeld = false; return publish(request); }
            armed_ = true; request.buttonsHeld = false;
            return publish(request);
        }
        // Reject the complete batch if stale/future input or clock ordering is
        // uncertain; never partially replay a gesture and leave a held latch.
        for (std::size_t i = 0; i < count_; ++i) {
            if (queue_[i].time > request.nowMs || request.nowMs - queue_[i].time > 250 ||
                    (i && queue_[i].time < queue_[i-1].time)) {
                invalidate(); lastPublished_ = request.nowMs;
                request.usable = false; request.buttonsHeld = false;
                return publish(request);
            }
        }
        ScopeControlSnapshot result;
        for (std::size_t i = 0; i < count_; ++i) {
            auto event = request;
            // The window and game threads may publish in different orders.
            // Clamp late-dispatched events to the previous native publication
            // so genuine between-frame taps survive without time reversal.
            event.nowMs = (std::max)(lastPublished_, queue_[i].time);
            event.buttonsHeld = queue_[i].held != 0;
            result = publish(event);
            lastPublished_ = event.nowMs;
        }
        count_ = 0;
        observed_ &= asyncHeld; // Missing UP recovery; never synthesize DOWN.
        request.buttonsHeld = observed_ != 0;
        result = publish(request);
        lastPublished_ = request.nowMs;
        return result;
    }
private:
    struct Event { std::uint64_t time = 0; std::uint32_t held = 0; };
    std::array<Event, capacity> queue_ {};
    ScopeControlInput context_;
    std::uint64_t lastPublished_ = 0;
    std::uint32_t observed_ = 0;
    std::size_t count_ = 0;
    bool active_ = false, armed_ = false;
};
} // namespace bone_eater::input
