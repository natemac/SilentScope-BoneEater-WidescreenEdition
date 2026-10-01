#include "input/selected_hid_config.h"

#include "external/rapidjson/document.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>

namespace bone_eater::input {
namespace {
using Value = rapidjson::Value;
using Error = SelectedHidConfigError;

std::string_view string(const Value& value) {
    return {value.GetString(), value.GetStringLength()};
}

bool fail(SelectedHidConfigResult& result, Error error, const char* field) {
    result.error = error;
    result.field = field;
    return false;
}

bool object(const Value& value, std::initializer_list<std::string_view> names,
        const char* field, SelectedHidConfigResult& result) {
    if (!value.IsObject()) return fail(result, Error::WrongType, field);
    for (auto member = value.MemberBegin(); member != value.MemberEnd(); ++member) {
        const auto name = string(member->name);
        if (std::find(names.begin(), names.end(), name) == names.end())
            return fail(result, Error::UnknownField, field);
        for (auto previous = value.MemberBegin(); previous != member; ++previous) {
            if (string(previous->name) == name) return fail(result, Error::DuplicateField, field);
        }
    }
    return true;
}

const Value* required(const Value& value, const char* name, const char* field,
        SelectedHidConfigResult& result) {
    const auto member = value.FindMember(name);
    if (member == value.MemberEnd()) {
        fail(result, Error::MissingField, field);
        return nullptr;
    }
    return &member->value;
}

bool hexIdentifier(const Value& value, std::uint16_t& out) {
    if (!value.IsString() || value.GetStringLength() != 4) return false;
    unsigned parsed = 0;
    for (char ch : string(value)) {
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<unsigned>(ch - 'a') + 10;
        else if (ch >= 'A' && ch <= 'F') digit = static_cast<unsigned>(ch - 'A') + 10;
        else return false;
        parsed = parsed * 16 + digit;
    }
    out = static_cast<std::uint16_t>(parsed);
    return true;
}

bool interfaceIdentity(const Value& value) {
    if (!value.IsString()) return false;
    const auto path = string(value);
    if (path.empty() || path.size() > selectedHidPathMaximumBytes) return false;
    // Encoding has already been validated by RapidJSON. Reject C0/C1 controls
    // and DEL; preserve all remaining identity bytes exactly without trimming.
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto ch = static_cast<unsigned char>(path[i]);
        if (ch < 0x20 || ch == 0x7F) return false;
        if (ch == 0xC2 && i + 1 < path.size()) {
            const auto next = static_cast<unsigned char>(path[i + 1]);
            if (next >= 0x80 && next <= 0x9F) return false;
        }
    }
    return true;
}

bool usage(const Value& value, bool nullable, std::optional<unsigned>& out) {
    if (nullable && value.IsNull()) { out.reset(); return true; }
    if (!value.IsUint() || value.GetUint() < 1 || value.GetUint() > 16) return false;
    out = value.GetUint();
    return true;
}

const char* explanation(Error error) noexcept {
    switch (error) {
        case Error::None: return "valid profile";
        case Error::Empty: return "supplied profile is empty";
        case Error::TooLarge: return "profile exceeds the 16KiB limit";
        case Error::InvalidJson: return "invalid JSON or UTF-8";
        case Error::InvalidStructure: return "profile must be a JSON object";
        case Error::MissingField: return "required field is missing";
        case Error::DuplicateField: return "duplicate object member";
        case Error::UnknownField: return "unknown or inapplicable object member";
        case Error::WrongType: return "incorrect JSON value type";
        case Error::UnsupportedVersion: return "schema_version must be integer 1";
        case Error::UnsupportedMode: return "mode must be legacy or selected_hid";
        case Error::UnsupportedDevice: return "requires four-digit hex VID1209 and PID0001 or PID0002";
        case Error::UnsupportedContract: return "contract must be xgunner_report3_v1";
        case Error::UnsupportedCoordinateSpace: return "only calibrated_game_content coordinates are supported";
        case Error::InvalidPath: return "requires a nonempty exact interface identity, at most 4096 UTF-8 bytes without controls";
        case Error::InvalidMapping: return "requires distinct trigger/scope usages 1..16; only scope_left_usage may be null";
        case Error::InvalidReportAge: return "requires an explicit positive integer millisecond limit without overflow";
    }
    return "invalid input profile";
}
} // namespace

std::string SelectedHidConfigResult::message() const {
    const std::string description = explanation(error);
    return field.empty() ? description : field + ": " + description;
}

SelectedHidConfigResult parseSelectedHidConfig(std::string_view json) {
    SelectedHidConfigResult result;
    if (json.empty()) { result.error = Error::Empty; return result; }
    if (json.size() > selectedHidConfigMaximumBytes) { result.error = Error::TooLarge; return result; }
    const auto nul = json.find('\0');
    if (nul != std::string_view::npos) {
        result.error = Error::InvalidJson; result.offset = nul; return result;
    }
    rapidjson::Document root;
    // Iterative parsing bounds call-stack use even for deeply nested malformed
    // profiles inside the byte limit. Do not permit comments, trailing JSON,
    // NaN/Infinity, approximate integer conversions or invalid UTF-8.
    root.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
    if (root.HasParseError()) {
        result.error = Error::InvalidJson; result.offset = root.GetErrorOffset(); return result;
    }
    if (!root.IsObject()) { result.error = Error::InvalidStructure; return result; }
    if (!object(root, {"schema_version", "mode", "device", "coordinate_space", "buttons", "max_report_age_ms"},
            "root", result)) return result;
    const auto* version = required(root, "schema_version", "schema_version", result);
    if (!version) return result;
    if (!version->IsUint() || version->GetUint() != 1) {
        fail(result, Error::UnsupportedVersion, "schema_version"); return result;
    }
    const auto* mode = required(root, "mode", "mode", result);
    if (!mode) return result;
    if (!mode->IsString()) { fail(result, Error::WrongType, "mode"); return result; }
    SelectedHidConfiguration configuration;
    if (string(*mode) == "legacy") {
        if (!object(root, {"schema_version", "mode"}, "root", result)) return result;
        result.config = std::move(configuration);
        return result;
    }
    if (string(*mode) != "selected_hid") { fail(result, Error::UnsupportedMode, "mode"); return result; }

    SelectedHidProfile profile;
    const auto* device = required(root, "device", "device", result);
    if (!device || !object(*device, {"vid", "pid", "interface_path", "contract"}, "device", result)) return result;
    const auto* vid = required(*device, "vid", "device.vid", result);
    if (!vid) return result;
    const auto* pid = required(*device, "pid", "device.pid", result);
    if (!pid) return result;
    if (!hexIdentifier(*vid, profile.vendor) || profile.vendor != 0x1209) {
        fail(result, Error::UnsupportedDevice, "device.vid"); return result;
    }
    if (!hexIdentifier(*pid, profile.product) || (profile.product != 1 && profile.product != 2)) {
        fail(result, Error::UnsupportedDevice, "device.pid"); return result;
    }
    const auto* path = required(*device, "interface_path", "device.interface_path", result);
    if (!path) return result;
    if (!interfaceIdentity(*path)) { fail(result, Error::InvalidPath, "device.interface_path"); return result; }
    profile.interfacePath.assign(path->GetString(), path->GetStringLength());
    const auto* contract = required(*device, "contract", "device.contract", result);
    if (!contract) return result;
    if (!contract->IsString() || string(*contract) != "xgunner_report3_v1") {
        fail(result, Error::UnsupportedContract, "device.contract"); return result;
    }
    const auto* space = required(root, "coordinate_space", "coordinate_space", result);
    if (!space) return result;
    if (!space->IsString() || string(*space) != "calibrated_game_content") {
        fail(result, Error::UnsupportedCoordinateSpace, "coordinate_space"); return result;
    }
    const auto* buttons = required(root, "buttons", "buttons", result);
    if (!buttons || !object(*buttons, {"trigger_usage", "scope_right_usage", "scope_left_usage"}, "buttons", result)) return result;
    const auto* trigger = required(*buttons, "trigger_usage", "buttons.trigger_usage", result);
    if (!trigger) return result;
    const auto* right = required(*buttons, "scope_right_usage", "buttons.scope_right_usage", result);
    if (!right) return result;
    const auto* left = required(*buttons, "scope_left_usage", "buttons.scope_left_usage", result);
    if (!left) return result;
    if (!usage(*trigger, false, profile.buttons.trigger) || !usage(*right, false, profile.buttons.scopeRight) ||
            !usage(*left, true, profile.buttons.scopeLeft) || !validSelectedGunButtonMapping(profile.buttons)) {
        fail(result, Error::InvalidMapping, "buttons"); return result;
    }
    const auto* age = required(root, "max_report_age_ms", "max_report_age_ms", result);
    if (!age) return result;
    using Rep = std::chrono::milliseconds::rep;
    if (!age->IsUint64() || !age->GetUint64() ||
            age->GetUint64() > static_cast<std::uint64_t>(selectedHidMaximumReportAge.count())) {
        fail(result, Error::InvalidReportAge, "max_report_age_ms"); return result;
    }
    profile.maxReportAge = std::chrono::milliseconds(static_cast<Rep>(age->GetUint64()));
    configuration.mode = GunSourceMode::SelectedHid;
    configuration.selected = std::move(profile);
    result.config = std::move(configuration);
    return result;
}

} // namespace bone_eater::input
