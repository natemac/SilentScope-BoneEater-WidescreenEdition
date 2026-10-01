#include "calibration.h"
#include "launcher.h"
#include <shellapi.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace bone_eater::launcher;
namespace {
void output(const std::string& value) {
    DWORD written = 0;
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE)
        WriteFile(handle, value.data(), static_cast<DWORD>(value.size()), &written, nullptr);
}
void writeReport(const std::filesystem::path& path, const std::string& text) {
    // Explicit dry-run report only; do not overwrite game/settings/user files.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error(windowsError("Create new dry-run report"));
    DWORD written = 0;
    const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!okay) throw std::runtime_error(windowsError("Write dry-run report", error));
}
std::filesystem::path logPath(const std::filesystem::path& game) {
    SYSTEMTIME now {}; GetLocalTime(&now);
    std::wostringstream name;
    name << L"launcher-" << now.wYear << L'-' << std::setfill(L'0') << std::setw(2) << now.wMonth
         << L'-' << std::setw(2) << now.wDay << L'-' << std::setw(2) << now.wHour << std::setw(2) << now.wMinute
         << std::setw(2) << now.wSecond << L'-' << GetCurrentProcessId() << L".log";
    return game.parent_path() / L"desktop" / name.str();
}
std::string logTail(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size <= 0) return {};
    const auto count = size > 2048 ? 2048 : size;
    file.seekg(-count, std::ios::end);
    std::string text(static_cast<std::size_t>(count), '\0');
    file.read(text.data(), count);
    text.resize(static_cast<std::size_t>(file.gcount()));
    // Runtime output may be in a legacy code page. Keep the UI bounded and
    // printable without pretending arbitrary log bytes are valid UTF-8.
    for (char& character : text)
        if (static_cast<unsigned char>(character) >= 128 || (character < 32 && character != '\n' && character != '\r' && character != '\t')) character = '?';
    return text;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    bool noDialog = false;
    try {
        int count = 0;
        LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw) throw std::runtime_error(windowsError("Read launcher arguments"));
        std::vector<std::wstring> args;
        try { for (int i = 1; i < count; ++i) args.emplace_back(raw[i]); }
        catch (...) { LocalFree(raw); throw; }
        LocalFree(raw);
        bool dryRun = false, diagnose = false, help = false, closeGame = false;
        std::filesystem::path report;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (args[i] == L"--dry-run") dryRun = true;
            else if (args[i] == L"--diagnose") diagnose = true;
            else if (args[i] == L"--close-game") closeGame = true;
            else if (args[i] == L"--no-dialog") noDialog = true;
            else if (args[i] == L"--help") help = true;
            else if (args[i] == L"--report" && report.empty() && i + 1 < args.size()) report = args[++i];
            else throw std::runtime_error("Unknown or incomplete launcher option. Use --help.");
        }
        if (help) {
            const std::string text = "Bone Eater prototype launcher\n\nDouble-click to play with launch-settings.json beside this launcher.\n"
                "view: balanced125 (default) or closer150 (comparison).\nmain_dof_off: false (default) or true (experimental).\ninput_profile: null for cursor input, or an explicit game-relative/absolute profile.\n\n"
                "Optional scope object: omit to preserve legacy controls. mode: legacy (default) or toggle_hold.\n"
                "bindings: 1-8 distinct uppercase names; defaults [\"ENTER\",\"RBUTTON\"]. Supported: ENTER, SPACE, LBUTTON, RBUTTON, MBUTTON, XBUTTON1, XBUTTON2, A-Z, 0-9.\n"
                "hold_ms: integer 100-1000 (default 250). low_gain/high_gain: greater than 0 through 1 (defaults 0.25/0.10).\n"
                "low_smoothing_ms/high_smoothing_ms: 0-250 (defaults 35/55). Values are experimental comfort tuning, not optical magnification.\n"
                "toggle_hold: tap opens lower zoom; hold selects higher zoom; release returns lower; next tap closes.\n"
                "Example addition: \"scope\": {\"mode\":\"toggle_hold\",\"bindings\":[\"ENTER\",\"RBUTTON\"]}.\n\n"
                "--dry-run: validate launch choices and report arguments; starts no child.\n--report PATH: write the dry-run JSON report.\n--diagnose: run the game's file/profile check without gameplay.\n--close-game: request exit from exactly this workspace's registered game; never force kill.\n--no-dialog: write errors to inherited stdout only.\n\nNo execution-policy change or external loader is used.";
            output(text + "\n");
            if (!noDialog) MessageBoxW(nullptr, widen(text).c_str(), L"Play Bone Eater", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if ((!report.empty() && !dryRun) || (dryRun && diagnose)) throw std::runtime_error("Use --report only with --dry-run; dry-run and diagnose are separate modes.");
        if (closeGame && (dryRun || diagnose || !report.empty()))
            throw std::runtime_error("--close-game is separate from dry-run, diagnose and report.");
        const auto root = ownDirectory();
        if (closeGame) {
            // Do not parse launch settings or acquire the normal launch mutex:
            // the existing companion intentionally owns that mutex until exit.
            const auto result = requestGameClose(verifiedGameExecutable(root));
            std::ostringstream text;
            text << "Game PID " << result.processId << ": close request "
                 << (result.accepted ? "accepted" : "not acknowledged") << "; process "
                 << (result.exited ? "exited" : "still running after the bounded wait");
            if (result.exited) text << " (exit code " << result.exitCode << ')';
            if (result.messageError) text << "; message error " << result.messageError;
            text << ".\nNo forced termination was requested by the companion.\n";
            if (!result.accepted) text << "Delivery may have timed out; a late request can still complete.\n";
            output(text.str());
            if (!noDialog) MessageBoxW(nullptr, widen(text.str()).c_str(), L"Bone Eater close request",
                MB_OK | (result.exited ? MB_ICONINFORMATION : MB_ICONWARNING));
            return result.exited ? 0 : (result.accepted ? 2 : 1);
        }
        const auto settingsFile = root / L"launch-settings.json";
        const auto settings = readSettings(settingsFile);
        const auto game = verifiedGame(root);
        const auto argv = arguments(settings, diagnose);
        const auto environment = currentEnvironment();
        const auto running = matchingProcesses(game);
        if (dryRun) {
            const auto json = dryRunReport(game, settingsFile, settings, argv, environment, running);
            if (!report.empty()) writeReport(report, json);
            output(json);
            if (!noDialog) MessageBoxW(nullptr, L"Launch settings and argument construction passed. No child process was started.\n\nThis dry-run does not validate input-profile contents or perform the complete game file check; use --diagnose separately.\n\nArguments and removed environment-variable names are in the requested report or inherited output. Existing game processes are reported, not stopped.", L"Play Bone Eater", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        const auto name = instanceName(game);
        HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex) throw std::runtime_error(windowsError("Create launcher instance guard"));
        struct MutexGuard { HANDLE value; bool owned = false; ~MutexGuard() { if (owned) ReleaseMutex(value); CloseHandle(value); } } lock {mutex};
        const DWORD acquired = WaitForSingleObject(mutex, 0);
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) throw std::runtime_error("This game's launcher is already active. Close the game before launching another instance.");
        lock.owned = true;
        // Repeat inside the launcher-instance guard; external direct launches
        // cannot be made atomic without cooperation from the game executable.
        if (!running.empty() || !matchingProcesses(game).empty()) throw std::runtime_error("This working game is already running. Close it before playing or checking files; no process was stopped.");
        std::filesystem::create_directories(game.parent_path() / L"desktop");
        if (!diagnose) prepareBetaCalibration(game);
        const auto log = logPath(game);
        const DWORD result = runChild(game, game.parent_path(), argv, environment, log);
        if (result != 0) {
            std::ostringstream text;
            text << "Bone Eater exited with code " << result << " (0x" << std::hex << result << ").\n\nStartup output: " << narrow(log.wstring())
                 << "\nGame log: " << narrow((game.parent_path() / L"desktop/game.log").wstring()) << "\n\n" << logTail(log);
            throw std::runtime_error(text.str());
        }
        if (diagnose) {
            const auto text = "The file/profile check passed. No gameplay or physical-gun test was performed.\n\n" + narrow(log.wstring()) + "\n\n" + logTail(log);
            output(text + "\n");
            if (!noDialog) MessageBoxW(nullptr, widen(text).c_str(), L"Bone Eater check", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    } catch (const std::exception& error) {
        const auto text = std::string("Bone Eater launcher: ") + error.what();
        output(text + "\n");
        if (!noDialog) {
            try { MessageBoxW(nullptr, widen(text).c_str(), L"Bone Eater could not complete the launch", MB_OK | MB_ICONERROR); }
            catch (...) { MessageBoxW(nullptr, L"Launcher failed. Run with --no-dialog from a terminal to inspect the error.", L"Bone Eater launcher", MB_OK | MB_ICONERROR); }
        }
        return 1;
    }
}
