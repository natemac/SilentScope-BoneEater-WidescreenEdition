#pragma once

#include "input/aim_state.h"
#include "input/selected_hid_runtime.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

namespace bone_eater::input {

struct SelectedHidBridgeStatus {
    bool configured = false, selected = false, shutdownRequested = false;
    std::uint64_t cabinetGeneration = 0;
    std::optional<SelectedHidRuntimeSnapshot> runtime; // Cached, not a fresh HID read.
    AimSnapshot aim; // Last canonical publication (also lifecycle disarms), copied under its transaction lock.
};

// Cabinet/publication transaction around one already reviewed runtime owner.
// The injectable constructor is for isolated fake-reader fixtures and opens no
// device. The production process retains its instance until process exit so
// native/API callbacks can never race a freed owner or a pending HID worker.
class SelectedHidCabinet {
public:
    explicit SelectedHidCabinet(SelectedHidConfiguration configuration);
    SelectedHidCabinet(SelectedHidConfiguration configuration,
        std::unique_ptr<SelectedHidRuntimeReader> reader, SelectedHidRuntime::Now now,
        AimState& publication);
    SelectedHidRuntimeCommand start();
    SelectedHidRuntimeCommand stop(std::chrono::milliseconds grace);

    // Shared with the selected NDD service-cache updater. No blocking reader
    // start/stop is performed with this mutex held. The cache getter takes this
    // same lock before copying service fields and resolving its one gun tuple.
    std::unique_lock<std::mutex> lockServiceCache();
    bool copyPacket(void* destination, const void* serviceCache, std::size_t bytes,
        bool mainWindowFocused);
    SelectedHidBridgeStatus status() const;

private:
    void checkConfiguration();
    void publish(const SelectedHidRuntimeSnapshot& snapshot);
    SelectedHidRuntimeSnapshot disarmed(GunSourceClock::time_point now) const;

    std::unique_ptr<SelectedHidRuntime> runtime_;
    std::chrono::milliseconds maximumAge_;
    SelectedHidRuntime::Now now_;
    AimState* publication_ = nullptr;
    mutable std::mutex packetMutex_;
    SelectedHidRuntimeSnapshot last_;
    std::uint64_t cabinetGeneration_ = 0;
    bool shutdownRequested_ = false;
};

// Startup only, before upstream/native threads: immutable one-time selection.
// Malformed/repeated initialization throws. No device is opened. Explicit
// Legacy creates no selected reader; absence of any config remains Legacy.
void initializeSelectedHidBridge(SelectedHidConfiguration configuration);
SelectedHidRuntimeCommand startSelectedHidBridge() noexcept;
SelectedHidRuntimeCommand stopSelectedHidBridge(
    std::chrono::milliseconds grace = std::chrono::milliseconds(1500)) noexcept;
bool selectedHidExclusive() noexcept;
SelectedHidBridgeStatus readSelectedHidBridgeStatus() noexcept;

std::unique_lock<std::mutex> lockSelectedNddServiceCache();
// actualMainWindow must be the existing registered NDD_MAIN_WINDOW. Focus is
// verified against that exact visible/noniconic same-process foreground root.
bool copySelectedNddPacket(void* destination, const void* serviceCache, std::size_t bytes,
    void* actualMainWindow) noexcept;

} // namespace bone_eater::input
