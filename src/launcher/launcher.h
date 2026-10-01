#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bone_eater::launcher {
struct AdaptiveScopeSettings {
    bool enabled = false;
    double lowMaxGain = 1.0, highMaxGain = 0.7;
    double speedStart = 0.15, speedFull = 0.8;
    double rampUpMs = 100, rampDownMs = 140;
    bool edgePanEnabled = false;
    double edgeBand = 0.04, edgeDwellMs = 200, edgeMaxSpeed = 0.25;
};
struct ScopeSettings {
    std::string shape = "circle";
    std::string mode = "legacy";
    std::vector<std::string> bindings {"ENTER", "RBUTTON"};
    unsigned holdMs = 250;
    double lowGain = 0.25;
    double highGain = 0.10;
    double lowSmoothingMs = 35;
    double highSmoothingMs = 55;
    std::optional<AdaptiveScopeSettings> adaptive;
};
struct Settings {
    std::string view = "balanced125";
    bool mainDofOff = false;
    std::wstring inputProfile;
    std::optional<ScopeSettings> scope;
};
struct Environment {
    std::vector<wchar_t> block;
    std::vector<std::wstring> removed;
    std::size_t retainedCount = 0;
};
std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
Settings parseSettings(const std::string& text);
Settings readSettings(const std::filesystem::path& file);
std::vector<std::wstring> arguments(const Settings& settings, bool diagnose);
std::wstring quoteArgument(const std::wstring& value);
std::wstring commandLine(const std::vector<std::wstring>& values);
Environment isolateEnvironment(const std::vector<std::wstring>& source);
Environment currentEnvironment();
std::filesystem::path ownDirectory();
std::filesystem::path verifiedGameExecutable(const std::filesystem::path& root);
std::filesystem::path verifiedGame(const std::filesystem::path& root);
struct CloseResult {
    DWORD processId = 0;
    bool accepted = false;
    bool exited = false;
    DWORD exitCode = STILL_ACTIVE;
    DWORD messageError = 0;
};
// Exact-path cooperating runtime only; no generic HWND/PID option or kill.
// A timeout is ambiguous: delivery may complete after this call returns.
CloseResult requestGameClose(const std::filesystem::path& executable,
    DWORD exitWaitMs = 3000, UINT messageWaitMs = 1000);
std::vector<DWORD> matchingProcesses(const std::filesystem::path& executable);
std::wstring instanceName(const std::filesystem::path& executable);
std::string dryRunReport(const std::filesystem::path& executable,
    const std::filesystem::path& settingsFile, const Settings& settings,
    const std::vector<std::wstring>& argv, const Environment& environment,
    const std::vector<DWORD>& running);
// Owns no global handles and does not kill the child. Waits until child exit;
// only these three explicitly listed standard handles are inherited.
DWORD runChild(const std::filesystem::path& executable,
    const std::filesystem::path& workingDirectory,
    const std::vector<std::wstring>& argv, const Environment& environment,
    const std::filesystem::path& outputFile);
std::string windowsError(const char* operation, DWORD error = GetLastError());
} // namespace bone_eater::launcher
