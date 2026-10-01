#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bone_eater::input {

using HidClock = std::chrono::steady_clock;

struct HidValueCapability {
    std::uint16_t usagePage = 0, usageMin = 0, usageMax = 0, link = 0, bitSize = 0;
    std::uint8_t report = 0;
    bool absolute = false;
    std::int64_t logicalMin = 0, logicalMax = 0;
};
struct HidButtonCapability {
    std::uint16_t usagePage = 0, usageMin = 0, usageMax = 0, link = 0;
    std::uint8_t report = 0;
};
struct SelectedHidContract {
    bool valid = false;
    std::uint16_t xLink = 0, yLink = 0, buttonLink = 0;
};

// Bound the first implementation to the descriptor actually inventoried here.
// Identify axes by usage, never by descriptor order or guessed packet offsets.
inline SelectedHidContract selectedHidContract(std::uint16_t page, std::uint16_t usage,
        std::uint16_t reportBytes, const std::vector<HidValueCapability>& values,
        const std::vector<HidButtonCapability>& buttons) noexcept {
    SelectedHidContract result;
    if (page != 1 || usage != 5 || reportBytes != 15) return result;
    unsigned xs = 0, ys = 0, groups = 0;
    for (const auto& cap : values) {
        if (cap.usagePage != 1) continue;
        const bool x = cap.usageMin <= 0x30 && cap.usageMax >= 0x30;
        const bool y = cap.usageMin <= 0x31 && cap.usageMax >= 0x31;
        if (!x && !y) continue;
        if (!cap.absolute || cap.bitSize != 16 || cap.logicalMin != 0 || cap.logicalMax != 65535 ||
                cap.report != 3 || cap.link != 1) return {};
        if (x) { ++xs; result.xLink = cap.link; }
        if (y) { ++ys; result.yLink = cap.link; }
    }
    for (const auto& cap : buttons) {
        if (cap.usagePage != 9) continue;
        if (cap.report != 3 || cap.link != 1 || cap.usageMin != 1 || cap.usageMax != 16) return {};
        ++groups;
        result.buttonLink = cap.link;
    }
    result.valid = xs == 1 && ys == 1 && groups == 1;
    return result;
}

enum class SelectedHidStatus { Unselected, Waiting, Active, InvalidReport, Disconnected, Stopped, Stale };

struct SelectedHidSample {
    bool valid = false;
    SelectedHidStatus status = SelectedHidStatus::Unselected;
    std::uint16_t x = 0, y = 0, buttons = 0; // Raw usage1..16 bits; no gameplay meanings assigned.
    std::uint64_t session = 0, generation = 0;
    HidClock::time_point timestamp {};
    double normalizedX() const noexcept { return x / 65535.0; }
    double normalizedY() const noexcept { return y / 65535.0; }
};

// Portable publication/freshness boundary. One report becomes one immutable
// copied sample; invalid/stale samples have no owned button bits.
class SelectedHidState {
public:
    explicit SelectedHidState(std::uint64_t session) {
        sample_.session = session;
        sample_.status = SelectedHidStatus::Waiting;
    }
    bool publish(const std::uint8_t* report, std::size_t length,
            std::uint32_t x, std::uint32_t y, std::uint32_t buttons,
            HidClock::time_point timestamp = HidClock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++sample_.generation;
        if (!report || length != 15 || report[0] != 3 || x > 65535 || y > 65535 || buttons > 65535) {
            sample_.valid = false;
            sample_.status = SelectedHidStatus::InvalidReport;
            sample_.buttons = 0;
            return false;
        }
        sample_.x = static_cast<std::uint16_t>(x);
        sample_.y = static_cast<std::uint16_t>(y);
        sample_.buttons = static_cast<std::uint16_t>(buttons);
        sample_.timestamp = timestamp;
        sample_.valid = true;
        sample_.status = SelectedHidStatus::Active;
        return true;
    }
    void invalidate(SelectedHidStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_.valid = false;
        sample_.buttons = 0;
        sample_.status = status;
        ++sample_.generation;
    }
    SelectedHidSample snapshot(std::chrono::milliseconds maxAge) const {
        std::lock_guard<std::mutex> lock(mutex_);
        // Take 'now' after acquiring the sample. A timestamp captured before
        // the lock could precede a concurrently published report and falsely
        // classify a fresh stationary gun sample as future/stale.
        return aged(sample_, maxAge, HidClock::now());
    }
    SelectedHidSample snapshot(std::chrono::milliseconds maxAge, HidClock::time_point now) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return aged(sample_, maxAge, now);
    }
private:
    static SelectedHidSample aged(SelectedHidSample result, std::chrono::milliseconds maxAge,
            HidClock::time_point now) noexcept {
        if (result.valid && (maxAge.count() <= 0 || now < result.timestamp || now - result.timestamp > maxAge)) {
            result.valid = false;
            result.buttons = 0;
            result.status = SelectedHidStatus::Stale;
        }
        return result;
    }
    mutable std::mutex mutex_;
    SelectedHidSample sample_;
};

struct SelectedHidSelection {
    // Optional exact case-insensitive interface path; VID/PID are always
    // mandatory. Empty path permits only one unique matching HID collection.
    std::wstring interfacePath;
    std::uint16_t vendor = 0, product = 0;
};
enum class SelectedHidStart {
    Started, UnsupportedSelection, NotFound, Ambiguous, EnumerationFailed,
    OpenFailed, DescriptorMismatch, ResourceFailure, CancellationPending
};

class SelectedHidReader {
public:
    SelectedHidReader();
    ~SelectedHidReader();
    SelectedHidReader(const SelectedHidReader&) = delete;
    SelectedHidReader& operator=(const SelectedHidReader&) = delete;

    // No environment/default activation: caller must explicitly select/start.
    // Read-only HID input handle, no Raw Input registration or keyboard capture.
    SelectedHidStart start(const SelectedHidSelection& selection) noexcept;
    // Immediately invalidates buttons. Waits at most grace for cancellation.
    // False means cancellation still pending; buffers/handles stay owned until
    // the OS finishes. Destruction can detach that cleanup worker safely.
    bool stop(std::chrono::milliseconds grace = std::chrono::milliseconds(1500)) noexcept;
    // Caller chooses age after measuring the device's stationary report cadence.
    // Invalid samples release buttons while selected ownership stays explicit;
    // they must not silently transfer ownership to the emulated OS mouse.
    SelectedHidSample snapshot(std::chrono::milliseconds maxAge) const noexcept;
private:
#ifdef BONE_EATER_SELECTED_HID_TESTING
    friend bool selectedHidCancellationFixture();
#endif
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#ifdef BONE_EATER_SELECTED_HID_TESTING
// Local named-pipe fixture only; never opens a physical input device.
bool selectedHidCancellationFixture();
#endif

} // namespace bone_eater::input
