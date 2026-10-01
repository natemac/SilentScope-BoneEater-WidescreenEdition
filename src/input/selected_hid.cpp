#include "input/selected_hid.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

namespace bone_eater::input {
namespace {

std::atomic<std::uint64_t> nextSession {0};

bool supported(const SelectedHidSelection& selection) noexcept {
    return selection.vendor == 0x1209 && (selection.product == 1 || selection.product == 2) &&
        selection.interfacePath.size() <= 32768 && selection.interfacePath.find(L'\0') == std::wstring::npos;
}

SelectedHidStart resolve(const SelectedHidSelection& selection, std::wstring& selected) {
    std::vector<RAWINPUTDEVICELIST> devices;
    bool enumerated = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1) || count > 16384)
            return SelectedHidStart::EnumerationFailed;
        devices.resize(count);
        if (!count) { enumerated = true; break; }
        const auto actual = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
        if (actual != UINT(-1)) {
            if (actual > devices.size()) return SelectedHidStart::EnumerationFailed;
            devices.resize(actual);
            enumerated = true;
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return SelectedHidStart::EnumerationFailed;
    }
    if (!enumerated) return SelectedHidStart::EnumerationFailed;
    unsigned matches = 0;
    for (const auto& device : devices) {
        if (device.dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info {};
        info.cbSize = sizeof(info);
        UINT length = sizeof(info);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &info, &length) == UINT(-1) ||
                info.dwType != RIM_TYPEHID || info.hid.dwVendorId != selection.vendor ||
                info.hid.dwProductId != selection.product || info.hid.usUsagePage != 1 || info.hid.usUsage != 5) continue;
        length = 0;
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, nullptr, &length) == UINT(-1) ||
                !length || length > 32768) continue;
        std::vector<wchar_t> name(length + 1, 0);
        if (GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, name.data(), &length) == UINT(-1)) continue;
        std::wstring path(name.data());
        if (!selection.interfacePath.empty() && _wcsicmp(path.c_str(), selection.interfacePath.c_str())) continue;
        ++matches;
        selected = std::move(path);
    }
    return matches == 1 ? SelectedHidStart::Started :
        matches ? SelectedHidStart::Ambiguous : SelectedHidStart::NotFound;
}

struct Session {
    SelectedHidState state {++nextSession};
    HANDLE device = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr, doneEvent = nullptr, inputEvent = nullptr;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    SelectedHidContract contract;
    std::array<std::uint8_t, 15> buffer {};
    OVERLAPPED operation {};
    std::atomic<bool> stopping {false};
#ifdef BONE_EATER_SELECTED_HID_TESTING
    std::atomic<bool> pendingForTest {false};
#endif

    ~Session() {
        if (preparsed) HidD_FreePreparsedData(preparsed);
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        if (inputEvent) CloseHandle(inputEvent);
        if (stopEvent) CloseHandle(stopEvent);
        if (doneEvent) CloseHandle(doneEvent);
    }
};

bool descriptor(Session& session, const SelectedHidSelection& selection) {
    HIDD_ATTRIBUTES attributes {};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(session.device, &attributes) || attributes.VendorID != selection.vendor ||
            attributes.ProductID != selection.product || !HidD_GetPreparsedData(session.device, &session.preparsed)) return false;
    HIDP_CAPS caps {};
    if (HidP_GetCaps(session.preparsed, &caps) != HIDP_STATUS_SUCCESS ||
            caps.NumberInputValueCaps > 256 || caps.NumberInputButtonCaps > 256) return false;
    USHORT valueCount = caps.NumberInputValueCaps, buttonCount = caps.NumberInputButtonCaps;
    std::vector<HIDP_VALUE_CAPS> nativeValues(valueCount);
    std::vector<HIDP_BUTTON_CAPS> nativeButtons(buttonCount);
    if (!valueCount || !buttonCount ||
            HidP_GetValueCaps(HidP_Input, nativeValues.data(), &valueCount, session.preparsed) != HIDP_STATUS_SUCCESS ||
            HidP_GetButtonCaps(HidP_Input, nativeButtons.data(), &buttonCount, session.preparsed) != HIDP_STATUS_SUCCESS ||
            valueCount > nativeValues.size() || buttonCount > nativeButtons.size()) return false;
    std::vector<HidValueCapability> values;
    std::vector<HidButtonCapability> buttons;
    for (USHORT i = 0; i < valueCount; ++i) {
        const auto& cap = nativeValues[i];
        values.push_back({cap.UsagePage, cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage,
            cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage, cap.LinkCollection, cap.BitSize,
            cap.ReportID, cap.IsAbsolute != FALSE, cap.LogicalMin, cap.LogicalMax});
    }
    for (USHORT i = 0; i < buttonCount; ++i) {
        const auto& cap = nativeButtons[i];
        buttons.push_back({cap.UsagePage, cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage,
            cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage, cap.LinkCollection, cap.ReportID});
    }
    session.contract = selectedHidContract(caps.UsagePage, caps.Usage, caps.InputReportByteLength, values, buttons);
    return session.contract.valid;
}

bool decode(Session& session, DWORD length) {
    if (length != session.buffer.size() || session.buffer[0] != 3) return false;
    ULONG x = 0, y = 0, usageCount = 16;
    std::array<USAGE, 16> usages {};
    auto report = reinterpret_cast<PCHAR>(session.buffer.data());
    if (HidP_GetUsageValue(HidP_Input, 1, session.contract.xLink, 0x30, &x,
            session.preparsed, report, length) != HIDP_STATUS_SUCCESS ||
        HidP_GetUsageValue(HidP_Input, 1, session.contract.yLink, 0x31, &y,
            session.preparsed, report, length) != HIDP_STATUS_SUCCESS ||
        HidP_GetUsages(HidP_Input, 9, session.contract.buttonLink, usages.data(), &usageCount,
            session.preparsed, report, length) != HIDP_STATUS_SUCCESS || usageCount > usages.size()) return false;
    std::uint32_t buttons = 0;
    for (ULONG i = 0; i < usageCount; ++i) {
        if (usages[i] < 1 || usages[i] > 16) return false;
        buttons |= 1u << (usages[i] - 1);
    }
    return session.state.publish(session.buffer.data(), length, x, y, buttons);
}

// Cancel completion may be delayed by a faulty driver. Never release OVERLAPPED
// or its buffer before the kernel has finished using them. The caller's stop
// wait is bounded; this worker keeps its shared Session alive if cancellation
// outlasts that bound. No new reads are issued after stopping.
void cancelAndDrain(Session& session) noexcept {
    CancelIoEx(session.device, &session.operation);
    DWORD transferred = 0;
    for (;;) {
        if (GetOverlappedResult(session.device, &session.operation, &transferred, FALSE)) return;
        if (GetLastError() != ERROR_IO_INCOMPLETE) return;
        WaitForSingleObject(session.inputEvent, 1000);
    }
}

void run(std::shared_ptr<Session> session) noexcept {
    bool pending = false;
    SelectedHidStatus finalStatus = SelectedHidStatus::Disconnected;
    try {
        const HANDLE waits[] {session->stopEvent, session->inputEvent};
        while (!session->stopping.load()) {
            ResetEvent(session->inputEvent);
            session->operation = {};
            session->operation.hEvent = session->inputEvent;
            DWORD transferred = 0;
            const BOOL immediate = ReadFile(session->device, session->buffer.data(),
                static_cast<DWORD>(session->buffer.size()), &transferred, &session->operation);
            if (!immediate) {
                if (GetLastError() != ERROR_IO_PENDING) break;
                pending = true;
#ifdef BONE_EATER_SELECTED_HID_TESTING
                session->pendingForTest = true;
#endif
                const auto wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0 + 1 || session->stopping.load()) {
                    cancelAndDrain(*session);
                    pending = false;
                    break;
                }
                if (!GetOverlappedResult(session->device, &session->operation, &transferred, FALSE)) {
                    if (GetLastError() == ERROR_IO_INCOMPLETE) cancelAndDrain(*session);
                    pending = false;
                    break;
                }
                pending = false;
            }
            if (session->stopping.load()) break;
            if (!decode(*session, transferred)) session->state.invalidate(SelectedHidStatus::InvalidReport);
        }
        if (session->stopping.load()) finalStatus = SelectedHidStatus::Stopped;
    } catch (...) {
        if (pending) cancelAndDrain(*session);
    }
    try { session->state.invalidate(finalStatus); } catch (...) {}
    SetEvent(session->doneEvent);
}

} // namespace

struct SelectedHidReader::Impl {
    std::mutex command;
    std::shared_ptr<Session> session;
    std::thread worker;

    bool stopLocked(std::chrono::milliseconds grace) {
        auto active = std::atomic_load(&session);
        if (!active) return true;
        active->stopping.store(true);
        active->state.invalidate(SelectedHidStatus::Stopped);
        SetEvent(active->stopEvent);
        CancelIoEx(active->device, nullptr);
        if (!worker.joinable()) return true;
        const auto delay = static_cast<DWORD>(std::clamp<std::int64_t>(grace.count(), 0, 5000));
        if (WaitForSingleObject(active->doneEvent, delay) != WAIT_OBJECT_0) return false;
        worker.join();
        return true;
    }
};

SelectedHidReader::SelectedHidReader() : impl_(std::make_unique<Impl>()) {}

SelectedHidReader::~SelectedHidReader() {
    stop();
    // On an unresponsive driver, the worker's shared capture owns every buffer,
    // event and device handle until cancellation eventually completes.
    if (impl_->worker.joinable()) impl_->worker.detach();
}

SelectedHidStart SelectedHidReader::start(const SelectedHidSelection& selection) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->command);
        if (!impl_->stopLocked(std::chrono::milliseconds(1500))) return SelectedHidStart::CancellationPending;
        std::atomic_store(&impl_->session, std::shared_ptr<Session>{});
        if (!supported(selection)) return SelectedHidStart::UnsupportedSelection;
        std::wstring path;
        const auto resolved = resolve(selection, path);
        if (resolved != SelectedHidStart::Started) return resolved;
        auto session = std::make_shared<Session>();
        session->device = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (session->device == INVALID_HANDLE_VALUE) return SelectedHidStart::OpenFailed;
        if (!descriptor(*session, selection)) return SelectedHidStart::DescriptorMismatch;
        session->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        session->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        session->inputEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!session->stopEvent || !session->doneEvent || !session->inputEvent) return SelectedHidStart::ResourceFailure;
        impl_->worker = std::thread(run, session);
        std::atomic_store(&impl_->session, std::move(session));
        return SelectedHidStart::Started;
    } catch (...) { return SelectedHidStart::ResourceFailure; }
}

bool SelectedHidReader::stop(std::chrono::milliseconds grace) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->command);
        return impl_->stopLocked(grace);
    } catch (...) { return false; }
}

SelectedHidSample SelectedHidReader::snapshot(std::chrono::milliseconds maxAge) const noexcept {
    try {
        auto session = std::atomic_load(&impl_->session);
        if (!session) return {};
        auto sample = session->state.snapshot(maxAge);
        // A completion can race stop after its worker-side flag check. Never
        // expose that final publication as active once stop has been requested.
        if (session->stopping.load()) {
            sample.valid = false;
            sample.buttons = 0;
            sample.status = SelectedHidStatus::Stopped;
        }
        return sample;
    } catch (...) { return {}; }
}

#ifdef BONE_EATER_SELECTED_HID_TESTING
bool selectedHidCancellationFixture() {
    SelectedHidReader reader;
    auto session = std::make_shared<Session>();
    const std::wstring name = L"\\\\.\\pipe\\BoneEaterSelectedHidTest-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(++nextSession);
    session->device = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 15, 15, 0, nullptr);
    if (session->device == INVALID_HANDLE_VALUE) return false;
    struct Client {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Client() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } client;
    client.value = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (client.value == INVALID_HANDLE_VALUE) return false;
    // The local client has connected before the server's first read. It sends
    // no bytes, so the HID decoder is never invoked on pipe data.
    session->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    session->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    session->inputEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!session->stopEvent || !session->doneEvent || !session->inputEvent) return false;
    reader.impl_->worker = std::thread(run, session);
    std::atomic_store(&reader.impl_->session, session);
    const auto deadline = GetTickCount64() + 1000;
    while (!session->pendingForTest.load() && GetTickCount64() < deadline) Sleep(1);
    const bool wasPending = session->pendingForTest.load();
    std::array<std::uint8_t, 15> fixture {};
    fixture[0] = 3;
    session->state.publish(fixture.data(), fixture.size(), 32767, 32767, 0xFFFF);
    const bool stopped = reader.stop(std::chrono::milliseconds(1000));
    const auto sample = reader.snapshot(std::chrono::milliseconds(100));
    return wasPending && stopped && !sample.valid && sample.buttons == 0 &&
        sample.status == SelectedHidStatus::Stopped;
}
#endif

} // namespace bone_eater::input
