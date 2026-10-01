#include "render/d3d11_diagnostics.h"
#include "render/native_viewport.h"
#include "render/capture_alpha.h"
#include "render/window_layout.h"
#include "render/auxiliary_windows.h"
#include "camera/camera_diagnostics.h"
#include "diagnostics/output_policy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <share.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bone_eater::render {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kMetadataInterval = std::chrono::seconds(1);
constexpr auto kSummaryInterval = std::chrono::seconds(5);

// Titles identify output slots only. Their actual content is not yet verified.
enum class Role : std::size_t { Unknown, Main, MultiDisplay0, MultiDisplay1, MultiDisplayOther, Count };

struct RegisteredWindow {
    HWND window = nullptr;
    std::wstring title;
};

struct WindowRegistry {
    std::mutex mutex;
    std::array<RegisteredWindow, 3> outputs;
};

WindowRegistry& windowRegistry() {
    static WindowRegistry registry;
    return registry;
}

bool registeredTitle(HWND window, std::wstring& title) {
    auto& registry = windowRegistry();
    std::lock_guard<std::mutex> guard(registry.mutex);
    for (const auto& output : registry.outputs) {
        if (window && output.window == window) {
            title = output.title;
            return true;
        }
    }
    return false;
}

const char* roleName(Role role) noexcept {
    switch (role) {
        case Role::Main: return "main";
        case Role::MultiDisplay0: return "multidisplay0";
        case Role::MultiDisplay1: return "multidisplay1";
        case Role::MultiDisplayOther: return "multidisplay";
        default: return "unknown";
    }
}

struct Metadata {
    HWND window = nullptr;
    void* mainReference = nullptr;
    std::wstring title;
    Role role = Role::Unknown;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT buffers = 0;
    UINT samples = 0;
    UINT sampleQuality = 0;
    UINT textureWidth = 0;
    UINT textureHeight = 0;
    DXGI_FORMAT textureFormat = DXGI_FORMAT_UNKNOWN;
    UINT textureSamples = 0;
    DWORD presentThread = 0;
    BOOL windowed = FALSE;
    LONG clientWidth = 0;
    LONG clientHeight = 0;
    void* device = nullptr;
    bool adapterKnown = false;
    LUID adapterLuid {};
    std::wstring adapterName;

    bool operator==(const Metadata& other) const noexcept {
        return window == other.window && mainReference == other.mainReference &&
            title == other.title && role == other.role && width == other.width &&
            height == other.height && format == other.format &&
            buffers == other.buffers && samples == other.samples &&
            sampleQuality == other.sampleQuality && windowed == other.windowed &&
            textureWidth == other.textureWidth && textureHeight == other.textureHeight &&
            textureFormat == other.textureFormat && textureSamples == other.textureSamples &&
            presentThread == other.presentThread &&
            clientWidth == other.clientWidth && clientHeight == other.clientHeight &&
            device == other.device && adapterKnown == other.adapterKnown &&
            adapterLuid.HighPart == other.adapterLuid.HighPart &&
            adapterLuid.LowPart == other.adapterLuid.LowPart &&
            adapterName == other.adapterName;
    }
};

struct ChainState {
    Metadata metadata;
    bool registered = false;
    Clock::time_point nextMetadata {};
    Clock::time_point lastPresent {};
    Clock::time_point sampleStart {};
    Clock::time_point nextCapture {};
    std::uint64_t totalCalls = 0;
    std::uint64_t sampleCalls = 0;
    std::uint64_t intervalCount = 0;
    double intervalTotalMs = 0.0;
    double intervalMinMs = std::numeric_limits<double>::max();
    double intervalMaxMs = 0.0;
};

struct Telemetry {
    std::mutex mutex;
    std::unordered_map<IDXGISwapChain*, ChainState> chains;
    std::array<std::uint64_t, static_cast<std::size_t>(Role::Count)> roleCalls {};
    Clock::time_point start = Clock::now();
    FILE* output = nullptr;
    bool opened = false;
    DWORD pid = GetCurrentProcessId();
    char sessionUtc[32] {};

    ~Telemetry() {
        if (output) {
            std::fclose(output);
        }
    }
};

std::atomic<bool> failed { false };

struct ComRelease {
    template<typename T> void operator()(T* object) const noexcept {
        if (object) object->Release();
    }
};
template<typename T> using ComOwner = std::unique_ptr<T, ComRelease>;

bool environmentFlag(const wchar_t* name, bool defaultValue) noexcept {
    wchar_t value[8] {};
    const DWORD length = GetEnvironmentVariableW(
        name, value, static_cast<DWORD>(std::size(value)));
    if (length == 1 && value[0] == L'0') return false;
    if (length == 1 && value[0] == L'1') return true;
    return defaultValue;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

void csvText(FILE* output, const std::string& text) noexcept {
    std::fputc('"', output);
    for (const char character : text) {
        if (character == '"') {
            std::fputc('"', output);
        }
        // Keep each telemetry record on one line, even for unusual window titles.
        std::fputc(character == '\r' || character == '\n' ? ' ' : character, output);
    }
    std::fputc('"', output);
}

bool openOutput(Telemetry& state) noexcept {
    if (state.opened) {
        return state.output != nullptr;
    }
    state.opened = true;
    if (!CreateDirectoryW(L"desktop", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    state.output = _wfsopen(L"desktop\\graphics.csv", L"ab", _SH_DENYWR);
    if (!state.output) {
        return false;
    }
    SYSTEMTIME utc {};
    GetSystemTime(&utc);
    std::snprintf(state.sessionUtc, sizeof(state.sessionUtc),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth,
        utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
    _fseeki64(state.output, 0, SEEK_END);
    if (_ftelli64(state.output) == 0) {
        std::fputs("session_utc,pid,event,api,elapsed_ms,swapchain,hwnd,title,role,"
            "main_reference,buffer_width,buffer_height,format,buffer_count,"
            "sample_count,sample_quality,windowed,client_width,client_height,"
            "device,adapter_known,adapter_luid,adapter_name,chain_present_calls,"
            "role_present_calls,sample_present_calls,sample_ms,observed_calls_hz,"
            "mean_interval_ms,min_interval_ms,max_interval_ms,sync_interval,"
            "present_flags,texture_width,texture_height,texture_format,texture_samples,"
            "present_thread\n", state.output);
    }
    return true;
}

Role classify(HWND window, const std::wstring& title, void* mainReference) noexcept {
    // Auxiliary output titles take precedence over an accidental main HWND.
    constexpr auto prefixLength = std::size(L"Aska MultiDisplay") - 1;
    if (title.size() >= prefixLength && _wcsnicmp(title.c_str(), L"Aska MultiDisplay", prefixLength) == 0) {
        const wchar_t* suffix = title.c_str() + prefixLength;
        while (*suffix == L' ') ++suffix;
        // Runtime titles include "[0](multipssID:33)". Match the complete
        // index token so that, for example, [10] is never mistaken for [1].
        if ((suffix[0] == L'[' && suffix[1] == L'0' && suffix[2] == L']') ||
                std::wcscmp(suffix, L"0") == 0) return Role::MultiDisplay0;
        if ((suffix[0] == L'[' && suffix[1] == L'1' && suffix[2] == L']') ||
                std::wcscmp(suffix, L"1") == 0) return Role::MultiDisplay1;
        return Role::MultiDisplayOther;
    }
    if (_wcsicmp(title.c_str(), L"ASKA") == 0 || (window && window == mainReference)) {
        return Role::Main;
    }
    return Role::Unknown;
}

void refreshSlowMetadata(IDXGISwapChain* chain, Metadata& metadata) {
    wchar_t title[256] {};
    DWORD_PTR copied = 0;
    // Cross-thread GetWindowText can block the Present thread indefinitely.
    // Bound WM_GETTEXT to 1 ms, once per second; retain the last known title on
    // timeout and use the explicit NDD main HWND even if the owner is busy.
    if (!registeredTitle(metadata.window, metadata.title) && metadata.window &&
            SendMessageTimeoutW(metadata.window, WM_GETTEXT,
            std::size(title), reinterpret_cast<LPARAM>(title),
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 1, &copied)) {
        metadata.title = title;
    }
    RECT client {};
    if (metadata.window && GetClientRect(metadata.window, &client)) {
        metadata.clientWidth = client.right - client.left;
        metadata.clientHeight = client.bottom - client.top;
    }

    // GetDesc describes the swapchain configuration. Record the real texture
    // independently so a resized client or unusual DXGI descriptor cannot be
    // mistaken for a change in rendering resolution. Release immediately: no
    // retained backbuffer reference may obstruct ResizeBuffers.
    ID3D11Texture2D* rawTexture = nullptr;
    metadata.textureWidth = metadata.textureHeight = metadata.textureSamples = 0;
    metadata.textureFormat = DXGI_FORMAT_UNKNOWN;
    if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&rawTexture))) && rawTexture) {
        ComOwner<ID3D11Texture2D> texture(rawTexture);
        D3D11_TEXTURE2D_DESC description {};
        texture->GetDesc(&description);
        metadata.textureWidth = description.Width;
        metadata.textureHeight = description.Height;
        metadata.textureFormat = description.Format;
        metadata.textureSamples = description.SampleDesc.Count;
    }

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&device))) && device) {
        ComOwner<ID3D11Device> ownDevice(device);
        metadata.device = device;
        metadata.adapterKnown = false;
        metadata.adapterLuid = {};
        metadata.adapterName.clear();
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice) {
            ComOwner<IDXGIDevice> ownDxgiDevice(dxgiDevice);
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
                ComOwner<IDXGIAdapter> ownAdapter(adapter);
                DXGI_ADAPTER_DESC description {};
                if (SUCCEEDED(adapter->GetDesc(&description))) {
                    metadata.adapterKnown = true;
                    metadata.adapterLuid = description.AdapterLuid;
                    metadata.adapterName = description.Description;
                }
            }
        }
    }
}

const char* captureBackbuffer(IDXGISwapChain* chain, const Metadata& metadata) {
    ID3D11Texture2D* rawBuffer = nullptr;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&rawBuffer))) || !rawBuffer) {
        return "capture_no_buffer";
    }
    ComOwner<ID3D11Texture2D> buffer(rawBuffer);
    D3D11_TEXTURE2D_DESC description {};
    buffer->GetDesc(&description);
    const bool rgba = description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        description.Format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        description.Format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    if (!rgba && !bgra) return "capture_unsupported_format";
    constexpr std::uint64_t maximumCaptureBytes = 256ULL * 1024 * 1024;
    if (!description.Width || !description.Height || description.Width > 16384 ||
        description.Height > 16384 || description.ArraySize != 1 ||
        static_cast<std::uint64_t>(description.Width) * description.Height * 4 > maximumCaptureBytes) {
        return "capture_unsupported_size";
    }

    ID3D11Device* rawDevice = nullptr;
    buffer->GetDevice(&rawDevice);
    if (!rawDevice) return "capture_no_device";
    ComOwner<ID3D11Device> device(rawDevice);
    ID3D11DeviceContext* rawContext = nullptr;
    device->GetImmediateContext(&rawContext);
    if (!rawContext) return "capture_no_context";
    ComOwner<ID3D11DeviceContext> context(rawContext);

    // No pipeline bindings are touched: only copies to observer-owned resources.
    ID3D11Texture2D* source = buffer.get();
    ComOwner<ID3D11Texture2D> resolved;
    D3D11_TEXTURE2D_DESC copyDescription = description;
    copyDescription.MipLevels = 1;
    copyDescription.ArraySize = 1;
    copyDescription.SampleDesc.Count = 1;
    copyDescription.SampleDesc.Quality = 0;
    copyDescription.Usage = D3D11_USAGE_DEFAULT;
    copyDescription.BindFlags = 0;
    copyDescription.CPUAccessFlags = 0;
    copyDescription.MiscFlags = 0;
    if (description.SampleDesc.Count > 1) {
        UINT formatSupport = 0;
        if (FAILED(device->CheckFormatSupport(description.Format, &formatSupport)) ||
            !(formatSupport & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE)) {
            return "capture_unsupported_msaa";
        }
        ID3D11Texture2D* rawResolved = nullptr;
        if (FAILED(device->CreateTexture2D(&copyDescription, nullptr, &rawResolved))) {
            return "capture_resolve_allocation_failed";
        }
        resolved.reset(rawResolved);
        context->ResolveSubresource(resolved.get(), 0, buffer.get(), 0, description.Format);
        source = resolved.get();
    }
    copyDescription.Usage = D3D11_USAGE_STAGING;
    copyDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* rawStaging = nullptr;
    if (FAILED(device->CreateTexture2D(&copyDescription, nullptr, &rawStaging))) {
        return "capture_staging_allocation_failed";
    }
    ComOwner<ID3D11Texture2D> staging(rawStaging);
    context->CopyResource(staging.get(), source);

    const std::size_t rowBytes = static_cast<std::size_t>(description.Width) * 4;
    std::vector<unsigned char> pixels(rowBytes * description.Height);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return "capture_map_failed";
    }
    if (mapped.RowPitch < rowBytes || !mapped.pData) {
        context->Unmap(staging.get(), 0);
        return "capture_invalid_pitch";
    }
    observeCaptureAlpha(roleName(metadata.role), metadata.window,
        static_cast<std::uint32_t>(description.Format), description.Width,
        description.Height, mapped.RowPitch, mapped.pData);
    for (UINT y = 0; y < description.Height; ++y) {
        const auto* src = static_cast<const unsigned char*>(mapped.pData) +
            static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* dest = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        for (UINT x = 0; x < description.Width; ++x) {
            dest[x * 4] = src[x * 4 + (rgba ? 2 : 0)];
            dest[x * 4 + 1] = src[x * 4 + 1];
            dest[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
            dest[x * 4 + 3] = 255;
        }
    }
    context->Unmap(staging.get(), 0);

    if (!CreateDirectoryW(L"desktop\\captures", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return "capture_directory_failed";
    }
    wchar_t path[192] {};
    std::swprintf(path, std::size(path), L"desktop\\captures\\%hs-%p.bmp",
        roleName(metadata.role), static_cast<void*>(metadata.window));
    const std::wstring temporary = std::wstring(path) + L".tmp";
    FILE* rawFile = nullptr;
    if (_wfopen_s(&rawFile, temporary.c_str(), L"wb") != 0 || !rawFile) {
        return "capture_open_failed";
    }
    BITMAPFILEHEADER fileHeader {};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER imageHeader {};
    imageHeader.biSize = sizeof(imageHeader);
    imageHeader.biWidth = static_cast<LONG>(description.Width);
    imageHeader.biHeight = -static_cast<LONG>(description.Height); // top-down
    imageHeader.biPlanes = 1;
    imageHeader.biBitCount = 32;
    imageHeader.biCompression = BI_RGB;
    imageHeader.biSizeImage = static_cast<DWORD>(pixels.size());
    bool ok = std::fwrite(&fileHeader, sizeof(fileHeader), 1, rawFile) == 1;
    ok = std::fwrite(&imageHeader, sizeof(imageHeader), 1, rawFile) == 1 && ok;
    ok = std::fwrite(pixels.data(), pixels.size(), 1, rawFile) == 1 && ok;
    ok = std::fclose(rawFile) == 0 && ok;
    if (!ok) return "capture_write_failed";
    if (!MoveFileExW(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        return "capture_replace_failed";
    }
    return "capture_saved";
}

void writeRecord(Telemetry& state, IDXGISwapChain* chain, const ChainState& sample,
                 Clock::time_point now, const char* event, const char* api,
                 UINT syncInterval, UINT flags) {
    const auto& m = sample.metadata;
    const double elapsed = std::chrono::duration<double, std::milli>(now - state.start).count();
    const double sampleMs = std::chrono::duration<double, std::milli>(now - sample.sampleStart).count();
    const double hz = sample.intervalTotalMs > 0.0 ?
        sample.intervalCount * 1000.0 / sample.intervalTotalMs : 0.0;
    const double mean = sample.intervalCount ? sample.intervalTotalMs / sample.intervalCount : 0.0;
    const double minimum = sample.intervalCount ? sample.intervalMinMs : 0.0;
    std::fprintf(state.output, "%s,%lu,%s,%s,%.3f,%p,%p,", state.sessionUtc,
        static_cast<unsigned long>(state.pid), event, api, elapsed,
        static_cast<void*>(chain), static_cast<void*>(m.window));
    csvText(state.output, utf8(m.title));
    std::fprintf(state.output, ",%s,%p,%u,%u,%u,%u,%u,%u,%d,%ld,%ld,%p,%d,%08lx:%08lx,",
        roleName(m.role), m.mainReference, m.width, m.height, static_cast<UINT>(m.format),
        m.buffers, m.samples, m.sampleQuality, m.windowed ? 1 : 0,
        m.clientWidth, m.clientHeight, m.device, m.adapterKnown ? 1 : 0,
        static_cast<unsigned long>(m.adapterLuid.HighPart),
        static_cast<unsigned long>(m.adapterLuid.LowPart));
    csvText(state.output, utf8(m.adapterName));
    std::fprintf(state.output, ",%llu,%llu,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%u,%u,%lu\n",
        static_cast<unsigned long long>(sample.totalCalls),
        static_cast<unsigned long long>(state.roleCalls[static_cast<std::size_t>(m.role)]),
        static_cast<unsigned long long>(sample.sampleCalls), sampleMs, hz, mean,
        minimum, sample.intervalMaxMs, syncInterval, flags,
        m.textureWidth, m.textureHeight, static_cast<UINT>(m.textureFormat),
        m.textureSamples, static_cast<unsigned long>(m.presentThread));
    std::fflush(state.output);
    if (std::ferror(state.output)) {
        failed.store(true, std::memory_order_relaxed);
    }
}

}  // namespace

void registerD3D11Window(void* window, const wchar_t* title) noexcept {
    if (!window) return;
    try {
        const auto hwnd = static_cast<HWND>(window);
        const std::wstring text = title ? title : L"";
        const Role role = classify(hwnd, text, nullptr);
        const int slot = role == Role::Main ? 0 : role == Role::MultiDisplay0 ? 1 :
            role == Role::MultiDisplay1 ? 2 : -1;
        auto& registry = windowRegistry();
        std::lock_guard<std::mutex> guard(registry.mutex);
        // Handles can be recycled for an unrelated window. An unknown creation
        // explicitly removes a previous record instead of inheriting its role.
        for (auto& output : registry.outputs) {
            if (output.window == hwnd) output = {};
        }
        if (slot >= 0) registry.outputs[static_cast<std::size_t>(slot)] = {hwnd, text};
    } catch (...) {
    }
}

void registerD3D11Window(void* window, const char* title) noexcept {
    wchar_t converted[256] {};
    if (title && !MultiByteToWideChar(CP_ACP, 0, title, -1, converted,
            static_cast<int>(std::size(converted)))) {
        // Known game titles fit in this fixed buffer; a failed conversion clears
        // any recycled identity and leaves diagnostics' bounded fallback intact.
        converted[0] = L'\0';
    }
    registerD3D11Window(window, converted);
}

const char* registeredD3D11WindowRole(void* window) noexcept {
    if (!window) return "unknown";
    try {
        auto& registry = windowRegistry();
        std::lock_guard<std::mutex> guard(registry.mutex);
        constexpr std::array<const char*, 3> names {{"main", "multidisplay0", "multidisplay1"}};
        for (std::size_t slot = 0; slot < registry.outputs.size(); ++slot) {
            if (registry.outputs[slot].window == window) return names[slot];
        }
    } catch (...) {
    }
    return "unknown";
}

void observeD3D11PresentFailure(IDXGISwapChain* swapchain, void* knownMainWindow,
                               unsigned int syncInterval, unsigned int presentFlags,
                               const char* presentApi, long result) noexcept {
    if (SUCCEEDED(result)) return;
    // Failure-only, independent of ordinary telemetry/capture enablement and
    // its file-failure latch. The hook itself branches before calling us.
    static std::atomic<unsigned int> failures {0};
    const unsigned int sequence = failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence > 32) return;
    const auto tick = GetTickCount64();
    const DWORD pid = GetCurrentProcessId();
    const DWORD thread = GetCurrentThreadId();
    SYSTEMTIME utc {};
    GetSystemTime(&utc);
    try {
        DXGI_SWAP_CHAIN_DESC description {};
        const HRESULT descriptionResult = swapchain ? swapchain->GetDesc(&description) : E_POINTER;
        ID3D11Device* rawDevice = nullptr;
        const HRESULT deviceResult = swapchain ?
            swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&rawDevice)) : E_POINTER;
        ComOwner<ID3D11Device> device(rawDevice);
        const bool reasonKnown = SUCCEEDED(deviceResult) && device != nullptr;
        const HRESULT reason = reasonKnown ? device->GetDeviceRemovedReason() : E_NOINTERFACE;
        const char* role = registeredD3D11WindowRole(description.OutputWindow);
        // One bounded append, using our own file handle rather than stdout.
        // FlushFileBuffers is intentional: the native engine exits(-1) directly
        // for most failed main Presents, bypassing normal telemetry cleanup.
        char line[1536] {};
        const int length = std::snprintf(line, sizeof(line),
            "schema=1 utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu thread=%lu tick=%llu "
            "sequence=%u api=%.16s swapchain=%p hwnd=%p known_main=%p role=%s "
            "sync=%u flags=0x%08X present_hr=0x%08lX desc_hr=0x%08lX "
            "width=%u height=%u format=%u windowed=%d device=%p device_hr=0x%08lX "
            "removal_reason_known=%u removal_reason=0x%08lX limit=32\r\n",
            static_cast<unsigned int>(utc.wYear), static_cast<unsigned int>(utc.wMonth),
            static_cast<unsigned int>(utc.wDay), static_cast<unsigned int>(utc.wHour),
            static_cast<unsigned int>(utc.wMinute), static_cast<unsigned int>(utc.wSecond),
            static_cast<unsigned int>(utc.wMilliseconds), static_cast<unsigned long>(pid),
            static_cast<unsigned long>(thread), static_cast<unsigned long long>(tick), sequence,
            presentApi ? presentApi : "unknown", static_cast<void*>(swapchain),
            static_cast<void*>(description.OutputWindow), knownMainWindow, role,
            syncInterval, presentFlags, static_cast<unsigned long>(result),
            static_cast<unsigned long>(descriptionResult), description.BufferDesc.Width,
            description.BufferDesc.Height, static_cast<unsigned int>(description.BufferDesc.Format),
            static_cast<int>(description.Windowed), static_cast<void*>(device.get()),
            static_cast<unsigned long>(deviceResult), reasonKnown ? 1U : 0U,
            static_cast<unsigned long>(reason));
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(line)) return;
        OutputDebugStringA(line);
        if (!CreateDirectoryW(L"desktop", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
        const HANDLE file = CreateFileW(L"desktop\\present-failures-v1.log", GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        // GENERIC_WRITE is required by FlushFileBuffers. Explicit EOF offsets
        // keep independent handles appending, instead of overwriting byte zero.
        // This remains synchronous: the handle has no FILE_FLAG_OVERLAPPED.
        OVERLAPPED endOfFile {};
        endOfFile.Offset = endOfFile.OffsetHigh = 0xFFFFFFFF;
        DWORD written = 0;
        const BOOL appended = WriteFile(file, line, static_cast<DWORD>(length), &written, &endOfFile);
        const DWORD writeError = appended ? ERROR_SUCCESS : GetLastError();
        const BOOL flushed = appended && written == static_cast<DWORD>(length) ? FlushFileBuffers(file) : FALSE;
        const DWORD flushError = flushed ? ERROR_SUCCESS :
            appended && written == static_cast<DWORD>(length) ? GetLastError() : ERROR_WRITE_FAULT;
        CloseHandle(file);
        if (!appended || written != static_cast<DWORD>(length) || !flushed) {
            char failureLine[256] {};
            std::snprintf(failureLine, sizeof(failureLine),
                "BoneEater Present failure record persistence failed pid=%lu sequence=%u "
                "written=%lu expected=%d write_error=%lu flush_error=%lu\n",
                static_cast<unsigned long>(pid), sequence, static_cast<unsigned long>(written), length,
                static_cast<unsigned long>(writeError), static_cast<unsigned long>(flushError));
            OutputDebugStringA(failureLine);
        }
    } catch (...) {
        // Diagnostics cannot change the original HRESULT or trigger a new exit.
    }
}

void observeD3D11Present(IDXGISwapChain* swapchain, void* knownMainWindow,
                         unsigned int syncInterval, unsigned int presentFlags,
                         const char* presentApi) noexcept {
    if (!swapchain || (presentFlags & DXGI_PRESENT_TEST)) {
        return;
    }
    static const bool cameraEnabled = diagnostics::optionalOutputEnabled() && environmentFlag(L"BONE_EATER_CAMERA_TELEMETRY", false);
    if (cameraEnabled && knownMainWindow) {
        DXGI_SWAP_CHAIN_DESC cameraOutput {};
        if (SUCCEEDED(swapchain->GetDesc(&cameraOutput)) && cameraOutput.OutputWindow == knownMainWindow) {
            bone_eater::camera::observeCameraState();
            bone_eater::render::observeNativeMainViewport(knownMainWindow);
        }
    }
    static const bool isEnabled = diagnostics::optionalOutputEnabled() && environmentFlag(L"BONE_EATER_GRAPHICS_TELEMETRY", true);
    static const bool captureEnabled = diagnostics::optionalOutputEnabled() && environmentFlag(L"BONE_EATER_CAPTURE", false);
    bool recordEnabled = isEnabled && !failed.load(std::memory_order_relaxed);
    if (!recordEnabled && !diagnosticWindowLayoutEnabled()) {
        return;
    }
    // Diagnostics must never propagate an allocation or filesystem failure into
    // the game's render loop. COM interfaces are queried only, never retained.
    try {
        static Telemetry state;
        std::lock_guard<std::mutex> lock(state.mutex);
        if (recordEnabled && !openOutput(state)) {
            failed.store(true, std::memory_order_relaxed);
            recordEnabled = false;
        }
        const auto now = Clock::now();
        auto& sample = state.chains[swapchain];
        const bool first = !sample.registered;
        DXGI_SWAP_CHAIN_DESC description {};
        if (FAILED(swapchain->GetDesc(&description))) {
            return;
        }
        const auto& prior = sample.metadata;
        const bool descriptorChanged = prior.window != description.OutputWindow ||
            prior.mainReference != knownMainWindow || prior.width != description.BufferDesc.Width ||
            prior.height != description.BufferDesc.Height || prior.format != description.BufferDesc.Format ||
            prior.buffers != description.BufferCount || prior.samples != description.SampleDesc.Count ||
            prior.sampleQuality != description.SampleDesc.Quality || prior.windowed != description.Windowed;
        bool changed = false;
        if (first || descriptorChanged || now >= sample.nextMetadata) {
            Metadata observed = prior; // strings copy only during metadata sampling
            if (observed.window != description.OutputWindow) {
                observed.title.clear();
                observed.clientWidth = observed.clientHeight = 0;
            }
            observed.window = description.OutputWindow;
            observed.mainReference = knownMainWindow;
            observed.width = description.BufferDesc.Width;
            observed.height = description.BufferDesc.Height;
            observed.format = description.BufferDesc.Format;
            observed.buffers = description.BufferCount;
            observed.samples = description.SampleDesc.Count;
            observed.sampleQuality = description.SampleDesc.Quality;
            observed.windowed = description.Windowed;
            observed.presentThread = GetCurrentThreadId();
            refreshSlowMetadata(swapchain, observed);
            observed.role = classify(observed.window, observed.title, knownMainWindow);
            if (!auxiliaryWindowManaged(observed.window)) {
                observeDiagnosticWindow(observed.window, roleName(observed.role),
                                        observed.textureWidth, observed.textureHeight);
            }
            changed = !(observed == prior);
            sample.metadata = std::move(observed);
            sample.nextMetadata = now + kMetadataInterval;
        }
        if (first) {
            sample.sampleStart = now;
            sample.registered = true;
        } else {
            const double intervalMs = std::chrono::duration<double, std::milli>(now - sample.lastPresent).count();
            ++sample.intervalCount;
            sample.intervalTotalMs += intervalMs;
            if (intervalMs < sample.intervalMinMs) sample.intervalMinMs = intervalMs;
            if (intervalMs > sample.intervalMaxMs) sample.intervalMaxMs = intervalMs;
        }
        sample.lastPresent = now;
        ++sample.totalCalls;
        ++sample.sampleCalls;
        ++state.roleCalls[static_cast<std::size_t>(sample.metadata.role)];
        const bool summary = now - sample.sampleStart >= kSummaryInterval;
        if (recordEnabled && (first || changed || summary)) {
            writeRecord(state, swapchain, sample, now,
                first ? "register" : changed ? "metadata" : "summary",
                presentApi ? presentApi : "Present", syncInterval, presentFlags);
        }
        if (recordEnabled && captureEnabled && sample.totalCalls >= 30 && now >= sample.nextCapture) {
            sample.nextCapture = now + kSummaryInterval;
            const char* captureEvent = captureBackbuffer(swapchain, sample.metadata);
            writeRecord(state, swapchain, sample, Clock::now(), captureEvent,
                presentApi ? presentApi : "Present", syncInterval, presentFlags);
        }
        if (summary) {
            sample.sampleStart = now;
            sample.sampleCalls = 0;
            sample.intervalCount = 0;
            sample.intervalTotalMs = 0.0;
            sample.intervalMinMs = std::numeric_limits<double>::max();
            sample.intervalMaxMs = 0.0;
        }
    } catch (...) {
        failed.store(true, std::memory_order_relaxed);
    }
}

}  // namespace bone_eater::render
