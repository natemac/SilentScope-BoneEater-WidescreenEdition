#include "launcher.h"
#include "../render/window_exit_protocol.h"
#include <array>
#include <stdexcept>

namespace bone_eater::launcher {
namespace {
namespace protocol = bone_eater::render::window_exit_protocol;
struct Process {
    HANDLE value;
    ~Process() { if (value) CloseHandle(value); }
};
struct WindowIdentity {
    HWND window = nullptr;
    DWORD thread = 0;
    UINT_PTR cookie = 0;
    std::wstring title;
    bool valid = false;
    bool operator==(const WindowIdentity&) const = default;
};
struct Enumeration {
    DWORD process;
    std::vector<WindowIdentity> registrations;
    bool failed = false;
};
BOOL CALLBACK collect(HWND window, LPARAM context) noexcept {
    auto& result = *reinterpret_cast<Enumeration*>(context);
    try {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!thread || process != result.process) return TRUE;
        const auto cookie = reinterpret_cast<UINT_PTR>(GetPropW(window, protocol::property));
        if (!cookie) return TRUE;
        std::array<wchar_t, 80> type {};
        std::array<wchar_t, 256> title {};
        const int titleLength = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        const int classLength = GetClassNameW(window, type.data(), static_cast<int>(type.size()));
        const bool valid = titleLength > 0 && titleLength < static_cast<int>(title.size()) - 1 &&
            classLength > 0 && classLength < static_cast<int>(type.size()) - 1 &&
            std::wstring_view(type.data()) == protocol::windowClass && protocol::mainTitle(title.data()) &&
            GetAncestor(window, GA_ROOT) == window && !GetWindow(window, GW_OWNER);
        result.registrations.push_back({window, thread, cookie, title.data(), valid});
    } catch (...) { result.failed = true; return FALSE; }
    return TRUE;
}
WindowIdentity selectWindow(DWORD process) {
    Enumeration state {process, {}, false};
    if (!EnumWindows(collect, reinterpret_cast<LPARAM>(&state)) || state.failed)
        throw std::runtime_error("Cannot verify registered game windows; no close request was sent.");
    if (state.registrations.size() != 1 || !state.registrations.front().valid)
        throw std::runtime_error("Expected exactly one registered AskaWnd main window with an ASKA title. The game may be starting, use an older runtime, or have ambiguous registration; no close request was sent.");
    return state.registrations.front();
}
FILETIME verifyProcess(HANDLE process, DWORD id, const std::filesystem::path& executable) {
    if (GetProcessId(process) != id || WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        throw std::runtime_error("The selected game process exited or changed before the close request.");
    std::array<wchar_t, 32768> path {};
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
        throw std::runtime_error(windowsError("Revalidate game executable"));
    const auto current = std::filesystem::canonical(std::filesystem::path(std::wstring(path.data(), length)));
    if (CompareStringOrdinal(current.c_str(), -1, executable.c_str(), -1, TRUE) != CSTR_EQUAL)
        throw std::runtime_error("The selected process is not this working game; no close request was sent.");
    FILETIME creation {}, exit {}, kernel {}, user {};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
        throw std::runtime_error(windowsError("Read retained game lifetime"));
    return creation;
}
}

CloseResult requestGameClose(const std::filesystem::path& executable, DWORD exitWaitMs, UINT messageWaitMs) {
    if (exitWaitMs > 10000 || !messageWaitMs || messageWaitMs > 2000)
        throw std::runtime_error("Close request wait exceeds its bounded limit.");
    const auto canonical = std::filesystem::canonical(executable);
    const auto candidates = matchingProcesses(canonical);
    if (candidates.size() != 1)
        throw std::runtime_error("Expected exactly one running copy of this workspace's game; no process was closed.");
    const DWORD id = candidates.front();
    Process process {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id)};
    if (!process.value) throw std::runtime_error(windowsError("Retain running game identity"));
    const auto created = verifyProcess(process.value, id, canonical);
    const auto window = selectWindow(id);
    const UINT message = RegisterWindowMessageW(protocol::message);
    if (!message) throw std::runtime_error(windowsError("Register game close request"));

    // Retain the process handle throughout. Recheck both the unique process set
    // and the full registration immediately before dispatch. The receiver's
    // per-install cookie closes the remaining HWND-reuse race after this check.
    if (matchingProcesses(canonical) != candidates || !(selectWindow(id) == window))
        throw std::runtime_error("Game/window registration changed; no close request was sent.");
    const auto currentCreation = verifyProcess(process.value, id, canonical);
    if (CompareFileTime(&created, &currentCreation) != 0)
        throw std::runtime_error("Game process lifetime changed; no close request was sent.");

    CloseResult result; result.processId = id;
    DWORD_PTR reply = 0;
    SetLastError(ERROR_SUCCESS);
    const bool delivered = SendMessageTimeoutW(window.window, message, window.cookie, protocol::version,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, messageWaitMs, &reply) != 0;
    result.messageError = delivered ? ERROR_SUCCESS : GetLastError();
    result.accepted = delivered && reply == static_cast<DWORD_PTR>(protocol::accepted);
    const DWORD waited = WaitForSingleObject(process.value, exitWaitMs);
    if (waited == WAIT_FAILED) throw std::runtime_error(windowsError("Wait for requested game exit"));
    result.exited = waited == WAIT_OBJECT_0;
    if (result.exited && !GetExitCodeProcess(process.value, &result.exitCode))
        throw std::runtime_error(windowsError("Read requested game exit status"));
    return result;
}
} // namespace bone_eater::launcher
