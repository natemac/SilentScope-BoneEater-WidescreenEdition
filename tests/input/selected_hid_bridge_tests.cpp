#include "input/selected_hid_bridge.h"
#include "input/selected_hid_packet.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bone_eater::input;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
std::atomic<std::int64_t> milliseconds {1000};
GunSourceClock::time_point at(std::int64_t ms) {
    return GunSourceClock::time_point(std::chrono::duration_cast<GunSourceClock::duration>(std::chrono::milliseconds(ms)));
}
GunSourceClock::time_point now() noexcept { return at(milliseconds.load()); }
SelectedHidConfiguration configuration() {
    SelectedHidConfiguration config;
    config.mode = GunSourceMode::SelectedHid;
    config.selected.emplace();
    auto& profile = *config.selected;
    profile.vendor = 0x1209; profile.product = 1;
    profile.interfacePath = "\\\\?\\fixture";
    profile.buttons = {4u, 16u, 1u};
    profile.maxReportAge = 100ms;
    return config;
}
SelectedHidSample sample(std::uint64_t generation, std::uint16_t buttons = 0, std::int64_t stamp = 1000) {
    return {true, SelectedHidStatus::Active, 0x1234, 0xABCD, buttons, 7, generation, at(stamp)};
}
struct State {
    mutable std::mutex mutex;
    std::condition_variable changed;
    SelectedHidSample report = sample(1);
    unsigned starts = 0, stops = 0, reads = 0;
    bool holdStart = false, holdStop = false, startEntered = false, stopEntered = false, release = false;
    bool stopResult = true;
    void publish(SelectedHidSample value) { std::lock_guard<std::mutex> lock(mutex); report = value; }
    bool entered(bool start) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return start ? startEntered : stopEntered; });
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); release = true; changed.notify_all(); }
};
class Reader final : public SelectedHidRuntimeReader {
public:
    explicit Reader(std::shared_ptr<State> state) : state_(std::move(state)) {}
    SelectedHidStart start(const SelectedHidSelection&) noexcept override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->starts; state_->startEntered = true; state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return !state_->holdStart || state_->release; });
        return SelectedHidStart::Started;
    }
    bool stop(std::chrono::milliseconds) noexcept override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->stops; state_->stopEntered = true; state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return !state_->holdStop || state_->release; });
        return state_->stopResult;
    }
    SelectedHidSample snapshot(std::chrono::milliseconds) const noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex); ++state_->reads; return state_->report;
    }
private:
    std::shared_ptr<State> state_;
};
using Packet = std::array<std::uint8_t, selectedNddPacketBytes>;
Packet packet(SelectedHidCabinet& cabinet, const Packet& cache, bool focused = true) {
    Packet output;
    CHECK(cabinet.copyPacket(output.data(), cache.data(), output.size(), focused));
    return output;
}
void preservedService(const Packet& before, const Packet& after) {
    for (std::size_t i = 0; i < before.size(); ++i)
        if (i != 4 && (i < 8 || i > 11)) CHECK(before[i] == after[i]);
    CHECK((before[4] & 0x1F) == (after[4] & 0x1F));
}
void arm(SelectedHidCabinet& cabinet, const Packet& cache, AimState& aim) {
    CHECK(cabinet.start() == SelectedHidRuntimeCommand::Started);
    packet(cabinet, cache);
    if (!aim.read().valid) packet(cabinet, cache);
    CHECK(aim.read().valid && aim.read().armed);
}

void unopenedGettersAreNeutralButPreserveServices() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0xFF);
    const auto value = packet(cabinet, cache);
    preservedService(cache, value);
    CHECK(value[4] == 0x1F && value[8] == 0x80 && value[9] == 0 && value[10] == 0x7F && value[11] == 0xFF);
    CHECK(!aim.read().valid && !aim.read().aimKnown && !aim.read().scopeIntentHeld());
    CHECK(aim.read().cabinetGeneration == 1 && state->reads == 0 && state->starts == 0);
}
void eachReturnedPacketAndMetadataUseOneCopiedReport() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0xA5);
    arm(cabinet, cache, aim);
    state->publish(sample(2, 0x8008));
    const auto readCount = state->reads;
    const auto value = packet(cabinet, cache);
    const auto first = aim.read();
    CHECK(state->reads == readCount + 1);
    CHECK((value[4] & 0xE0) == 0xA0 && first.scopeRightHeld && first.triggerHeld);
    CHECK(value[8] == 0xED && value[9] == 0xCB && value[10] == 0xAB && value[11] == 0xCD);
    CHECK(first.rawX == 0x1234 && first.rawY == 0xABCD && first.sourceIdentity.generation == 2);
    milliseconds = 1050;
    const auto repeated = packet(cabinet, cache);
    const auto second = aim.read();
    CHECK(repeated == value && state->reads == readCount + 2);
    CHECK(second.cabinetGeneration == first.cabinetGeneration + 1);
    CHECK(second.sourceConsumptionGeneration == first.sourceConsumptionGeneration + 1);
    CHECK(second.sourceIdentity.generation == 2 && second.sourceIdentity.receivedAt == at(1000));
    CHECK(second.sampledAt == at(1050));
    CHECK(cabinet.status().cabinetGeneration == second.cabinetGeneration);
}
void frozenServiceCacheCannotRetainStaleGunHolds() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0xE7); const auto original = cache;
    arm(cabinet, cache, aim);
    state->publish(sample(2, 0x8009));
    CHECK((packet(cabinet, cache)[4] & 0xE0) == 0xE0);
    milliseconds = 1101; // The service updater has not run again: cache is frozen.
    const auto released = packet(cabinet, cache);
    CHECK((released[4] & 0xE0) == 0);
    preservedService(original, released);
    CHECK(released[8] == 0xED && released[9] == 0xCB && released[10] == 0xAB && released[11] == 0xCD);
    CHECK(cache == original && !aim.read().valid && aim.read().sourceState == GunSourceState::Stale);
    state->publish(sample(3, 0x8000, 1101));
    CHECK((packet(cabinet, cache)[4] & 0xE0) == 0);
    CHECK(aim.read().sourceState == GunSourceState::AwaitNeutral);
    state->publish(sample(4, 0, 1101)); packet(cabinet, cache); CHECK(aim.read().valid);
}
void lossOfFocusReleasesAndRequiresANewerNeutralReport() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache {}; arm(cabinet, cache, aim);
    state->publish(sample(2, 8)); CHECK((packet(cabinet, cache)[4] & 0x20) != 0);
    CHECK((packet(cabinet, cache, false)[4] & 0xE0) == 0 && !aim.read().valid);
    CHECK((packet(cabinet, cache)[4] & 0xE0) == 0);
    CHECK(aim.read().sourceState == GunSourceState::AwaitFreshReport);
    state->publish(sample(3, 8)); CHECK((packet(cabinet, cache)[4] & 0xE0) == 0);
    state->publish(sample(4)); packet(cabinet, cache); CHECK(aim.read().valid);
}
void shutdownReleasesCanonicalIntentBeforeBlockedDriverStop() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0xFF); arm(cabinet, cache, aim);
    state->publish(sample(2, 0x8008)); packet(cabinet, cache);
    const auto before = aim.read(); CHECK(before.triggerHeld);
    state->holdStop = true;
    auto stop = std::async(std::launch::async, [&] { return cabinet.stop(100ms); });
    const bool entered = state->entered(false);
    const auto releasedBeforeCompletion = aim.read();
    auto getter = std::async(std::launch::async, [&] { return packet(cabinet, cache); });
    const bool responsive = getter.wait_for(1s) == std::future_status::ready;
    state->unblock();
    const auto value = getter.get(); const auto result = stop.get();
    CHECK(entered && responsive && result == SelectedHidRuntimeCommand::Stopped);
    CHECK(!releasedBeforeCompletion.valid && !releasedBeforeCompletion.triggerHeld && !releasedBeforeCompletion.scopeIntentHeld());
    CHECK(releasedBeforeCompletion.cabinetGeneration == before.cabinetGeneration);
    CHECK(releasedBeforeCompletion.sourceIdentity.generation == before.sourceIdentity.generation);
    CHECK((value[4] & 0xE0) == 0 && value[8] == 0xED && value[9] == 0xCB);
    preservedService(cache, value);
    CHECK(cabinet.status().shutdownRequested);
    CHECK(cabinet.start() == SelectedHidRuntimeCommand::Stopped && state->starts == 1);
}
void stopDuringBlockedStartCanNeverRevivePacketButtons() {
    auto state = std::make_shared<State>(); AimState aim;
    state->holdStart = true; state->report = sample(1, 0xFFFF);
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0xFF);
    auto starter = std::async(std::launch::async, [&] { return cabinet.start(); });
    const bool entered = state->entered(true);
    auto shutdown = std::async(std::launch::async, [&] {
        const auto result = cabinet.stop(0ms);
        return std::make_pair(result, packet(cabinet, cache));
    });
    const bool responsive = shutdown.wait_for(1s) == std::future_status::ready;
    state->unblock();
    const auto stopped = shutdown.get(); const auto started = starter.get();
    CHECK(entered && responsive);
    CHECK(stopped.first == SelectedHidRuntimeCommand::StopRequested);
    CHECK(started == SelectedHidRuntimeCommand::Stopped);
    CHECK((stopped.second[4] & 0xE0) == 0 && !aim.read().valid);
    CHECK(state->reads == 0 && state->starts == 1 && state->stops >= 1);
    CHECK(cabinet.start() == SelectedHidRuntimeCommand::Stopped);
}
void serviceCacheAndReturnedTuplesCannotTearAcrossThreads() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0x55); arm(cabinet, cache, aim);
    state->publish(sample(2, 0x8000));
    auto writer = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 1000; ++i) {
            auto guard = cabinet.lockServiceCache();
            cache.fill(i & 1 ? 0x55 : 0xAA);
        }
    });
    auto reader = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 1000; ++i) {
            const auto value = packet(cabinet, cache);
            for (unsigned j = 0; j < value.size(); ++j)
                if (j != 4 && (j < 8 || j > 11)) CHECK(value[j] == value[0]);
            CHECK((value[4] & 0xE0) == 0x80 && value[8] == 0xED && value[9] == 0xCB);
        }
    });
    writer.get(); reader.get();
    const auto value = aim.read();
    CHECK(value.valid && value.sourceIdentity.generation == 2 && value.sourceIdentity.receivedAt == at(1000));
    CHECK(value.cabinetGeneration == cabinet.status().cabinetGeneration);
}
void invalidBuffersAndCachedStatusDoNotConsumeReports() {
    auto state = std::make_shared<State>(); AimState aim;
    SelectedHidCabinet cabinet(configuration(), std::make_unique<Reader>(state), now, aim);
    Packet cache; cache.fill(0x55); Packet output; output.fill(0xAA);
    const auto before = output;
    CHECK(!cabinet.copyPacket(nullptr, cache.data(), output.size(), true));
    CHECK(!cabinet.copyPacket(output.data(), nullptr, output.size(), true));
    CHECK(!cabinet.copyPacket(output.data(), cache.data(), output.size()-1, true));
    CHECK(output == before && state->reads == 0 && cabinet.status().cabinetGeneration == 0);
    cabinet.status(); cabinet.status();
    CHECK(state->reads == 0 && !aim.read().valid);
}
}
int main() {
    struct Test { const char* name; void(*run)(); };
    const Test tests[] {
        {"unopened neutral packets", unopenedGettersAreNeutralButPreserveServices},
        {"single report packet/publication", eachReturnedPacketAndMetadataUseOneCopiedReport},
        {"frozen cache loss/rearm", frozenServiceCacheCannotRetainStaleGunHolds},
        {"focus loss and fresh neutral", lossOfFocusReleasesAndRequiresANewerNeutralReport},
        {"canonical release before blocked stop", shutdownReleasesCanonicalIntentBeforeBlockedDriverStop},
        {"stop during blocked start", stopDuringBlockedStartCanNeverRevivePacketButtons},
        {"service cache concurrency", serviceCacheAndReturnedTuplesCannotTearAcrossThreads},
        {"invalid buffers and cached diagnostics", invalidBuffersAndCachedStatusDoNotConsumeReports},
    };
    for (const auto& test : tests) {
        milliseconds = 1000;
        try { test.run(); }
        catch (const std::exception& error) { std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; return 1; }
    }
    std::cout << "Passed " << sizeof(tests)/sizeof(tests[0]) << " selected cabinet integration cases\n";
}
