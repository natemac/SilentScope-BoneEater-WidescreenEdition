#include "render/native_viewport.h"
#include "diagnostics/output_policy.h"
#include <chrono>
#include <cstdio>
#include <share.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace bone_eater::render {
namespace {

constexpr std::uint64_t kFileSize = 21031056;
constexpr DWORD kImageSize = 0x1511000;
constexpr std::uintptr_t kWindowGlobal = 0x12E17A0;
constexpr std::uintptr_t kFrameBufferGlobal = 0x12E24E8;
constexpr std::uintptr_t kFrameBufferVtable = 0x703E78;
constexpr char kExpectedHash[] =
    "98de62b05925224bdda5c414f5be2c7a50fa210176708f8552e95bd81fab40a6";
static_assert(sizeof(void*) == 8, "The native viewport offsets describe the pinned x64 game");

bool readBytes(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        destination, size, &copied) != FALSE && copied == size;
}

template<typename T>
bool readAt(std::uintptr_t base, std::size_t offset, T& value) noexcept {
    return base && base <= std::numeric_limits<std::uintptr_t>::max() - offset &&
        readBytes(base + offset, &value, sizeof(value));
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& data, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

const char* verifyPe(std::uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos {};
    if (!readAt(base, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x100000) {
        return "pe_dos_invalid";
    }
    IMAGE_NT_HEADERS64 nt {};
    if (!readAt(base, static_cast<std::size_t>(dos.e_lfanew), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage != kImageSize) return "pe_image_mismatch";
    return "verified";
}

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct HashStorage {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~HashStorage() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

const char* verifyFile(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return "module_path_unavailable";
    FileHandle file;
    file.value = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return "module_file_unreadable";
    LARGE_INTEGER fileSize {};
    if (!GetFileSizeEx(file.value, &fileSize) || fileSize.QuadPart != static_cast<LONGLONG>(kFileSize)) {
        return "file_size_mismatch";
    }
    HashStorage storage;
    if (BCryptOpenAlgorithmProvider(&storage.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return "sha256_provider_failed";
    }
    DWORD objectSize = 0, digestSize = 0, bytes = 0;
    if (BCryptGetProperty(storage.algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0) < 0 ||
            BCryptGetProperty(storage.algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize), &bytes, 0) < 0 ||
            !objectSize || objectSize > 1024 * 1024 || digestSize != 32) return "sha256_properties_failed";
    storage.object.resize(objectSize);
    if (BCryptCreateHash(storage.algorithm, &storage.hash, storage.object.data(), objectSize,
            nullptr, 0, 0) < 0) return "sha256_create_failed";
    std::array<unsigned char, 65536> block {};
    std::uint64_t total = 0;
    for (;;) {
        DWORD size = 0;
        if (!ReadFile(file.value, block.data(), static_cast<DWORD>(block.size()), &size, nullptr)) {
            return "module_file_read_failed";
        }
        if (!size) break;
        total += size;
        if (total > kFileSize || BCryptHashData(storage.hash, block.data(), size, 0) < 0) {
            return "sha256_update_failed";
        }
    }
    if (total != kFileSize) return "module_file_size_changed";
    std::array<unsigned char, 32> digest {};
    if (BCryptFinishHash(storage.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        return "sha256_finish_failed";
    }
    std::array<char, 65> hex {};
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    return std::strcmp(hex.data(), kExpectedHash) == 0 ? "verified" : "sha256_mismatch";
}

} // namespace

const char* verifyNativeGameModule(void* module) noexcept {
    struct Validation {
        std::mutex mutex;
        HMODULE module = nullptr;
        bool attempted = false;
        const char* status = "module_not_loaded";
    };
    static Validation validation;
    try {
        std::lock_guard<std::mutex> lock(validation.mutex);
        const HMODULE loaded = GetModuleHandleW(L"gamendd.dll");
        if (loaded != validation.module) {
            validation.module = loaded;
            validation.attempted = false;
            validation.status = "module_not_loaded";
        }
        if (!loaded || !module) return "module_not_loaded";
        if (loaded != module) return "module_identity_mismatch";
        const char* pe = verifyPe(reinterpret_cast<std::uintptr_t>(loaded));
        if (std::strcmp(pe, "verified") != 0) return pe;
        if (!validation.attempted) {
            validation.attempted = true;
            validation.status = "validation_failed";
            validation.status = verifyFile(loaded);
        }
        return validation.status;
    } catch (...) {
        return "validation_failed";
    }
}

NativeViewportSnapshot readNativeMainViewport(void* expectedMainWindow) noexcept {
    NativeViewportSnapshot result;
    if (!expectedMainWindow) { result.status = "main_window_unknown"; return result; }
    const HMODULE module = GetModuleHandleW(L"gamendd.dll");
    result.moduleBase = reinterpret_cast<std::uintptr_t>(module);
    result.status = verifyNativeGameModule(module);
    if (std::strcmp(result.status, "verified") != 0) return result;

    if (!readAt(result.moduleBase, kWindowGlobal, result.windowObject) ||
            !readAt(result.moduleBase, kFrameBufferGlobal, result.frameBufferObject) ||
            !result.windowObject || !result.frameBufferObject) {
        result.status = "native_owners_unresolved"; return result;
    }
    std::array<unsigned char, 0x58> window {};
    std::array<unsigned char, 0x48> framebuffer {};
    if (!readAt(result.windowObject, 0, window) || !readAt(result.frameBufferObject, 0, framebuffer)) {
        result.status = "native_fields_unreadable"; return result;
    }
    if (field<std::uintptr_t>(framebuffer, 0) != result.moduleBase + kFrameBufferVtable) {
        result.status = "framebuffer_vftable_mismatch"; return result;
    }
    result.window = reinterpret_cast<void*>(field<std::uintptr_t>(window, 0x28));
    if (result.window != expectedMainWindow) { result.status = "main_hwnd_mismatch"; return result; }
    result.clientWidth = field<std::int32_t>(window, 0x40);
    result.clientHeight = field<std::int32_t>(window, 0x44);
    result.left = field<std::int32_t>(window, 0x48);
    result.top = field<std::int32_t>(window, 0x4C);
    result.right = field<std::int32_t>(window, 0x50);
    result.bottom = field<std::int32_t>(window, 0x54);
    result.logicalWidth = field<std::uint16_t>(framebuffer, 0x28);
    result.logicalHeight = field<std::uint16_t>(framebuffer, 0x2A);
    result.resourceWidth = field<std::uint16_t>(framebuffer, 0x38);
    result.resourceHeight = field<std::uint16_t>(framebuffer, 0x3A);
    if (result.clientWidth <= 0 || result.clientHeight <= 0 ||
            result.clientWidth > 65536 || result.clientHeight > 65536 ||
            result.left < 0 || result.top < 0 || result.right <= result.left || result.bottom <= result.top ||
            result.right > result.clientWidth || result.bottom > result.clientHeight ||
            !result.logicalWidth || !result.logicalHeight || !result.resourceWidth || !result.resourceHeight) {
        result.status = "native_geometry_invalid"; return result;
    }
    DWORD owner = 0;
    RECT client {};
    const auto hwnd = static_cast<HWND>(expectedMainWindow);
    if (!GetWindowThreadProcessId(hwnd, &owner) || owner != GetCurrentProcessId() ||
            !GetClientRect(hwnd, &client)) {
        result.status = "native_client_not_current"; return result;
    }
    result.osClientWidth = client.right - client.left;
    result.osClientHeight = client.bottom - client.top;
    if (result.osClientWidth <= 0 || result.osClientHeight <= 0 ||
            result.osClientWidth > 65536 || result.osClientHeight > 65536 ||
            result.right > result.osClientWidth || result.bottom > result.osClientHeight) {
        result.status = "fit_outside_os_client"; return result;
    }

    std::uintptr_t windowAgain = 0, frameBufferAgain = 0;
    std::array<unsigned char, 0x58> repeatedWindow {};
    std::array<unsigned char, 0x48> repeatedFrameBuffer {};
    RECT repeatedClient {};
    if (GetModuleHandleW(L"gamendd.dll") != module ||
            !readAt(result.moduleBase, kWindowGlobal, windowAgain) || windowAgain != result.windowObject ||
            !readAt(result.moduleBase, kFrameBufferGlobal, frameBufferAgain) || frameBufferAgain != result.frameBufferObject ||
            !readAt(windowAgain, 0, repeatedWindow) || !readAt(frameBufferAgain, 0, repeatedFrameBuffer) ||
            field<std::uintptr_t>(repeatedWindow, 0x28) != field<std::uintptr_t>(window, 0x28) ||
            field<std::uintptr_t>(repeatedFrameBuffer, 0) != field<std::uintptr_t>(framebuffer, 0) ||
            std::memcmp(window.data() + 0x40, repeatedWindow.data() + 0x40, 0x18) != 0 ||
            std::memcmp(framebuffer.data() + 0x28, repeatedFrameBuffer.data() + 0x28, 4) != 0 ||
            std::memcmp(framebuffer.data() + 0x38, repeatedFrameBuffer.data() + 0x38, 4) != 0 ||
            !GetClientRect(hwnd, &repeatedClient) || !EqualRect(&client, &repeatedClient)) {
        result.status = "native_geometry_changed"; return result;
    }
    result.valid = true;
    result.status = "resolved";
    return result;
}

void observeNativeMainViewport(void* expectedMainWindow) noexcept {
    if (!diagnostics::optionalOutputEnabled()) return;
    using Clock = std::chrono::steady_clock;
    struct Log {
        std::mutex mutex;
        Clock::time_point next {};
        FILE* file = nullptr;
        bool attempted = false;
        ~Log() { if (file) std::fclose(file); }
    };
    static Log log;
    try {
        std::unique_lock<std::mutex> lock(log.mutex, std::try_to_lock);
        const auto now = Clock::now();
        if (!lock || now < log.next) return;
        log.next = now + std::chrono::seconds(1);
        if (!log.attempted) {
            log.attempted = true;
            CreateDirectoryW(L"desktop", nullptr);
            log.file = _wfsopen(L"desktop\\viewport-v2.csv", L"ab", _SH_DENYWR);
            if (log.file) {
                _fseeki64(log.file, 0, SEEK_END);
                if (!_ftelli64(log.file)) std::fputs("pid,tick_ms,status,hwnd,client_width,client_height,left,top,right,bottom,logical_width,logical_height,resource_width,resource_height,os_client_width,os_client_height\n", log.file);
            }
        }
        if (!log.file) return;
        const auto v = readNativeMainViewport(expectedMainWindow);
        std::fprintf(log.file, "%lu,%llu,%s,%p,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%d,%d\n",
            static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(GetTickCount64()),
            v.status, v.window, v.clientWidth, v.clientHeight, v.left, v.top, v.right, v.bottom,
            v.logicalWidth, v.logicalHeight, v.resourceWidth, v.resourceHeight, v.osClientWidth, v.osClientHeight);
        std::fflush(log.file);
    } catch (...) {}
}

} // namespace bone_eater::render
