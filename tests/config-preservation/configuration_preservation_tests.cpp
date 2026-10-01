// Compile the actual startup function unchanged. The renamed entry is never run;
// upstream and device integration are inert stubs, not a game/device fixture.
#define main unused_standalone_entry
#include "../../src/standalone/main.cpp"
#undef main
#include "launcher/launcher.h"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>

int main_implementation(int, char**) { throw std::runtime_error("Game entry must never run in this fixture"); }
namespace bone_eater::input {
void initializeSelectedHidBridge(SelectedHidConfiguration) {
    throw std::runtime_error("Device bridge must never initialize in this fixture");
}
SelectedHidRuntimeCommand stopSelectedHidBridge(std::chrono::milliseconds) noexcept {
    return SelectedHidRuntimeCommand::Stopped;
}
SelectedHidRuntimeDiagnostic SelectedHidRuntime::diagnose(const SelectedHidConfiguration&) {
    throw std::runtime_error("Runtime validation must never run in this fixture");
}
}

namespace fs = std::filesystem;
namespace launch = bone_eater::launcher;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected an error without changing protected files");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    input.exceptions(std::ios::badbit);
    require(input.is_open(), "Read fixture file");
    return std::string(std::istreambuf_iterator<char>(input), {});
}
void write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    // A rewrite with identical bytes must still fail the preservation check.
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(24));
}
struct State {
    std::string bytes;
    fs::file_time_type modified;
    DWORD attributes;
    bool operator==(const State&) const = default;
};
State state(const fs::path& path) {
    return {read(path), fs::last_write_time(path), GetFileAttributesW(path.c_str())};
}
using Inventory = std::map<fs::path, State>;
Inventory inventory(const fs::path& root) {
    Inventory result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(root), state(entry.path()));
    return result;
}
struct Files {
    fs::path root = fs::temp_directory_path() / (L"bone-config-preservation-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Files() { require(fs::create_directory(root), "Create unique fixture directory"); }
    ~Files() {
        // The exact fresh directory above is the sole deletion scope.
        std::error_code error;
        for (const auto& entry : fs::recursive_directory_iterator(root, error))
            if (entry.is_regular_file()) SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        fs::remove_all(root, error);
    }
};
}

int wmain(int argc, wchar_t** argv) {
    // runChild launches this same executable with the real normal/diagnose argv.
    // This branch only reports arguments; it performs no filesystem/device work.
    if (argc > 1 && (std::wstring_view(argv[1]) == L"-2Display" || std::wstring_view(argv[1]) == L"--diagnose")) {
        for (int index = 0; index < argc; ++index) std::cout << launch::narrow(argv[index]) << '\n';
        std::cout << "INERT_CONFIGURATION_CHILD\n";
        return 42;
    }
    try {
        require(argc == 1, "Unexpected fixture arguments");
        Files files;
        const auto game = files.root / L"runtime/game";
        const auto controls = game / L"desktop/bone-eater-controls.xml";
        fs::create_directories(controls.parent_path());
        unsigned groups = 0;

        prepare_default_controls(controls);
        const auto defaults = read(controls);
        require(defaults.find("<analog name=\"Gun X\" devid=\"\"/>") != std::string::npos &&
            defaults.find("<analog name=\"Gun Y\" devid=\"\"/>") != std::string::npos &&
            defaults.find("<button name=\"Start\" vkey=\"13\"") != std::string::npos,
            "First-start controls select native cursor fallback and expected Start");
        const auto generated = state(controls);
        prepare_default_controls(controls);
        require(state(controls) == generated, "Second start must not rewrite generated defaults");
        ++groups;

        // Preserve nondefault bytes/line endings, not merely parsed values.
        const std::string customized = "<?xml version=\"1.0\"?>\r\n<games><!-- owner settings -->\r\n"
            "<game name=\"Silent Scope: Bone Eater\"><buttons>"
            "<button name=\"Start\" vkey=\"32\" devid=\"user-device\"/>"
            "</buttons><analogs/></game></games>\r\n";
        write(controls, customized);
        const auto existing = state(controls);
        for (int launch = 0; launch != 3; ++launch) prepare_default_controls(controls);
        require(state(controls) == existing, "Customized controls must retain exact bytes/mtime/attributes");
        ++groups;

        write(controls, "");
        const auto empty = state(controls);
        prepare_default_controls(controls);
        require(state(controls) == empty, "Existing empty controls are preserved, not silently reset");
        write(controls, customized);
        require(SetFileAttributesW(controls.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Make existing fixture readonly");
        const auto readonly = state(controls);
        prepare_default_controls(controls);
        require(state(controls) == readonly, "Existing readonly controls must not require write access");
        require(SetFileAttributesW(controls.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "Release fixture readonly flag");
        ++groups;

        const auto beforeMissingParent = inventory(files.root);
        rejects([&] { prepare_default_controls(game / L"missing-parent/controls.xml"); });
        require(inventory(files.root) == beforeMissingParent && !fs::exists(game / L"missing-parent"),
            "Failed first-start write cannot rewrite neighboring settings or create parents");
        ++groups;

        const auto settingsFile = files.root / L"launch-settings.json";
        const auto profile = fs::path(L"desktop/profiles/設定 gun.json");
        write(settingsFile, "{\r\n \"schema_version\": 1, \"view\": \"closer150\",\r\n"
            " \"main_dof_off\": true, \"input_profile\": \"desktop/profiles/設定 gun.json\"\r\n}\r\n");
        write(game / profile, "{\"schema_version\":1, \"mode\":\"legacy\"}\r\n");
        write(game / L"conf/nvram/testmode-v.xml", "fixture calibration bytes\r\n");
        write(game / L"conf/nvram/testmode-v.crc", std::string("\x00\x12\xA5\xFF", 4));
        write(game / L"desktop/resize.json", "{\"fixture\":\"saved placement\"}\r\n");
        write(game / L"desktop/patches.json", "{\"fixture\":\"saved choices\"}\r\n");
        const auto protectedFiles = inventory(files.root);
        const auto settings = launch::readSettings(settingsFile);
        require(settings.view == "closer150" && settings.mainDofOff && settings.inputProfile == profile.wstring(),
            "Nondefault settings/profile survive reading");
        const auto selected = bone_eater::standalone::loadInputProfile(game, profile);
        (void)selected; // Parsing this explicit legacy fixture must open no device.
        const auto play = launch::arguments(settings, false);
        const auto diagnosis = launch::arguments(settings, true);
        const auto environment = launch::currentEnvironment();
        const auto report = launch::dryRunReport(game / L"BoneEater.exe", settingsFile, settings, play, environment, {});
        require(report.find("\"child_started\":false") != std::string::npos, "Dry-run evidence remains no-child");
        require(inventory(files.root) == protectedFiles, "Read/validate/argument/dry-run paths must preserve all protected files");
        ++groups;

        // Start and wait for an inert child through the real launcher lifecycle.
        // The executable lives outside the protected settings tree.
        const auto self = executable_directory() / fs::path(argv[0]).filename();
        for (const auto& childArgs : {play, diagnosis}) {
            const auto log = files.root / (childArgs == play ? L"play.log" : L"diagnose.log");
            require(launch::runChild(self, game, childArgs, environment, log) == 42, "Inert child exit propagation");
            const auto output = read(log);
            require(output.find("INERT_CONFIGURATION_CHILD") != std::string::npos &&
                output.find(launch::narrow(profile.wstring())) != std::string::npos, "Exact configured profile reaches inert child");
            fs::remove(log);
            require(inventory(files.root) == protectedFiles, "Normal/diagnose child completion must not rewrite settings");
        }
        ++groups;

        write(settingsFile, "{ invalid user settings; do not reset }");
        const auto malformed = inventory(files.root);
        rejects([&] { (void)launch::readSettings(settingsFile); });
        require(inventory(files.root) == malformed, "Invalid settings rejection preserves existing bytes and neighbors");
        ++groups;

        std::cout << groups << " isolated configuration-preservation groups passed; no game or device opened\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Configuration preservation fixture failed: " << error.what() << '\n';
        return 1;
    }
}
