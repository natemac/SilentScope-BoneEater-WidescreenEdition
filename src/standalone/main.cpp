// Game-specific application entry for the source-derived Bone Eater runtime.
// Runtime integration follows Spice2x's GPL-3.0 licensing; see vendor source notices.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "camera/main_fov_zoom.h"
#include "input/selected_hid_bridge.h"
#include "input/scope_settings.h"
#include "standalone/input_profile.h"
#include "diagnostics/output_policy.h"

int main_implementation(int argc, char *argv[]);

namespace {
std::filesystem::path input_profile_argument(int argc, int optionIndex) {
    int wideCount = 0;
    auto release = [](LPWSTR* value) noexcept { if (value) LocalFree(value); };
    std::unique_ptr<LPWSTR, decltype(release)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &wideCount), release);
    if (!arguments || wideCount != argc || optionIndex < 1 || optionIndex + 1 >= wideCount ||
            std::wstring_view(arguments.get()[optionIndex]) != L"--input-config") {
        throw std::runtime_error("Cannot resolve the exact Windows --input-config path.");
    }
    const std::filesystem::path result(arguments.get()[optionIndex + 1]);
    if (result.empty()) throw std::runtime_error("--input-config requires a nonempty path.");
    return result;
}

void prepare_default_controls(const std::filesystem::path &path) {
    if (std::filesystem::exists(path)) return;
    std::ofstream output(path);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<games>\n"
              "  <game name=\"Silent Scope: Bone Eater\">\n    <buttons>\n";
    const std::pair<const char *, int> bindings[] = {
        {"Service", '9'}, {"Test", VK_F3}, {"Coin Mech", '5'},
        {"Start", VK_RETURN}, {"Up", VK_UP}, {"Down", VK_DOWN},
        {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Gun Pressed", VK_LBUTTON},
        {"Scope Right", VK_RBUTTON}, {"Scope Left", 255}
    };
    for (const auto &[name, key] : bindings) {
        output << "      <button name=\"" << name << "\" vkey=\"" << key
               << "\" analogtype=\"0\" devid=\"\"/>\n";
    }
    // Both axes must remain unbound to select the native mouse fallback.
    output << "    </buttons>\n    <analogs>\n"
              "      <analog name=\"Gun X\" devid=\"\"/>\n"
              "      <analog name=\"Gun Y\" devid=\"\"/>\n"
              "    </analogs>\n    <lights/>\n    <options/>\n  </game>\n</games>\n";
    output.close();
}

std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) {
        throw std::runtime_error("Cannot determine the BoneEater executable location.");
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

bool preflight(const std::filesystem::path &root) {
    const char *required[] = {
        "modules/arkndd.dll", "modules/gamendd.dll", "modules/libavs-win64.dll",
        "modules/libavs-win64-ea3.dll", "prop/avs-config.xml", "prop/ea3-config.xml"
    };
    bool ready = true;
    for (const auto *relative : required) {
        if (!std::filesystem::is_regular_file(root / relative)) {
            std::cerr << "Missing required game file: " << relative << '\n';
            ready = false;
        }
    }
    for (const auto *relative : {"data", "arkdata", "conf"}) {
        if (!std::filesystem::is_directory(root / relative)) {
            std::cerr << "Missing required game directory: " << relative << '\n';
            ready = false;
        }
    }
    return ready;
}
}

int main(int argc, char **argv) {
    try {
        bool diagnose = false;
        bool originalLayout = false;
        bool widePreview = false;
        bool mainMonitor = false;
        bool nativeWide = false;
        bool twoDisplay = false;
        std::optional<std::filesystem::path> inputProfile;
        std::vector<std::string> forwarded;
        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--help") {
                std::cout << "Bone Eater standalone development runtime\n"
                             "Place BoneEater.exe beside the game's data, modules and prop folders.\n"
                             "Default: original cameras, fitted development windows, local startup services.\n"
                             "--diagnose  Check required files without starting the game.\n"
                             "--quiet-diagnostics  Suppress optional tracing/captures/timing; retain functional adapters and failures.\n"
                             "--input-config PATH  Load an explicit gun profile; relative paths use the game directory. Diagnose checks it without opening a device.\n"
                             "--original-layout  Keep the game's saved desktop window arrangement.\n"
                             "--wide-preview     Test the engine's response to a 16:9 main window.\n"
                             "--main-monitor     Fit the 16:9 main window to its monitor; no display-mode change.\n"
                             "--native-display-observe  Log original native main-display initialization.\n"
                             "--native-wide      Experiment with native 1920x1080 resources.\n"
                             "--native-movie-fit Fit main movies without stretching; requires --native-wide and -2Display.\n"
                             "-2Display         Test native front-HUD composition; put this argument first.\n"
                             "--native-hud-fit   Fit the native front portrait layer within the wide canvas.\n"
                             "--native-front-observe  Measure the retained front HUD resource and blend states.\n"
                             "--native-front-capture  Capture front HUD RGB/alpha for diagnosis (may stall rendering).\n"
                             "--native-rear-hud-fit  Fit recognized rear menu layers to the same canvas.\n"
                             "--native-input-wide  Test paired wide aiming and inverse-fitted menu input.\n"
                             "--native-input-desktop  Use full desktop aiming without cabinet margins.\n"
                             "--native-input-ownership-observe  Trace completed desktop input through ScopeCamera; requires --native-input-desktop; quiet suppresses it.\n"
                             "--native-precision-bypass  Experimental legacy precision-assist bypass; preserves native optical buttons and authored Y.\n"
                             "--native-menu-margin  Experimental Main_Quarter white backing fit; requires native wide/HUD/desktop reticle.\n"
                             "--native-normal-start-backing  Experimental settled Normal Start backing fit; requires native wide/HUD/desktop reticle.\n"
                             "--native-options-backing  Experimental settled Options backing fit; requires native wide/HUD/desktop reticle.\n"
                             "--desktop-reticle  Replace portrait reticle with a main-view aiming mark.\n"
                             "--native-battle-background-filter  Test removal of two portrait gradient sprites; requires --desktop-reticle.\n"
                             "--native-battle-background-align   Align verified rear HUD/dialogue backing; requires --desktop-reticle and fitted front/rear HUD; incompatible with filtering.\n"
                             "--native-replay-align  Align the short replay's rear mask; requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.\n"
                             "--native-achievement-align  Align only the achievement rear backing; requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.\n"
                             "--native-ranking-backing  Clip the ranking white backing to the front panel; requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.\n"
                             "--capture-frames   Save diagnostic backbuffer BMPs (may stall rendering).\n"
                             "--scope-overlay    Test the native scope as a cursor-following overlay.\n"
                             "--scope-mode legacy|toggle_hold and --scope-* options configure scoped buttons, gain and smoothing.\n"
                             "--scope-adaptive-enabled 0|1 enables speed-based scoped gain; default 0.\n"
                             "--scope-adaptive-low-max-gain N / --scope-adaptive-high-max-gain N set travel gains (up to 2).\n"
                             "--scope-adaptive-speed-start N / --scope-adaptive-speed-full N use normalized content lengths per second.\n"
                             "--scope-adaptive-ramp-up-ms N / --scope-adaptive-ramp-down-ms N set gain response.\n"
                             "--scope-adaptive-edge-enabled 0|1 plus edge-band, edge-dwell-ms, edge-max-speed options add dwell panning.\n"
                             "--camera-telemetry  Log read-only camera state for the verified game build.\n"
                             "--native-camera-observe  Observe native camera recalculation and ownership.\n"
                             "--native-dof-observe  Record native depth-of-field setup without changing optics.\n"
                             "--native-main-dof-off  Experimental main-view DOF disable; includes read-only tracing.\n"
                             "--native-vertical-fov  Test main-camera vertical coverage preservation.\n"
                             "--native-fov-zoom N  Zoom the preserved-height view by 1.0 to 1.5 (enables vertical FOV correction).\n"
                             "--native-framing-observe  Record final native camera look-at commits.\n"
                             "--aim-framing      Experiment with aim-driven pitch and scope-held offset lock.\n"
                             "--park-auxiliary   Test monitored offscreen scope-window placement.\n"
                             "Other options are passed to the integrated runtime for diagnostics.\n"
                             "Widescreen camera and scope conversion are still under development.\n";
                return 0;
            }
            if (option == "--diagnose") diagnose = true;
            else if (option == "--quiet-diagnostics") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS", L"1")) {
                    throw std::runtime_error("Cannot configure optional diagnostic output.");
                }
            }
            else if (option == "--input-config") {
                if (inputProfile || i + 1 >= argc) {
                    throw std::runtime_error("Specify --input-config once with a profile path.");
                }
                inputProfile = input_profile_argument(argc, i);
                ++i;
            }
            // The game parses the actual OS command line for this exact token.
            // Consume it here so it does not reach the runtime option parser.
            else if (option == "-2Display") twoDisplay = true;
            else if (const auto* name = bone_eater::input::scopeEnvironmentName(option)) {
                if (++i >= argc) throw std::runtime_error(option + " requires a value.");
                bone_eater::input::validateScopeOption(option, argv[i]);
                const std::string setting = option == "--scope-hold-ms" ?
                    std::to_string(static_cast<unsigned>(bone_eater::input::scopeNumber(argv[i],100,1000,true))) : argv[i];
                if (!SetEnvironmentVariableA(name, setting.c_str())) throw std::runtime_error("Cannot configure scope controls.");
            }
            else if (option == "--original-layout") originalLayout = true;
            else if (option == "--wide-preview") widePreview = true;
            else if (option == "--main-monitor") mainMonitor = true;
            else if (option == "--native-display-observe") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_DISPLAY", L"observe");
            else if (option == "--native-wide") {
                nativeWide = true;
                widePreview = true;
                SetEnvironmentVariableW(L"BONE_EATER_NATIVE_DISPLAY", L"wide");
            }
            else if (option == "--native-movie-fit") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_MOVIE_FIT", L"1")) {
                    throw std::runtime_error("Cannot configure main movie fitting.");
                }
            }
            else if (option == "--capture-frames") SetEnvironmentVariableW(L"BONE_EATER_CAPTURE", L"1");
            else if (option == "--native-hud-fit") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", L"fit");
            else if (option == "--native-front-observe") SetEnvironmentVariableW(L"BONE_EATER_FRONT_OBSERVE", L"1");
            else if (option == "--native-front-capture") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_FRONT_OBSERVE", L"1") ||
                    !SetEnvironmentVariableW(L"BONE_EATER_FRONT_CAPTURE", L"1")) {
                    std::cerr << "Could not enable native front capture.\n";
                    return 1;
                }
            }
            else if (option == "--native-rear-hud-fit") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_REAR_HUD", L"fit");
            else if (option == "--native-input-wide") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", L"wide");
            else if (option == "--native-input-desktop") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", L"desktop");
            else if (option == "--native-input-ownership-observe") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT_OWNERSHIP_OBSERVE", L"1")) {
                    throw std::runtime_error("Cannot configure native input ownership diagnostics.");
                }
            }
            else if (option == "--desktop-reticle") SetEnvironmentVariableW(L"BONE_EATER_DESKTOP_RETICLE", L"1");
            else if (option == "--native-precision-bypass") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_PRECISION_BYPASS", L"1")) {
                    throw std::runtime_error("Cannot configure native precision bypass.");
                }
            }
            else if (option == "--native-normal-start-backing") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_NORMAL_START_BACKING", L"1")) {
                    throw std::runtime_error("Cannot configure native Normal Start backing fit.");
                }
            }
            else if (option == "--native-options-backing") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_OPTIONS_BACKING", L"1")) {
                    throw std::runtime_error("Cannot configure native Options backing fit.");
                }
            }
            else if (option == "--native-menu-margin") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_MENU_MARGIN", L"1")) {
                    throw std::runtime_error("Cannot configure native menu backing fit.");
                }
            }
            else if (option == "--native-battle-background-filter") SetEnvironmentVariableW(L"BONE_EATER_BATTLE_BACKGROUND_FILTER", L"1");
            else if (option == "--native-battle-background-align") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_BATTLE_BACKGROUND_ALIGN", L"1")) {
                    throw std::runtime_error("Cannot configure rear illumination alignment.");
                }
            }
            else if (option == "--scope-overlay") SetEnvironmentVariableW(L"BONE_EATER_SCOPE_OVERLAY", L"1");
            else if (option == "--native-replay-align") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_REPLAY_ALIGN", L"1")) {
                    throw std::runtime_error("Cannot configure replay mask alignment.");
                }
            }
            else if (option == "--native-achievement-align") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_ACHIEVEMENT_ALIGN", L"1")) {
                    throw std::runtime_error("Cannot configure achievement backing alignment.");
                }
            }
            else if (option == "--native-ranking-backing") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_NATIVE_RANKING_BACKING", L"1")) {
                    throw std::runtime_error("Cannot configure ranking backing correction.");
                }
            }
            else if (option == "--camera-telemetry") SetEnvironmentVariableW(L"BONE_EATER_CAMERA_TELEMETRY", L"1");
            else if (option == "--native-camera-observe") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_CAMERA_OBSERVE", L"1");
            else if (option == "--native-dof-observe") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_DOF_OBSERVE", L"1")) {
                    throw std::runtime_error("Cannot enable depth-of-field observation.");
                }
            }
            else if (option == "--native-main-dof-off") {
                if (!SetEnvironmentVariableW(L"BONE_EATER_DOF_MAIN_OFF", L"1") ||
                        !SetEnvironmentVariableW(L"BONE_EATER_DOF_OBSERVE", L"1")) {
                    throw std::runtime_error("Cannot configure the main-view DOF experiment.");
                }
            }
            else if (option == "--native-vertical-fov") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_VERTICAL_FOV", L"1");
            else if (option == "--native-fov-zoom") {
                if (++i >= argc || !bone_eater::camera::parseMainFovZoom(argv[i])) {
                    throw std::runtime_error("--native-fov-zoom requires a decimal number from 1.0 to 1.5.");
                }
                if (!SetEnvironmentVariableA("BONE_EATER_NATIVE_FOV_ZOOM", argv[i]) ||
                        !SetEnvironmentVariableW(L"BONE_EATER_NATIVE_VERTICAL_FOV", L"1")) {
                    throw std::runtime_error("Cannot configure main-camera field of view.");
                }
            }
            else if (option == "--native-framing-observe") SetEnvironmentVariableW(L"BONE_EATER_NATIVE_FRAMING_OBSERVE", L"1");
            else if (option == "--aim-framing") SetEnvironmentVariableW(L"BONE_EATER_AIM_FRAMING", L"1");
            else if (option == "--park-auxiliary") SetEnvironmentVariableW(L"BONE_EATER_AUXILIARY_WINDOWS", L"offscreen");
            else forwarded.emplace_back(option);
        }

        // Validate all effective scope values before loading any game module.
        // Cross-field mistakes must report the normal startup error, not silently
        // disable the native input adapter from its noexcept installation path.
        (void)bone_eater::input::readScopedMotionSettings([](const char* name, const char* fallback) {
            char value[256] {};
            const auto size = GetEnvironmentVariableA(name, value, sizeof(value));
            if (!size) return std::string(fallback);
            if (size >= sizeof(value)) throw std::runtime_error(std::string(name) + " is too long.");
            return std::string(value, size);
        });

        // Match the module's exact opt-in spelling, including inherited flags.
        // Reject conflicting geometry/visibility experiments before game entry.
        const auto enabled = [](const wchar_t* name) {
            wchar_t value[4] {};
            return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
        };
        if (enabled(L"BONE_EATER_BATTLE_BACKGROUND_ALIGN") &&
                enabled(L"BONE_EATER_BATTLE_BACKGROUND_FILTER")) {
            throw std::runtime_error("Rear illumination alignment and background filtering are mutually exclusive.");
        }
        if (enabled(L"BONE_EATER_NATIVE_INPUT_OWNERSHIP_OBSERVE")) {
            wchar_t inputSetting[16] {};
            if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", inputSetting, 16) != 7 ||
                    std::wstring_view(inputSetting) != L"desktop") {
                throw std::runtime_error("--native-input-ownership-observe requires --native-input-desktop.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_MOVIE_FIT") && (!nativeWide || !twoDisplay)) {
            throw std::runtime_error("--native-movie-fit requires --native-wide and -2Display (put -2Display first).");
        }
        if (enabled(L"BONE_EATER_NATIVE_REPLAY_ALIGN")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-replay-align requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_ACHIEVEMENT_ALIGN")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-achievement-align requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_RANKING_BACKING")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-ranking-backing requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }

        if (enabled(L"BONE_EATER_NATIVE_NORMAL_START_BACKING")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-normal-start-backing requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_OPTIONS_BACKING")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-options-backing requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_MENU_MARGIN")) {
            wchar_t hudSetting[8] {};
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !fittedHud || !enabled(L"BONE_EATER_DESKTOP_RETICLE")) {
                throw std::runtime_error("--native-menu-margin requires --native-wide, -2Display, --native-hud-fit and --desktop-reticle.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_PRECISION_BYPASS")) {
            wchar_t inputSetting[16] {}, hudSetting[8] {};
            const bool desktop = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", inputSetting, 16) == 7 &&
                std::wstring_view(inputSetting) == L"desktop";
            const bool fittedHud = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_HUD", hudSetting, 8) == 3 &&
                std::wstring_view(hudSetting) == L"fit";
            if (!nativeWide || !twoDisplay || !desktop || !fittedHud) {
                throw std::runtime_error("--native-precision-bypass requires --native-wide, -2Display, --native-input-desktop and --native-hud-fit.");
            }
        }

        const auto root = executable_directory();
        if (!bone_eater::diagnostics::optionalOutputEnabled()) {
            std::cout << "Optional tracing, captures and timing are quiet; functional adapters, startup and failure diagnostics remain enabled.\n";
        }
        if (mainMonitor && (originalLayout || !widePreview)) {
            throw std::runtime_error("--main-monitor requires --native-wide or --wide-preview and cannot use --original-layout.");
        }
        bone_eater::input::SelectedHidConfiguration inputConfiguration;
        if (inputProfile) {
            inputConfiguration = bone_eater::standalone::loadInputProfile(root, *inputProfile);
            const auto validation = bone_eater::input::SelectedHidRuntime::diagnose(inputConfiguration);
            if (validation != bone_eater::input::SelectedHidRuntimeDiagnostic::Ready &&
                    validation != bone_eater::input::SelectedHidRuntimeDiagnostic::Legacy) {
                throw std::runtime_error("Input profile failed runtime configuration validation.");
            }
        }
        if (enabled(L"BONE_EATER_NATIVE_PRECISION_BYPASS") &&
                inputConfiguration.mode != bone_eater::input::GunSourceMode::Legacy) {
            throw std::runtime_error("--native-precision-bypass currently supports legacy input only; selected HID is not eligible.");
        }
        std::filesystem::current_path(root);
        if (!preflight(root)) {
            std::cerr << "Prepare the working game copy and deploy the executable there first.\n";
            return 2;
        }
        if (diagnose) {
            if (inputProfile) {
                std::cout << "Input profile is valid; no input device was opened.\n";
            }
            std::cout << "Required game files found. Runtime execution has not been tested by this check.\n"
                         "The game will run in this process, without an external loader process.\n";
            return 0;
        }

        // One immutable, process-lifetime owner. This constructs/validates only;
        // native NDD attach starts a requested selected reader later.
        bone_eater::input::initializeSelectedHidBridge(std::move(inputConfiguration));
        std::filesystem::create_directories(root / "desktop");
        prepare_default_controls(root / "desktop/bone-eater-controls.xml");
        // Until the final single-screen compositor is ready, keep all original
        // camera outputs readable on a desktop without changing their projection.
        if (originalLayout) SetEnvironmentVariableW(L"BONE_EATER_LAYOUT", L"off");
        else if (mainMonitor) {
            if (!SetEnvironmentVariableW(L"BONE_EATER_LAYOUT", L"wide-monitor")) {
                throw std::runtime_error("Cannot configure main-monitor window placement.");
            }
        }
        else if (widePreview) SetEnvironmentVariableW(L"BONE_EATER_LAYOUT", L"wide");
        else if (!GetEnvironmentVariableW(L"BONE_EATER_LAYOUT", nullptr, 0)) {
            SetEnvironmentVariableW(L"BONE_EATER_LAYOUT", L"diagnostic");
        }
        // Keep this development build's controls, patches and logs within its working copy.
        // -ea is upstream's basic local boot service; it does not provide player-card saves.
        std::vector<std::string> arguments = {argv[0]};
        arguments.insert(arguments.end(), forwarded.begin(), forwarded.end());
        // Upstream keeps the FIRST occurrence of scalar options. User options
        // precede defaults so explicit diagnostic paths/options take effect.
        const std::vector<std::string> defaults = {
            "-runas", "user", "-ea", "-w", "-cfgpath", "desktop/bone-eater-controls.xml",
            "-resizecfgpath", "desktop/resize.json", "-patchcfgpath", "desktop/patches.json",
            "-y", "desktop/game.log", "-nocolor"
        };
        arguments.insert(arguments.end(), defaults.begin(), defaults.end());
        std::vector<char *> pointers;
        pointers.reserve(arguments.size() + 1);
        for (auto &argument : arguments) {
            pointers.push_back(argument.data());
        }
        pointers.push_back(nullptr);
        std::cout << "Bone Eater development runtime: starting the configured game view.\n";
        const int result = main_implementation(static_cast<int>(arguments.size()), pointers.data());
        bone_eater::input::stopSelectedHidBridge();
        return result;
    } catch (const std::exception &error) {
        bone_eater::input::stopSelectedHidBridge();
        std::cerr << "Bone Eater startup failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        bone_eater::input::stopSelectedHidBridge();
        std::cerr << "Bone Eater startup failed with an unknown exception.\n";
        return 1;
    }
}
