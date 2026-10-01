#pragma once

#include "input/selected_hid_adapter.h"
#include "input/selected_hid_config.h"

#include <memory>
#include <mutex>
#include <optional>

namespace bone_eater::input {

enum class SelectedHidRuntimeDiagnostic {
    Ready, Legacy, InvalidMode, InvalidProfile, InvalidDevice, InvalidPath,
    InvalidContract, InvalidCoordinateSpace, InvalidMapping, InvalidReportAge,
};
enum class SelectedHidRuntimeLifecycle {
    InvalidConfiguration, Legacy, Ready, Starting, Running, StartFailed,
    Stopping, CancellationPending, Stopped,
};
enum class SelectedHidRuntimeCommand {
    Legacy, InvalidConfiguration, Started, AlreadyRunning, Busy, Failed,
    Stopped, StopRequested, CancellationPending,
};

struct SelectedHidRuntimeSnapshot {
    SelectedHidRuntimeDiagnostic diagnostic = SelectedHidRuntimeDiagnostic::InvalidProfile;
    SelectedHidRuntimeLifecycle lifecycle = SelectedHidRuntimeLifecycle::InvalidConfiguration;
    // Explicit selected ownership persists through failures. Only Legacy lets
    // a future cabinet caller use its existing mouse/API gun branch.
    bool selectedOwnership = false;
    std::optional<SelectedHidStart> lastReaderStart;
    SelectedHidAdapterResult input;
    HidClock::time_point consumedAt {};
};

// Narrow dependency boundary for deterministic lifecycle fixtures. Production
// construction installs the existing read-only SelectedHidReader. Implementors
// must make snapshot bounded/nonblocking apart from its short publication lock,
// and retain pending I/O buffers through stop(false) and destruction exactly as
// that reader does. start/stop are never invoked concurrently by this owner.
class SelectedHidRuntimeReader {
public:
    virtual ~SelectedHidRuntimeReader() = default;
    virtual SelectedHidStart start(const SelectedHidSelection& selection) noexcept = 0;
    virtual bool stop(std::chrono::milliseconds grace) noexcept = 0;
    virtual SelectedHidSample snapshot(std::chrono::milliseconds maxAge) const noexcept = 0;
};

class SelectedHidRuntime {
public:
    using Now = HidClock::time_point (*)() noexcept;

    // Both constructors only copy/validate configuration and convert its exact
    // path; neither enumerates, opens or registers an input device. Malformed
    // constructed values stay InvalidConfiguration, never become Legacy.
    explicit SelectedHidRuntime(SelectedHidConfiguration configuration);
    SelectedHidRuntime(SelectedHidConfiguration configuration,
        std::unique_ptr<SelectedHidRuntimeReader> reader, Now now = &HidClock::now);
    ~SelectedHidRuntime();
    SelectedHidRuntime(const SelectedHidRuntime&) = delete;
    SelectedHidRuntime& operator=(const SelectedHidRuntime&) = delete;

    static SelectedHidRuntimeDiagnostic diagnose(const SelectedHidConfiguration& configuration);
    SelectedHidRuntimeCommand start();
    // Releases effective buttons before calling the bounded reader stop. A
    // concurrent start receives a latched stop request and performs its cleanup
    // before returning. StopRequested does not mean that cleanup is complete.
    SelectedHidRuntimeCommand stop(std::chrono::milliseconds grace = std::chrono::milliseconds(1500));

    // Caller attests that its validated main window currently has input focus.
    // Exactly one copied report per Running poll, then 'now', then policy. No
    // game-window discovery, device reconnect, legacy polling or button merging.
    SelectedHidRuntimeSnapshot poll(bool mainWindowFocused);
    // Observational cached metadata only: never use this in place of poll to
    // refresh gameplay eligibility, because it deliberately performs no aging.
    SelectedHidRuntimeSnapshot statusSnapshot() const;

    // Object and module code must outlive all concurrent method calls. Before
    // destruction, the owner must join any thread executing start/stop/poll.
    // A cancelled driver's pending worker remains owned by SelectedHidReader.
private:
    void disarmLocked(SelectedHidStatus status);
    SelectedHidRuntimeSnapshot snapshotLocked() const;

    const SelectedHidConfiguration configuration_;
    SelectedHidSelection selection_;
    std::unique_ptr<SelectedHidRuntimeReader> reader_;
    const Now now_;
    mutable std::mutex mutex_;
    SelectedHidAdapter adapter_;
    SelectedHidRuntimeSnapshot state_;
    bool commandInFlight_ = false, stopRequested_ = false, readerNeedsStop_ = false;
    std::chrono::milliseconds stopGrace_ {1500};
};

} // namespace bone_eater::input
