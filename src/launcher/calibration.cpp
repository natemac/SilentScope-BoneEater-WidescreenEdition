#include "calibration.h"
#include "launcher.h"
#include "player_data.h"
#include <fstream>
#include <regex>
#include <cstdint>
#include <stdexcept>

namespace bone_eater::launcher {
std::string calibrationCrc(const std::string& bytes) {
    uint32_t crc = 0xffffffff;
    for (unsigned char c : bytes) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    crc ^= 0xffffffff;
    std::string result;
    for (int i = 0; i < 4; ++i) result += static_cast<char>((crc >> (8 * i)) & 255);
    return result;
}
namespace {
std::smatch unique(const std::string& text, const std::string& pattern) {
    const std::regex expression(pattern);
    std::sregex_iterator it(text.begin(), text.end(), expression), end;
    if (it == end) throw std::runtime_error("Unrecognized gun calibration schema; no reset performed.");
    const auto result = *it;
    if (++it != end) throw std::runtime_error("Duplicate gun calibration fields; no reset performed.");
    return result;
}
std::string read(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 1024 * 1024) throw std::runtime_error("Calibration file exceeds size limit.");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read beta calibration.");
    return {std::istreambuf_iterator<char>(file), {}};
}
void writeNew(const std::filesystem::path& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create calibration backup/staging file.");
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!okay) throw std::runtime_error("Cannot save calibration backup/staging file.");
}
void ordinaryPath(const std::filesystem::path& path) {
    for (auto p = path; !p.empty(); p = p.parent_path()) {
        const DWORD attributes = GetFileAttributesW(p.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Redirected calibration path refused.");
        if (p == p.parent_path()) break;
    }
}
void ensureCabinetMarker(const std::filesystem::path& path) {
    // ARK opens this marker to select the cabinet coordinate contract. Its
    // contents are not consumed; preserve an existing file, including empty.
    try {
        ordinaryPath(path);
        std::filesystem::create_directories(path.parent_path());
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error(windowsError("Open/create dx"));
        CloseHandle(file);
        if (!std::filesystem::is_regular_file(path))
            throw std::runtime_error("The path is not a regular file.");
    } catch (const std::exception& error) {
        throw std::runtime_error("Cannot prepare required cabinet marker: " + narrow(path.wstring()) +
            "\nAn empty file named dx (no extension) must be readable here for correct widescreen aiming. "
            "Check folder permissions and ensure dx is a file, not a folder. No game launched.\n" + error.what());
    }
}
}
std::pair<std::string, std::string> fullRangeCalibration(const std::string& xml, const std::string& crc) {
    if (calibrationCrc(xml) != crc) throw std::runtime_error("Calibration XML/CRC mismatch; preserve and inspect conf/nvram/testmode-v files.");
    const auto section = unique(xml, "<guncontrolleCheck>([\\s\\S]*?)</guncontrolleCheck>");
    std::string body = section[1];
    const auto calibrated = unique(body, "<calibrated>[\\s\\S]*?<current[^>]*>\\s*[01]\\s*</current>[\\s\\S]*?</calibrated>");
    const std::string calibratedText = calibrated[0];
    const auto flag = unique(calibratedText, "<current[^>]*>\\s*([01])\\s*</current>");
    body.replace(calibrated.position() + flag.position(1), flag.length(1), "1");
    for (const auto* side : {"left", "right"}) for (const auto* field : {"Min", "Max", "Nut"}) for (const auto* axis : {"X", "Y"}) {
        const std::string name = std::string(side) + "Calibration" + field + axis;
        const std::string value = std::string(field) == "Min" ? "0" : std::string(field) == "Max" ? "4095" : "2047";
        const auto element = unique(body, "<" + name + ">([\\s\\S]*?)</" + name + ">");
        const std::string contents = element[1];
        unique(contents, "<factory[^>]*>\\s*" + value + "\\s*</factory>");
        const auto current = unique(contents, "<current[^>]*>\\s*([0-9]+)\\s*</current>");
        if (std::stoul(current[1]) > 4095) throw std::runtime_error("Unknown gun calibration range.");
        body.replace(static_cast<size_t>(element.position(1) + current.position(1)), current.length(1), value);
    }
    std::string updated = xml;
    updated.replace(section.position(1), section.length(1), body);
    return {updated, calibrationCrc(updated)};
}
void prepareBetaCalibration(const std::filesystem::path& game, bool repair) {
    const auto dir = game.parent_path();
    const auto user = playerDataDirectory(game);
    const auto backup = user / L"calibration-initialization-r4";
    const auto marker = backup / L"complete.txt";
    const auto xmlPath = user / L"conf/nvram/testmode-v.xml";
    const auto crcPath = user / L"conf/nvram/testmode-v.crc";
    const auto templatePath = dir / L"prop/testmode-v.xml";
    const auto dx = user / L"conf/raw/dx";
    for (const auto& p : {backup, marker, xmlPath, crcPath, templatePath, dx}) ordinaryPath(p);
    if (!matchingProcesses(game).empty()) throw std::runtime_error("Close the game before preparing calibration.");
    if (std::filesystem::exists(dx) && !std::filesystem::is_regular_file(dx))
        throw std::runtime_error("Required cabinet marker conf/raw/dx is not a regular file. An empty file named dx (no extension) is required; no game launched.");
    if (std::filesystem::exists(backup)) {
        for (const auto& entry : std::filesystem::directory_iterator(backup)) {
            if (entry.is_directory() && !std::filesystem::exists(entry.path() / "complete.txt"))
                throw std::runtime_error("Interrupted calibration transaction in user/calibration-initialization-r4; inspect saved files before retrying.");
        }
    }
    const bool hasXml = std::filesystem::exists(xmlPath), hasCrc = std::filesystem::exists(crcPath);
    if (hasXml != hasCrc) throw std::runtime_error("Incomplete calibration XML/CRC pair; preserve and inspect conf/nvram/testmode-v files.");
    const auto xml = read(hasXml ? xmlPath : templatePath);
    if (!hasXml) {
        unique(xml, "<testModeValue>[\\s\\S]*</testModeValue>");
        unique(xml, "<version[^>]*>\\s*9\\s*</version>");
    }
    const auto crc = hasXml ? read(crcPath) : calibrationCrc(xml);
    auto updated = fullRangeCalibration(xml, crc);
    if (!hasXml) {
        // The supplied cabinet template has never acknowledged the clock.
        // A fresh PC install uses host time (zero offset); existing NVRAM is
        // deliberately excluded so its clock preference stays untouched.
        const auto clock = unique(updated.first, "<clock>([\\s\\S]*?)</clock>");
        const std::string clockBody = clock[1];
        const auto offset = unique(clockBody, "<clock_offset>([\\s\\S]*?)</clock_offset>");
        const std::string offsetBody = offset[1];
        unique(offsetBody, "<current[^>]*>\\s*0\\s*</current>");
        const auto flag = unique(offsetBody, "<is_set[^>]*>\\s*([01])\\s*</is_set>");
        updated.first.replace(clock.position(1) + offset.position(1) + flag.position(1), flag.length(1), "1");
        updated.second = calibrationCrc(updated.first);
    }
    const bool initialized = std::filesystem::exists(marker);
    if (initialized && read(marker) != "Full-range calibrated cabinet setup r4.\n")
        throw std::runtime_error("Unknown calibration initialization marker.");
    if (initialized && hasXml && updated.first != xml && !repair)
        throw std::runtime_error("Gun calibration changed since setup. Files were preserved. To restore widescreen full-range aiming, close the game and run this launcher with --repair-calibration.");
    if (!hasXml || updated.first != xml) {
        std::filesystem::create_directories(backup);
        unsigned index = 1;
        auto transaction = backup / std::to_string(index);
        while (std::filesystem::exists(transaction)) transaction = backup / std::to_string(++index);
        std::filesystem::create_directory(transaction);
        writeNew(transaction / L"source.xml", xml);
        writeNew(transaction / L"source.crc", crc);
        writeNew(transaction / L"source-kind.txt", hasXml ? "Existing NVRAM\n" : "Supplied prop template\n");
        writeNew(transaction / L"updated.xml", updated.first);
        writeNew(transaction / L"updated.crc", updated.second);
        if (!matchingProcesses(game).empty() ||
            (hasXml && (read(xmlPath) != xml || read(crcPath) != crc)) ||
            (!hasXml && (std::filesystem::exists(xmlPath) || std::filesystem::exists(crcPath))))
            throw std::runtime_error("Game/calibration changed during setup; no replacement made.");
        std::filesystem::create_directories(xmlPath.parent_path());
        if (!MoveFileExW((transaction / L"updated.xml").c_str(), xmlPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
            !MoveFileExW((transaction / L"updated.crc").c_str(), crcPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Calibration setup interrupted; source saved in user/calibration-initialization-r4. No game launched.");
        if (read(xmlPath) != updated.first || read(crcPath) != updated.second)
            throw std::runtime_error("Calibration verification failed; no game launched.");
        writeNew(transaction / L"complete.txt", "Verified\n");
    }
    // Check on every launch, even after calibration initialization completed:
    // a removed/renamed marker must not silently restore cabinet aiming.
    ensureCabinetMarker(dx);
    if (!initialized) {
        std::filesystem::create_directories(backup);
        writeNew(marker, "Full-range calibrated cabinet setup r4.\n");
    }
}
}
