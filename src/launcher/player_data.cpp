#include "player_data.h"
#include "launcher.h"
#include <fstream>
#include <regex>
#include <stdexcept>
namespace bone_eater::launcher {
namespace {
std::string read(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Settings file exceeds limit.");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read supplied settings.");
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit); out << value; out.close();
}
void ordinary(const std::filesystem::path& path) {
    for (auto p = path; !p.empty(); p = p.parent_path()) {
        const auto a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Redirected user-data path refused.");
        if (p == p.parent_path()) break;
    }
}
void copyTree(const std::filesystem::path& from, const std::filesystem::path& to) {
    ordinary(from);
    if (!std::filesystem::exists(from)) return;
    if (!std::filesystem::is_directory(from)) throw std::runtime_error("Expected conf directory.");
    std::uintmax_t total = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(from)) {
        ordinary(e.path());
        const auto target = to / e.path().lexically_relative(from);
        if (e.is_directory()) std::filesystem::create_directories(target);
        else if (e.is_regular_file()) {
            total += e.file_size();
            if (total > 1024ull * 1024 * 1024) throw std::runtime_error("Supplied conf exceeds 1 GiB; inspect before migration.");
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy_file(e.path(), target);
        } else throw std::runtime_error("Unsupported file in supplied conf.");
    }
}
}
std::filesystem::path playerDataDirectory(const std::filesystem::path& game) { return game.parent_path().parent_path() / L"user"; }
std::string redirectedAvsConfig(const std::string& source) {
    std::string result = source;
    for (const auto* name : {"nvram", "raw"}) {
        const std::regex re(std::string("(<") + name + ">[\\s\\S]*?<device[^>]*>)([^<]*)(</device>[\\s\\S]*?</" + name + ">)");
        std::sregex_iterator it(result.begin(), result.end(), re), end;
        if (it == end) throw std::runtime_error("AVS configuration lacks a supported nvram/raw mount.");
        const auto match = *it;
        if (++it != end) throw std::runtime_error("Duplicate AVS nvram/raw mount.");
        result.replace(match.position(2), match.length(2), std::string("../user/conf/") + name);
    }
    return result;
}
void preparePlayerData(const std::filesystem::path& game) {
    const auto user = playerDataDirectory(game);
    const auto staging = user.parent_path() / L"user.setup";
    const auto source = game.parent_path() / L"prop/avs-config.xml";
    ordinary(user); ordinary(staging); ordinary(source);
    if (!matchingProcesses(game).empty()) throw std::runtime_error("Close the game before preparing user data.");
    const auto config = redirectedAvsConfig(read(source));
    if (!std::filesystem::exists(user)) {
        if (std::filesystem::exists(staging)) throw std::runtime_error("Incomplete user.setup migration; inspect it before retrying. Supplied game files are unchanged.");
        std::filesystem::create_directories(staging / L"conf/nvram");
        std::filesystem::create_directories(staging / L"conf/raw");
        copyTree(game.parent_path() / L"conf", staging / L"conf");
        for (const auto* file : {L"bone-eater-controls.xml", L"resize.json", L"patches.json"}) {
            const auto old = game.parent_path() / L"desktop" / file;
            ordinary(old);
            if (std::filesystem::exists(old)) std::filesystem::copy_file(old, staging / file);
        }
        write(staging / L"avs-config.xml", config);
        write(staging / L"layout-v1.txt", "Widescreen Edition private nvram/raw mounts v1.\n");
        std::filesystem::rename(staging, user);
    } else {
        ordinary(user / L"layout-v1.txt"); ordinary(user / L"avs-config.xml");
        if (!std::filesystem::exists(user / L"layout-v1.txt") ||
            read(user / L"layout-v1.txt") != "Widescreen Edition private nvram/raw mounts v1.\n")
            throw std::runtime_error("Unrecognized user folder; existing files preserved.");
        if (read(user / L"avs-config.xml") != config)
            throw std::runtime_error("Supplied AVS configuration or user/avs-config.xml changed. Inspect the mount configuration before launching.");
    }
    const auto bookkeeping = user / L"conf/raw/bookkeeping";
    ordinary(bookkeeping);
    std::filesystem::create_directories(bookkeeping);
}
}
