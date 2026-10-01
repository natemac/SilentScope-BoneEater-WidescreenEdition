#pragma once
#include "input/scoped_motion.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <cerrno>
#include <algorithm>

namespace bone_eater::input {
inline int scopeBindingKey(const std::string& token) {
    if (token == "ENTER") return 13;
    if (token == "SPACE") return 32;
    if (token == "LBUTTON") return 1;
    if (token == "RBUTTON") return 2;
    if (token == "MBUTTON") return 4;
    if (token == "XBUTTON1") return 5;
    if (token == "XBUTTON2") return 6;
    if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') ||
            (token[0] >= '0' && token[0] <= '9'))) return token[0];
    throw std::runtime_error("Unsupported scope binding: " + token);
}
inline std::vector<int> parseScopeBindings(const std::string& value) {
    std::vector<int> result;
    std::size_t begin = 0;
    do {
        const auto end = value.find(',', begin);
        const auto key = scopeBindingKey(value.substr(begin, end - begin));
        if (result.size() == 8 || std::find(result.begin(), result.end(), key) != result.end())
            throw std::runtime_error("Scope bindings must contain 1-8 distinct keys.");
        result.push_back(key);
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (true);
    return result;
}
inline double scopeNumber(const std::string& value, double minimum, double maximum, bool integer = false) {
    char* end = nullptr; errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (value.empty() || value.front() == ' ' || errno || end != value.c_str() + value.size() ||
        !std::isfinite(parsed) || parsed < minimum || parsed > maximum ||
        (integer && std::floor(parsed) != parsed)) throw std::runtime_error("Invalid scope numeric setting.");
    return parsed;
}
// Shared by standalone preflight and native installation. Resolve the effective
// Win32 environment after CLI options, rather than the CRT environment snapshot.
template<class ReadEnvironment>
ScopedMotionSettings readScopedMotionSettings(ReadEnvironment read) {
    ScopedMotionSettings result;
    auto number = [&](const char* name, const char* fallback, double lo, double hi) {
        try { return scopeNumber(read(name, fallback), lo, hi); }
        catch (...) { throw std::runtime_error(std::string("Invalid scope setting: ") + name); }
    };
    result.lowGain = number("BONE_EATER_SCOPE_LOW_GAIN", ".25", 0, 1);
    result.highGain = number("BONE_EATER_SCOPE_HIGH_GAIN", ".10", 0, 1);
    result.lowMs = number("BONE_EATER_SCOPE_LOW_SMOOTHING_MS", "35", 0, 250);
    result.highMs = number("BONE_EATER_SCOPE_HIGH_SMOOTHING_MS", "55", 0, 250);
    if (result.lowGain <= 0 || result.highGain <= 0) throw std::runtime_error("Scope precision gains must be positive.");
    bool specified = false;
    auto adaptiveNumber = [&](const char* name, const char* fallback, double lo, double hi) {
        if (!read(name, "").empty()) specified = true;
        return number(name, fallback, lo, hi);
    };
    auto flag = [&](const char* name) {
        const auto value = read(name, "");
        if (value.empty()) return false;
        specified = true;
        if (value != "0" && value != "1") throw std::runtime_error(std::string(name) + " must be 0 or 1.");
        return value == "1";
    };
    auto& a = result.adaptive;
    a.enabled = flag("BONE_EATER_SCOPE_ADAPTIVE_ENABLED");
    a.lowMaxGain = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_LOW_MAX_GAIN", "1", 0, 2);
    a.highMaxGain = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_HIGH_MAX_GAIN", ".7", 0, 2);
    a.speedStart = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_SPEED_START", ".15", .01, 4);
    a.speedFull = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_SPEED_FULL", ".8", .01, 4);
    a.rampUpMs = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_RAMP_UP_MS", "100", 10, 1000);
    a.rampDownMs = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_RAMP_DOWN_MS", "140", 10, 1000);
    const bool edge = flag("BONE_EATER_SCOPE_ADAPTIVE_EDGE_ENABLED");
    a.edgePan = edge && a.enabled;
    a.edgeBand = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_EDGE_BAND", ".04", .01, .2);
    a.edgeDwellMs = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_EDGE_DWELL_MS", "200", 50, 1000);
    a.edgeMaxSpeed = adaptiveNumber("BONE_EATER_SCOPE_ADAPTIVE_EDGE_MAX_SPEED", ".25", .01, 2);
    if (specified && (a.lowMaxGain < result.lowGain || a.highMaxGain < result.highGain))
        throw std::runtime_error("Scope adaptive max gains must be at least their corresponding precision gains.");
    if (a.speedFull <= a.speedStart)
        throw std::runtime_error("Scope adaptive speed_full must exceed speed_start.");
    return result;
}
inline const char* scopeEnvironmentName(const std::string& flag) {
    if (flag == "--scope-shape") return "BONE_EATER_SCOPE_SHAPE";
    if (flag == "--scope-mode") return "BONE_EATER_SCOPE_MODE";
    if (flag == "--scope-bindings") return "BONE_EATER_SCOPE_BINDINGS";
    if (flag == "--scope-hold-ms") return "BONE_EATER_SCOPE_HOLD_MS";
    if (flag == "--scope-low-gain") return "BONE_EATER_SCOPE_LOW_GAIN";
    if (flag == "--scope-high-gain") return "BONE_EATER_SCOPE_HIGH_GAIN";
    if (flag == "--scope-low-smoothing-ms") return "BONE_EATER_SCOPE_LOW_SMOOTHING_MS";
    if (flag == "--scope-high-smoothing-ms") return "BONE_EATER_SCOPE_HIGH_SMOOTHING_MS";
    if (flag == "--scope-adaptive-enabled") return "BONE_EATER_SCOPE_ADAPTIVE_ENABLED";
    if (flag == "--scope-adaptive-low-max-gain") return "BONE_EATER_SCOPE_ADAPTIVE_LOW_MAX_GAIN";
    if (flag == "--scope-adaptive-high-max-gain") return "BONE_EATER_SCOPE_ADAPTIVE_HIGH_MAX_GAIN";
    if (flag == "--scope-adaptive-speed-start") return "BONE_EATER_SCOPE_ADAPTIVE_SPEED_START";
    if (flag == "--scope-adaptive-speed-full") return "BONE_EATER_SCOPE_ADAPTIVE_SPEED_FULL";
    if (flag == "--scope-adaptive-ramp-up-ms") return "BONE_EATER_SCOPE_ADAPTIVE_RAMP_UP_MS";
    if (flag == "--scope-adaptive-ramp-down-ms") return "BONE_EATER_SCOPE_ADAPTIVE_RAMP_DOWN_MS";
    if (flag == "--scope-adaptive-edge-enabled") return "BONE_EATER_SCOPE_ADAPTIVE_EDGE_ENABLED";
    if (flag == "--scope-adaptive-edge-band") return "BONE_EATER_SCOPE_ADAPTIVE_EDGE_BAND";
    if (flag == "--scope-adaptive-edge-dwell-ms") return "BONE_EATER_SCOPE_ADAPTIVE_EDGE_DWELL_MS";
    if (flag == "--scope-adaptive-edge-max-speed") return "BONE_EATER_SCOPE_ADAPTIVE_EDGE_MAX_SPEED";
    return nullptr;
}
inline void validateScopeOption(const std::string& flag, const std::string& value) {
    if (flag == "--scope-shape") {
        if (value != "circle" && value != "angled") throw std::runtime_error("Scope shape must be circle or angled.");
    } else if (flag == "--scope-mode") {
        if (value != "legacy" && value != "toggle_hold") throw std::runtime_error("Scope mode must be legacy or toggle_hold.");
    } else if (flag == "--scope-bindings") (void)parseScopeBindings(value);
    else if (flag == "--scope-hold-ms") (void)scopeNumber(value, 100, 1000, true);
    else if (flag == "--scope-adaptive-enabled" || flag == "--scope-adaptive-edge-enabled") {
        if (value != "0" && value != "1") throw std::runtime_error("Scope adaptive switches must be 0 or 1.");
    }
    else if (flag == "--scope-adaptive-low-max-gain" || flag == "--scope-adaptive-high-max-gain") {
        if (scopeNumber(value, 0, 2.0) == 0) throw std::runtime_error("Scope adaptive gain must be positive.");
    }
    else if (flag == "--scope-adaptive-speed-start" || flag == "--scope-adaptive-speed-full")
        (void)scopeNumber(value, .01, 4.0);
    else if (flag == "--scope-adaptive-ramp-up-ms" || flag == "--scope-adaptive-ramp-down-ms")
        (void)scopeNumber(value, 10, 1000);
    else if (flag == "--scope-adaptive-edge-band") (void)scopeNumber(value, .01, .2);
    else if (flag == "--scope-adaptive-edge-dwell-ms") (void)scopeNumber(value, 50, 1000);
    else if (flag == "--scope-adaptive-edge-max-speed") (void)scopeNumber(value, .01, 2.0);
    else if (flag == "--scope-low-gain" || flag == "--scope-high-gain") {
        if (scopeNumber(value, 0, 1) == 0) throw std::runtime_error("Scope gain must be positive.");
    } else (void)scopeNumber(value, 0, 250);
}
} // namespace bone_eater::input
