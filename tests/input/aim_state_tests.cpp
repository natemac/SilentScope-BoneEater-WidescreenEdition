#include "input/aim_state.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void basicSamples() {
    bone_eater::input::AimState state;
    auto sample = state.read();
    require(!sample.valid && sample.generation == 0, "unsampled state must be invalid");
    require(sample.normalizedX == 0.5 && sample.normalizedY == 0.5,
            "unsampled visual fallback should be centered");

    state.publish(0, 65535, true, false, true);
    sample = state.read();
    require(sample.valid && sample.generation == 1, "first sample metadata");
    require(sample.normalizedX == 0.0 && sample.normalizedY == 1.0, "normalized endpoint fidelity");
    require(sample.rawX == 0 && sample.rawY == 65535, "raw endpoint fidelity");
    require(sample.scopeRightHeld && !sample.scopeLeftHeld && sample.triggerHeld,
            "independent button fidelity");
    require(sample.scopeIntentHeld(), "right scope intent");

    const auto previous = sample;
    state.publish(32768, 32767, false, true, false);
    sample = state.read();
    require(sample.generation == 2 && sample.sampledAt >= previous.sampledAt,
            "generation/time must advance monotonically");
    require(std::abs(sample.normalizedX - 32768.0 / 65535.0) < 1e-15,
            "center must not be rounded to a different aim sample");
    require(sample.scopeIntentHeld() && !sample.triggerHeld, "left scope intent/release");

    state.publish(65535, 0, false, false, false);
    sample = state.read();
    require(!sample.scopeIntentHeld(), "scope release");
    require(sample.normalizedX == 1.0 && sample.normalizedY == 0.0, "opposite endpoints");
    require(previous.rawX == 0 && previous.triggerHeld, "snapshots must remain immutable copies");
}

void coherentConcurrentSamples() {
    bone_eater::input::AimState state;
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
    std::atomic<unsigned> ready{0};
    constexpr std::uint64_t count = 100000;
    std::vector<std::thread> readers;
    for (unsigned n = 0; n < 4; ++n) {
        readers.emplace_back([&] {
            std::uint64_t previousGeneration = 0;
            std::chrono::steady_clock::time_point previousTime{};
            ++ready;
            do {
                const auto sample = state.read();
                if (!sample.valid) {
                    continue;
                }
                // Every axis and button encodes the same generation; a torn
                // sample, or a timestamp moved backwards, violates this check.
                const auto x = static_cast<std::uint16_t>(sample.generation);
                const auto y = static_cast<std::uint16_t>(65535u - x);
                if (sample.rawX != x || sample.rawY != y ||
                    sample.normalizedX != static_cast<double>(x) / 65535.0 ||
                    sample.normalizedY != static_cast<double>(y) / 65535.0 ||
                    sample.scopeRightHeld != ((x & 1) != 0) ||
                    sample.scopeLeftHeld != ((x & 2) != 0) ||
                    sample.triggerHeld != ((x & 4) != 0) ||
                    sample.generation < previousGeneration || sample.sampledAt < previousTime) {
                    failed = true;
                }
                previousGeneration = sample.generation;
                previousTime = sample.sampledAt;
            } while (!finished.load());
        });
    }
    while (ready.load() != readers.size()) {
        std::this_thread::yield();
    }
    for (std::uint64_t generation = 1; generation <= count; ++generation) {
        const auto x = static_cast<std::uint16_t>(generation);
        state.publish(x, static_cast<std::uint16_t>(65535u - x),
                      (x & 1) != 0, (x & 2) != 0, (x & 4) != 0);
    }
    finished = true;
    for (auto& reader : readers) {
        reader.join();
    }
    require(!failed.load(), "concurrent reader observed a torn/nonmonotonic sample");
    require(state.read().generation == count, "publisher lost a generation");
}
}  // namespace

int main() {
    try {
        basicSamples();
        coherentConcurrentSamples();
        bone_eater::input::publishAimSample(1024, 2048, true, false, false);
        const auto global = bone_eater::input::readAimSnapshot();
        require(global.valid && global.rawX == 1024 && global.rawY == 2048,
                "global publisher and reader must share a single state");
        std::cout << "PASS endpoints, intent/release, metadata, immutable copies, shared runtime state, "
                     "and 100000 samples with four concurrent readers\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
