#pragma once
#include <filesystem>
#include <string>
namespace bone_eater::launcher {
std::filesystem::path playerDataDirectory(const std::filesystem::path& game);
std::string redirectedAvsConfig(const std::string& source);
void preparePlayerData(const std::filesystem::path& game);
}
