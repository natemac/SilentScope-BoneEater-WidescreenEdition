#include "calibration.h"
#include "launcher.h"
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
}
std::pair<std::string, std::string> fullRangeCalibration(const std::string& xml, const std::string& crc) {
    if (calibrationCrc(xml) != crc) throw std::runtime_error("Calibration XML/CRC mismatch; preserve and inspect conf/nvram/testmode-v files.");
    const auto section = unique(xml, "<guncontrolleCheck>([\\s\\S]*?)</guncontrolleCheck>");
    std::string body = section[1];
    unique(body, "<calibrated>[\\s\\S]*?<current[^>]*>\\s*[01]\\s*</current>[\\s\\S]*?</calibrated>");
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
void prepareBetaCalibration(const std::filesystem::path& game) {
    // Only the new compact package. Development and original installations stay untouched.
    const auto dir = game.parent_path();
    if (dir.filename() != L"game" || (!std::filesystem::exists(dir / L"ADD GAME FILES HERE.md") && !std::filesystem::exists(dir / L"ADD GAME FILES.md"))) return;
    const auto backup = dir / L"desktop/calibration-initialization-r3";
    const auto marker = backup / L"complete.txt";
    const auto xmlPath = dir / L"conf/nvram/testmode-v.xml";
    const auto crcPath = dir / L"conf/nvram/testmode-v.crc";
    for (const auto& p : {backup, marker, xmlPath, crcPath}) ordinaryPath(p);
    if (std::filesystem::exists(marker)) {
        if (read(marker) != "Full-range cabinet calibration initialized by beta r3.\n") throw std::runtime_error("Unknown calibration initialization marker.");
        return;
    }
    if (std::filesystem::exists(backup)) throw std::runtime_error("Incomplete calibration setup. Original files are in desktop/calibration-initialization-r3; inspect before retrying.");
    const auto xml = read(xmlPath), crc = read(crcPath);
    const auto updated = fullRangeCalibration(xml, crc);
    std::filesystem::create_directories(backup);
    writeNew(backup / L"testmode-v.xml", xml);
    writeNew(backup / L"testmode-v.crc", crc);
    writeNew(backup / L"updated.xml", updated.first);
    writeNew(backup / L"updated.crc", updated.second);
    if (!matchingProcesses(game).empty() || read(xmlPath) != xml || read(crcPath) != crc)
        throw std::runtime_error("Game/calibration changed during setup; no replacement made.");
    if (!MoveFileExW((backup / L"updated.xml").c_str(), xmlPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
        !MoveFileExW((backup / L"updated.crc").c_str(), crcPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Calibration setup interrupted; originals saved in desktop/calibration-initialization-r3. No game launched.");
    if (read(xmlPath) != updated.first || read(crcPath) != updated.second) throw std::runtime_error("Calibration verification failed; no game launched.");
    writeNew(marker, "Full-range cabinet calibration initialized by beta r3.\n");
}
}
