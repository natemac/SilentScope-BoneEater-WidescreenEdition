#include "input/selected_hid_config.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bone_eater::input;
using Error = SelectedHidConfigError;

namespace {
void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line));
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

// Synthetic schema fixtures only. These button assignments and age are not a
// physical XGUNNER profile, and no test opens or enumerates any device.
const std::string selected = R"json({
"schema_version":1,"mode":"selected_hid",
"device":{"vid":"1209","pid":"0001","interface_path":"\\\\?\\hid#fixture","contract":"xgunner_report3_v1"},
"coordinate_space":"calibrated_game_content",
"buttons":{"trigger_usage":4,"scope_right_usage":16,"scope_left_usage":null},
"max_report_age_ms":1234
})json";
const std::string legacy = R"({"schema_version":1,"mode":"legacy"})";

std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto found = text.find(from);
    CHECK(found != std::string::npos);
    text.replace(found, from.size(), to);
    return text;
}
void rejected(const std::string& json, Error error) {
    const auto result = parseSelectedHidConfig(json);
    CHECK(!result);
    CHECK(!result.config);
    CHECK(result.error == error);
    CHECK(!result.message().empty());
}
std::string withAge(const std::string& text) { return replace(selected, "\"max_report_age_ms\":1234", "\"max_report_age_ms\":" + text); }
std::string withPath(const std::string& escaped) {
    return replace(selected, R"("interface_path":"\\\\?\\hid#fixture")", "\"interface_path\":\"" + escaped + "\"");
}

void explicitLegacyAndNoImplicitFallback() {
    const auto result = parseSelectedHidConfig(legacy);
    CHECK(result && result.error == Error::None);
    CHECK(result.config->mode == GunSourceMode::Legacy);
    CHECK(!result.config->selected);
    CHECK(SelectedHidConfiguration {}.mode == GunSourceMode::Legacy);
    rejected("", Error::Empty);
    rejected(" ", Error::InvalidJson);
    rejected("{}", Error::MissingField);
    rejected("null", Error::InvalidStructure);
    rejected("[]", Error::InvalidStructure);
    rejected("1", Error::InvalidStructure);
    rejected("true", Error::InvalidStructure);
}

void completeProfilePreservesExplicitValues() {
    const auto result = parseSelectedHidConfig(selected);
    CHECK(result && result.config->mode == GunSourceMode::SelectedHid);
    CHECK(result.config->selected);
    const auto& value = *result.config->selected;
    CHECK(value.vendor == 0x1209 && value.product == 1);
    CHECK(value.interfacePath == R"(\\?\hid#fixture)");
    CHECK(value.contract == SelectedHidConfigContract::XgunnerReport3V1);
    CHECK(value.coordinateSpace == SelectedHidCoordinateSpace::CalibratedGameContent);
    CHECK(value.buttons.trigger == 4u && value.buttons.scopeRight == 16u && !value.buttons.scopeLeft);
    CHECK(value.maxReportAge == std::chrono::milliseconds(1234));
    const auto second = parseSelectedHidConfig(replace(replace(selected, "\"pid\":\"0001\"", "\"pid\":\"0002\""),
        "\"scope_left_usage\":null", "\"scope_left_usage\":8"));
    CHECK(second && second.config->selected->product == 2);
    CHECK(second.config->selected->buttons.scopeLeft == 8u);
}

void modesAndSchemaDoNotCoerce() {
    for (const auto* value : {"\"auto\"", "\"hid\"", "\"selected-hid\"", "\"Legacy\"", "\"\""}) {
        rejected(replace(legacy, "\"mode\":\"legacy\"", std::string("\"mode\":") + value), Error::UnsupportedMode);
    }
    for (const auto* value : {"null", "true", "0", "[]", "{}"})
        rejected(replace(legacy, "\"mode\":\"legacy\"", std::string("\"mode\":") + value), Error::WrongType);
    for (const auto* value : {"0", "2", "-1", "1.0", "1e0", "true", "\"1\"", "null", "4294967296"})
        rejected(replace(legacy, "\"schema_version\":1", std::string("\"schema_version\":") + value), Error::UnsupportedVersion);
}

void requiredFieldsCannotBeOmitted() {
    const std::vector<std::pair<std::string, std::string>> omissions {
        {"\"schema_version\":1,", ""},
        {"\"mode\":\"selected_hid\",", ""},
        {R"("device":{"vid":"1209","pid":"0001","interface_path":"\\\\?\\hid#fixture","contract":"xgunner_report3_v1"},)", ""},
        {"\"vid\":\"1209\",", ""},
        {"\"pid\":\"0001\",", ""},
        {R"("interface_path":"\\\\?\\hid#fixture",)", ""},
        {",\"contract\":\"xgunner_report3_v1\"", ""},
        {"\"coordinate_space\":\"calibrated_game_content\",", ""},
        {R"("buttons":{"trigger_usage":4,"scope_right_usage":16,"scope_left_usage":null},)", ""},
        {"\"trigger_usage\":4,", ""},
        {"\"scope_right_usage\":16,", ""},
        {",\"scope_left_usage\":null", ""},
        {",\n\"max_report_age_ms\":1234", ""},
    };
    for (const auto& omission : omissions) rejected(replace(selected, omission.first, omission.second), Error::MissingField);
}

void duplicateKeysAreRejectedAfterJsonDecoding() {
    rejected(replace(selected, "\"schema_version\":1,", "\"schema_version\":1,\"schema_version\":1,"), Error::DuplicateField);
    rejected(replace(selected, "\"mode\":\"selected_hid\",", "\"mode\":\"selected_hid\",\"mo\\u0064e\":\"legacy\","), Error::DuplicateField);
    rejected(replace(selected, "\"vid\":\"1209\",", "\"vid\":\"1209\",\"vid\":\"1209\","), Error::DuplicateField);
    rejected(replace(selected, "\"trigger_usage\":4,", "\"trigger_usage\":4,\"trigger_usage\":5,"), Error::DuplicateField);
}

void unknownFieldsAndSelectedFieldsInLegacyAreRejected() {
    rejected(replace(selected, "\"schema_version\":1,", "\"schema_version\":1,\"fallback\":\"mouse\","), Error::UnknownField);
    rejected(replace(selected, "\"vid\":\"1209\",", "\"vid\":\"1209\",\"name\":\"anything\","), Error::UnknownField);
    rejected(replace(selected, "\"trigger_usage\":4,", "\"trigger_usage\":4,\"reload_usage\":3,"), Error::UnknownField);
    rejected(replace(legacy, "\"mode\":\"legacy\"", "\"mode\":\"legacy\",\"device\":null"), Error::UnknownField);
    rejected(replace(selected, "\"schema_version\":1,", "\"schema_version\":1,\"\\u001b[31m\":1,"), Error::UnknownField);
    const auto hostile = parseSelectedHidConfig(replace(selected, "\"schema_version\":1,", "\"schema_version\":1,\"\\u001b[31m\":1,"));
    CHECK(hostile.message().find('\x1B') == std::string::npos); // Never echo control-bearing keys.
}

void objectTypesAreStrict() {
    rejected(replace(selected, R"("device":{"vid":"1209","pid":"0001","interface_path":"\\\\?\\hid#fixture","contract":"xgunner_report3_v1"})",
        "\"device\":[]"), Error::WrongType);
    rejected(replace(selected, R"("buttons":{"trigger_usage":4,"scope_right_usage":16,"scope_left_usage":null})",
        "\"buttons\":null"), Error::WrongType);
}

void deviceAndDescriptorStayBounded() {
    for (const auto* value : {"\"046D\"", "\"0x1209\"", "\"120\"", "\"120G\"", "1209", "null", "true"})
        rejected(replace(selected, "\"vid\":\"1209\"", std::string("\"vid\":") + value), Error::UnsupportedDevice);
    for (const auto* value : {"\"0000\"", "\"0003\"", "\"1\"", "\" 001\"", "1", "null"})
        rejected(replace(selected, "\"pid\":\"0001\"", std::string("\"pid\":") + value), Error::UnsupportedDevice);
    for (const auto* value : {"\"any\"", "\"xgunner_report3_v2\"", "null", "3"})
        rejected(replace(selected, "\"contract\":\"xgunner_report3_v1\"", std::string("\"contract\":") + value), Error::UnsupportedContract);
}

void unsupportedCoordinateSpacesCannotPass() {
    for (const auto* value : {"\"monitor\"", "\"desktop\"", "\"auto\"", "null", "0", "{}"})
        rejected(replace(selected, "\"coordinate_space\":\"calibrated_game_content\"", std::string("\"coordinate_space\":") + value), Error::UnsupportedCoordinateSpace);
}

void buttonValuesAreExplicitDistinctUsages() {
    for (const auto* value : {"0", "17", "-1", "4.0", "4e0", "\"4\"", "true", "null", "4294967296"})
        rejected(replace(selected, "\"trigger_usage\":4", std::string("\"trigger_usage\":") + value), Error::InvalidMapping);
    rejected(replace(selected, "\"scope_right_usage\":16", "\"scope_right_usage\":null"), Error::InvalidMapping);
    rejected(replace(selected, "\"scope_right_usage\":16", "\"scope_right_usage\":4"), Error::InvalidMapping);
    for (const auto* value : {"0", "17", "4", "16", "false", "\"null\""})
        rejected(replace(selected, "\"scope_left_usage\":null", std::string("\"scope_left_usage\":") + value), Error::InvalidMapping);
    CHECK(parseSelectedHidConfig(replace(selected, "\"trigger_usage\":4", "\"trigger_usage\":1")));
}

void ageLimitsCannotCoerceOrOverflowComparison() {
    for (const auto* value : {"0", "-1", "1234.0", "1e3", "\"1234\"", "true", "null", "[]", "18446744073709551615", "18446744073709551616"})
        rejected(withAge(value), Error::InvalidReportAge);
    CHECK(parseSelectedHidConfig(withAge("1")));
    const auto ceiling = static_cast<std::uint64_t>(selectedHidMaximumReportAge.count());
    const auto maximum = parseSelectedHidConfig(withAge(std::to_string(ceiling)));
    CHECK(maximum && maximum.config->selected->maxReportAge == selectedHidMaximumReportAge);
    rejected(withAge(std::to_string(ceiling + 1)), Error::InvalidReportAge);
    // The exact upper bound converts to the reader's common comparison duration
    // without wrapping negative. This is not a proposed usable hardware timeout.
    const SelectedHidAgeComparison converted = maximum.config->selected->maxReportAge;
    CHECK(converted.count() > 0);
}

void interfacePathPreservesUtf8ButRejectsControlsAndNulls() {
    for (const auto* escaped : {"", "bad\\npath", "bad\\u0000path", "bad\\u007fpath", "bad\\u0085path"})
        rejected(withPath(escaped), Error::InvalidPath);
    const std::string path = std::string("fixture-") + "\xCE\xB2";
    const auto utf8 = parseSelectedHidConfig(withPath(path));
    CHECK(utf8 && utf8.config->selected->interfacePath == path);
    CHECK(parseSelectedHidConfig(withPath(" identity with spaces ")));
    CHECK(parseSelectedHidConfig(withPath(std::string(selectedHidPathMaximumBytes, 'x'))));
    rejected(withPath(std::string(selectedHidPathMaximumBytes + 1, 'x')), Error::InvalidPath);
    std::string multibyte;
    for (unsigned i = 0; i < 2049; ++i) multibyte += "\xCE\xB2";
    rejected(withPath(multibyte), Error::InvalidPath); // Byte limit, not codepoint count.
    rejected(withPath(std::string("bad-") + '\xFF'), Error::InvalidJson);
    rejected(withPath(std::string("bad-") + '\xC0' + '\xAF'), Error::InvalidJson);
    rejected(withPath("bad-\\ud800"), Error::InvalidJson);
}

void invalidJsonAndTrailingPayloadCannotPass() {
    for (const auto& json : {legacy + legacy, legacy + "garbage", "//comment\n" + legacy,
            replace(legacy, "\"mode\":\"legacy\"", "\"mode\":\"legacy\","),
            replace(selected, "1234", "NaN"), replace(selected, "1234", "Infinity")})
        rejected(json, Error::InvalidJson);
    std::string nul = legacy;
    nul.push_back('\0');
    nul += selected;
    const auto result = parseSelectedHidConfig(nul);
    CHECK(!result && result.error == Error::InvalidJson && result.offset == legacy.size());
}

void profileByteBoundAndDeepSyntaxAreSafe() {
    std::string maximum = legacy;
    maximum.append(selectedHidConfigMaximumBytes - maximum.size(), ' ');
    CHECK(parseSelectedHidConfig(maximum));
    maximum.push_back(' ');
    rejected(maximum, Error::TooLarge);
    const std::string deeplyNested = std::string(3000, '[') + "0" + std::string(3000, ']');
    rejected(deeplyNested, Error::InvalidStructure);
    rejected(std::string(3000, '['), Error::InvalidJson);
}

void resultsOwnDataAndFailuresNeverExposePartialProfiles() {
    auto bytes = selected;
    const auto first = parseSelectedHidConfig(bytes);
    CHECK(first);
    bytes.assign(bytes.size(), 'x');
    CHECK(first.config->selected->interfacePath == R"(\\?\hid#fixture)");
    const auto failed = parseSelectedHidConfig(withAge("0")); // Last parsed field fails.
    CHECK(!failed && !failed.config && failed.field == "max_report_age_ms");
    CHECK(first.config->selected->maxReportAge == std::chrono::milliseconds(1234));
    const auto later = parseSelectedHidConfig(legacy);
    CHECK(later && !later.config->selected && later.field.empty() && later.error == Error::None);
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] {
        {"explicit legacy and no fallback", explicitLegacyAndNoImplicitFallback},
        {"complete profiles", completeProfilePreservesExplicitValues},
        {"mode and schema types", modesAndSchemaDoNotCoerce},
        {"all required fields", requiredFieldsCannotBeOmitted},
        {"duplicate decoded keys", duplicateKeysAreRejectedAfterJsonDecoding},
        {"unknown and inapplicable fields", unknownFieldsAndSelectedFieldsInLegacyAreRejected},
        {"object types", objectTypesAreStrict},
        {"device and descriptor contract", deviceAndDescriptorStayBounded},
        {"coordinate contract", unsupportedCoordinateSpacesCannotPass},
        {"button mapping", buttonValuesAreExplicitDistinctUsages},
        {"report age representation", ageLimitsCannotCoerceOrOverflowComparison},
        {"interface identity and UTF-8", interfacePathPreservesUtf8ButRejectsControlsAndNulls},
        {"JSON syntax and trailing payload", invalidJsonAndTrailingPayloadCannotPass},
        {"bounded iterative parsing", profileByteBoundAndDeepSyntaxAreSafe},
        {"owned atomic result", resultsOwnDataAndFailuresNeverExposePartialProfiles},
    };
    for (const auto& test : tests) {
        try { test.run(); }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "Passed " << (sizeof(tests) / sizeof(tests[0])) << " selected-HID config cases\n";
    return 0;
}
