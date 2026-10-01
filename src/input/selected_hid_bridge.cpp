#include "input/selected_hid_bridge.h"
#include "input/selected_hid_packet.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace bone_eater::input {
namespace {
// These function-local holders intentionally survive static destruction. The
// process owns the module and callback addresses; an exit callback only stops
// the reader, never frees the object under still-running native/API callbacks.
std::atomic<SelectedHidCabinet*>& cabinet() {
    static auto* value = new std::atomic<SelectedHidCabinet*> {nullptr};
    return *value;
}
std::atomic<bool>& initialized() {
    static auto* value = new std::atomic<bool> {false};
    return *value;
}
std::mutex& initializationMutex() {
    static auto* value = new std::mutex;
    return *value;
}
void onExit() { (void) stopSelectedHidBridge(std::chrono::milliseconds(0)); }

std::chrono::milliseconds configuredAge(const SelectedHidConfiguration& configuration) {
    return configuration.selected ? configuration.selected->maxReportAge : std::chrono::milliseconds(0);
}

bool mainFocused(HWND window) noexcept {
    if (!window || !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
    DWORD owner = 0;
    if (!GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId()) return false;
    const auto root = GetAncestor(window, GA_ROOT);
    const auto foreground = GetForegroundWindow();
    if (!root || root != window || !foreground || GetAncestor(foreground, GA_ROOT) != root) return false;
    DWORD repeatedOwner = 0;
    return IsWindowVisible(window) && !IsIconic(window) &&
        GetWindowThreadProcessId(window, &repeatedOwner) && repeatedOwner == owner &&
        GetForegroundWindow() == foreground;
}
} // namespace

SelectedHidCabinet::SelectedHidCabinet(SelectedHidConfiguration configuration)
    : runtime_(std::make_unique<SelectedHidRuntime>(configuration)),
      maximumAge_(configuredAge(configuration)), now_(&GunSourceClock::now) {
    checkConfiguration();
}

SelectedHidCabinet::SelectedHidCabinet(SelectedHidConfiguration configuration,
        std::unique_ptr<SelectedHidRuntimeReader> reader, SelectedHidRuntime::Now now,
        AimState& publication)
    : runtime_(std::make_unique<SelectedHidRuntime>(configuration, std::move(reader), now)),
      maximumAge_(configuredAge(configuration)), now_(now ? now : &GunSourceClock::now), publication_(&publication) {
    checkConfiguration();
}

void SelectedHidCabinet::checkConfiguration() {
    last_ = runtime_->statusSnapshot();
    if (last_.diagnostic != SelectedHidRuntimeDiagnostic::Ready || !last_.selectedOwnership)
        throw std::invalid_argument("Selected cabinet requires a valid explicit selected HID configuration.");
}

void SelectedHidCabinet::publish(const SelectedHidRuntimeSnapshot& snapshot) {
    if (publication_) publication_->publishSelected(snapshot.input.output, maximumAge_, snapshot.consumedAt, cabinetGeneration_);
    else publishSelectedAimSample(snapshot.input.output, maximumAge_, snapshot.consumedAt, cabinetGeneration_);
}

SelectedHidRuntimeSnapshot SelectedHidCabinet::disarmed(GunSourceClock::time_point now) const {
    auto result = last_;
    result.lifecycle = SelectedHidRuntimeLifecycle::Stopping;
    result.input.validation = SelectedHidValidation::Stopped;
    result.input.candidate.status = GunSampleStatus::Disconnected;
    result.input.candidate.rawButtons = 0;
    result.input.output.buttons = {};
    result.input.output.armed = result.input.output.sourceUsable = false;
    result.input.output.state = GunSourceState::Disconnected;
    result.consumedAt = now;
    return result;
}

SelectedHidRuntimeCommand SelectedHidCabinet::start() {
    {
        std::lock_guard<std::mutex> lock(packetMutex_);
        if (shutdownRequested_) return SelectedHidRuntimeCommand::Stopped;
    }
    const auto result = runtime_->start();
    bool shutdown = false;
    {
        std::lock_guard<std::mutex> lock(packetMutex_);
        shutdown = shutdownRequested_;
    }
    // A stop can arrive just before runtime_->start claims its own command.
    // Sticky bridge shutdown still blocks every packet, and this post-check
    // drains a late-opened reader instead of reviving selected ownership.
    return shutdown ? runtime_->stop(std::chrono::milliseconds(0)) : result;
}

SelectedHidRuntimeCommand SelectedHidCabinet::stop(std::chrono::milliseconds grace) {
    {
        std::lock_guard<std::mutex> lock(packetMutex_);
        shutdownRequested_ = true;
        last_ = disarmed(now_());
        publish(last_); // Scope/camera intent releases before any driver wait.
    }
    return runtime_->stop(grace);
}

std::unique_lock<std::mutex> SelectedHidCabinet::lockServiceCache() {
    return std::unique_lock<std::mutex>(packetMutex_);
}

bool SelectedHidCabinet::copyPacket(void* destination, const void* serviceCache, std::size_t bytes,
        bool mainWindowFocused) {
    if (!destination || !serviceCache || bytes != selectedNddPacketBytes) return false;
    std::lock_guard<std::mutex> lock(packetMutex_);
    std::memmove(destination, serviceCache, selectedNddPacketBytes);
    // Neutralize cached gun bits BEFORE any policy operation that might throw.
    // Frozen service/menu state is permitted; cached trigger holds never are.
    auto neutral = last_.input.output;
    neutral.mode = GunSourceMode::SelectedHid;
    neutral.armed = neutral.sourceUsable = false;
    neutral.buttons = {};
    packSelectedNddGun(static_cast<std::uint8_t*>(destination), bytes, neutral);
    last_ = shutdownRequested_ ? disarmed(now_()) : runtime_->poll(mainWindowFocused);
    ++cabinetGeneration_;
    std::array<std::uint8_t, selectedNddPacketBytes> complete;
    std::memcpy(complete.data(), destination, complete.size());
    const bool packed = packSelectedNddGun(complete.data(), complete.size(), last_.input.output);
    if (!packed) throw std::logic_error("Selected cabinet received a non-selected runtime tuple.");
    publish(last_);
    // ARK's native caller does not inspect the getter's AL result. Keep the
    // destination neutral until all C++ policy/publication work succeeds.
    std::memmove(destination, complete.data(), complete.size());
    return true;
}

SelectedHidBridgeStatus SelectedHidCabinet::status() const {
    std::lock_guard<std::mutex> lock(packetMutex_);
    return {true, true, shutdownRequested_, cabinetGeneration_, runtime_->statusSnapshot(),
        publication_ ? publication_->read() : readAimSnapshot()};
}

void initializeSelectedHidBridge(SelectedHidConfiguration configuration) {
    std::lock_guard<std::mutex> lock(initializationMutex());
    if (initialized().load(std::memory_order_acquire))
        throw std::logic_error("Input mode is immutable after startup initialization.");
    const auto diagnostic = SelectedHidRuntime::diagnose(configuration);
    if (diagnostic == SelectedHidRuntimeDiagnostic::Legacy) {
        initialized().store(true, std::memory_order_release);
        return;
    }
    if (diagnostic != SelectedHidRuntimeDiagnostic::Ready)
        throw std::invalid_argument("Selected input configuration failed runtime validation.");
    auto owner = std::make_unique<SelectedHidCabinet>(std::move(configuration));
    if (std::atexit(&onExit)) throw std::runtime_error("Cannot register selected input exit cleanup.");
    cabinet().store(owner.release(), std::memory_order_release);
    initialized().store(true, std::memory_order_release);
}

SelectedHidRuntimeCommand startSelectedHidBridge() noexcept {
    try {
        auto* owner = cabinet().load(std::memory_order_acquire);
        return owner ? owner->start() : SelectedHidRuntimeCommand::Legacy;
    } catch (...) {
        (void) stopSelectedHidBridge(std::chrono::milliseconds(0));
        return SelectedHidRuntimeCommand::Failed;
    }
}

SelectedHidRuntimeCommand stopSelectedHidBridge(std::chrono::milliseconds grace) noexcept {
    try {
        auto* owner = cabinet().load(std::memory_order_acquire);
        return owner ? owner->stop(grace) : SelectedHidRuntimeCommand::Legacy;
    } catch (...) { return SelectedHidRuntimeCommand::Failed; }
}

bool selectedHidExclusive() noexcept {
    try { return cabinet().load(std::memory_order_acquire) != nullptr; }
    catch (...) { return false; }
}

SelectedHidBridgeStatus readSelectedHidBridgeStatus() noexcept {
    try {
        auto* owner = cabinet().load(std::memory_order_acquire);
        return owner ? owner->status() : SelectedHidBridgeStatus {initialized().load(), false, false, 0, std::nullopt};
    } catch (...) { return {}; }
}

std::unique_lock<std::mutex> lockSelectedNddServiceCache() {
    auto* owner = cabinet().load(std::memory_order_acquire);
    return owner ? owner->lockServiceCache() : std::unique_lock<std::mutex>();
}

bool copySelectedNddPacket(void* destination, const void* serviceCache, std::size_t bytes,
        void* actualMainWindow) noexcept {
    try {
        auto* owner = cabinet().load(std::memory_order_acquire);
        return owner && owner->copyPacket(destination, serviceCache, bytes, mainFocused(static_cast<HWND>(actualMainWindow)));
    } catch (...) {
        (void) stopSelectedHidBridge(std::chrono::milliseconds(0));
        return false;
    }
}

} // namespace bone_eater::input
