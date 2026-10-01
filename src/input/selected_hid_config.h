#pragma once

#include "input/source_policy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace bone_eater::input {

constexpr std::size_t selectedHidConfigMaximumBytes = 16 * 1024;
constexpr std::size_t selectedHidPathMaximumBytes = 4096;
// This is a representation ceiling, not a recommended timeout. Keeping the
// common clock/millisecond duration representable prevents comparison overflow.
using SelectedHidAgeComparison = std::common_type_t<GunSourceClock::duration, std::chrono::milliseconds>;
constexpr auto selectedHidMaximumReportAge = std::chrono::duration_cast<std::chrono::milliseconds>(
    (SelectedHidAgeComparison::max)());

enum class SelectedHidCoordinateSpace { CalibratedGameContent };
enum class SelectedHidConfigContract { XgunnerReport3V1 };

struct SelectedHidProfile {
    std::uint16_t vendor = 0, product = 0;
    std::string interfacePath; // Exact UTF-8 identity, never an automatic fallback.
    SelectedHidConfigContract contract = SelectedHidConfigContract::XgunnerReport3V1;
    SelectedHidCoordinateSpace coordinateSpace = SelectedHidCoordinateSpace::CalibratedGameContent;
    SelectedGunButtonMapping buttons;
    std::chrono::milliseconds maxReportAge {0}; // No default hardware timeout.
};

struct SelectedHidConfiguration {
    GunSourceMode mode = GunSourceMode::Legacy;
    std::optional<SelectedHidProfile> selected;
};

enum class SelectedHidConfigError {
    None, Empty, TooLarge, InvalidJson, InvalidStructure, MissingField,
    DuplicateField, UnknownField, WrongType, UnsupportedVersion, UnsupportedMode,
    UnsupportedDevice, UnsupportedContract, UnsupportedCoordinateSpace,
    InvalidPath, InvalidMapping, InvalidReportAge,
};

struct SelectedHidConfigResult {
    // Failure has no config at all; callers must not reinterpret an error as
    // a legacy fallback. Only absence of a requested profile chooses defaults.
    std::optional<SelectedHidConfiguration> config;
    SelectedHidConfigError error = SelectedHidConfigError::None;
    std::string field; // Controlled schema field path, never untrusted JSON text.
    std::size_t offset = 0; // Byte position only for InvalidJson.
    explicit operator bool() const noexcept { return config.has_value(); }
    std::string message() const;
};

// Strict schema1 parser only; no file/device I/O, environment writes, selection,
// calibration, freshness decisions or runtime activation. Empty/malformed supplied
// data is an error. Exact current-content coordinates are the only supported
// space. The descriptor and explicit timeout still need runtime/hardware checks.
// Allocation failures may propagate to the launcher's ordinary exception handler.
SelectedHidConfigResult parseSelectedHidConfig(std::string_view json);

} // namespace bone_eater::input
