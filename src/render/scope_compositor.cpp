#include "render/scope_compositor.h"
#include "render/title_compositor.h"
#include "render/lobby_impact.h"
#include "render/d3d11_diagnostics.h"
#include "input/aim_state.h"
#include "input/scope_control.h"
#include "input/native_input_ownership.h"
#include "input/native_aim.h"
#include "input/native_wide_input.h"
#include "render/reticle_policy.h"
#include "render/native_reticle.h"
#include "render/native_viewport.h"
#include "render/native_hud.h"
#include "diagnostics/output_policy.h"
#include "util/logging.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <share.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace bone_eater::render {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kFreshnessLimit = std::chrono::milliseconds(250);
std::atomic<ULONGLONG> desktopReticleDrawAt {0};
std::array<std::atomic<ULONGLONG>, 2> desktopMeterDrawAt {};

struct ReleaseCom {
    template<typename T> void operator()(T* object) const noexcept {
        if (object) object->Release();
    }
};
template<typename T> using Com = std::unique_ptr<T, ReleaseCom>;

template<typename T, typename F>
HRESULT createCom(Com<T>& result, F create) {
    T* value = nullptr;
    const HRESULT hr = create(&value);
    if (FAILED(hr)) { if (value) value->Release(); return hr; }
    result.reset(value);
    return value ? hr : E_FAIL;
}

struct Resources {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11DeviceContext1> context1;
    Com<ID3DDeviceContextState> passState;
    Com<ID3D11Texture2D> source;
    Com<ID3D11ShaderResourceView> sourceView;
    Com<ID3D11VertexShader> vertexShader;
    Com<ID3D11PixelShader> pixelShader;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11BlendState> blend;
    Com<ID3D11RasterizerState> rasterizer;
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    bool pipelineReady = false;
};

struct State {
    std::mutex mutex;
    Resources gpu;
    HWND sourceWindow = nullptr;
    HWND targetWindow = nullptr;
    Clock::time_point sourceAt {};
    Clock::time_point start = Clock::now();
    Clock::time_point nextInitialization {};
    Clock::time_point nextLog {};
    Clock::time_point nextFailureLog {};
    std::uint64_t copies = 0;
    std::uint64_t draws = 0;
    UINT targetWidth = 0;
    UINT targetHeight = 0;
    FILE* log = nullptr;
    bool logAttempted = false;
    const char* lastEvent = "";
    input::NativeAimSnapshot nativeAim;
    input::NativeRaySnapshot nativeRay;
    NativeViewportSnapshot viewport;
    float centerX = 0, centerY = 0;
    ~State() { if (log) std::fclose(log); }
};

// SwapDeviceContextState isolates the entire graphics pipeline, including stages
// a hand-written partial state saver could miss. Fail closed if D3D11.1 context
// state support is unavailable instead of modifying a game's unknown bindings.
struct PassStateGuard {
    ID3D11DeviceContext1* context = nullptr;
    Com<ID3DDeviceContextState> previous;
    PassStateGuard(ID3D11DeviceContext1* value, ID3DDeviceContextState* pass) : context(value) {
        ID3DDeviceContextState* rawPrevious = nullptr;
        context->SwapDeviceContextState(pass, &rawPrevious);
        previous.reset(rawPrevious);
    }
    ~PassStateGuard() { context->SwapDeviceContextState(previous.get(), nullptr); }
};

bool enabled() noexcept {
    static const bool value = [] {
        wchar_t text[4] {};
        return GetEnvironmentVariableW(L"BONE_EATER_SCOPE_OVERLAY", text,
            static_cast<DWORD>(std::size(text))) == 1 && text[0] == L'1';
    }();
    return value;
}

bool angledLens() noexcept {
    static const bool value = [] {
        wchar_t text[16] {};
        return GetEnvironmentVariableW(L"BONE_EATER_SCOPE_SHAPE", text, 16) == 6 &&
            std::wstring_view(text) == L"angled";
    }();
    return value;
}

bool reticleEnabled() noexcept {
    static const bool value = [] {
        wchar_t text[4] {};
        return GetEnvironmentVariableW(L"BONE_EATER_DESKTOP_RETICLE", text, 4) == 1 && text[0] == L'1';
    }();
    return value;
}

DXGI_FORMAT unormFormat(DXGI_FORMAT format) noexcept {
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    return format;
}

bool supportedFormat(DXGI_FORMAT format) noexcept {
    const auto unorm = unormFormat(format);
    return unorm == DXGI_FORMAT_R8G8B8A8_UNORM || unorm == DXGI_FORMAT_B8G8R8A8_UNORM;
}

void event(State& state, const char* name, HRESULT hr = S_OK,
            const input::AimSnapshot* aim = nullptr) noexcept {
    if (!diagnostics::optionalOutputEnabled()) {
        // Successful/suppressed-draw telemetry is optional. Keep real graphics
        // failures visible, bounded independently of alternating event names.
        if (FAILED(hr)) {
            const auto now = Clock::now();
            if (now >= state.nextFailureLog) {
                state.nextFailureLog = now + std::chrono::seconds(5);
                try { log_warning("bone-eater", "Scope compositor failure: {} HRESULT=0x{:08x}",
                    name, static_cast<unsigned long>(hr)); } catch (...) {}
            }
        }
        return;
    }
    const auto now = Clock::now();
    if (std::strcmp(state.lastEvent, name) == 0 && now < state.nextLog) return;
    state.lastEvent = name;
    state.nextLog = now + std::chrono::seconds(5);
    if (!state.logAttempted) {
        state.logAttempted = true;
        CreateDirectoryW(L"desktop", nullptr);
        state.log = _wfsopen(L"desktop\\scope-v3.csv", L"ab", _SH_DENYWR);
        if (state.log) {
            _fseeki64(state.log, 0, SEEK_END);
            if (_ftelli64(state.log) == 0) std::fputs(
                "pid,elapsed_ms,thread,event,hresult,source_hwnd,target_hwnd,target_width,"
                "target_height,copy_count,draw_count,aim_generation,aim_x,aim_y,source_age_ms,"
                "native_generation,native_x,native_y,cabinet_x,cabinet_y,native_age_ms,"
                "viewport_status,overlay_x,overlay_y,ark_width,ark_height,ark_dimensions_valid,"
                "ray_generation,ray_x,ray_y,ray_camera,ray_age_ms,scope_off_known,scope_off\n", state.log);
        }
    }
    if (!state.log) return;
    const double elapsed = std::chrono::duration<double, std::milli>(now - state.start).count();
    const double age = state.copies ? std::chrono::duration<double, std::milli>(now - state.sourceAt).count() : -1;
    std::fprintf(state.log, "%lu,%.3f,%lu,%s,0x%08lx,%p,%p,%u,%u,%llu,%llu,%llu,%.9g,%.9g,%.3f,",
        static_cast<unsigned long>(GetCurrentProcessId()), elapsed,
        static_cast<unsigned long>(GetCurrentThreadId()), name, static_cast<unsigned long>(hr),
        static_cast<void*>(state.sourceWindow), static_cast<void*>(state.targetWindow),
        state.targetWidth, state.targetHeight, static_cast<unsigned long long>(state.copies),
        static_cast<unsigned long long>(state.draws),
        static_cast<unsigned long long>(aim ? aim->generation : 0),
        aim ? aim->normalizedX : 0, aim ? aim->normalizedY : 0, age);
    std::fprintf(state.log, "%llu,%.9g,%.9g,%d,%d,%.3f,%s,%.9g,%.9g,",
        static_cast<unsigned long long>(state.nativeAim.generation), state.nativeAim.logicalX,
        state.nativeAim.logicalY, state.nativeAim.cabinetX, state.nativeAim.cabinetY,
        state.nativeAim.valid ? std::chrono::duration<double, std::milli>(now - state.nativeAim.sampledAt).count() : -1,
        state.viewport.status, state.centerX, state.centerY);
    std::fprintf(state.log, "%d,%d,%d,%llu,%.9g,%.9g,%p,%.3f,%d,%d\n",
        state.nativeAim.observedArkWidth, state.nativeAim.observedArkHeight,
        state.nativeAim.dimensionsValid ? 1 : 0,
        static_cast<unsigned long long>(state.nativeRay.generation),
        state.nativeRay.logicalX, state.nativeRay.logicalY,
        reinterpret_cast<void*>(state.nativeRay.camera),
        state.nativeRay.valid ? std::chrono::duration<double, std::milli>(now - state.nativeRay.sampledAt).count() : -1,
        state.nativeRay.scopeOffKnown ? 1 : 0, state.nativeRay.scopeOff ? 1 : 0);
    std::fflush(state.log);
}

constexpr char kShaders[] = R"(
cbuffer ScopePlacement : register(b0) {
    float2 aimCenterPixels;
    float radiusPixels;
    float drawLens;
    float4 contentRect;
    float4 meterOptions;
};
Texture2D scopeImage : register(t0);
SamplerState scopeSampler : register(s0);
float4 vsMain(uint vertex : SV_VertexID) : SV_POSITION {
    float2 p = vertex == 0 ? float2(-1,-1) : vertex == 1 ? float2(-1,3) : float2(3,-1);
    return float4(p, 0, 1);
}
float4 psMain(float4 position : SV_POSITION) : SV_TARGET {
    // Side rulers are a separate fixed-aspect replacement. Anchor their short
    // horizontal ticks to both content edges, never to the old portrait width.
    // Both normal reticle and lens modes can draw them; menus never request them.
    if (meterOptions.x > 0.5 && position.y >= contentRect.y && position.y < contentRect.w) {
        float scale = meterOptions.y;
        float offset = (position.y - contentRect.y) / (12.0 * scale);
        float index = floor(offset + 0.5);
        float distanceY = abs(offset - index) * 12.0 * scale;
        float major = fmod(index,10.0) < 0.5 ? 1.8 : fmod(index,5.0) < 0.5 ? 1.35 : 1.0;
        float length = 18.0 * scale * major;
        float fromLeft = position.x - contentRect.x;
        float fromRight = contentRect.z - position.x;
        bool leftEnabled = fmod(meterOptions.x,2.0) > 0.5;
        bool rightEnabled = meterOptions.x > 1.5;
        if (distanceY <= 1.5 * scale && ((leftEnabled && fromLeft >= 0 && fromLeft < length) ||
                (rightEnabled && fromRight > 0 && fromRight <= length))) return float4(1,0.16,0.20,0.90);
    }
    if (drawLens < 0.5) {
        float2 d = abs(position.xy - aimCenterPixels);
        float scale = max(radiusPixels / 90.0, 0.5);
        bool menu = drawLens < -0.5;
        float ring = abs(length(d) - radiusPixels);
        // Main-view replacement: broken red circle and full horizontal/vertical
        // guides, with a quieter compact cross for menus. No portrait clipping.
        bool arc = !menu && min(d.x,d.y) > radiusPixels * 0.20;
        float guide = min(d.x,d.y);
        bool guideRange = !menu || max(d.x,d.y) < 16 * scale;
        bool border = (arc && ring < 3.2 * scale) || (guideRange && guide < 1.7 * scale);
        clip(border ? 1 : -1);
        bool core = (arc && ring < 2.0 * scale) || (guideRange && guide < 0.65 * scale);
        return core ? float4(1,0.16,0.20,0.90) : float4(0.12,0,0,0.65);
    }
    float2 q = (position.xy - aimCenterPixels) / radiusPixels;
    float radius = length(q);
    if (meterOptions.z > 0.5) {
        // Inset native aperture: long shallow slopes and short steep corner joins.
        // Only coverage changes; retain the original source UV/optical scale below.
        float2 a = abs(q) / float2(800.0/480.0, 1.0);
        float edge = max(a.x - 0.975, a.y - 0.950);
        float shallow = (a.y + 0.40*a.x - 1.070) / 1.077033;
        float steep = (a.y + 1.60*a.x - 2.140) / 1.886796;
        // Smooth maximum rounds joins inward, never exposing more native border.
        float blend = max(0.020 - abs(edge-shallow), 0.0) / 0.020;
        edge = max(edge, shallow) + blend*blend*0.005;
        blend = max(0.020 - abs(edge-steep), 0.0) / 0.020;
        edge = max(edge, steep) + blend*blend*0.005;
        radius = 1.0 + edge;
    }
    clip(1.0 - radius);
    // Centered 480x480 crop of measured 800x480 scope: UV x=.2.. .8, y=0..1.
    // This retains the optical center and scales both source axes equally.
    float2 uv = float2(0.5,0.5) + q * float2(0.3,0.5);
    float4 color = scopeImage.Sample(scopeSampler, uv);
    color.a = saturate((1.0 - radius) * radiusPixels);
    return color;
}
)";

HRESULT createPipeline(Resources& gpu) {
    Com<ID3D11Device1> device1;
    HRESULT hr = createCom(device1, [&](ID3D11Device1** out) {
        return gpu.device->QueryInterface(IID_PPV_ARGS(out));
    });
    if (FAILED(hr)) return hr;
    hr = createCom(gpu.context1, [&](ID3D11DeviceContext1** out) {
        return gpu.context->QueryInterface(IID_PPV_ARGS(out));
    });
    if (FAILED(hr)) return hr;
    const D3D_FEATURE_LEVEL level = gpu.device->GetFeatureLevel();
    const UINT stateFlags = (gpu.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
        D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    hr = createCom(gpu.passState, [&](ID3DDeviceContextState** out) {
        return device1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, out);
    });
    if (FAILED(hr)) return hr;

    // Windows 11 supplies this compiler; load only from System32. Compiled shader
    // objects remain on the device after the compiler DLL reference is released.
    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!compiler) return HRESULT_FROM_WIN32(GetLastError());
    struct CompilerLibrary { HMODULE value; ~CompilerLibrary() { FreeLibrary(value); } } library {compiler};
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    if (!compile) return E_NOINTERFACE;
    auto shader = [&](const char* entry, const char* profile, Com<ID3DBlob>& blob) {
        return createCom(blob, [&](ID3DBlob** out) {
            return compile(kShaders, sizeof(kShaders) - 1, "BoneEaterScope", nullptr, nullptr,
                entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, out, nullptr);
        });
    };
    Com<ID3DBlob> vertexCode, pixelCode;
    hr = shader("vsMain", "vs_4_0", vertexCode);
    if (FAILED(hr)) return hr;
    hr = shader("psMain", "ps_4_0", pixelCode);
    if (FAILED(hr)) return hr;
    hr = createCom(gpu.vertexShader, [&](ID3D11VertexShader** out) {
        return gpu.device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, out);
    });
    if (FAILED(hr)) return hr;
    hr = createCom(gpu.pixelShader, [&](ID3D11PixelShader** out) {
        return gpu.device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, out);
    });
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC constants {};
    constants.ByteWidth = 48;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = createCom(gpu.constants, [&](ID3D11Buffer** out) { return gpu.device->CreateBuffer(&constants, nullptr, out); });
    if (FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC sampler {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = createCom(gpu.sampler, [&](ID3D11SamplerState** out) { return gpu.device->CreateSamplerState(&sampler, out); });
    if (FAILED(hr)) return hr;
    D3D11_BLEND_DESC blend {};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = createCom(gpu.blend, [&](ID3D11BlendState** out) { return gpu.device->CreateBlendState(&blend, out); });
    if (FAILED(hr)) return hr;
    D3D11_RASTERIZER_DESC raster {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    hr = createCom(gpu.rasterizer, [&](ID3D11RasterizerState** out) {
        return gpu.device->CreateRasterizerState(&raster, out);
    });
    gpu.pipelineReady = SUCCEEDED(hr);
    return hr;
}

void copyScope(State& state, IDXGISwapChain* chain, HWND window) {
    Com<ID3D11Texture2D> backbuffer;
    HRESULT hr = createCom(backbuffer, [&](ID3D11Texture2D** out) { return chain->GetBuffer(0, IID_PPV_ARGS(out)); });
    if (FAILED(hr)) { event(state, "source_buffer_unavailable", hr); return; }
    D3D11_TEXTURE2D_DESC description {};
    backbuffer->GetDesc(&description);
    if (description.Width != 800 || description.Height != 480 || description.SampleDesc.Count != 1 ||
            description.ArraySize != 1 || !supportedFormat(description.Format)) {
        state.sourceAt = {};
        event(state, "source_format_not_measured");
        return;
    }
    Com<ID3D11Device> device;
    hr = createCom(device, [&](ID3D11Device** out) { return chain->GetDevice(IID_PPV_ARGS(out)); });
    if (FAILED(hr)) { event(state, "source_device_unavailable", hr); return; }
    if (device.get() != state.gpu.device.get()) {
        state.gpu = {};
        state.gpu.device = std::move(device);
        ID3D11DeviceContext* context = nullptr;
        state.gpu.device->GetImmediateContext(&context);
        state.gpu.context.reset(context);
        state.sourceAt = {};
        state.nextInitialization = {};
    }
    if (!state.gpu.context) { event(state, "source_context_unavailable"); return; }
    if (!state.gpu.source || state.gpu.sourceFormat != description.Format) {
        state.gpu.sourceView.reset();
        state.gpu.source.reset();
        state.sourceAt = {};
        D3D11_TEXTURE2D_DESC copy = description;
        copy.MipLevels = 1;
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        copy.Format = unormFormat(description.Format);
        hr = createCom(state.gpu.source, [&](ID3D11Texture2D** out) {
            return state.gpu.device->CreateTexture2D(&copy, nullptr, out);
        });
        if (FAILED(hr)) { event(state, "source_copy_allocation_failed", hr); return; }
        hr = createCom(state.gpu.sourceView, [&](ID3D11ShaderResourceView** out) {
            return state.gpu.device->CreateShaderResourceView(state.gpu.source.get(), nullptr, out);
        });
        if (FAILED(hr)) {
            state.gpu.source.reset();
            event(state, "source_view_failed", hr);
            return;
        }
        state.gpu.sourceFormat = description.Format;
    }
    state.gpu.context->CopyResource(state.gpu.source.get(), backbuffer.get());
    state.sourceAt = Clock::now();
    state.sourceWindow = window;
    ++state.copies;
    // Normal copies are silent: they must not alternate with draw/hidden events
    // and fill a CSV twice per frame. Main output reports summary counters.
}

void drawScope(State& state, IDXGISwapChain* chain, HWND window) {
    desktopReticleDrawAt.store(0);
    for (auto& side : desktopMeterDrawAt) side.store(0);
    state.targetWindow = window;
    const auto aim = input::readAimSnapshot();
    state.nativeAim = input::readNativeAimSnapshot();
    state.nativeRay = input::readNativeRaySnapshot();
    state.centerX = state.centerY = 0;
    const auto now = Clock::now();
    const bool replacement = reticleEnabled() && readNativeHudGeometry().valid;
    const auto scope = input::readScopeControlSnapshot();
    const auto scopeNow = GetTickCount64();
    const bool logicalScope = input::scopeControlRequested() ?
        (input::scopeControlNativeReady() && scope.enabled && scopeNow >= scope.updatedAtMs &&
         scopeNow - scope.updatedAtMs <= 100) : aim.scopeIntentHeld();
    const bool lensRequested = enabled() && logicalScope;
    if (!input::aimUsableAt(aim, now, kFreshnessLimit) || (!lensRequested && !replacement)) {
        event(state, "scope_not_requested", S_OK, &aim);
        return;
    }
    if (!std::isfinite(aim.normalizedX) || !std::isfinite(aim.normalizedY) ||
            aim.normalizedX < 0 || aim.normalizedX > 1 || aim.normalizedY < 0 || aim.normalizedY > 1) {
        event(state, "aim_invalid", S_OK, &aim); return;
    }
    state.viewport = readNativeMainViewport(window);
    if (!state.viewport.valid) { event(state, "native_viewport_unresolved", S_OK, &aim); return; }
    Com<ID3D11Device> device;
    HRESULT hr = createCom(device, [&](ID3D11Device** out) { return chain->GetDevice(IID_PPV_ARGS(out)); });
    if (FAILED(hr)) { event(state, "target_device_unavailable", hr, &aim); return; }
    // A menu reticle must initialize from the main device even before the gun
    // monitor has presented. A device change discards the old scope resources.
    if (device.get() != state.gpu.device.get()) {
        state.gpu = {};
        state.gpu.device = std::move(device);
        ID3D11DeviceContext* context = nullptr;
        state.gpu.device->GetImmediateContext(&context);
        state.gpu.context.reset(context);
        state.sourceAt = {};
        state.nextInitialization = {};
    }
    if (!state.gpu.context) { event(state, "target_context_unavailable", E_FAIL, &aim); return; }
    const auto desktop = input::readDesktopInputSnapshot();
    const bool rayFresh = state.nativeRay.valid && state.nativeRay.sampledAt <= now &&
        now - state.nativeRay.sampledAt <= kFreshnessLimit;
    const bool sourceFresh = state.gpu.sourceView && state.sourceAt != Clock::time_point{} &&
        state.sourceAt <= now && now - state.sourceAt <= kFreshnessLimit;
    const bool wideMenu = lobbyReticleRequested() || resultsReticleRequested();
    auto placement = chooseReticle(replacement, lensRequested && !wideMenu,
        desktop.valid, desktop.gameplay && !wideMenu, desktop.updatedAtMs, GetTickCount64(), desktop.x, desktop.y,
        rayFresh, state.nativeRay.scopeOffKnown, state.nativeRay.scopeOff,
        state.nativeRay.logicalX, state.nativeRay.logicalY, sourceFresh);
    // Lobby/results selection consumes desktop coordinates. Render its circle
    // and guides across the full target, without gameplay rulers.
    if(wideMenu && placement.kind == ReticleKind::Menu) placement.kind = ReticleKind::Gameplay;
    if (placement.kind == ReticleKind::None) { event(state, "reticle_state_unavailable", S_OK, &aim); return; }
    const bool lens = placement.kind == ReticleKind::Lens;
    if (!state.gpu.pipelineReady) {
        if (now < state.nextInitialization) return;
        state.nextInitialization = now + std::chrono::seconds(5);
        hr = createPipeline(state.gpu);
        if (FAILED(hr)) { event(state, "pipeline_unavailable", hr, &aim); return; }
    }
    Com<ID3D11Texture2D> backbuffer;
    hr = createCom(backbuffer, [&](ID3D11Texture2D** out) { return chain->GetBuffer(0, IID_PPV_ARGS(out)); });
    if (FAILED(hr)) { event(state, "target_buffer_unavailable", hr, &aim); return; }
    D3D11_TEXTURE2D_DESC target {};
    backbuffer->GetDesc(&target);
    state.targetWidth = target.Width;
    state.targetHeight = target.Height;
    if (!target.Width || !target.Height || target.SampleDesc.Count != 1 || !supportedFormat(target.Format)) {
        event(state, "target_format_unsupported", S_OK, &aim); return;
    }
    // Preserve the exact logical point used by the native scope world-ray call,
    // including the game's own smoothing and offsets. Letterbox margins and OS
    // client/backbuffer scaling affect presentation only, never game input.
    // Do not clamp the center to keep the circle on-screen; clip its border.
    const auto& v = state.viewport;
    state.centerX = static_cast<float>((v.left + static_cast<double>(placement.x) /
        v.logicalWidth * (v.right - v.left)) * target.Width / v.osClientWidth);
    state.centerY = static_cast<float>((v.top + static_cast<double>(placement.y) /
        v.logicalHeight * (v.bottom - v.top)) * target.Height / v.osClientHeight);
    if (!std::isfinite(state.centerX) || !std::isfinite(state.centerY)) {
        event(state, "native_placement_invalid", S_OK, &aim); return;
    }
    Com<ID3D11RenderTargetView> targetView;
    D3D11_RENDER_TARGET_VIEW_DESC view {};
    view.Format = unormFormat(target.Format);
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = createCom(targetView, [&](ID3D11RenderTargetView** out) {
        return state.gpu.device->CreateRenderTargetView(backbuffer.get(), &view, out);
    });
    if (FAILED(hr)) { event(state, "target_view_failed", hr, &aim); return; }

    const auto meters = chooseSideMeters(replacement && !wideMenu, placement.kind, nativeSideMetersVisible(),
        v.left, v.top, v.right, v.bottom, v.osClientWidth, v.osClientHeight, target.Width, target.Height);
    {
        PassStateGuard restore(state.gpu.context1.get(), state.gpu.passState.get());
        auto* context = state.gpu.context.get();
        const std::array<float, 12> constants {{
            state.centerX,
            state.centerY,
            static_cast<float>(target.Height) * (lens ? 0.18f : 0.08333f),
            lens ? 1.0f : placement.kind == ReticleKind::Menu ? -1.0f : 0.0f,
            meters.left, meters.top, meters.right, meters.bottom,
            static_cast<float>(meters.mask), meters.scale, angledLens() ? 1.0f : 0.0f, 0.0f
        }};
        context->UpdateSubresource(state.gpu.constants.get(), 0, nullptr, constants.data(), 0, 0);
        auto* constantBuffer = state.gpu.constants.get();
        auto* sampler = state.gpu.sampler.get();
        auto* source = lens ? state.gpu.sourceView.get() : nullptr;
        auto* output = targetView.get();
        D3D11_VIEWPORT viewport {0, 0, static_cast<float>(target.Width), static_cast<float>(target.Height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(state.gpu.rasterizer.get());
        context->OMSetRenderTargets(1, &output, nullptr);
        context->OMSetBlendState(state.gpu.blend.get(), nullptr, 0xffffffff);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(state.gpu.vertexShader.get(), nullptr, 0);
        context->PSSetShader(state.gpu.pixelShader.get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        context->PSSetSamplers(0, 1, &sampler);
        context->PSSetShaderResources(0, 1, &source);
        context->Draw(3, 0);
        // The compositor's saved context state must retain no swapchain-owned
        // RTV or SRV; unbind before restoring the game's entire context state.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
    }
    ++state.draws;
    if (replacement) desktopReticleDrawAt.store(GetTickCount64());
    for (unsigned index = 0; index < desktopMeterDrawAt.size(); ++index)
        if (meters.mask & (1U << index)) desktopMeterDrawAt[index].store(GetTickCount64());
    event(state, lens ? "overlay_draw" : "reticle_draw", S_OK, &aim);
}

} // namespace

void composeScopeD3D11Present(IDXGISwapChain* swapchain, void* knownMainWindow,
                              unsigned int presentFlags) noexcept {
    composeTitleD3D11Present(swapchain, knownMainWindow, presentFlags);
    composeLobbyImpact(swapchain, knownMainWindow, presentFlags);
    if ((!enabled() && !reticleEnabled()) || !swapchain || (presentFlags & DXGI_PRESENT_TEST)) return;
    try {
        static State state;
        std::unique_lock<std::mutex> guard(state.mutex, std::try_to_lock);
        if (!guard) return;
        DXGI_SWAP_CHAIN_DESC description {};
        if (FAILED(swapchain->GetDesc(&description))) return;
        const char* role = registeredD3D11WindowRole(description.OutputWindow);
        if (std::strcmp(role, "multidisplay0") == 0) copyScope(state, swapchain, description.OutputWindow);
        else if (std::strcmp(role, "main") == 0 || description.OutputWindow == knownMainWindow) {
            drawScope(state, swapchain, description.OutputWindow);
        }
    } catch (...) {
        // Opt-in composition must not unwind through the game's Present hook.
    }
}

bool desktopReticleReady() noexcept {
    const auto last = desktopReticleDrawAt.load();
    return last && GetTickCount64() - last <= 100;
}

bool desktopMetersReady(unsigned sideMask) noexcept {
    if (!sideMask || sideMask > 3) return false;
    const auto now = GetTickCount64();
    for (unsigned index = 0; index < desktopMeterDrawAt.size(); ++index) {
        if (!(sideMask & (1U << index))) continue;
        const auto last = desktopMeterDrawAt[index].load();
        if (!last || now - last > 100) return false;
    }
    return true;
}

} // namespace bone_eater::render
