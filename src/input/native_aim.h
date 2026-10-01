#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace bone_eater::input {

struct NativeAimSnapshot {
    // Engine-accepted logical coordinates, without compositor transforms.
    float logicalX = 0;
    float logicalY = 0;
    std::int32_t cabinetX = 0;
    std::int32_t cabinetY = 0;
    // Read-only ARK mapping dimensions observed after this native input call.
    // They are not the main FrameBuffer's separately configured dimensions.
    std::int32_t observedArkWidth = 0;
    std::int32_t observedArkHeight = 0;
    bool dimensionsValid = false;
    bool valid = false;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point sampledAt {};
};

// A separate clock/generation from BMPU intent. Consumers must compare sample
// ages, not equate their generation numbers. Header-only state permits testing
// the native byte contract and coherent publication without loading the game.
class NativeAimState {
public:
    void publish(const std::array<std::uint8_t, 52>& output,
                 std::int32_t arkWidth = 0, std::int32_t arkHeight = 0) {
        NativeAimSnapshot value;
        static_assert(sizeof(float) == 4 && sizeof(std::int32_t) == 4);
        std::memcpy(&value.logicalX, output.data() + 0x24, 4);
        std::memcpy(&value.logicalY, output.data() + 0x28, 4);
        std::memcpy(&value.cabinetX, output.data() + 0x2C, 4);
        std::memcpy(&value.cabinetY, output.data() + 0x30, 4);
        value.observedArkWidth = arkWidth;
        value.observedArkHeight = arkHeight;
        value.dimensionsValid = arkWidth > 0 && arkWidth <= 65535 &&
            arkHeight > 0 && arkHeight <= 65535;
        value.valid = std::isfinite(value.logicalX) && std::isfinite(value.logicalY) &&
            value.cabinetX >= 0 && value.cabinetX <= 4095 &&
            value.cabinetY >= 0 && value.cabinetY <= 4095;
        const std::lock_guard<std::mutex> lock(mutex_);
        value.generation = snapshot_.generation + 1;
        value.sampledAt = std::chrono::steady_clock::now();
        snapshot_ = value;
    }

    void invalidate() {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.valid = false;
        ++snapshot_.generation;
        snapshot_.sampledAt = std::chrono::steady_clock::now();
    }

    NativeAimSnapshot read() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    NativeAimSnapshot snapshot_;
};

struct NativeRaySnapshot {
    // Exact final float arguments used by ScopeCamera's screen-to-ray call,
    // after its native smoothing/offsets. No output-space transformation.
    float logicalX = 0;
    float logicalY = 0;
    std::uintptr_t camera = 0;
    // Native script Scope Off flag, independently validated through ScopeOffUI.
    // Unknown fails closed. This flag is not a physical scope-button state.
    bool scopeOffKnown = false;
    bool scopeOff = true;
    bool valid = false;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point sampledAt {};
};

class NativeRayState {
public:
    void publish(float x, float y, std::uintptr_t camera, bool identityVerified,
                 bool scopeOffKnown = false, bool scopeOff = true) {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.logicalX = x;
        snapshot_.logicalY = y;
        snapshot_.camera = camera;
        snapshot_.scopeOffKnown = scopeOffKnown;
        snapshot_.scopeOff = !scopeOffKnown || scopeOff;
        snapshot_.valid = identityVerified && camera && std::isfinite(x) && std::isfinite(y);
        ++snapshot_.generation;
        snapshot_.sampledAt = std::chrono::steady_clock::now();
    }

    NativeRaySnapshot read() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex mutex_;
    NativeRaySnapshot snapshot_;
};

// Opt-in, before game entry: observes only the verified gamendd caller of the
// native export. No additional native getter calls. The observer preserves raw
// output; separately opting into BONE_EATER_NATIVE_INPUT=wide enables a guarded
// logical-pair conversion after that raw snapshot has been published.
// Requires detour.cpp, native_viewport.cpp's module validator, and bcrypt.lib.
void installNativeAimObserver(void* gamendd, void* arkndd) noexcept;
// Ready only after the verified native StartTrigger caller adapter is installed.
bool scopeStartRoutingReady() noexcept;
NativeAimSnapshot readNativeAimSnapshot();
// A valid ray observation does not by itself prove scope activation. Consumers
// must additionally check actual native activation, input intent, and freshness.
NativeRaySnapshot readNativeRaySnapshot();

} // namespace bone_eater::input
