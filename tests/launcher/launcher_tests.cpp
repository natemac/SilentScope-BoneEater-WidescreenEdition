#include "../../src/input/scope_settings.h"
#include "launcher.h"
#include <shellapi.h>
#include <algorithm>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace bone_eater::launcher;
namespace {
int cases = 0;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } require(failed, "Expected rejection"); }
std::vector<std::wstring> entries(const Environment& environment) {
    std::vector<std::wstring> result;
    for (const wchar_t* item = environment.block.data(); *item; item += wcslen(item) + 1) result.emplace_back(item);
    return result;
}
std::string read(const std::filesystem::path& path) { std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; }
}
int wmain(int argc, wchar_t** argv) {
    try {
        require(argc == 2, "Pass only the inert fixture executable path");
        const auto fixture = std::filesystem::canonical(argv[1]);
        const std::string defaultJson = R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":null})";
        const auto defaults = parseSettings(defaultJson);
        require(defaults.view == "balanced125" && !defaults.mainDofOff && defaults.inputProfile.empty(), "Defaults changed");
        const auto play = arguments(defaults, false);
        require(play[0] == L"BoneEater.exe" && play[1] == L"-2Display" && play[5] == L"1.25", "Default argument order");
        require(commandLine(play).find(L"-api") == std::wstring::npos, "No API by default");
        require(std::count(play.begin(), play.end(), L"--native-replay-align") == 1, "Replay alignment enabled once");
        require(std::count(play.begin(), play.end(), L"--native-ranking-backing") == 1, "Accepted ranking backing correction enabled once");
        require(std::count(play.begin(), play.end(), L"--native-menu-margin") == 1, "Accepted Main_Quarter margin correction enabled once");
        require(std::count(play.begin(), play.end(), L"--native-normal-start-backing") == 1, "Accepted Normal Start backing correction enabled once");
        require(std::count(play.begin(), play.end(), L"--native-options-backing") == 1, "Accepted Options backing correction enabled once");
        require(std::count(play.begin(), play.end(), L"--native-precision-bypass") == 0, "Experimental precision bypass remains disabled");
        require(std::count(play.begin(), play.end(), L"--quiet-diagnostics") == 1, "Player preset disables optional continuous tracing");
        require(!defaults.scope && commandLine(play).find(L"--scope-mode") == std::wstring::npos, "Old settings preserve legacy scope arguments");
        ++cases;
        const auto closer = parseSettings(R"({"schema_version":1,"view":"closer150","main_dof_off":true,"input_profile":"desktop/雪 profile.json"})");
        const auto comparison = arguments(closer, false);
        require(comparison[5] == L"1.5" && std::count(comparison.begin(), comparison.end(), L"--native-main-dof-off") == 1 && comparison.back() == L"desktop/雪 profile.json", "Comparison choices");
        const auto diagnostic = arguments(closer, true);
        require(diagnostic == std::vector<std::wstring>{L"BoneEater.exe", L"--diagnose", L"--input-config", L"desktop/雪 profile.json"}, "Diagnostic must not enable gameplay optics");
        ++cases;
        for (const std::string& bad : std::vector<std::string>{"", "[]", "{}", defaultJson + "garbage", std::string(16385, ' '),
            R"({"schema_version":1,"view":"balanced125","view":"closer150","main_dof_off":false,"input_profile":null})",
            R"({"schema_version":2,"view":"balanced125","main_dof_off":false,"input_profile":null})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":0,"input_profile":null})",
            R"({"schema_version":1,"view":"other","main_dof_off":false,"input_profile":null})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":""})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":"C:profile.json"})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":"\\profile.json"})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":"a\u0000b"})",
            R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":null,"extra":1})"}) rejects([&] { parseSettings(bad); });
        rejects([&] { parseSettings(defaultJson + std::string(1, '\0')); });
        rejects([&] { parseSettings(std::string("\xff") + defaultJson); });
        ++cases;
        const auto scopeJson = [&](const std::string& scope) { return defaultJson.substr(0, defaultJson.size() - 1) + ",\"scope\":" + scope + "}"; };
        const auto scoped = parseSettings(scopeJson(R"({"mode":"toggle_hold"})"));
        require(scoped.scope && !scoped.scope->adaptive && scoped.scope->mode == "toggle_hold" && scoped.scope->bindings == std::vector<std::string>{"ENTER", "RBUTTON"}
            && scoped.scope->holdMs == 250 && scoped.scope->lowGain == 0.25 && scoped.scope->highGain == 0.1
            && scoped.scope->lowSmoothingMs == 35 && scoped.scope->highSmoothingMs == 55, "Scope defaults");
        require(parseSettings(scopeJson("{}")).scope->mode == "legacy", "Empty scope object must not opt into new behavior");
        bone_eater::input::validateScopeOption("--scope-shape", "angled");
        bone_eater::input::validateScopeOption("--scope-shape", "circle");
        rejects([&] { bone_eater::input::validateScopeOption("--scope-shape", "square"); });
        require(std::string(bone_eater::input::scopeEnvironmentName("--scope-shape")) == "BONE_EATER_SCOPE_SHAPE", "Native shape environment transport");
        require(scoped.scope->shape == "circle", "Absent shape preserves original circle");
        const auto angled = parseSettings(scopeJson(R"({"mode":"toggle_hold","shape":"angled"})"));
        require(angled.scope->shape == "angled", "Angled JSON parsed");
        require(commandLine(arguments(angled,false)).find(L"--scope-shape angled") != std::wstring::npos, "Shape argument transported");
        require(parseSettings(scopeJson(R"({"shape":"circle"})")).scope->shape == "circle", "Explicit circle");
        for (const auto& bad : {R"({"shape":"square"})", R"({"shape":true})", R"({"shape":"angled","shape":"circle"})"})
            rejects([&] { parseSettings(scopeJson(bad)); });
        const auto scopedArgs = arguments(scoped, false);
        const auto flagValue = [&](const std::vector<std::wstring>& args, const std::wstring& flag) {
            const auto found = std::find(args.begin(), args.end(), flag);
            require(found != args.end() && found + 1 != args.end() && std::count(args.begin(), args.end(), flag) == 1, "Scope flag missing, duplicated or incomplete");
            return *(found + 1);
        };
        require(flagValue(scopedArgs, L"--scope-mode") == L"toggle_hold" && flagValue(scopedArgs, L"--scope-bindings") == L"ENTER,RBUTTON"
            && flagValue(scopedArgs, L"--scope-hold-ms") == L"250" && std::stod(flagValue(scopedArgs, L"--scope-low-gain")) == 0.25
            && std::stod(flagValue(scopedArgs, L"--scope-high-gain")) == 0.1
            && flagValue(scopedArgs, L"--scope-low-smoothing-ms") == L"35" && flagValue(scopedArgs, L"--scope-high-smoothing-ms") == L"55", "Scope runtime argument transport");
        require(arguments(scoped, true) == std::vector<std::wstring>{L"BoneEater.exe", L"--diagnose"}, "Scope config must not activate gameplay in diagnose mode");
        const auto scopedReport = dryRunReport(L"C:\\fixture\\BoneEater.exe", L"C:\\fixture\\settings.json", scoped, scopedArgs, isolateEnvironment({}), {});
        require(scopedReport.find("\"scope\":{\"mode\":\"toggle_hold\",\"bindings\":[\"ENTER\",\"RBUTTON\"]") != std::string::npos
            && scopedReport.find("\"child_started\":false") != std::string::npos, "Dry run describes resolved scope choices without executing them");
        ++cases;
        require(scoped.scope->holdRelease == "exit", "Omitted hold_release defaults to exit");
        require(flagValue(scopedArgs, L"--scope-hold-release") == L"exit", "Default release transported");
        const auto lowerRelease = parseSettings(scopeJson(R"({"mode":"toggle_hold","hold_release":"lower"})"));
        require(flagValue(arguments(lowerRelease, false), L"--scope-hold-release") == L"lower", "Explicit lower policy retained");
        const auto exitRelease = parseSettings(scopeJson(R"({"mode":"toggle_hold","hold_release":"exit"})"));
        require(exitRelease.scope->holdRelease == "exit", "Exit release parsed");
        require(flagValue(arguments(exitRelease, false), L"--scope-hold-release") == L"exit", "Exit release transported");
        const auto exitReport = dryRunReport(L"C:\\fixture\\BoneEater.exe", L"C:\\fixture\\settings.json", exitRelease, arguments(exitRelease, false), isolateEnvironment({}), {});
        require(exitReport.find("\"hold_release\":\"exit\"") != std::string::npos, "Release policy reported");
        require(std::string(bone_eater::input::scopeEnvironmentName("--scope-hold-release")) == "BONE_EATER_SCOPE_HOLD_RELEASE", "Release environment transport");
        for (const auto* policy : {"lower", "exit"}) bone_eater::input::validateScopeOption("--scope-hold-release", policy);
        rejects([&] { bone_eater::input::validateScopeOption("--scope-hold-release", "other"); });
        for (const auto* bad : {R"({"hold_release":2})", R"({"hold_release":true})", R"({"hold_release":"other"})", R"({"hold_release":"exit\u0000"})", R"({"hold_release":"exit","hold_release":"lower"})"})
            rejects([&] { parseSettings(scopeJson(bad)); });
        ++cases;
        const auto adaptive = parseSettings(scopeJson(R"({"mode":"toggle_hold","adaptive":{"enabled":true,"low_max_gain":1.25,"high_max_gain":0.8,"speed_start":0.2,"speed_full":0.9,"ramp_up_ms":110,"ramp_down_ms":150,"edge_pan":{"enabled":true,"band":0.05,"dwell_ms":220,"max_speed":0.3}}})"));
        require(adaptive.scope && adaptive.scope->adaptive && adaptive.scope->adaptive->enabled &&
            adaptive.scope->adaptive->edgePanEnabled && adaptive.scope->adaptive->lowMaxGain == 1.25 &&
            adaptive.scope->adaptive->edgeDwellMs == 220, "Adaptive settings parsed");
        const auto adaptiveArgs = arguments(adaptive, false);
        require(flagValue(adaptiveArgs, L"--scope-adaptive-enabled") == L"1" &&
            flagValue(adaptiveArgs, L"--scope-adaptive-edge-enabled") == L"1" &&
            std::stod(flagValue(adaptiveArgs, L"--scope-adaptive-speed-full")) == .9 &&
            std::stod(flagValue(adaptiveArgs, L"--scope-adaptive-edge-band")) == .05, "Adaptive settings transported");
        require(arguments(adaptive, true) == std::vector<std::wstring>{L"BoneEater.exe", L"--diagnose"}, "Adaptive settings do not activate diagnosis");
        const auto adaptiveReport = dryRunReport(L"C:\\fixture\\BoneEater.exe", L"C:\\fixture\\settings.json", adaptive, adaptiveArgs, isolateEnvironment({}), {});
        require(adaptiveReport.find("\"adaptive\":{\"enabled\":true") != std::string::npos &&
            adaptiveReport.find("\"edge_pan\":{\"enabled\":true") != std::string::npos, "Dry run reports effective adaptive settings");
        require(!parseSettings(scopeJson(R"({"adaptive":{"edge_pan":{"enabled":true}}})")).scope->adaptive->enabled,
            "Adaptive and edge pan remain off unless master is enabled");
        const auto tinyGain = parseSettings(scopeJson(R"({"low_gain":0.000001,"adaptive":{"low_max_gain":0.000001}})"));
        require(std::stod(flagValue(arguments(tinyGain,false), L"--scope-adaptive-low-max-gain")) == .000001,
            "Positive sub-1e-5 adaptive gain survives launcher transport");
        ++cases;
        const auto scopeLimits = parseSettings(scopeJson(R"({"mode":"legacy","bindings":["SPACE","LBUTTON","MBUTTON","XBUTTON1","XBUTTON2","A","Z","0"],"hold_ms":100,"low_gain":1,"high_gain":0.00001,"low_smoothing_ms":0,"high_smoothing_ms":250})"));
        require(scopeLimits.scope->bindings.size() == 8 && scopeLimits.scope->holdMs == 100 && scopeLimits.scope->lowGain == 1
            && scopeLimits.scope->highGain == 0.00001 && scopeLimits.scope->lowSmoothingMs == 0 && scopeLimits.scope->highSmoothingMs == 250, "Scope valid limits");
        require(flagValue(arguments(scopeLimits, false), L"--scope-bindings") == L"SPACE,LBUTTON,MBUTTON,XBUTTON1,XBUTTON2,A,Z,0", "Generic scope bindings transported in order");
        require(parseSettings(scopeJson(R"({"hold_ms":1000,"bindings":["9"]})")).scope->holdMs == 1000, "Upper hold bound and numeric key");
        ++cases;
        for (const std::string& badScope : std::vector<std::string>{"null", "[]", "true",
            R"({"mode":"hold"})", R"({"mode":true})", R"({"mode":"legacy\u0000"})", R"({"extra":1})", R"({"hold_ms":250,"hold_ms":500})",
            R"({"bindings":[]})", R"({"bindings":"ENTER"})", R"({"bindings":[13]})", R"({"bindings":["ENTER","ENTER"]})",
            R"({"bindings":["enter"]})", R"({"bindings":["ENTER\u0000"]})", R"({"bindings":["ENTER,RBUTTON"]})", R"({"bindings":["AA"]})",
            R"({"bindings":["A","B","C","D","E","F","G","H","I"]})",
            R"({"hold_ms":99})", R"({"hold_ms":1001})", R"({"hold_ms":250.5})", R"({"hold_ms":-1})", R"({"hold_ms":true})",
            R"({"low_gain":0})", R"({"low_gain":1.01})", R"({"high_gain":-0.1})", R"({"high_gain":"0.1"})", R"({"low_gain":1e999})",
            R"({"low_smoothing_ms":-1})", R"({"high_smoothing_ms":251})", R"({"high_smoothing_ms":null})"})
            rejects([&] { parseSettings(scopeJson(badScope)); });
        rejects([&] { parseSettings(defaultJson.substr(0, defaultJson.size() - 1) + R"(,"scope":{},"scope":{}})"); });
        for (const std::string& badAdaptive : std::vector<std::string>{"null", "[]", R"({"enabled":1})",
            R"({"enabled":true,"enabled":false})", R"({"low_max_gain":0.1})", R"({"high_max_gain":0})",
            R"({"speed_start":0.8,"speed_full":0.8})", R"({"speed_start":0})", R"({"speed_full":4.1})",
            R"({"ramp_up_ms":0})", R"({"ramp_down_ms":"140"})", R"({"edge_pan":null})",
            R"({"edge_pan":{"enabled":1}})", R"({"edge_pan":{"band":0.5}})",
            R"({"edge_pan":{"dwell_ms":10}})", R"({"edge_pan":{"max_speed":0}})",
            R"({"edge_pan":{"band":0.04,"band":0.05}})", R"({"unknown":1})"})
            rejects([&] { parseSettings(scopeJson(std::string("{\"adaptive\":") + badAdaptive + "}")); });
        rejects([&] { parseSettings(scopeJson(R"({"high_gain":0.9,"adaptive":{}})")); });
        ++cases;
        auto source = std::vector<std::wstring>{L"Path=first", L"PATH=second", L"=C:=C:\\working", L"SystemRoot=C:\\Windows",
            L"bone_eater_CAPTURE=1", L"BONE_EATER_BATTLE_BACKGROUND_FILTER=1", L"Bone_Eater_NATIVE_MOVIE_FIT=1", L"BONE_EATER=keep"};
        const auto original = source;
        const auto clean = isolateEnvironment(source);
        const auto kept = entries(clean);
        require(source == original && clean.removed.size() == 3 && kept.size() == 4, "Environment isolation changed source/count");
        require(std::find(kept.begin(), kept.end(), L"PATH=second") != kept.end() && std::find(kept.begin(), kept.end(), L"=C:=C:\\working") != kept.end(), "Windows environment casing or drive entry");
        require(isolateEnvironment({}).block == std::vector<wchar_t>{0, 0}, "Empty environment terminator");
        rejects([&] { isolateEnvironment({L"no separator"}); });
        ++cases;
        const std::vector<std::wstring> quoted {L"BoneEater.exe", L"-2Display", L"space 雪 path", L"C:\\tail space\\", L"literal\"quote", L"", L"line\nbreak"};
        const auto line = commandLine(quoted);
        int parsedCount = 0; auto parsed = CommandLineToArgvW(line.c_str(), &parsedCount);
        require(parsed && parsedCount == static_cast<int>(quoted.size()), "Quoted argument count");
        for (int i = 0; i < parsedCount; ++i) require(quoted[i] == parsed[i], "Quoted argument round trip");
        LocalFree(parsed);
        rejects([&] { commandLine({std::wstring(32767, L'x')}); });
        ++cases;
        require(instanceName(L"C:\\Game\\BoneEater.exe") == instanceName(L"c:\\game\\boneeater.EXE"), "Instance identity ignores case");
        require(instanceName(L"C:\\A\\BoneEater.exe") != instanceName(L"C:\\B\\BoneEater.exe"), "Separate paths have separate identity");
        ++cases;
        const auto temp = std::filesystem::temp_directory_path() / (L"bone-eater-launcher-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(temp), "Create isolated fixture directory");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); } } cleanup {temp};
        const auto folder = temp / L"fixture 雪 directory";
        std::filesystem::create_directory(folder);
        const auto copied = folder / L"inert fixture 雪.exe";
        std::filesystem::copy_file(fixture, copied);
        auto environment = currentEnvironment();
        auto inherited = entries(environment);
        inherited.push_back(L"LAUNCHER_FIXTURE_KEEP=kept-雪");
        inherited.push_back(L"BONE_EATER_CAPTURE=1");
        inherited.push_back(L"BONE_EATER_BATTLE_BACKGROUND_FILTER=1");
        inherited.push_back(L"BONE_EATER_NATIVE_MOVIE_FIT=1");
        environment = isolateEnvironment(inherited);
        const auto output = folder / L"fixture-output.log";
        require(runChild(copied, folder, quoted, environment, output) == 0, "Actual Windows child contract");
        require(read(output).find("exact Unicode argv") != std::string::npos && read(output).find("stderr captured") != std::string::npos, "Both output streams captured");
        require(matchingProcesses(std::filesystem::canonical(copied)).empty(), "Completed child not reported live");
        ++cases;
        const auto eventPrefix = L"Local\\BoneEaterLauncherFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto readyName = eventPrefix + L"-ready", releaseName = eventPrefix + L"-release";
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
        require(ready && release, "Create inert child synchronization");
        auto live = std::async(std::launch::async, [&] { return runChild(copied, folder,
            {L"BoneEater.exe", L"--wait-event", readyName, releaseName}, environment, folder / L"live.log"); });
        const auto readyResult = WaitForSingleObject(ready, 5000);
        const auto matching = matchingProcesses(std::filesystem::canonical(copied));
        const auto unrelated = matchingProcesses(temp / copied.filename());
        SetEvent(release);
        const auto exitCode = live.get();
        CloseHandle(ready); CloseHandle(release);
        require(readyResult == WAIT_OBJECT_0 && exitCode == 0 && matching.size() == 1 && unrelated.empty(), "Live duplicate detection is exact-path only");
        ++cases;
        const auto failed = folder / L"failure.log";
        require(runChild(copied, folder, {L"BoneEater.exe", L"--exit-42"}, environment, failed) == 42, "Exit code preserved");
        require(read(failed).find("startup failure detail") != std::string::npos, "Failure detail captured");
        rejects([&] { runChild(copied, folder, quoted, environment, failed); });
        rejects([&] { runChild(folder / L"missing.exe", folder, quoted, environment, folder / L"missing.log"); });
        ++cases;
        const auto config = temp / L"settings.json";
        { std::ofstream file(config, std::ios::binary); file << defaultJson; }
        require(readSettings(config).view == "balanced125", "Bounded settings file");
        { std::ofstream file(config, std::ios::binary); file << std::string(16385, ' '); }
        rejects([&] { readSettings(config); });
        rejects([&] { verifiedGame(temp); });
        std::filesystem::create_directories(temp / L"runtime/game/modules");
        std::filesystem::create_directories(temp / L"runtime/game/prop");
        for (const auto* name : {L"BoneEater.exe", L"modules/gamendd.dll", L"modules/arkndd.dll", L"prop/avs-config.xml"}) { std::ofstream file(temp / L"runtime/game" / name); file << "fixture, never executed"; }
        require(verifiedGame(temp) == std::filesystem::canonical(temp / L"runtime/game/BoneEater.exe"), "Only fixed working-copy path resolved");
        std::filesystem::create_directory(temp / L"game");
        rejects([&] { verifiedGame(temp); }); // Incomplete compact tree must not launch legacy copy.
        std::filesystem::copy(temp / L"runtime/game", temp / L"game", std::filesystem::copy_options::recursive);
        require(verifiedGame(temp) == std::filesystem::canonical(temp / L"game/BoneEater.exe"), "Compact game path wins");
        std::filesystem::remove_all(temp / L"game");
        const auto report = dryRunReport(copied, config, defaults, play, clean, {1234});
        require(report.find("\"child_started\":false") != std::string::npos && report.find("\"matching_processes\":[1234]") != std::string::npos && report.find("first") == std::string::npos, "Dry-run metadata does not disclose inherited values");
        ++cases;
        std::cout << cases << " launcher fixture groups passed; no game launched\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "launcher test failed: " << error.what() << '\n'; return 1; }
}
