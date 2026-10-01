#include "render/capture_alpha.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgiformat.h>
#include <share.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

namespace bone_eater::render {
namespace {

struct Output {
    std::mutex mutex;
    FILE* file = nullptr;
    bool failed = false;
    std::uint64_t sequence = 0;
    ULONGLONG start = GetTickCount64();
    ~Output() { if (file) std::fclose(file); }
};

} // namespace

void observeCaptureAlpha(const char* role, void* window, std::uint32_t format,
                         std::uint32_t width, std::uint32_t height,
                         std::size_t rowPitch, const void* pixels) noexcept {
    const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool bgrx = format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if ((!rgba && !bgra && !bgrx) || !pixels || !width || !height || width > 16384 || height > 16384 ||
            rowPitch < static_cast<std::size_t>(width) * 4 ||
            rowPitch > std::numeric_limits<std::size_t>::max() / height ||
            static_cast<std::uint64_t>(width) * height * 4 > 256ULL * 1024 * 1024) return;

    try {
        static Output output;
        std::unique_lock<std::mutex> guard(output.mutex, std::try_to_lock);
        if (!guard || output.failed) return;
        if (!output.file) {
            if (!CreateDirectoryW(L"desktop/captures", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                output.failed = true;
                return;
            }
            output.file = _wfsopen(L"desktop/captures/raw-alpha-v1.csv", L"a", _SH_DENYWR);
            if (!output.file) { output.failed = true; return; }
            if (std::fseek(output.file, 0, SEEK_END) || std::ftell(output.file) < 0) {
                output.failed = true;
                return;
            }
            if (std::ftell(output.file) == 0) {
                std::fputs("schema,pid,sequence,elapsed_ms,thread,role,hwnd,width,height,dxgi_format,alpha_defined,"
                    "pixel_count,raw_byte3_min,raw_byte3_max,alpha0_count,alpha255_count,alpha_partial_count,"
                    "rgb_near_white_count,rgb_near_black_count,white_alpha0_count,white_alpha255_count", output.file);
                for (unsigned value = 0; value < 256; ++value) std::fprintf(output.file, ",byte3_%03u", value);
                std::fputc('\n', output.file);
            }
        }

        std::array<std::uint64_t, 256> histogram {};
        std::uint64_t white = 0, black = 0, whiteAlpha0 = 0, whiteAlpha255 = 0;
        unsigned minimum = 255, maximum = 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto* row = static_cast<const unsigned char*>(pixels) + static_cast<std::size_t>(y) * rowPitch;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto* pixel = row + static_cast<std::size_t>(x) * 4;
                const unsigned alpha = pixel[3];
                ++histogram[alpha];
                if (alpha < minimum) minimum = alpha;
                if (alpha > maximum) maximum = alpha;
                if (pixel[0] >= 250 && pixel[1] >= 250 && pixel[2] >= 250) {
                    ++white;
                    if (!alpha) ++whiteAlpha0;
                    if (alpha == 255) ++whiteAlpha255;
                }
                if (pixel[0] <= 5 && pixel[1] <= 5 && pixel[2] <= 5) ++black;
            }
        }
        // Fixed literals keep the diagnostic CSV valid even if a caller passes
        // an arbitrary window title rather than one of the registered roles.
        const char* safeRole = "unknown";
        if (role && (!std::strcmp(role, "main") || !std::strcmp(role, "multidisplay0") ||
                     !std::strcmp(role, "multidisplay1"))) safeRole = role;
        const auto count = static_cast<std::uint64_t>(width) * height;
        std::fprintf(output.file,
            "1,%lu,%llu,%llu,%lu,%s,%p,%u,%u,%u,%u,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(++output.sequence),
            static_cast<unsigned long long>(GetTickCount64() - output.start),
            static_cast<unsigned long>(GetCurrentThreadId()), safeRole, window, width, height, format,
            bgrx ? 0u : 1u, static_cast<unsigned long long>(count), minimum, maximum,
            static_cast<unsigned long long>(histogram[0]), static_cast<unsigned long long>(histogram[255]),
            static_cast<unsigned long long>(count - histogram[0] - histogram[255]),
            static_cast<unsigned long long>(white), static_cast<unsigned long long>(black),
            static_cast<unsigned long long>(whiteAlpha0), static_cast<unsigned long long>(whiteAlpha255));
        for (const auto value : histogram) std::fprintf(output.file, ",%llu", static_cast<unsigned long long>(value));
        std::fputc('\n', output.file);
        std::fflush(output.file);
        if (std::ferror(output.file)) output.failed = true;
    } catch (...) {
        // Capture diagnostics must never escape into the native render loop.
    }
}

} // namespace bone_eater::render
