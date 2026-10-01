#pragma once

#include "input/source_policy.h"

#include <chrono>
#include <cstdint>
#include <mutex>

namespace bone_eater::input {

struct AimSnapshot {
    // The same device/main-view coordinates passed to the NDD cabinet packet,
    // before that protocol's X inversion. Not a camera-adjusted world ray.
    std::uint16_t rawX = 0;
    std::uint16_t rawY = 0;
    double normalizedX = 0.5;
    double normalizedY = 0.5;

    // Input intent only: these do not prove the engine activated its scope.
    bool scopeRightHeld = false;
    bool scopeLeftHeld = false;
    bool triggerHeld = false;

    // False until the first cabinet update; default center is only a fallback.
    bool valid = false;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point sampledAt{};

    // sampledAt/generation describe publication at the cabinet boundary, not
    // new physical reports. Legacy publication retains its existing behavior.
    GunSourceMode sourceMode = GunSourceMode::Legacy;
    GunSourceState sourceState = GunSourceState::Unavailable;
    bool aimKnown = false, armed = false;
    GunSampleIdentity sourceIdentity;
    std::chrono::milliseconds sourceMaximumAge {0}; // Explicit selected profile.
    std::uint64_t sourceConsumptionGeneration = 0;
    // Counts actual returned selected NDD packets. Lifecycle disarms can publish
    // new canonical metadata without pretending another packet was consumed.
    std::uint64_t cabinetGeneration = 0;

    bool scopeIntentHeld() const noexcept { return scopeRightHeld || scopeLeftHeld; }
};

// The lock protects the entire sample; readers cannot combine axes/buttons from
// different cabinet updates. No input is polled and no camera state is stored.
class AimState {
public:
    void publish(std::uint16_t x, std::uint16_t y,
                 bool scopeRight, bool scopeLeft, bool trigger);
    void publishSelected(const GunSourceOutput& input, std::chrono::milliseconds maximumReportAge,
        GunSourceClock::time_point consumedAt, std::uint64_t cabinetGeneration);
    AimSnapshot read() const;

private:
    mutable std::mutex mutex_;
    AimSnapshot snapshot_;
};

// One runtime instance shared by the cabinet input publisher and compositor.
void publishAimSample(std::uint16_t x, std::uint16_t y,
                      bool scopeRight, bool scopeLeft, bool trigger);
void publishSelectedAimSample(const GunSourceOutput& input, std::chrono::milliseconds maximumReportAge,
    GunSourceClock::time_point consumedAt, std::uint64_t cabinetGeneration);
AimSnapshot readAimSnapshot();

// All aim-intent consumers use this boundary. Selected mode additionally checks
// the original receipt age; cabinet polling cannot renew an old hardware report.
// maximumCabinetAge is the consumer's existing freshness limit, independent of
// the explicitly chosen physical-report age. Future times fail closed.
bool aimUsableAt(const AimSnapshot& sample, GunSourceClock::time_point now,
    std::chrono::milliseconds maximumCabinetAge) noexcept;

}  // namespace bone_eater::input
