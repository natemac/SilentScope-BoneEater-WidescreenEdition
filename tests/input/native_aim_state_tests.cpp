#include "input/native_aim.h"

#include <atomic>
#include <cassert>
#include <limits>
#include <thread>
#include <vector>

using bone_eater::input::NativeAimState;
using bone_eater::input::NativeRayState;

std::array<std::uint8_t, 52> packet(float x, float y, std::int32_t rawX, std::int32_t rawY) {
    std::array<std::uint8_t, 52> value;
    value.fill(0xA5); // Unrelated buttons/fields must not affect coordinate decoding.
    std::memcpy(value.data() + 0x24, &x, 4);
    std::memcpy(value.data() + 0x28, &y, 4);
    std::memcpy(value.data() + 0x2C, &rawX, 4);
    std::memcpy(value.data() + 0x30, &rawY, 4);
    return value;
}

int main() {
    NativeAimState state;
    assert(!state.read().valid && state.read().generation == 0);
    const auto bytes = packet(768.f, 580.f, 2048, 2047);
    const auto untouched = bytes;
    state.publish(bytes, 800, 1280);
    const auto first = state.read();
    assert(first.valid && first.generation == 1);
    assert(first.logicalX == 768.f && first.logicalY == 580.f);
    assert(first.cabinetX == 2048 && first.cabinetY == 2047);
    assert(first.dimensionsValid && first.observedArkWidth == 800 && first.observedArkHeight == 1280);
    assert(bytes == untouched); // Publishing preserves the entire caller output.
    state.publish(packet(-1.f, 1400.f, 0, 4095));
    assert(state.read().valid); // Logical coordinates are not clamped or normalized.
    assert(!state.read().dimensionsValid); // Missing dimensions do not invent a coordinate domain.
    state.publish(packet(std::numeric_limits<float>::quiet_NaN(), 1.f, 1, 1));
    assert(!state.read().valid);
    state.publish(packet(1.f, std::numeric_limits<float>::infinity(), 1, 1));
    assert(!state.read().valid);
    state.publish(packet(1.f, 1.f, -1, 1));
    assert(!state.read().valid);
    state.publish(packet(1.f, 1.f, 1, 4096));
    assert(!state.read().valid);
    state.publish(bytes);
    const auto recovered = state.read();
    assert(recovered.valid && recovered.sampledAt >= first.sampledAt);
    state.invalidate();
    assert(!state.read().valid && state.read().generation == recovered.generation + 1);
    assert(first.valid && first.logicalX == 768.f); // Previous copies stay immutable.

    NativeRayState ray;
    assert(!ray.read().valid && ray.read().generation == 0);
    ray.publish(410.5f, 620.25f, 0x1000, true, true, false);
    const auto finalPoint = ray.read();
    assert(finalPoint.valid && finalPoint.logicalX == 410.5f && finalPoint.logicalY == 620.25f);
    assert(finalPoint.camera == 0x1000 && finalPoint.generation == 1);
    assert(finalPoint.scopeOffKnown && !finalPoint.scopeOff);
    // The final ray is independent of ARK's earlier point, including generations.
    assert(first.logicalX == 768.f && finalPoint.logicalX != first.logicalX);
    ray.publish(2.f, 3.f, 0x2000, false);
    assert(!ray.read().valid && ray.read().generation == 2);
    assert(!ray.read().scopeOffKnown && ray.read().scopeOff);
    ray.publish(2.f, 3.f, 0, true);
    assert(!ray.read().valid);
    ray.publish(std::numeric_limits<float>::infinity(), 3.f, 0x1000, true);
    assert(!ray.read().valid);
    ray.publish(-2.f, 1400.f, 0x1000, true);
    assert(ray.read().valid && ray.read().logicalX == -2.f); // No clamping or smoothing.
    ray.publish(2.f, 3.f, 0x1000, true, false, false);
    assert(!ray.read().scopeOffKnown && ray.read().scopeOff); // Unknown always fails closed.
    ray.publish(2.f, 3.f, 0x1000, true, true, true);
    assert(ray.read().scopeOffKnown && ray.read().scopeOff);

    NativeAimState concurrent;
    NativeRayState concurrentRay;
    std::atomic<bool> complete {false};
    std::atomic<bool> inconsistent {false};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            std::uint64_t prior = 0;
            std::chrono::steady_clock::time_point previousTime {};
            while (!complete.load()) {
                const auto sample = concurrent.read();
                if (sample.generation < prior || sample.sampledAt < previousTime) inconsistent = true;
                prior = sample.generation;
                previousTime = sample.sampledAt;
                if (sample.valid && (sample.logicalX != sample.cabinetX * 2.f ||
                        sample.logicalY != sample.cabinetY * 3.f ||
                        sample.cabinetX + sample.cabinetY != 4095)) inconsistent = true;
                const auto point = concurrentRay.read();
                if (point.valid && (point.logicalY != point.logicalX * 3.f ||
                        point.camera != static_cast<std::uintptr_t>(point.logicalX) + 1 ||
                        !point.scopeOffKnown || point.scopeOff != (point.camera % 2 == 0))) inconsistent = true;
            }
        });
    }
    for (int i = 0; i < 100000; ++i) {
        const int rawX = i % 4096, rawY = 4095 - rawX;
        concurrent.publish(packet(rawX * 2.f, rawY * 3.f, rawX, rawY));
        concurrentRay.publish(static_cast<float>(rawX), rawX * 3.f, rawX + 1, true, true, (rawX + 1) % 2 == 0);
    }
    complete = true;
    for (auto& thread : readers) thread.join();
    assert(!inconsistent.load());
    assert(concurrent.read().generation == 100000);
    assert(concurrentRay.read().generation == 100000);
}
