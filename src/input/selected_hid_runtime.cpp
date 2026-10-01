#include "input/selected_hid_runtime.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <utility>

namespace bone_eater::input {
namespace {
class WindowsSelectedReader final : public SelectedHidRuntimeReader {
public:
    SelectedHidStart start(const SelectedHidSelection& selection) noexcept override { return reader_.start(selection); }
    bool stop(std::chrono::milliseconds grace) noexcept override { return reader_.stop(grace); }
    SelectedHidSample snapshot(std::chrono::milliseconds age) const noexcept override { return reader_.snapshot(age); }
private:
    SelectedHidReader reader_;
};

bool exactPath(const std::string& input, std::wstring& output) {
    if (input.empty() || input.size() > selectedHidPathMaximumBytes || input.find('\0') != std::string::npos)
        return false;
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            converted.data(), count) != count) return false;
    for (const auto c : converted) if (c <= 0x1F || (c >= 0x7F && c <= 0x9F)) return false;
    output = std::move(converted); // Preserve exact identity: no trimming or path expansion.
    return true;
}

SelectedHidRuntimeDiagnostic inspect(const SelectedHidConfiguration& configuration,
        SelectedHidSelection& selection) {
    using Diagnostic = SelectedHidRuntimeDiagnostic;
    if (configuration.mode == GunSourceMode::Legacy)
        return configuration.selected ? Diagnostic::InvalidProfile : Diagnostic::Legacy;
    if (configuration.mode != GunSourceMode::SelectedHid) return Diagnostic::InvalidMode;
    if (!configuration.selected) return Diagnostic::InvalidProfile;
    const auto& profile = *configuration.selected;
    if (profile.vendor != 0x1209 || (profile.product != 1 && profile.product != 2)) return Diagnostic::InvalidDevice;
    if (profile.contract != SelectedHidConfigContract::XgunnerReport3V1) return Diagnostic::InvalidContract;
    if (profile.coordinateSpace != SelectedHidCoordinateSpace::CalibratedGameContent) return Diagnostic::InvalidCoordinateSpace;
    if (!validSelectedGunButtonMapping(profile.buttons)) return Diagnostic::InvalidMapping;
    if (profile.maxReportAge.count() <= 0 || profile.maxReportAge > selectedHidMaximumReportAge)
        return Diagnostic::InvalidReportAge;
    if (!exactPath(profile.interfacePath, selection.interfacePath)) return Diagnostic::InvalidPath;
    selection.vendor = profile.vendor;
    selection.product = profile.product;
    return Diagnostic::Ready;
}

std::chrono::milliseconds boundedGrace(std::chrono::milliseconds grace) noexcept {
    return std::chrono::milliseconds(std::clamp<std::chrono::milliseconds::rep>(grace.count(), 0, 5000));
}
} // namespace

SelectedHidRuntime::SelectedHidRuntime(SelectedHidConfiguration configuration)
    : SelectedHidRuntime(std::move(configuration), std::make_unique<WindowsSelectedReader>()) {}

SelectedHidRuntime::SelectedHidRuntime(SelectedHidConfiguration configuration,
        std::unique_ptr<SelectedHidRuntimeReader> reader, Now now)
    : configuration_(std::move(configuration)), reader_(std::move(reader)), now_(now ? now : &HidClock::now) {
    state_.diagnostic = inspect(configuration_, selection_);
    state_.selectedOwnership = configuration_.mode != GunSourceMode::Legacy || configuration_.selected.has_value();
    if (state_.diagnostic == SelectedHidRuntimeDiagnostic::Legacy) {
        state_.lifecycle = SelectedHidRuntimeLifecycle::Legacy;
    } else if (state_.diagnostic == SelectedHidRuntimeDiagnostic::Ready && reader_) {
        adapter_.configure(configuration_.selected->buttons);
        state_.lifecycle = SelectedHidRuntimeLifecycle::Ready;
        state_.input.validation = SelectedHidValidation::Waiting;
        state_.input.candidate.status = GunSampleStatus::Unavailable;
        state_.input.output = adapter_.snapshot();
    } else if (state_.diagnostic == SelectedHidRuntimeDiagnostic::Ready) {
        state_.diagnostic = SelectedHidRuntimeDiagnostic::InvalidProfile;
    }
}

SelectedHidRuntime::~SelectedHidRuntime() {
    try { stop(); } catch (...) {}
    // Reader destruction performs its own bounded stop and retains any pending
    // Session in the detached cancellation worker. All external calls must have
    // completed before reaching this destructor, as with the reader itself.
}

SelectedHidRuntimeDiagnostic SelectedHidRuntime::diagnose(const SelectedHidConfiguration& configuration) {
    SelectedHidSelection unused;
    return inspect(configuration, unused);
}

SelectedHidRuntimeSnapshot SelectedHidRuntime::snapshotLocked() const { return state_; }

void SelectedHidRuntime::disarmLocked(SelectedHidStatus status) {
    SelectedHidSample sample;
    sample.status = status;
    state_.consumedAt = now_();
    state_.input = adapter_.update(sample, configuration_.selected->maxReportAge, state_.consumedAt);
}

SelectedHidRuntimeCommand SelectedHidRuntime::start() {
    using Command = SelectedHidRuntimeCommand;
    using Lifecycle = SelectedHidRuntimeLifecycle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.lifecycle == Lifecycle::Legacy) return Command::Legacy;
        if (state_.lifecycle == Lifecycle::InvalidConfiguration) return Command::InvalidConfiguration;
        if (commandInFlight_) return Command::Busy;
        if (state_.lifecycle == Lifecycle::Running) return Command::AlreadyRunning;
        // An explicit stop retry must drain pending cancellation before restart.
        if (state_.lifecycle == Lifecycle::CancellationPending) return Command::CancellationPending;
        commandInFlight_ = true;
        stopRequested_ = false;
        readerNeedsStop_ = true;
        state_.lifecycle = Lifecycle::Starting;
        disarmLocked(SelectedHidStatus::Waiting);
    }

    // No policy/state mutex is held during device resolution, open or I/O stop.
    const auto started = reader_->start(selection_);
    std::chrono::milliseconds grace;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.lastReaderStart = started;
        if (!stopRequested_) {
            commandInFlight_ = false;
            if (started == SelectedHidStart::Started) {
                state_.lifecycle = Lifecycle::Running;
                return Command::Started;
            }
            state_.lifecycle = started == SelectedHidStart::CancellationPending ?
                Lifecycle::CancellationPending : Lifecycle::StartFailed;
            disarmLocked(SelectedHidStatus::Stopped);
            return started == SelectedHidStart::CancellationPending ? Command::CancellationPending : Command::Failed;
        }
        grace = stopGrace_;
        state_.lifecycle = Lifecycle::Stopping;
    }
    // stop() arriving while the reader opens never races a second reader call;
    // this same command performs the requested cleanup before publishing state.
    const bool stopped = reader_->stop(grace);
    std::lock_guard<std::mutex> lock(mutex_);
    readerNeedsStop_ = !stopped;
    commandInFlight_ = false;
    state_.lifecycle = stopped ? Lifecycle::Stopped : Lifecycle::CancellationPending;
    disarmLocked(SelectedHidStatus::Stopped);
    return stopped ? Command::Stopped : Command::CancellationPending;
}

SelectedHidRuntimeCommand SelectedHidRuntime::stop(std::chrono::milliseconds grace) {
    using Command = SelectedHidRuntimeCommand;
    using Lifecycle = SelectedHidRuntimeLifecycle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.lifecycle == Lifecycle::Legacy) return Command::Legacy;
        if (state_.lifecycle == Lifecycle::InvalidConfiguration) return Command::InvalidConfiguration;
        stopRequested_ = true;
        stopGrace_ = boundedGrace(grace);
        disarmLocked(SelectedHidStatus::Stopped);
        if (commandInFlight_) {
            state_.lifecycle = Lifecycle::Stopping;
            return Command::StopRequested;
        }
        if (!readerNeedsStop_) {
            state_.lifecycle = Lifecycle::Stopped;
            return Command::Stopped;
        }
        commandInFlight_ = true;
        state_.lifecycle = Lifecycle::Stopping;
        grace = stopGrace_;
    }
    const bool stopped = reader_->stop(grace);
    std::lock_guard<std::mutex> lock(mutex_);
    readerNeedsStop_ = !stopped;
    commandInFlight_ = false;
    state_.lifecycle = stopped ? Lifecycle::Stopped : Lifecycle::CancellationPending;
    return stopped ? Command::Stopped : Command::CancellationPending;
}

SelectedHidRuntimeSnapshot SelectedHidRuntime::poll(bool mainWindowFocused) {
    using Lifecycle = SelectedHidRuntimeLifecycle;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.lifecycle == Lifecycle::Legacy || state_.lifecycle == Lifecycle::InvalidConfiguration)
        return snapshotLocked();
    SelectedHidSample sample;
    if (state_.lifecycle == Lifecycle::Running) {
        sample = reader_->snapshot(configuration_.selected->maxReportAge);
    } else {
        sample.status = state_.lifecycle == Lifecycle::Ready || state_.lifecycle == Lifecycle::Starting ?
            SelectedHidStatus::Waiting : SelectedHidStatus::Stopped;
    }
    state_.consumedAt = now_(); // Clock acquisition follows the one copied report.
    state_.input = adapter_.update(sample, configuration_.selected->maxReportAge, state_.consumedAt, mainWindowFocused);
    return snapshotLocked();
}

SelectedHidRuntimeSnapshot SelectedHidRuntime::statusSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotLocked();
}

} // namespace bone_eater::input
