#pragma once

#include "input/selected_hid_config.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace bone_eater::standalone {

// A requested file is mandatory and bounded; malformed supplied data never
// becomes the default mouse mode. Paths are native filesystem strings (UTF-16
// on Windows), and relative paths are anchored to the executable directory.
inline input::SelectedHidConfiguration loadInputProfile(
        const std::filesystem::path& executableRoot, const std::filesystem::path& requested) {
    if (requested.empty()) throw std::runtime_error("--input-config requires a nonempty path.");
    if (!requested.is_absolute() && requested.has_root_path())
        throw std::runtime_error("Use an absolute input profile path or a path relative to the game directory.");
    const auto path = requested.is_absolute() ? requested : executableRoot / requested;
    if (!std::filesystem::is_regular_file(path))
        throw std::runtime_error("The requested input profile is not a regular file.");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open the requested input profile.");
    std::array<char, input::selectedHidConfigMaximumBytes + 1> bytes {};
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = stream.gcount();
    if (stream.bad() || (stream.fail() && !stream.eof()))
        throw std::runtime_error("Cannot read the requested input profile.");
    const auto result = input::parseSelectedHidConfig(
        std::string_view(bytes.data(), static_cast<std::size_t>(count)));
    if (!result) throw std::runtime_error("Invalid input profile: " + result.message());
    return *result.config;
}

} // namespace bone_eater::standalone
