#include "launcher.h"
#include <tlhelp32.h>
#include <bcrypt.h>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace bone_eater::launcher {
namespace {
class Handle {
public:
    HANDLE value = nullptr;
    explicit Handle(HANDLE item = nullptr) : value(item) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};
bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}
std::string windowsError(const char* operation, DWORD error) {
    wchar_t* message = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::string result = std::string(operation) + " failed (Windows " + std::to_string(error) + ")";
    if (count && message) {
        try { result += ": " + narrow(std::wstring(message, count)); }
        catch (...) { LocalFree(message); throw; }
        LocalFree(message);
    }
    return result;
}
std::filesystem::path ownDirectory() {
    std::array<wchar_t, 32768> path {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) throw std::runtime_error(windowsError("Locate launcher"));
    return std::filesystem::canonical(std::filesystem::path(std::wstring(path.data(), length)).parent_path());
}
std::filesystem::path verifiedGameExecutable(const std::filesystem::path& root) {
    const auto canonicalRoot = std::filesystem::canonical(root);
    // New player packages use game/; keep existing development installs usable.
    const auto compact = canonicalRoot / L"game" / L"BoneEater.exe";
    const auto expected = std::filesystem::exists(canonicalRoot / L"game") ? compact :
        canonicalRoot / L"runtime" / L"game" / L"BoneEater.exe";
    if (!std::filesystem::is_regular_file(expected)) throw std::runtime_error("Missing game/BoneEater.exe. Extract the complete package and launch Bone Eater WS-Edition.exe.");
    const auto actual = std::filesystem::canonical(expected);
    if (!samePath(actual, expected)) throw std::runtime_error("Working game path resolves outside its expected game location. Refusing redirected launch.");
    return actual;
}
std::filesystem::path verifiedGame(const std::filesystem::path& root) {
    const auto actual = verifiedGameExecutable(root);
    for (const auto* relative : {L"modules/gamendd.dll", L"modules/arkndd.dll", L"prop/avs-config.xml"})
        if (!std::filesystem::is_regular_file(actual.parent_path() / relative)) throw std::runtime_error("Missing original game file: " + narrow(relative) + ". Copy your original arkdata, data, modules and prop folders (conf is optional) beside BoneEater.exe in the game folder. Keep a backup of your originals; see README.md for setup.");
    return actual;
}
std::vector<DWORD> matchingProcesses(const std::filesystem::path& executable) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) throw std::runtime_error(windowsError("Check running game instances"));
    PROCESSENTRY32W entry {}; entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) throw std::runtime_error(windowsError("Enumerate running game instances"));
    std::vector<DWORD> matches;
    do {
        if (CompareStringOrdinal(entry.szExeFile, -1, executable.filename().c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process.valid()) {
            const DWORD error = GetLastError();
            if (error == ERROR_INVALID_PARAMETER) continue; // Snapshot entry exited.
            throw std::runtime_error("Another process named " + narrow(executable.filename().wstring()) + " is running but its path cannot be verified. Close it before launching this copy.");
        }
        std::array<wchar_t, 32768> path {}; DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &size)) {
            DWORD code = STILL_ACTIVE;
            if (GetExitCodeProcess(process.value, &code) && code != STILL_ACTIVE) continue;
            throw std::runtime_error(windowsError("Verify another game's process path"));
        }
        std::error_code error;
        const auto actual = std::filesystem::canonical(std::filesystem::path(std::wstring(path.data(), size)), error);
        if (error) throw std::runtime_error("Cannot resolve another game's executable path; refusing a possible duplicate launch.");
        if (samePath(actual, executable)) matches.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.value, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES) throw std::runtime_error(windowsError("Finish process enumeration"));
    return matches;
}
std::wstring instanceName(const std::filesystem::path& executable) {
    std::wstring path = executable.wstring();
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr, 0);
    if (!count) throw std::runtime_error(windowsError("Normalize instance identity"));
    std::wstring folded(count, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.data(), static_cast<int>(path.size()), folded.data(), count, nullptr, nullptr, 0))
        throw std::runtime_error(windowsError("Normalize instance identity"));
    std::array<unsigned char, 32> hash {};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(folded.data()),
        static_cast<ULONG>(folded.size() * sizeof(wchar_t)), hash.data(), static_cast<ULONG>(hash.size())) < 0)
        throw std::runtime_error("Cannot form launcher instance identity.");
    std::wostringstream name;
    name << L"Local\\BoneEaterPrototypeLauncher-" << std::hex << std::setfill(L'0');
    for (auto byte : hash) name << std::setw(2) << static_cast<unsigned>(byte);
    return name.str();
}
DWORD runChild(const std::filesystem::path& executable, const std::filesystem::path& workingDirectory,
        const std::vector<std::wstring>& argv, const Environment& environment, const std::filesystem::path& outputFile) {
    // Attribute-list inheritance prevents unrelated launcher handles from
    // entering the game despite bInheritHandles being required for stdio.
    SECURITY_ATTRIBUTES security {sizeof(security), nullptr, TRUE};
    Handle output(CreateFileW(outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output.valid()) throw std::runtime_error(windowsError("Create startup log"));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.valid()) throw std::runtime_error(windowsError("Open child input"));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) throw std::runtime_error(windowsError("Size child handle list"));
    std::vector<unsigned char> storage(bytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) throw std::runtime_error(windowsError("Initialize child handle list"));
    struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributesGuard() { DeleteProcThreadAttributeList(value); } } guard {attributes};
    HANDLE inherited[] {input.value, output.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error(windowsError("Restrict inherited child handles"));
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    // CREATE_NO_WINDOW hides only the console. Do not set SW_HIDE in startup
    // information: the game's first ShowWindow can consume that initial hint.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = output.value;
    startup.StartupInfo.hStdError = output.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child {};
    auto line = commandLine(argv);
    if (environment.block.size() < 2 || environment.block.back() != L'\0' || environment.block[environment.block.size() - 2] != L'\0')
        throw std::runtime_error("Child environment is not double-NUL terminated.");
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        const_cast<wchar_t*>(environment.block.data()), workingDirectory.c_str(), &startup.StartupInfo, &child))
        throw std::runtime_error(windowsError("Start working game"));
    Handle process(child.hProcess), thread(child.hThread);
    if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) throw std::runtime_error(windowsError("Wait for game exit"));
    DWORD result = 0;
    if (!GetExitCodeProcess(process.value, &result)) throw std::runtime_error(windowsError("Read game exit status"));
    return result;
}
} // namespace bone_eater::launcher
