#include "camera/camera_diagnostics.h"
#include "diagnostics/output_policy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <share.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace bone_eater::camera {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kFileSize = 21031056;
constexpr DWORD kImageSize = 0x1511000;
constexpr std::uintptr_t kActiveGlobal = 0x13DB588;
constexpr std::uintptr_t kScopeGlobal = 0x13DCF88;
constexpr std::uintptr_t kGameWrapperVtable = 0x10C5D70;
constexpr std::uintptr_t kScopeWrapperVtable = 0x10C7350;
constexpr std::uintptr_t kBaseWrapperVtable = 0x10CB750;
constexpr std::uintptr_t kAnimationWrapperVtable = 0x10C4B60;
constexpr std::uintptr_t kSRWrapperVtable = 0x10C73D0;
constexpr std::uintptr_t kKillWrapperVtable = 0x10C6200;
constexpr std::uintptr_t kCameraVtable = 0x6FDF48;
constexpr std::uintptr_t kCameraGetter = 0x25CFA0;
constexpr std::uintptr_t kCameraSetter = 0x2616D0;
constexpr std::size_t kCameraFieldsBegin = 0xDC4;
constexpr std::size_t kCameraFieldsEnd = 0xEBC;
constexpr std::size_t kCameraParamPointer = 0xDF0;
constexpr std::size_t kParamGeneration = 0x70;
constexpr std::size_t kProjectionP00 = 0x940;
constexpr std::size_t kProjectionP11 = 0x954;
constexpr char kExpectedSha256[] =
    "98de62b05925224bdda5c414f5be2c7a50fa210176708f8552e95bd81fab40a6";

static_assert(sizeof(void*) == 8, "Camera telemetry is for the pinned x64 module only");

struct Sample {
    const char* anchor = "";
    std::uintptr_t globalRva = 0;
    std::uintptr_t wrapper = 0;
    std::uintptr_t wrapperVtable = 0;
    const char* wrapperKind = "unknown";
    std::size_t targetOffset = 0;
    std::uintptr_t camera = 0;
    std::uintptr_t cameraVtable = 0;
    const char* status = "unresolved";
    bool fieldsRead = false;
    float aspectMultiplier = 0;
    float viewportRatio = 0;
    float width = 0;
    float height = 0;
    std::uint8_t slot = 0;
    // Optical input intent and derived projection are observed independently.
    // Pointer equality across anchors establishes sharing, not exclusive ownership.
    std::uintptr_t parameter = 0;
    std::uint32_t parameterGeneration = 0;
    bool generationRead = false;
    bool opticsRead = false;
    bool projectionRead = false;
    bool projectionRatioValid = false;
    const char* opticalStatus = "unresolved_not_sampled";
    float systemZoom = 0;
    float effectiveFocalLength = 0;
    float filmWidth = 0;
    float projectionP00 = 0;
    float projectionP11 = 0;
    double projectionRatio = 0;
};

struct Observer {
    std::mutex mutex;
    std::atomic<bool> failed {false};
    FILE* output = nullptr;
    Clock::time_point start = Clock::now();
    Clock::time_point nextSample {};
    std::uint64_t sequence = 0;
    DWORD process = GetCurrentProcessId();
    char sessionUtc[32] {};
    HMODULE lastModule = nullptr;
    bool validationAttempted = false;
    bool moduleValid = false;
    const char* moduleStatus = "module_not_loaded";
    char actualSha256[65] {};

    ~Observer() { if (output) std::fclose(output); }
};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct HashHandles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    // This storage outlives BCryptDestroyHash on every return/exception path.
    std::vector<unsigned char> object;
    ~HashHandles() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

bool enabled() noexcept {
    static const bool value = [] {
        wchar_t text[4] {};
        return diagnostics::optionalOutputEnabled() && GetEnvironmentVariableW(L"BONE_EATER_CAMERA_TELEMETRY", text,
            static_cast<DWORD>(std::size(text))) == 1 && text[0] == L'1';
    }();
    return value;
}

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

const char* hashModuleFile(HMODULE module, char (&hex)[65]) {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return "module_path_unavailable";
    FileHandle file;
    file.value = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return "module_file_unreadable";
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart != static_cast<LONGLONG>(kFileSize)) return "file_size_mismatch";

    HashHandles handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) < 0) return "sha256_provider_failed";
    DWORD objectSize = 0, digestSize = 0, bytes = 0;
    if (BCryptGetProperty(handles.algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0) < 0 ||
            BCryptGetProperty(handles.algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize), &bytes, 0) < 0 ||
            !objectSize || objectSize > 1024 * 1024 || digestSize != 32) {
        return "sha256_properties_failed";
    }
    handles.object.resize(objectSize);
    if (BCryptCreateHash(handles.algorithm, &handles.hash, handles.object.data(), objectSize,
            nullptr, 0, 0) < 0) return "sha256_create_failed";
    std::array<unsigned char, 65536> block {};
    std::uint64_t total = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.value, block.data(), static_cast<DWORD>(block.size()), &read, nullptr)) {
            return "module_file_read_failed";
        }
        if (!read) break;
        total += read;
        if (total > kFileSize || BCryptHashData(handles.hash, block.data(), read, 0) < 0) {
            return "sha256_update_failed";
        }
    }
    if (total != kFileSize) return "module_file_size_changed";
    std::array<unsigned char, 32> digest {};
    if (BCryptFinishHash(handles.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        return "sha256_finish_failed";
    }
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < digest.size(); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    return std::strcmp(hex, kExpectedSha256) == 0 ? "verified" : "sha256_mismatch";
}

const char* validateModule(HMODULE module, char (&hex)[65]) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos {};
    if (!readAt(base, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) || dos.e_lfanew > 0x100000) {
        return "pe_dos_invalid";
    }
    IMAGE_NT_HEADERS64 nt {};
    if (!readAt(base, static_cast<std::size_t>(dos.e_lfanew), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.SizeOfImage != kImageSize) {
        return "pe_image_mismatch";
    }
    const char* hashStatus = hashModuleFile(module, hex);
    if (std::strcmp(hashStatus, "verified") != 0) return hashStatus;
    std::uintptr_t getter = 0, setter = 0;
    if (!readAt(base, kCameraVtable + 0x20, getter) ||
            !readAt(base, kCameraVtable + 0x28, setter) ||
            getter != base + kCameraGetter || setter != base + kCameraSetter) {
        return "camera_vtable_entries_mismatch";
    }
    return "verified";
}

bool openOutput(Observer& observer) noexcept {
    if (observer.output) return true;
    if (!CreateDirectoryW(L"desktop", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    // Versioned file prevents appending a wider schema to existing schema-1 rows.
    observer.output = _wfsopen(L"desktop\\cameras-v2.csv", L"ab", _SH_DENYWR);
    if (!observer.output) return false;
    SYSTEMTIME utc {};
    GetSystemTime(&utc);
    std::snprintf(observer.sessionUtc, sizeof(observer.sessionUtc),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
        utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    _fseeki64(observer.output, 0, SEEK_END);
    if (_ftelli64(observer.output) == 0) {
        std::fputs("schema_version,session_utc,pid,sample,elapsed_ms,thread,anchor,global_rva,"
            "module_base,module_status,module_sha256,wrapper,wrapper_vftable,wrapper_kind,"
            "target_offset,camera,camera_vftable,status,aspect_multiplier,viewport_ratio,"
            "width,height,render_slot,effective_ratio,optical_status,camera_parameter,"
            "parameter_generation,system_zoom,effective_focal_length,film_width,"
            "projection_p00,projection_p11,projection_p11_over_p00\n", observer.output);
    }
    return true;
}

template<typename T, std::size_t N>
T field(const std::array<unsigned char, N>& data, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value, data.data() + offset - kCameraFieldsBegin, sizeof(value));
    return value;
}

void sampleOptics(Sample& result) noexcept {
    // These are exact-binary-proven float fields, not guessed camera function calls.
    // Group adjacent optical values in one RPM to reduce cross-update skew.
    // Stable generation detects some tears; it does not prove the projection
    // was recomputed in the same engine update as these optical parameters.
    std::array<unsigned char, 0x48 - 0x18> optics {};
    std::array<unsigned char, kProjectionP11 + sizeof(float) - kProjectionP00> projection {};
    result.generationRead = result.parameter &&
        readAt(result.parameter, kParamGeneration, result.parameterGeneration);
    const auto positiveFinite = [](float value) noexcept {
        return std::isfinite(value) && value >= std::numeric_limits<float>::min();
    };
    if (readAt(result.camera, kProjectionP00, projection)) {
        std::memcpy(&result.projectionP00, projection.data(), sizeof(float));
        std::memcpy(&result.projectionP11,
            projection.data() + kProjectionP11 - kProjectionP00, sizeof(float));
        result.projectionRead = true;
        if (positiveFinite(result.projectionP00) && positiveFinite(result.projectionP11)) {
            result.projectionRatio = static_cast<double>(result.projectionP11) / result.projectionP00;
            result.projectionRatioValid = std::isfinite(result.projectionRatio) && result.projectionRatio > 0;
        }
    }
    if (!result.parameter) { result.opticalStatus = "unresolved_parameter_null"; return; }
    if (readAt(result.parameter, 0x18, optics)) {
        std::memcpy(&result.systemZoom, optics.data(), sizeof(float));
        std::memcpy(&result.effectiveFocalLength, optics.data() + 0x28 - 0x18, sizeof(float));
        std::memcpy(&result.filmWidth, optics.data() + 0x44 - 0x18, sizeof(float));
        result.opticsRead = true;
    }
    std::uintptr_t parameterAgain = 0;
    std::uint32_t generationAgain = 0;
    if (!readAt(result.camera, kCameraParamPointer, parameterAgain) || parameterAgain != result.parameter) {
        result.opticalStatus = "unresolved_parameter_identity_changed";
        return;
    }
    if (!result.generationRead || !readAt(result.parameter, kParamGeneration, generationAgain)) {
        result.opticalStatus = "unresolved_parameter_generation_unreadable";
        return;
    }
    if (generationAgain != result.parameterGeneration) {
        result.opticalStatus = "unresolved_parameter_generation_changed";
        return;
    }
    if (!result.opticsRead) { result.opticalStatus = "unresolved_optical_fields_unreadable"; return; }
    if (!result.projectionRead) { result.opticalStatus = "unresolved_projection_unreadable"; return; }

    // Positivity describes the observed perspective branch, not every possible
    // engine camera. Retain raw values in the CSV even when this check fails.
    if (!positiveFinite(result.systemZoom) || !positiveFinite(result.effectiveFocalLength) ||
            !positiveFinite(result.filmWidth) || !positiveFinite(result.projectionP00) ||
            !positiveFinite(result.projectionP11)) {
        result.opticalStatus = "unresolved_invalid_perspective_fields";
        return;
    }
    result.opticalStatus = result.projectionRatioValid ? "resolved" : "unresolved_invalid_projection_ratio";
}

Sample sampleCamera(std::uintptr_t base, const char* anchor, std::uintptr_t global,
                    std::size_t targetOffset) noexcept {
    Sample result;
    result.anchor = anchor;
    result.globalRva = global;
    result.targetOffset = targetOffset;
    if (!readAt(base, global, result.wrapper)) { result.status = "unresolved_global_unreadable"; return result; }
    if (!result.wrapper) { result.status = "unresolved_wrapper_null"; return result; }
    if (!readAt(result.wrapper, 0, result.wrapperVtable)) {
        result.status = "unresolved_wrapper_unreadable"; return result;
    }
    if (result.wrapperVtable == base + kGameWrapperVtable) result.wrapperKind = "GameCamera";
    else if (result.wrapperVtable == base + kScopeWrapperVtable) result.wrapperKind = "ScopeCamera";
    else if (result.wrapperVtable == base + kBaseWrapperVtable) result.wrapperKind = "CCamera";
    else if (result.wrapperVtable == base + kAnimationWrapperVtable) result.wrapperKind = "AnimationCamera";
    else if (result.wrapperVtable == base + kSRWrapperVtable) result.wrapperKind = "SRCamera";
    else if (result.wrapperVtable == base + kKillWrapperVtable) result.wrapperKind = "KillCamera";
    else { result.status = "unresolved_wrapper_vftable"; return result; }
    // The owned-camera +0x60 member belongs to GameCamera and its proven
    // zero-offset subclasses. Framework::CCamera only establishes target +0x8.
    if (result.wrapperVtable == base + kBaseWrapperVtable && targetOffset != 0x8) {
        result.status = "unresolved_base_wrapper_has_no_owned_camera";
        return result;
    }
    if (!readAt(result.wrapper, targetOffset, result.camera)) {
        result.status = "unresolved_target_unreadable"; return result;
    }
    if (!result.camera) { result.status = "unresolved_camera_null"; return result; }
    if (!readAt(result.camera, 0, result.cameraVtable)) {
        result.status = "unresolved_camera_unreadable"; return result;
    }
    if (result.cameraVtable != base + kCameraVtable) {
        result.status = "unresolved_camera_vftable"; return result;
    }

    std::array<unsigned char, kCameraFieldsEnd - kCameraFieldsBegin> data {};
    if (!readAt(result.camera, kCameraFieldsBegin, data)) {
        result.status = "unresolved_camera_fields_unreadable"; return result;
    }
    result.parameter = field<std::uintptr_t>(data, kCameraParamPointer);
    sampleOptics(result);
    std::uintptr_t wrapperAgain = 0, targetAgain = 0, vtableAgain = 0, wrapperVtableAgain = 0;
    if (!readAt(base, global, wrapperAgain) || wrapperAgain != result.wrapper ||
            !readAt(result.wrapper, 0, wrapperVtableAgain) || wrapperVtableAgain != result.wrapperVtable ||
            !readAt(result.wrapper, targetOffset, targetAgain) || targetAgain != result.camera ||
            !readAt(result.camera, 0, vtableAgain) || vtableAgain != result.cameraVtable) {
        result.status = "unresolved_identity_changed";
        result.opticalStatus = "unresolved_camera_identity_changed";
        result.projectionRatioValid = false;
        return result;
    }
    result.aspectMultiplier = field<float>(data, 0xDC4);
    result.viewportRatio = field<float>(data, 0xEB0);
    result.width = field<float>(data, 0xEB4);
    result.height = field<float>(data, 0xEB8);
    result.slot = field<std::uint8_t>(data, 0xE8E);
    result.fieldsRead = true;
    result.status = std::isfinite(result.aspectMultiplier) && result.aspectMultiplier > 0 &&
        std::isfinite(result.viewportRatio) && result.viewportRatio > 0 &&
        std::isfinite(result.width) && result.width > 0 && result.width <= 65536 &&
        std::isfinite(result.height) && result.height > 0 && result.height <= 65536 ?
        "resolved" : "unresolved_invalid_fields";
    return result;
}

void writeSample(Observer& observer, const Sample& sample, Clock::time_point now) noexcept {
    const double elapsed = std::chrono::duration<double, std::milli>(now - observer.start).count();
    std::fprintf(observer.output,
        "2,%s,%lu,%llu,%.3f,%lu,%s,0x%llx,%p,%s,%s,%p,%p,%s,0x%zx,%p,%p,%s,",
        observer.sessionUtc, static_cast<unsigned long>(observer.process),
        static_cast<unsigned long long>(observer.sequence), elapsed,
        static_cast<unsigned long>(GetCurrentThreadId()), sample.anchor,
        static_cast<unsigned long long>(sample.globalRva), static_cast<void*>(observer.lastModule),
        observer.moduleStatus, observer.actualSha256,
        reinterpret_cast<void*>(sample.wrapper), reinterpret_cast<void*>(sample.wrapperVtable),
        sample.wrapperKind, sample.targetOffset, reinterpret_cast<void*>(sample.camera),
        reinterpret_cast<void*>(sample.cameraVtable), sample.status);
    if (sample.fieldsRead) {
        std::fprintf(observer.output, "%.9g,%.9g,%.9g,%.9g,%u,%.9g",
            sample.aspectMultiplier, sample.viewportRatio, sample.width, sample.height,
            static_cast<unsigned int>(sample.slot),
            static_cast<double>(sample.aspectMultiplier) * sample.viewportRatio);
    } else {
        std::fputs(",,,,,", observer.output);
    }
    std::fprintf(observer.output, ",%s,%p,", sample.opticalStatus, reinterpret_cast<void*>(sample.parameter));
    if (sample.generationRead) std::fprintf(observer.output, "%lu", static_cast<unsigned long>(sample.parameterGeneration));
    if (sample.opticsRead) {
        std::fprintf(observer.output, ",%.9g,%.9g,%.9g", sample.systemZoom, sample.effectiveFocalLength, sample.filmWidth);
    } else std::fputs(",,,", observer.output);
    if (sample.projectionRead) {
        std::fprintf(observer.output, ",%.9g,%.9g", sample.projectionP00, sample.projectionP11);
    } else std::fputs(",,", observer.output);
    if (sample.projectionRatioValid) std::fprintf(observer.output, ",%.12g", sample.projectionRatio);
    else std::fputc(',', observer.output);
    std::fputc('\n', observer.output);
}

} // namespace

void observeCameraState() noexcept {
    if (!enabled()) return;
    static Observer observer;
    try {
        std::unique_lock<std::mutex> guard(observer.mutex, std::try_to_lock);
        if (!guard || observer.failed) return;
        const auto now = Clock::now();
        if (now < observer.nextSample) return;
        observer.nextSample = now + std::chrono::milliseconds(200);
        if (!openOutput(observer)) { observer.failed = true; return; }
        ++observer.sequence;

        const HMODULE module = GetModuleHandleW(L"gamendd.dll");
        if (module != observer.lastModule) {
            observer.lastModule = module;
            observer.validationAttempted = false;
            observer.moduleValid = false;
            observer.actualSha256[0] = '\0';
        }
        if (!module) observer.moduleStatus = "module_not_loaded";
        else if (!observer.validationAttempted) {
            observer.validationAttempted = true;
            observer.moduleStatus = validateModule(module, observer.actualSha256);
            observer.moduleValid = std::strcmp(observer.moduleStatus, "verified") == 0;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        for (const auto& anchor : std::array<std::pair<const char*, std::uintptr_t>, 2> {{
                {"active", kActiveGlobal}, {"scope", kScopeGlobal}}}) {
            for (const std::size_t offset : {std::size_t{0x8}, std::size_t{0x60}}) {
                Sample sample;
                if (observer.moduleValid) sample = sampleCamera(base, anchor.first, anchor.second, offset);
                else {
                    sample.anchor = anchor.first;
                    sample.globalRva = anchor.second;
                    sample.targetOffset = offset;
                    sample.status = "unresolved_module";
                }
                writeSample(observer, sample, Clock::now());
            }
        }
        std::fflush(observer.output);
        if (std::ferror(observer.output)) observer.failed = true;
        // The first sample includes file hashing. Space the next observation
        // from completion as well so a slow validation cannot bunch samples.
        observer.nextSample = Clock::now() + std::chrono::milliseconds(200);
    } catch (...) {
        // No exception may cross the game's render hook. No game state is changed.
        observer.failed = true;
    }
}

} // namespace bone_eater::camera
