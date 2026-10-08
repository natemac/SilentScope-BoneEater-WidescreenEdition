#include "launcher.h"
#include "external/rapidjson/document.h"
#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bone_eater::launcher {
namespace {
AdaptiveScopeSettings parseAdaptive(const rapidjson::Value& value, double lowGain, double highGain) {
    if (!value.IsObject()) throw std::runtime_error("Scope adaptive must be an object.");
    const std::array<std::string_view, 7> names {"enabled", "low_max_gain", "high_max_gain", "speed_start", "speed_full", "ramp_up_ms", "ramp_down_ms"};
    for (auto item = value.MemberBegin(); item != value.MemberEnd(); ++item) {
        const std::string_view name(item->name.GetString(), item->name.GetStringLength());
        if (name != "edge_pan" && std::find(names.begin(), names.end(), name) == names.end())
            throw std::runtime_error("Unknown scope adaptive setting.");
        for (auto old = value.MemberBegin(); old != item; ++old)
            if (name == std::string_view(old->name.GetString(), old->name.GetStringLength()))
                throw std::runtime_error("Duplicate scope adaptive setting.");
    }
    AdaptiveScopeSettings result;
    auto boolean = [](const rapidjson::Value& object, const char* key, bool fallback) {
        if (!object.HasMember(key)) return fallback;
        if (!object[key].IsBool()) throw std::runtime_error(std::string("Scope adaptive ") + key + " must be boolean.");
        return object[key].GetBool();
    };
    auto number = [](const rapidjson::Value& object, const char* key, double fallback, double minimum, double maximum) {
        if (!object.HasMember(key)) return fallback;
        if (!object[key].IsNumber()) throw std::runtime_error(std::string("Scope adaptive ") + key + " must be numeric.");
        const double result = object[key].GetDouble();
        if (!std::isfinite(result) || result < minimum || result > maximum)
            throw std::runtime_error(std::string("Scope adaptive ") + key + " is outside its supported range.");
        return result;
    };
    result.enabled = boolean(value, "enabled", result.enabled);
    result.lowMaxGain = number(value, "low_max_gain", result.lowMaxGain, lowGain, 2.0);
    result.highMaxGain = number(value, "high_max_gain", result.highMaxGain, highGain, 2.0);
    if (result.lowMaxGain < lowGain || result.highMaxGain < highGain)
        throw std::runtime_error("Scope adaptive max gains must be at least their precision gains.");
    result.speedStart = number(value, "speed_start", result.speedStart, .01, 4.0);
    result.speedFull = number(value, "speed_full", result.speedFull, .01, 4.0);
    if (result.speedFull <= result.speedStart) throw std::runtime_error("Scope adaptive speed_full must exceed speed_start.");
    result.rampUpMs = number(value, "ramp_up_ms", result.rampUpMs, 10, 1000);
    result.rampDownMs = number(value, "ramp_down_ms", result.rampDownMs, 10, 1000);
    if (value.HasMember("edge_pan")) {
        const auto& edge = value["edge_pan"];
        if (!edge.IsObject()) throw std::runtime_error("Scope adaptive edge_pan must be an object.");
        const std::array<std::string_view, 4> keys {"enabled", "band", "dwell_ms", "max_speed"};
        for (auto item = edge.MemberBegin(); item != edge.MemberEnd(); ++item) {
            const std::string_view name(item->name.GetString(), item->name.GetStringLength());
            if (std::find(keys.begin(), keys.end(), name) == keys.end()) throw std::runtime_error("Unknown scope adaptive edge_pan setting.");
            for (auto old = edge.MemberBegin(); old != item; ++old)
                if (name == std::string_view(old->name.GetString(), old->name.GetStringLength()))
                    throw std::runtime_error("Duplicate scope adaptive edge_pan setting.");
        }
        result.edgePanEnabled = boolean(edge, "enabled", result.edgePanEnabled);
        result.edgeBand = number(edge, "band", result.edgeBand, .01, .2);
        result.edgeDwellMs = number(edge, "dwell_ms", result.edgeDwellMs, 50, 1000);
        result.edgeMaxSpeed = number(edge, "max_speed", result.edgeMaxSpeed, .01, 2.0);
    }
    return result;
}
ScopeSettings parseScope(const rapidjson::Value& value) {
    if (!value.IsObject()) throw std::runtime_error("Scope setting must be an object.");
    const std::array<std::string_view, 10> names {"hold_release", "shape", "mode", "bindings", "hold_ms", "low_gain", "high_gain", "low_smoothing_ms", "high_smoothing_ms", "adaptive"};
    for (auto item = value.MemberBegin(); item != value.MemberEnd(); ++item) {
        const std::string_view name(item->name.GetString(), item->name.GetStringLength());
        if (std::find(names.begin(), names.end(), name) == names.end()) throw std::runtime_error("Unknown scope setting.");
        for (auto old = value.MemberBegin(); old != item; ++old)
            if (name == std::string_view(old->name.GetString(), old->name.GetStringLength()))
                throw std::runtime_error("Duplicate scope setting.");
    }
    ScopeSettings result;
    if (value.HasMember("shape")) {
        const auto& shape = value["shape"];
        if (!shape.IsString()) throw std::runtime_error("Scope shape must be circle or angled.");
        result.shape.assign(shape.GetString(), shape.GetStringLength());
        if (result.shape != "circle" && result.shape != "angled")
            throw std::runtime_error("Scope shape must be circle or angled.");
    }
    if (value.HasMember("mode")) {
        const auto& mode = value["mode"];
        if (!mode.IsString()) throw std::runtime_error("Scope mode must be legacy or toggle_hold.");
        result.mode.assign(mode.GetString(), mode.GetStringLength());
        if (result.mode != "legacy" && result.mode != "toggle_hold") throw std::runtime_error("Scope mode must be legacy or toggle_hold.");
    }
    if (value.HasMember("bindings")) {
        const auto& bindings = value["bindings"];
        if (!bindings.IsArray() || bindings.Empty() || bindings.Size() > 8)
            throw std::runtime_error("Scope bindings must contain 1 to 8 distinct key names.");
        result.bindings.clear();
        const std::array<std::string_view, 7> keys {"ENTER", "SPACE", "LBUTTON", "RBUTTON", "MBUTTON", "XBUTTON1", "XBUTTON2"};
        for (const auto& binding : bindings.GetArray()) {
            if (!binding.IsString()) throw std::runtime_error("Scope bindings must be key-name strings.");
            const std::string name(binding.GetString(), binding.GetStringLength());
            const bool alphanumeric = name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'));
            if (!alphanumeric && std::find(keys.begin(), keys.end(), name) == keys.end())
                throw std::runtime_error("Unsupported scope binding; use ENTER, SPACE, LBUTTON, RBUTTON, MBUTTON, XBUTTON1, XBUTTON2, A-Z or 0-9.");
            if (std::find(result.bindings.begin(), result.bindings.end(), name) != result.bindings.end())
                throw std::runtime_error("Duplicate scope binding.");
            result.bindings.push_back(name);
        }
    }
    if (value.HasMember("hold_release")) {
        const auto& release = value["hold_release"];
        if (!release.IsString()) throw std::runtime_error("Scope hold_release must be lower or exit.");
        result.holdRelease.assign(release.GetString(), release.GetStringLength());
        if (result.holdRelease != "lower" && result.holdRelease != "exit")
            throw std::runtime_error("Scope hold_release must be lower or exit.");
    }
    if (value.HasMember("hold_ms")) {
        const auto& hold = value["hold_ms"];
        if (!hold.IsUint() || hold.GetUint() < 100 || hold.GetUint() > 1000)
            throw std::runtime_error("Scope hold_ms must be an integer from 100 to 1000.");
        result.holdMs = hold.GetUint();
    }
    auto number = [&](const char* name, double fallback, bool gain) {
        if (!value.HasMember(name)) return fallback;
        const auto& setting = value[name];
        if (!setting.IsNumber()) throw std::runtime_error(std::string("Scope ") + name + " must be numeric.");
        const double result = setting.GetDouble();
        if (!std::isfinite(result) || (gain ? result <= 0 || result > 1 : result < 0 || result > 250))
            throw std::runtime_error(std::string("Scope ") + name + (gain ? " must be greater than 0 and at most 1." : " must be between 0 and 250 milliseconds."));
        return result;
    };
    result.lowGain = number("low_gain", result.lowGain, true);
    result.highGain = number("high_gain", result.highGain, true);
    result.lowSmoothingMs = number("low_smoothing_ms", result.lowSmoothingMs, false);
    result.highSmoothingMs = number("high_smoothing_ms", result.highSmoothingMs, false);
    if (value.HasMember("adaptive")) result.adaptive = parseAdaptive(value["adaptive"], result.lowGain, result.highGain);
    return result;
}
std::wstring decimal(double value) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}
}
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    if (text.find('\0') != std::string::npos || text.size() > INT_MAX)
        throw std::runtime_error("Invalid text length or embedded NUL.");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Text is not valid UTF-8.");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size))
        throw std::runtime_error("Cannot convert UTF-8 text.");
    return result;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.find(L'\0') != std::wstring::npos || text.size() > INT_MAX)
        throw std::runtime_error("Invalid Windows text length or embedded NUL.");
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Windows Unicode text.");
    std::string result(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr))
        throw std::runtime_error("Cannot encode Windows text.");
    return result;
}
Settings parseSettings(const std::string& text) {
    if (text.empty() || text.size() > 16384 || text.find('\0') != std::string::npos)
        throw std::runtime_error("Launch settings must be nonempty UTF-8 JSON, at most 16 KiB, without NUL bytes.");
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) throw std::runtime_error("Launch settings must be a JSON object.");
    const std::array<std::string_view, 6> names {"schema_version", "view", "main_dof_off", "input_profile", "scope", "force_1080p"};
    for (auto item = doc.MemberBegin(); item != doc.MemberEnd(); ++item) {
        const std::string_view name(item->name.GetString(), item->name.GetStringLength());
        if (std::find(names.begin(), names.end(), name) == names.end()) throw std::runtime_error("Unknown launch setting.");
        for (auto old = doc.MemberBegin(); old != item; ++old)
            if (name == std::string_view(old->name.GetString(), old->name.GetStringLength()))
                throw std::runtime_error("Duplicate launch setting.");
    }
    for (auto name : names) if (name != "scope" && name != "force_1080p" && !doc.HasMember(name.data())) throw std::runtime_error("Missing launch setting.");
    if (!doc["schema_version"].IsInt() || doc["schema_version"].GetInt() != 1)
        throw std::runtime_error("Unsupported launch settings schema; expected 1.");
    if (!doc["view"].IsString() || !doc["main_dof_off"].IsBool()) throw std::runtime_error("Invalid launch view or DOF setting type.");
    Settings result;
    result.view.assign(doc["view"].GetString(), doc["view"].GetStringLength());
    if (result.view != "balanced125" && result.view != "closer150")
        throw std::runtime_error("View must be balanced125 or closer150.");
    result.mainDofOff = doc["main_dof_off"].GetBool();
    if (doc.HasMember("force_1080p")) {
        if (!doc["force_1080p"].IsBool()) throw std::runtime_error("force_1080p must be true or false.");
        result.force1080p = doc["force_1080p"].GetBool();
    }
    const auto& profile = doc["input_profile"];
    if (!profile.IsNull()) {
        if (!profile.IsString() || !profile.GetStringLength() || profile.GetStringLength() > 4096)
            throw std::runtime_error("Input profile must be null or a nonempty path of at most 4096 UTF-8 bytes.");
        result.inputProfile = widen(std::string(profile.GetString(), profile.GetStringLength()));
        const std::filesystem::path path(result.inputProfile);
        if (!path.is_absolute() && path.has_root_path()) throw std::runtime_error("Input profile must be absolute or relative to runtime/game.");
    }
    if (doc.HasMember("scope")) result.scope = parseScope(doc["scope"]);
    return result;
}
Settings readSettings(const std::filesystem::path& file) {
    if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Missing launch-settings.json beside the launcher.");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open launch settings.");
    std::array<char, 16385> data {};
    input.read(data.data(), data.size());
    if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Cannot read launch settings.");
    return parseSettings(std::string(data.data(), static_cast<std::size_t>(input.gcount())));
}
std::vector<std::wstring> arguments(const Settings& settings, bool diagnose) {
    std::vector<std::wstring> result {L"BoneEater.exe"};
    if (diagnose) result.push_back(L"--diagnose");
    else {
        result.insert(result.end(), {L"-2Display", L"--native-wide", L"--main-monitor", L"--native-fov-zoom",
            settings.view == "balanced125" ? L"1.25" : L"1.5", L"--native-hud-fit", L"--native-rear-hud-fit",
            L"--native-input-desktop", L"--scope-overlay", L"--desktop-reticle", L"--native-battle-background-align",
            L"--aim-framing", L"--park-auxiliary", L"--native-movie-fit", L"--native-replay-align", L"--native-ranking-backing",
            L"--native-menu-margin", L"--native-normal-start-backing", L"--native-options-backing", L"--quiet-diagnostics"});
        if (settings.mainDofOff) result.push_back(L"--native-main-dof-off");
        if (settings.scope) {
            const auto& scope = *settings.scope;
            std::string bindings;
            for (const auto& key : scope.bindings) { if (!bindings.empty()) bindings += ','; bindings += key; }
            result.insert(result.end(), {L"--scope-mode", widen(scope.mode), L"--scope-bindings", widen(bindings),
                L"--scope-shape", std::wstring(scope.shape.begin(), scope.shape.end()),
                L"--scope-hold-release", widen(scope.holdRelease),
                L"--scope-hold-ms", std::to_wstring(scope.holdMs), L"--scope-low-gain", decimal(scope.lowGain),
                L"--scope-high-gain", decimal(scope.highGain), L"--scope-low-smoothing-ms", decimal(scope.lowSmoothingMs),
                L"--scope-high-smoothing-ms", decimal(scope.highSmoothingMs)});
            if (scope.adaptive) {
                const auto& a = *scope.adaptive;
                result.insert(result.end(), {L"--scope-adaptive-enabled", a.enabled ? L"1" : L"0",
                    L"--scope-adaptive-low-max-gain", decimal(a.lowMaxGain),
                    L"--scope-adaptive-high-max-gain", decimal(a.highMaxGain),
                    L"--scope-adaptive-speed-start", decimal(a.speedStart),
                    L"--scope-adaptive-speed-full", decimal(a.speedFull),
                    L"--scope-adaptive-ramp-up-ms", decimal(a.rampUpMs),
                    L"--scope-adaptive-ramp-down-ms", decimal(a.rampDownMs),
                    L"--scope-adaptive-edge-enabled", a.edgePanEnabled ? L"1" : L"0",
                    L"--scope-adaptive-edge-band", decimal(a.edgeBand),
                    L"--scope-adaptive-edge-dwell-ms", decimal(a.edgeDwellMs),
                    L"--scope-adaptive-edge-max-speed", decimal(a.edgeMaxSpeed)});
            }
        }
    }
    if (!settings.inputProfile.empty()) result.insert(result.end(), {L"--input-config", settings.inputProfile});
    return result;
}
std::wstring quoteArgument(const std::wstring& value) {
    if (value.find(L'\0') != std::wstring::npos) throw std::runtime_error("NUL in a process argument.");
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (character == L'\"') result.push_back(L'\\');
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
std::wstring commandLine(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (const auto& value : values) {
        if (!result.empty()) result.push_back(L' ');
        result += quoteArgument(value);
    }
    if (result.empty() || result.size() >= 32767) throw std::runtime_error("Invalid or too long process command line.");
    return result;
}
namespace {
struct OrdinalLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
}
Environment isolateEnvironment(const std::vector<std::wstring>& source) {
    std::map<std::wstring, std::wstring, OrdinalLess> retained;
    Environment result;
    for (const auto& entry : source) {
        if (entry.find(L'\0') != std::wstring::npos) throw std::runtime_error("NUL in inherited environment entry.");
        const auto separator = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
        if (separator == std::wstring::npos || separator == 0) throw std::runtime_error("Invalid inherited environment entry.");
        const auto name = entry.substr(0, separator);
        if (name.size() >= 11 && CompareStringOrdinal(name.data(), 11, L"BONE_EATER_", 11, TRUE) == CSTR_EQUAL) {
            result.removed.push_back(name);
        } else retained.insert_or_assign(name, entry);
    }
    for (const auto& pair : retained) {
        result.block.insert(result.block.end(), pair.second.begin(), pair.second.end());
        result.block.push_back(L'\0');
    }
    if (result.block.empty()) result.block.push_back(L'\0');
    result.block.push_back(L'\0');
    result.retainedCount = retained.size();
    return result;
}
Environment currentEnvironment() {
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) throw std::runtime_error(windowsError("Read environment"));
    std::vector<std::wstring> entries;
    try { for (const wchar_t* item = block; *item; item += wcslen(item) + 1) entries.emplace_back(item); }
    catch (...) { FreeEnvironmentStringsW(block); throw; }
    FreeEnvironmentStringsW(block);
    return isolateEnvironment(entries);
}
std::string dryRunReport(const std::filesystem::path& executable, const std::filesystem::path& settingsFile,
        const Settings& settings, const std::vector<std::wstring>& argv, const Environment& environment,
        const std::vector<DWORD>& running) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    auto string = [&](const char* key, const std::wstring& value) { out.Key(key); const auto encoded = narrow(value); out.String(encoded.data(), static_cast<rapidjson::SizeType>(encoded.size())); };
    out.StartObject(); out.Key("schema_version"); out.Int(1); out.Key("child_started"); out.Bool(false);
    string("application", executable.wstring()); string("working_directory", executable.parent_path().wstring());
    string("settings_file", settingsFile.wstring()); string("command_line", commandLine(argv));
    out.Key("argv"); out.StartArray(); for (const auto& arg : argv) { const auto encoded = narrow(arg); out.String(encoded.data(), static_cast<rapidjson::SizeType>(encoded.size())); } out.EndArray();
    out.Key("view"); out.String(settings.view.c_str()); out.Key("main_dof_off"); out.Bool(settings.mainDofOff);
    out.Key("force_1080p"); out.Bool(settings.force1080p);
    out.Key("scope");
    if (!settings.scope) out.Null();
    else {
        const auto& scope = *settings.scope;
        out.StartObject();
        out.Key("mode"); out.String(scope.mode.c_str());
        out.Key("bindings"); out.StartArray(); for (const auto& key : scope.bindings) out.String(key.c_str()); out.EndArray();
        out.Key("hold_ms"); out.Uint(scope.holdMs);
        out.Key("hold_release"); out.String(scope.holdRelease.c_str());
        out.Key("low_gain"); out.Double(scope.lowGain); out.Key("high_gain"); out.Double(scope.highGain);
        out.Key("low_smoothing_ms"); out.Double(scope.lowSmoothingMs); out.Key("high_smoothing_ms"); out.Double(scope.highSmoothingMs);
        out.Key("shape"); out.String(scope.shape.c_str());
        out.Key("adaptive");
        if (!scope.adaptive) out.Null();
        else {
            const auto& a = *scope.adaptive;
            out.StartObject();
            out.Key("enabled"); out.Bool(a.enabled);
            out.Key("low_max_gain"); out.Double(a.lowMaxGain); out.Key("high_max_gain"); out.Double(a.highMaxGain);
            out.Key("speed_start"); out.Double(a.speedStart); out.Key("speed_full"); out.Double(a.speedFull);
            out.Key("ramp_up_ms"); out.Double(a.rampUpMs); out.Key("ramp_down_ms"); out.Double(a.rampDownMs);
            out.Key("edge_pan"); out.StartObject(); out.Key("enabled"); out.Bool(a.edgePanEnabled);
            out.Key("band"); out.Double(a.edgeBand); out.Key("dwell_ms"); out.Double(a.edgeDwellMs);
            out.Key("max_speed"); out.Double(a.edgeMaxSpeed); out.EndObject();
            out.EndObject();
        }
        out.EndObject();
    }
    out.Key("api_argument_enabled"); out.Bool(false);
    out.Key("retained_environment_count"); out.Uint64(environment.retainedCount);
    out.Key("removed_environment_names"); out.StartArray(); for (const auto& name : environment.removed) { const auto encoded = narrow(name); out.String(encoded.c_str()); } out.EndArray();
    out.Key("environment_policy"); out.String("Copy parent; remove BONE_EATER_ case-insensitively; deduplicate variable names case-insensitively; do not change parent.");
    out.Key("environment_values_redacted"); out.Bool(true);
    out.Key("input_profile_contents_validated"); out.Bool(false);
    out.Key("complete_runtime_preflight_performed"); out.Bool(false);
    out.Key("matching_processes"); out.StartArray(); for (auto pid : running) out.Uint(pid); out.EndArray();
    out.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize()) + "\n";
}
} // namespace bone_eater::launcher
