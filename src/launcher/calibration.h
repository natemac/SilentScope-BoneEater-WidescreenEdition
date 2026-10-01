#pragma once
#include <filesystem>
#include <string>
#include <utility>
namespace bone_eater::launcher {
std::string calibrationCrc(const std::string& bytes);
std::pair<std::string, std::string> fullRangeCalibration(const std::string& xml, const std::string& crc);
void prepareBetaCalibration(const std::filesystem::path& game);
}
