#pragma once

namespace bone_eater::camera {

enum class FramingMode {
    FixedWide,
    AimReframe,
};

struct VerticalPanConfig {
    // Positive pitch looks upward; the default excursion is eight degrees.
    double maxOffsetRadians = 0.13962634015954636;
    // In centered aim coordinates [-1, 1]; 0.20 leaves the central 20% idle.
    double deadzone = 0.20;
    double responseSeconds = 0.12;
    double maxSpeedRadiansPerSecond = 0.34906585039886590;
    // Reject a discontinuous update instead of snapping after a long stall.
    double maxDeltaSeconds = 0.10;
};

struct MainViewAimIntent {
    // Calibrated device/main-view intent: top = 0, bottom = 1.
    // Never supply resolved/world aim or coordinates affected by this pan.
    double normalizedY = 0.5;
};

struct VerticalPanInput {
    MainViewAimIntent aim;
    FramingMode mode = FramingMode::FixedWide;
    bool scopeHeld = false;
};

struct VerticalPanState {
    // A player-controlled offset only. The host still advances scripted camera
    // motion, recoil and other native camera behavior while this is locked.
    double offsetRadians = 0.0;
    double requestedOffsetRadians = 0.0;
    bool scopePanLocked = false;
};

enum class UpdateStatus {
    Accepted,
    NoTimeElapsed,
    InvalidDeltaTime,
    InvalidAim,
    InvalidMode,
};

struct VerticalPanUpdate {
    UpdateStatus status;
    VerticalPanState state;
};

class VerticalPanPolicy {
public:
    // Invalid configuration throws std::invalid_argument. Configuration stays
    // fixed for this instance; changing a live cap needs a separate policy.
    explicit VerticalPanPolicy(const VerticalPanConfig& config = {});

    // Rejected updates and zero-dt updates leave every state field unchanged.
    // Scope hold freezes the existing offset before any pan integration.
    // Release and mode changes continue smoothly from that same offset.
    VerticalPanUpdate update(const VerticalPanInput& input, double dtSeconds);

    // The only intentional discontinuous reset. Use at an explicit scene/aim
    // reinitialization boundary, not for scope activation or a routine frame.
    void resetScene() noexcept;

    const VerticalPanState& state() const noexcept { return state_; }
    const VerticalPanConfig& config() const noexcept { return config_; }

private:
    VerticalPanConfig config_;
    VerticalPanState state_;
};

}  // namespace bone_eater::camera
