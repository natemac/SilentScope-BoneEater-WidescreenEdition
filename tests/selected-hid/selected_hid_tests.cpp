#include "input/selected_hid.h"
#include <array>
#include <atomic>
#include <cassert>
#include <thread>

using namespace bone_eater::input;
using namespace std::chrono_literals;

int main() {
    // Descriptor order is the inventoried Ry,Rx,Y,X,hat. Selection is by usage.
    const std::vector<HidValueCapability> values {
        {1,0x34,0x34,1,16,3,true,0,65535}, {1,0x33,0x33,1,16,3,true,0,65535},
        {1,0x31,0x31,1,16,3,true,0,65535}, {1,0x30,0x30,1,16,3,true,0,65535},
        {1,0x39,0x39,1,8,3,true,1,8}};
    const std::vector<HidButtonCapability> buttons {{9,1,16,1,3}};
    const auto contract = selectedHidContract(1, 5, 15, values, buttons);
    assert(contract.valid && contract.xLink == 1 && contract.yLink == 1 && contract.buttonLink == 1);
    auto reordered = values;
    std::swap(reordered[0], reordered[3]);
    assert(selectedHidContract(1, 5, 15, reordered, buttons).valid);
    for (unsigned field = 0; field < 6; ++field) {
        auto invalid = values;
        switch (field) {
            case 0: invalid[3].absolute = false; break;
            case 1: invalid[3].report = 4; break;
            case 2: invalid[2].logicalMin = -32768; break;
            case 3: invalid[2].logicalMax = 4095; break;
            case 4: invalid[2].bitSize = 12; break;
            case 5: invalid[2].link = 2; break;
        }
        assert(!selectedHidContract(1, 5, 15, invalid, buttons).valid);
    }
    auto duplicate = values;
    duplicate.push_back(values[3]);
    assert(!selectedHidContract(1, 5, 15, duplicate, buttons).valid);
    auto missing = values;
    missing.erase(missing.begin() + 3);
    assert(!selectedHidContract(1, 5, 15, missing, buttons).valid);
    auto wrongButtons = buttons;
    wrongButtons[0].report = 4;
    assert(!selectedHidContract(1, 5, 15, values, wrongButtons).valid);
    assert(!selectedHidContract(1, 2, 15, values, buttons).valid);
    assert(!selectedHidContract(1, 5, 14, values, buttons).valid);

    std::array<std::uint8_t, 15> report {};
    report[0] = 3;
    const auto time = HidClock::time_point(10s);
    SelectedHidState state(42);
    assert(!state.snapshot(100ms, time).valid);
    assert(state.publish(report.data(), report.size(), 0, 65535, 0x8001, time));
    const auto first = state.snapshot(100ms, time);
    assert(first.valid && first.session == 42 && first.generation == 1);
    assert(first.normalizedX() == 0 && first.normalizedY() == 1 && first.buttons == 0x8001);
    assert(state.snapshot(100ms, time + 100ms).valid);
    const auto stale = state.snapshot(100ms, time + 101ms);
    assert(!stale.valid && stale.buttons == 0 && stale.status == SelectedHidStatus::Stale);
    assert(!state.snapshot(100ms, time - 1ms).valid);
    assert(!state.snapshot(0ms, time).valid);
    assert(first.valid && first.buttons == 0x8001); // Previous copies are immutable.
    assert(state.publish(report.data(), report.size(), 65535, 0, 0, time + 200ms));
    auto released = state.snapshot(100ms, time + 200ms);
    assert(released.valid && released.normalizedX() == 1 && released.normalizedY() == 0 && released.buttons == 0);
    assert(!state.publish(report.data(), 14, 1, 2, 3, time));
    assert(!state.snapshot(100ms, time).valid && state.snapshot(100ms, time).buttons == 0);
    report[0] = 4;
    assert(!state.publish(report.data(), report.size(), 1, 2, 3, time));
    report[0] = 3;
    assert(!state.publish(nullptr, report.size(), 1, 2, 3, time));
    assert(!state.publish(report.data(), report.size(), 65536, 0, 0, time));
    assert(!state.publish(report.data(), report.size(), 0, 65536, 0, time));
    assert(!state.publish(report.data(), report.size(), 0, 0, 65536, time));
    assert(state.publish(report.data(), report.size(), 1, 2, 3, time));
    state.invalidate(SelectedHidStatus::Disconnected);
    const auto disconnected = state.snapshot(100ms, time);
    assert(!disconnected.valid && disconnected.buttons == 0 && disconnected.status == SelectedHidStatus::Disconnected);

    // A gun report's axes and button bits must never come from different
    // publication generations, even with concurrent render/gameplay readers.
    SelectedHidState concurrent(99);
    std::atomic<bool> done {false};
    std::atomic<unsigned> observations {0};
    std::array<std::thread, 3> readers;
    for (auto& reader : readers) reader = std::thread([&] {
        std::uint64_t previous = 0;
        do {
            const auto sample = concurrent.snapshot(24h);
            if (sample.valid) {
                assert(sample.session == 99 && sample.generation >= previous);
                assert(sample.y == static_cast<std::uint16_t>(65535u - sample.x));
                assert(sample.buttons == sample.x);
                previous = sample.generation;
                ++observations;
            } else assert(sample.status == SelectedHidStatus::Waiting);
        } while (!done.load());
    });
    for (unsigned i = 0; i < 100000; ++i) {
        const auto x = i & 65535;
        assert(concurrent.publish(report.data(), report.size(), x, 65535 - x, x));
    }
    done = true;
    for (auto& reader : readers) reader.join();
    assert(observations > 0);

#ifdef _WIN32
    // Constructor/destructor and unsupported selection cannot open hardware.
    SelectedHidReader reader;
    assert(!reader.snapshot(100ms).valid);
    assert(reader.start({L"", 0, 0}) == SelectedHidStart::UnsupportedSelection);
    assert(reader.start({std::wstring(L"path\0suffix", 11), 0x1209, 1}) == SelectedHidStart::UnsupportedSelection);
    assert(reader.stop(0ms));
    assert(selectedHidCancellationFixture());
#endif
}
