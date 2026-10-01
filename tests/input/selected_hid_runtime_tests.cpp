#include "input/selected_hid_runtime.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bone_eater::input;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

std::atomic<std::int64_t> clockMs {1000};
std::atomic<unsigned> clockCalls {0};
HidClock::time_point at(std::int64_t milliseconds) {
    return HidClock::time_point(std::chrono::duration_cast<HidClock::duration>(std::chrono::milliseconds(milliseconds)));
}
HidClock::time_point fakeNow() noexcept { ++clockCalls; return at(clockMs.load()); }
SelectedHidSample report(std::uint64_t generation, std::uint16_t buttons = 0,
        std::int64_t receiptMs = 1000, std::uint64_t session = 1) {
    return {true, SelectedHidStatus::Active, 12345, 54321, buttons, session, generation, at(receiptMs)};
}
SelectedHidConfiguration profile() {
    SelectedHidConfiguration configuration;
    configuration.mode = GunSourceMode::SelectedHid;
    configuration.selected.emplace();
    auto& selected = *configuration.selected;
    selected.vendor = 0x1209; selected.product = 1;
    selected.interfacePath = "\\\\?\\HID#EXACT_\xC3\xA9_\xF0\x9F\x8E\xAE ";
    selected.buttons = {4u, 16u, 1u}; // Synthetic fixture, not physical assignments.
    selected.maxReportAge = 100ms; // Fixture limit, not a recommended device timeout.
    return configuration;
}

struct FakeState {
    mutable std::mutex mutex;
    std::condition_variable changed;
    SelectedHidSample sample = report(1);
    SelectedHidStart startResult = SelectedHidStart::Started;
    bool stopResult = true, blockStart = false, blockStop = false;
    bool startEntered = false, stopEntered = false, permitStart = false, permitStop = false;
    bool commandActive = false, overlap = false;
    unsigned starts = 0, stops = 0, snapshots = 0, destroyed = 0;
    std::chrono::milliseconds lastAge {0}, lastGrace {0};
    SelectedHidSelection selection;
    std::int64_t setClockAfterSnapshot = 0;

    void publish(SelectedHidSample value) { std::lock_guard<std::mutex> lock(mutex); sample = value; }
    bool entered(bool start) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return start ? startEntered : stopEntered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        permitStart = permitStop = true;
        changed.notify_all();
    }
};
class FakeReader final : public SelectedHidRuntimeReader {
public:
    explicit FakeReader(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    ~FakeReader() override { std::lock_guard<std::mutex> lock(state_->mutex); ++state_->destroyed; }
    SelectedHidStart start(const SelectedHidSelection& selection) noexcept override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->commandActive) state_->overlap = true;
        state_->commandActive = true;
        ++state_->starts;
        state_->selection = selection;
        state_->startEntered = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return !state_->blockStart || state_->permitStart; });
        state_->commandActive = false;
        return state_->startResult;
    }
    bool stop(std::chrono::milliseconds grace) noexcept override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->commandActive) state_->overlap = true;
        state_->commandActive = true;
        ++state_->stops;
        state_->lastGrace = grace;
        state_->stopEntered = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return !state_->blockStop || state_->permitStop; });
        state_->commandActive = false;
        return state_->stopResult;
    }
    SelectedHidSample snapshot(std::chrono::milliseconds age) const noexcept override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->commandActive) state_->overlap = true;
        ++state_->snapshots;
        state_->lastAge = age;
        const auto copied = state_->sample;
        if (state_->setClockAfterSnapshot) clockMs.store(state_->setClockAfterSnapshot);
        return copied;
    }
private:
    std::shared_ptr<FakeState> state_;
};
std::unique_ptr<SelectedHidRuntimeReader> backend(const std::shared_ptr<FakeState>& state) {
    return std::make_unique<FakeReader>(state);
}
void released(const SelectedHidRuntimeSnapshot& value) {
    CHECK(value.selectedOwnership);
    CHECK(value.input.output.buttons.neutral());
    CHECK(!value.input.output.armed && !value.input.output.sourceUsable);
}
void arm(SelectedHidRuntime& runtime) {
    CHECK(runtime.start() == SelectedHidRuntimeCommand::Started);
    // A lifecycle disarm can already consume the policy's initial transition;
    // fixture accepts at most one additional initial neutral boundary.
    if (!runtime.poll(true).input.output.armed) CHECK(runtime.poll(true).input.output.armed);
}

void diagnosisAndConstructionOpenNothingAndFreezeExactIdentity() {
    auto configuration = profile();
    auto state = std::make_shared<FakeState>();
    CHECK(SelectedHidRuntime::diagnose(configuration) == SelectedHidRuntimeDiagnostic::Ready);
    SelectedHidRuntime runtime(configuration, backend(state), fakeNow);
    CHECK(state->starts == 0 && state->stops == 0 && state->snapshots == 0);
    CHECK(runtime.statusSnapshot().lifecycle == SelectedHidRuntimeLifecycle::Ready);
    CHECK(runtime.statusSnapshot().input.validation == SelectedHidValidation::Waiting);
    configuration.selected->interfacePath = "changed";
    configuration.selected->buttons = {1u, 2u, {}};
    configuration.selected->maxReportAge = 1ms;
    CHECK(runtime.start() == SelectedHidRuntimeCommand::Started);
    const std::wstring expected = L"\\\\?\\HID#EXACT_\u00E9_\U0001F3AE ";
    CHECK(state->selection.interfacePath == expected); // Unicode and trailing space preserved.
    CHECK(state->selection.vendor == 0x1209 && state->selection.product == 1);
    CHECK(runtime.start() == SelectedHidRuntimeCommand::AlreadyRunning && state->starts == 1);
    runtime.poll(true);
    state->publish(report(2, 8));
    CHECK(runtime.poll(true).input.output.buttons.trigger);
    CHECK(state->lastAge == 100ms);
}

void legacyAndMalformedConfigurationsNeverAcquireAReader() {
    auto state = std::make_shared<FakeState>();
    {
        SelectedHidRuntime legacy({}, backend(state), fakeNow);
        CHECK(legacy.start() == SelectedHidRuntimeCommand::Legacy);
        CHECK(legacy.stop() == SelectedHidRuntimeCommand::Legacy);
        CHECK(!legacy.poll(true).selectedOwnership);
    }
    CHECK(state->starts == 0 && state->stops == 0 && state->snapshots == 0 && state->destroyed == 1);
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        auto configuration = profile();
        switch (mutation) {
            case 0: configuration.mode = static_cast<GunSourceMode>(-1); break;
            case 1: configuration.selected.reset(); break;
            case 2: configuration.mode = GunSourceMode::Legacy; break;
            case 3: configuration.selected->vendor = 0; break;
            case 4: configuration.selected->contract = static_cast<SelectedHidConfigContract>(-1); break;
            case 5: configuration.selected->coordinateSpace = static_cast<SelectedHidCoordinateSpace>(-1); break;
            case 6: configuration.selected->buttons = {}; break;
            case 7: configuration.selected->maxReportAge = 0ms; break;
            case 8: configuration.selected->interfacePath = ""; break;
            case 9: configuration.selected->interfacePath = std::string("abc\0def", 7); break;
            case 10: configuration.selected->interfacePath = "\xC0\xAF"; break;
            case 11: configuration.selected->interfacePath = "a\xC2\x85"; break;
            default: configuration.selected->interfacePath.assign(selectedHidPathMaximumBytes + 1, 'x'); break;
        }
        auto fake = std::make_shared<FakeState>();
        CHECK(SelectedHidRuntime::diagnose(configuration) != SelectedHidRuntimeDiagnostic::Ready);
        SelectedHidRuntime invalid(configuration, backend(fake), fakeNow);
        CHECK(invalid.start() == SelectedHidRuntimeCommand::InvalidConfiguration);
        CHECK(invalid.stop() == SelectedHidRuntimeCommand::InvalidConfiguration);
        const auto value = invalid.poll(true);
        CHECK(value.lifecycle == SelectedHidRuntimeLifecycle::InvalidConfiguration);
        released(value);
        CHECK(fake->starts == 0 && fake->stops == 0 && fake->snapshots == 0);
    }
}

void pollingIsOneTupleAndOnePostSnapshotClockRead() {
    auto state = std::make_shared<FakeState>();
    SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
    arm(runtime);
    const auto priorSnapshots = state->snapshots;
    const auto priorClocks = clockCalls.load();
    state->publish(report(2, 0x8008, 1001));
    state->setClockAfterSnapshot = 1001;
    clockMs = 1000;
    const auto value = runtime.poll(true);
    CHECK(state->snapshots == priorSnapshots + 1 && clockCalls == priorClocks + 1);
    CHECK(value.input.validation == SelectedHidValidation::ReadyForPolicy);
    CHECK(value.input.output.buttons.trigger && value.input.output.buttons.scopeRight);
    CHECK(value.input.output.aim.x == 12345 && value.input.output.aim.y == 54321);
    CHECK(value.input.output.aim.identity.generation == 2);
    CHECK(value.input.output.aim.identity.receivedAt == at(1001) && value.consumedAt == at(1001));
    const auto snapshots = state->snapshots, clocks = clockCalls.load();
    runtime.statusSnapshot(); runtime.statusSnapshot();
    CHECK(state->snapshots == snapshots && clockCalls == clocks); // Cached diagnostics never poll.
}

void staleDisconnectAndFocusNeedFreshNeutralRecovery() {
    auto state = std::make_shared<FakeState>();
    SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
    arm(runtime);
    state->publish(report(2, 0x8008));
    const auto held = runtime.poll(true);
    CHECK(held.input.output.buttons.trigger);
    clockMs = 1101;
    const auto stale = runtime.poll(true);
    released(stale);
    CHECK(stale.input.validation == SelectedHidValidation::Stale);
    CHECK(stale.input.output.aim.identity.receivedAt == at(1000));
    CHECK(stale.lifecycle == SelectedHidRuntimeLifecycle::Running && state->starts == 1);
    state->publish(report(3, 0x8008, 1101));
    CHECK(runtime.poll(true).input.output.state == GunSourceState::AwaitNeutral);
    state->publish(report(4, 0, 1101));
    CHECK(runtime.poll(true).input.output.armed);
    state->publish(report(5, 0x8000, 1101));
    CHECK(runtime.poll(true).input.output.buttons.scopeRight);
    auto disconnected = report(6, 0, 1101);
    disconnected.valid = false; disconnected.status = SelectedHidStatus::Disconnected;
    state->publish(disconnected);
    released(runtime.poll(true));
    CHECK(state->starts == 1); // No automatic reconnect or alternative source.
    state->publish(report(7, 0, 1101));
    CHECK(runtime.poll(true).input.output.armed);
    state->publish(report(8, 0, 1101));
    CHECK(runtime.poll(false).input.output.state == GunSourceState::Unfocused);
    CHECK(runtime.poll(true).input.output.state == GunSourceState::AwaitFreshReport);
    state->publish(report(9, 0x8000, 1101));
    CHECK(runtime.poll(true).input.output.state == GunSourceState::AwaitNeutral);
    state->publish(report(10, 0, 1101));
    CHECK(runtime.poll(true).input.output.armed);
}

void everyReaderStartFailureStaysExplicitlyDisarmed() {
    const SelectedHidStart failures[] {SelectedHidStart::UnsupportedSelection, SelectedHidStart::NotFound,
        SelectedHidStart::Ambiguous, SelectedHidStart::EnumerationFailed, SelectedHidStart::OpenFailed,
        SelectedHidStart::DescriptorMismatch, SelectedHidStart::ResourceFailure, SelectedHidStart::CancellationPending};
    for (const auto failure : failures) {
        auto state = std::make_shared<FakeState>();
        state->startResult = failure;
        SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
        const auto started = runtime.start();
        CHECK(started == (failure == SelectedHidStart::CancellationPending ?
            SelectedHidRuntimeCommand::CancellationPending : SelectedHidRuntimeCommand::Failed));
        const auto value = runtime.poll(true);
        released(value);
        CHECK(value.lastReaderStart == failure && state->starts == 1 && state->snapshots == 0);
        if (failure == SelectedHidStart::CancellationPending)
            CHECK(runtime.start() == SelectedHidRuntimeCommand::CancellationPending && state->starts == 1);
        CHECK(runtime.stop(0ms) == SelectedHidRuntimeCommand::Stopped);
    }
}

void stopBeforeStartIsIdempotentAndRestartHasNeutralBoundary() {
    auto state = std::make_shared<FakeState>();
    SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
    CHECK(runtime.stop() == SelectedHidRuntimeCommand::Stopped);
    CHECK(runtime.stop() == SelectedHidRuntimeCommand::Stopped && state->stops == 0);
    arm(runtime);
    state->publish(report(10, 8));
    const auto held = runtime.poll(true);
    CHECK(held.input.output.buttons.trigger);
    CHECK(runtime.stop(0ms) == SelectedHidRuntimeCommand::Stopped && state->stops == 1);
    const auto stopped = runtime.poll(true);
    released(stopped);
    CHECK(stopped.input.output.aim.identity.generation == 10);
    CHECK(runtime.stop() == SelectedHidRuntimeCommand::Stopped && state->stops == 1);
    state->publish(report(1, 8, 1000, 2));
    CHECK(runtime.start() == SelectedHidRuntimeCommand::Started);
    CHECK(runtime.poll(true).input.output.state == GunSourceState::Transition);
    CHECK(runtime.poll(true).input.output.state == GunSourceState::AwaitNeutral);
    state->publish(report(2, 0, 1000, 2));
    CHECK(runtime.poll(true).input.output.armed);
}

void pendingCancellationBlocksRestartAndRetainsReader() {
    auto state = std::make_shared<FakeState>();
    {
        SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
        arm(runtime);
        state->publish(report(2, 8));
        CHECK(runtime.poll(true).input.output.buttons.trigger);
        state->stopResult = false;
        CHECK(runtime.stop(999999ms) == SelectedHidRuntimeCommand::CancellationPending);
        CHECK(state->lastGrace == 5000ms && state->destroyed == 0);
        released(runtime.poll(true));
        CHECK(runtime.start() == SelectedHidRuntimeCommand::CancellationPending && state->starts == 1);
        CHECK(runtime.stop(-1ms) == SelectedHidRuntimeCommand::CancellationPending && state->lastGrace == 0ms);
        state->stopResult = true;
        CHECK(runtime.stop(0ms) == SelectedHidRuntimeCommand::Stopped);
        CHECK(state->destroyed == 0);
        CHECK(runtime.start() == SelectedHidRuntimeCommand::Started);
    }
    CHECK(state->destroyed == 1 && !state->overlap);
}

void stopDuringStartDisarmsWithoutWaitingOrOverlappingReaderCommands() {
    for (const bool completes : {false, true}) {
        auto state = std::make_shared<FakeState>();
        state->blockStart = true;
        state->stopResult = completes;
        SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
        auto starter = std::async(std::launch::async, [&] { return runtime.start(); });
        const bool entered = state->entered(true);
        auto observer = std::async(std::launch::async, [&] {
            const auto whileOpening = runtime.poll(true);
            const auto duplicate = runtime.start();
            const auto stopped = runtime.stop(0ms);
            const auto afterStop = runtime.poll(true);
            return std::make_tuple(whileOpening, duplicate, stopped, afterStop);
        });
        const bool responsive = observer.wait_for(1s) == std::future_status::ready;
        state->release(); // Always unblock workers before assertions or unwinding.
        const auto observed = observer.get();
        const auto finished = starter.get();
        CHECK(entered && responsive);
        CHECK(std::get<0>(observed).lifecycle == SelectedHidRuntimeLifecycle::Starting);
        released(std::get<0>(observed)); released(std::get<3>(observed));
        CHECK(std::get<1>(observed) == SelectedHidRuntimeCommand::Busy);
        CHECK(std::get<2>(observed) == SelectedHidRuntimeCommand::StopRequested);
        CHECK(finished == (completes ? SelectedHidRuntimeCommand::Stopped : SelectedHidRuntimeCommand::CancellationPending));
        CHECK(state->starts == 1 && state->stops == 1 && state->snapshots == 0 && !state->overlap);
    }
}

void pollsRemainReleasedWhileReaderStopIsBlocked() {
    auto state = std::make_shared<FakeState>();
    SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
    arm(runtime);
    state->publish(report(2, 0x8008));
    CHECK(runtime.poll(true).input.output.buttons.trigger);
    state->blockStop = true;
    const auto snapshotCount = state->snapshots;
    auto stopper = std::async(std::launch::async, [&] { return runtime.stop(100ms); });
    const bool entered = state->entered(false);
    auto observer = std::async(std::launch::async, [&] {
        const auto snapshot = runtime.poll(true);
        const auto start = runtime.start();
        const auto stop = runtime.stop(0ms);
        return std::make_tuple(snapshot, start, stop);
    });
    const bool responsive = observer.wait_for(1s) == std::future_status::ready;
    state->release();
    const auto observed = observer.get();
    const auto result = stopper.get();
    CHECK(entered && responsive && result == SelectedHidRuntimeCommand::Stopped);
    released(std::get<0>(observed));
    CHECK(std::get<0>(observed).lifecycle == SelectedHidRuntimeLifecycle::Stopping);
    CHECK(std::get<1>(observed) == SelectedHidRuntimeCommand::Busy);
    CHECK(std::get<2>(observed) == SelectedHidRuntimeCommand::StopRequested);
    CHECK(state->snapshots == snapshotCount && state->stops == 1 && !state->overlap);
}

void concurrentPollsSerializeOneConsumptionPerTuple() {
    auto state = std::make_shared<FakeState>();
    SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
    arm(runtime);
    state->publish(report(2, 0x8000));
    CHECK(runtime.poll(true).input.output.buttons.scopeRight);
    const auto before = state->snapshots;
    std::vector<std::future<std::vector<SelectedHidRuntimeSnapshot>>> workers;
    for (unsigned worker = 0; worker < 8; ++worker) workers.push_back(std::async(std::launch::async, [&] {
        std::vector<SelectedHidRuntimeSnapshot> values;
        for (unsigned i = 0; i < 50; ++i) values.push_back(runtime.poll(true));
        return values;
    }));
    std::set<std::uint64_t> consumptions;
    for (auto& worker : workers) for (const auto& value : worker.get()) {
        CHECK(value.input.output.buttons.scopeRight && !value.input.output.buttons.trigger);
        CHECK(value.input.output.aim.identity.generation == 2 && value.input.output.aim.identity.receivedAt == at(1000));
        CHECK(value.input.output.aim.x == 12345 && value.input.output.aim.y == 54321);
        CHECK(consumptions.insert(value.input.output.consumptionGeneration).second);
    }
    CHECK(consumptions.size() == 400 && state->snapshots == before + 400 && !state->overlap);
}

void destructionRequestsCleanupButDoesNotClaimPendingIOCompleted() {
    auto state = std::make_shared<FakeState>();
    state->stopResult = false;
    {
        SelectedHidRuntime runtime(profile(), backend(state), fakeNow);
        CHECK(runtime.start() == SelectedHidRuntimeCommand::Started);
    }
    CHECK(state->starts == 1 && state->stops == 1 && state->destroyed == 1);
    // The fake only verifies delegation/lifetime order. Actual OVERLAPPED buffer
    // retention remains the separately tested SelectedHidReader contract.
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] {
        {"no-open diagnosis and exact immutable identity", diagnosisAndConstructionOpenNothingAndFreezeExactIdentity},
        {"legacy and malformed config never open", legacyAndMalformedConfigurationsNeverAcquireAReader},
        {"one tuple and post-snapshot clock", pollingIsOneTupleAndOnePostSnapshotClockRead},
        {"stale disconnect and focus recovery", staleDisconnectAndFocusNeedFreshNeutralRecovery},
        {"all reader start failures", everyReaderStartFailureStaysExplicitlyDisarmed},
        {"stop idempotence and restart boundary", stopBeforeStartIsIdempotentAndRestartHasNeutralBoundary},
        {"pending cancellation ownership", pendingCancellationBlocksRestartAndRetainsReader},
        {"stop during blocked start", stopDuringStartDisarmsWithoutWaitingOrOverlappingReaderCommands},
        {"poll during blocked stop", pollsRemainReleasedWhileReaderStopIsBlocked},
        {"serialized concurrent polls", concurrentPollsSerializeOneConsumptionPerTuple},
        {"destructor cleanup delegation", destructionRequestsCleanupButDoesNotClaimPendingIOCompleted},
    };
    for (const auto& test : tests) {
        clockMs = 1000;
        clockCalls = 0;
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Passed " << (sizeof(tests) / sizeof(tests[0])) << " selected HID runtime cases\n";
    return 0;
}
