#include "launcher.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace bone_eater::launcher;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } require(failed, "Expected refusal before close"); }
struct Child {
    HANDLE process = nullptr, ready = nullptr, release = nullptr;
    DWORD id = 0;
    Child(const std::filesystem::path& file, const std::wstring& mode) {
        static unsigned sequence = 0;
        const auto prefix = L"Local\\BoneEaterCloseFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
        const auto readyName = prefix + L"-ready", releaseName = prefix + L"-release";
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        if (!ready || !release) { cleanup(); throw std::runtime_error("Create fixture events"); }
        auto command = commandLine({L"fixture.exe", readyName, releaseName, mode});
        STARTUPINFOW startup {}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child {};
        if (!CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, file.parent_path().c_str(), &startup, &child)) {
            cleanup(); throw std::runtime_error("Start inert close fixture");
        }
        process = child.hProcess; id = child.dwProcessId; CloseHandle(child.hThread);
        if (WaitForSingleObject(ready, 3000) != WAIT_OBJECT_0) {
            cleanup(); throw std::runtime_error("Inert fixture did not become ready");
        }
    }
    ~Child() { cleanup(); }
    void cleanup() {
        if (release) SetEvent(release);
        if (process) { WaitForSingleObject(process, 7000); CloseHandle(process); process = nullptr; }
        if (ready) { CloseHandle(ready); ready = nullptr; }
        if (release) { CloseHandle(release); release = nullptr; }
    }
    bool live() const { return WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
};
}
int wmain(int argc, wchar_t** argv) {
    unsigned cases = 0;
    try {
        require(argc == 3, "Pass the inert window fixture and companion paths");
        const auto fixture = std::filesystem::canonical(argv[1]);
        const auto companion = std::filesystem::canonical(argv[2]);
        // This file is deliberately unique to these tests; it is never the
        // workspace game executable or an arbitrary user-selected live target.
        const auto folder = std::filesystem::current_path() / (L"close-fixture-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(folder), "Create test directory");
        const auto owned = folder / L"owned 雪.exe";
        std::filesystem::copy_file(fixture, owned);
        rejects([&] { requestGameClose(owned, 0, 100); }); ++cases;
        { Child child(owned, L"normal");
          const auto result = requestGameClose(owned, 2000, 500);
          require(result.processId == child.id && result.accepted && result.exited && result.exitCode == 0,
              "Real request must acknowledge and observe inert process exit"); ++cases; }
        { Child child(owned, L"scaled");
          const auto result = requestGameClose(owned, 2000, 500);
          require(result.accepted && result.exited, "Native localized scale title must remain eligible"); ++cases; }
        { Child child(owned, L"pending");
          const auto result = requestGameClose(owned, 0, 500);
          require(result.accepted && !result.exited && child.live(), "Acceptance does not imply process exit"); ++cases; }
        for (const auto* mode : {L"old", L"ambiguous"}) {
            Child child(owned, mode); rejects([&] { requestGameClose(owned, 0, 100); });
            require(child.live(), "Refused registration must leave child running"); ++cases;
        }
        { Child child(owned, L"stale");
          const auto result = requestGameClose(owned, 0, 500);
          require(!result.accepted && !result.exited && child.live(), "Overwritten cookie must reject in receiver"); ++cases; }
        { Child child(owned, L"hung");
          const auto before = GetTickCount64();
          const auto result = requestGameClose(owned, 0, 50);
          require(!result.accepted && !result.exited && child.live() && GetTickCount64() - before < 1500,
              "Hung callback must return bounded ambiguous delivery without killing"); ++cases; }
        { Child first(owned, L"pending"), second(owned, L"pending");
          rejects([&] { requestGameClose(owned, 0, 100); });
          require(first.live() && second.live(), "Multiple exact-path processes must remain untouched"); ++cases; }
        const auto otherFolder = folder / L"other"; std::filesystem::create_directory(otherFolder);
        const auto foreign = otherFolder / owned.filename(); std::filesystem::copy_file(fixture, foreign);
        { Child child(foreign, L"pending");
          rejects([&] { requestGameClose(owned, 0, 100); });
          require(child.live(), "Same basename with another path must remain untouched"); ++cases; }
        { Child other(foreign, L"pending"), child(owned, L"normal");
          const auto result = requestGameClose(owned, 2000, 500);
          require(result.accepted && result.exited && other.live(), "Exact owned path alone is closed"); ++cases; }
        rejects([&] { requestGameClose(owned, 10001, 500); });
        rejects([&] { requestGameClose(owned, 0, 0); }); ++cases;
        const auto gameFolder = folder / L"runtime/game";
        std::filesystem::create_directories(gameFolder);
        const auto fakeGame = gameFolder / L"BoneEater.exe";
        const auto copiedCompanion = folder / L"Play Bone Eater.exe";
        const auto settings = folder / L"launch-settings.json";
        std::filesystem::copy_file(fixture, fakeGame);
        std::filesystem::copy_file(companion, copiedCompanion);
        { std::ofstream file(settings); file << "malformed deliberately; close must ignore settings"; }
        { Child child(fakeGame, L"normal");
          // The normal companion's lifetime mutex may already be owned; the
          // explicit close-only route must not acquire it.
          HANDLE mutex = CreateMutexW(nullptr, TRUE, instanceName(std::filesystem::canonical(fakeGame)).c_str());
          require(mutex != nullptr, "Create held normal-launch guard");
          DWORD result = 999;
          try { result = runChild(copiedCompanion, folder, {L"Play Bone Eater.exe", L"--close-game", L"--no-dialog"},
              currentEnvironment(), folder / L"cli-close.log"); }
          catch (...) { ReleaseMutex(mutex); CloseHandle(mutex); throw; }
          ReleaseMutex(mutex); CloseHandle(mutex);
          require(result == 0 && !child.live(), "CLI close must bypass malformed settings and held launch guard"); ++cases; }
        { Child child(fakeGame, L"old");
          const auto result = runChild(copiedCompanion, folder, {L"Play Bone Eater.exe", L"--close-game", L"--no-dialog"},
              currentEnvironment(), folder / L"cli-old.log");
          require(result == 1 && child.live(), "CLI must refuse older unregistered runtime"); ++cases; }
        { Child child(fakeGame, L"pending");
          const auto result = runChild(copiedCompanion, folder,
              {L"Play Bone Eater.exe", L"--close-game", L"--dry-run", L"--no-dialog"},
              currentEnvironment(), folder / L"cli-conflict.log");
          require(result == 1 && child.live(), "Conflicting CLI modes must fail before any request"); ++cases; }
        // No recursive cleanup or process termination. Remove the four known
        // fixture artifacts after every child has left its normal message loop.
        for (const auto& file : {folder / L"cli-close.log", folder / L"cli-old.log", folder / L"cli-conflict.log",
                fakeGame, copiedCompanion, settings}) std::filesystem::remove(file);
        std::filesystem::remove(gameFolder); std::filesystem::remove(folder / L"runtime");
        std::filesystem::remove(foreign); std::filesystem::remove(otherFolder);
        std::filesystem::remove(owned); std::filesystem::remove(folder);
        std::cout << cases << " close-command fixture groups passed; only inert children used\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Close-command fixture after " << cases << " groups: " << error.what() << '\n'; return 1;
    }
}
