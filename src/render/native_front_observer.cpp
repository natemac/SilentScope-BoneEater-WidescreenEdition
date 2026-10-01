#include "render/title_compositor.h"
#include "render/native_front_observer.h"
#include "diagnostics/output_policy.h"
#include "render/native_viewport.h"
#include "render/d3d11_diagnostics.h"
#include "render/capture_alpha.h"
#include "render/native_owned_hud_runtime.h"
#include "render/native_battle_background.h"
#include "render/wide_hud_compositor.h"
#include "render/native_hud.h"
#include "render/briefing_pale_quad.h"
#include "render/hud_seam_join.h"
#include "render/ranking_notice_identity.h"
#include "render/score_label_identity.h"
#include "render/lobby_impact.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "util/detour.h"
#include "util/logging.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <intrin.h>
#include <sstream>
#include <share.h>
#include <string>
#include <vector>

namespace bone_eater::render {
namespace {
bool wideHudRequested() noexcept {
    static const bool requested=[] {wchar_t value[4]{};const auto n=GetEnvironmentVariableW(L"BONE_EATER_WIDE_HUD",value,4);
        return n==0||(n==1&&value[0]==L'1');}();
    return requested;
}
using Microsoft::WRL::ComPtr;
using Clear = void(__fastcall*)(void*, const float*);
using Draw = void(__fastcall*)(void*);
Clear originalClear = nullptr;
Draw originalDraw = nullptr;
std::uintptr_t base = 0;
std::atomic<bool> enabled {false};
std::atomic<DWORD> ownerThread {0};
std::atomic<ULONGLONG> nextUnresolvedReport {0};
// Renderer member at global12E17A8, not the enclosing native window12E17A0.
// Constructor2EFF60 writes this exact vtable; the two objects differ by0x60.
constexpr std::uintptr_t kBackendVtable = 0x7045F8;
constexpr std::uint64_t kFrontKey = 0x7274406300001100;

struct Source {
    std::uintptr_t helper = 0, sprite = 0, material = 0, descriptor = 0;
    std::uintptr_t storage = 0, texture = 0, backend = 0, context = 0;
    std::uintptr_t rtvHolder = 0, srvHolder = 0, rtv = 0, srv = 0, resource = 0;
    bool operator==(const Source&) const = default;
};
struct ThreadState {
    Source source;
    Source lastResolvedDrawSource;
    ULONGLONG nextResolve = 0, nextClear = 0, nextDraw = 0;
    ULONGLONG lastResolvedDrawTick = 0, nextCapture = 0;
    std::uint64_t clears = 0, draws = 0;
    std::uint64_t lastCapturedDraws = 0, captureSequence = 0;
    std::uint64_t lastResolvedDrawClearSerial = 0;
    bool ready = false, inHook = false;
    std::uint64_t consumerClear = 0;
    unsigned consumerIntervals = 0, consumerChecks = 0, consumerMatches = 0, consumerRows = 0;
    unsigned consumerOwnedChecks = 0, consumerSrvQueries = 0;
    bool consumerActive = false;
};
thread_local ThreadState threadState;
struct Output { FILE* file = nullptr; bool failed = false; };
// Hooks persist to process exit. Avoid destructor ordering against the game DLL.
Output* output = nullptr;
bool diagnosticFrontEnabled = false;
bool captureEnabled = false; // Immutable after enabled's release publication.
bool consumerEnabled = false; // Same publication; no work when disabled.
bool presentationEnabled = false; // Additional opt-in, immutable after publication.
bool consumerOwnedOnly = false; // Broad cross-check remains the default.
Output* consumerOutput = nullptr;
ULONGLONG captureSession = 0;

template<typename T>
bool read(std::uintptr_t object, std::size_t offset, T& value) noexcept {
    if (!object || object > std::numeric_limits<std::uintptr_t>::max() - offset ||
            object + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(object + offset),
        &value, sizeof(T), &copied) && copied == sizeof(T);
}

bool resolveOnce(Source& s) noexcept {
    std::uintptr_t vt = 0;
    std::uint32_t slot = 0, state = 0;
    std::uint64_t key = 0;
    std::array<std::uint16_t, 2> dimensions {}, textureDimensions {};
    std::uintptr_t backReference = 0;
    return read(base, 0x13D8E98, s.helper) && read(s.helper, 0, vt) && vt == base + 0x10C07A8 &&
        read(s.helper, 0x30, slot) && slot == 34 && read(s.helper, 0x48, state) && state == 1 &&
        read(s.helper, 0x28, s.sprite) && read(s.sprite, 0, vt) && vt == base + 0x10CD1E8 &&
        read(s.sprite, 0x308, s.material) && read(s.material, 0, vt) && vt == base + 0x706570 &&
        read(s.material, 0x50, s.descriptor) && read(s.descriptor, 0, key) && key == kFrontKey &&
        read(s.descriptor, 0x18, dimensions) && dimensions[0] == 768 && dimensions[1] == 1366 &&
        read(s.descriptor, 0x60, s.storage) && read(s.storage, 0, s.texture) &&
        read(s.texture, 0x30, backReference) && backReference == s.descriptor &&
        read(s.texture, 8, textureDimensions) && textureDimensions == dimensions &&
        read(s.texture, 0x18, s.rtvHolder) && read(s.rtvHolder, 0, s.rtv) && s.rtv &&
        read(s.texture, 0x20, s.srvHolder) && read(s.srvHolder, 0, s.srv) && s.srv &&
        read(s.texture, 0x48, s.resource) && s.resource &&
        read(base, 0x12E17A8, s.backend) && read(s.backend, 0, vt) && vt == base + kBackendVtable &&
        read(base, 0x12E5818, s.context) && s.context;
}

bool resolve(Source& result) noexcept {
    Source repeated;
    return resolveOnce(result) && resolveOnce(repeated) && result == repeated;
}

// Only pointer/identity reads on unrelated native draws. No RPM, COM, clock,
// allocation, file I/O or page query before this exact native target match.
bool targetBoundFast(const Source& s, std::uintptr_t requiredBackend = 0) noexcept {
    if (!s.backend || !s.texture || (requiredBackend && requiredBackend != s.backend)) return false;
    __try {
        if (*reinterpret_cast<volatile std::uintptr_t*>(base + 0x12E17A8) != s.backend ||
                *reinterpret_cast<volatile std::uintptr_t*>(s.backend) != base + kBackendVtable) return false;
        auto* targets = reinterpret_cast<volatile std::uintptr_t*>(s.backend + 0x3706B8);
        for (unsigned i = 0; i < 8; ++i) if (targets[i] == s.texture) return true;
        return false;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                 GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

bool drawType(void* primitive) noexcept {
    __try {
        const auto kind = *reinterpret_cast<volatile std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(primitive) + 0x18);
        return kind < 8 && kind != 6; // Native type6 returns without any draw.
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                 GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

struct Sample {
    Source source;
    D3D11_TEXTURE2D_DESC texture {};
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN, srvFormat = DXGI_FORMAT_UNKNOWN;
    D3D11_BLEND_DESC blend {};
    std::array<float, 4> factors {}, clear {};
    UINT mask = 0;
    unsigned target = 0;
    bool defaultBlend = false;
};

// All objects are borrowed from a repeated native chain on the same native
// render thread. QueryInterface/GetResource/GetDevice references are scoped.
bool identity(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> first, second;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&first))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&second))) && first.Get() == second.Get();
}

const char* sample(const Source& expected, bool isDraw, Sample& result, bool requireFrontOutput = true) {
    Source current;
    if (!resolve(current) || !(current == expected) || (requireFrontOutput && !targetBoundFast(current))) return "native_chain_changed";
    result.source = current;
    auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(current.rtv);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(current.srv);
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(current.context);
    ComPtr<ID3D11Resource> targetResource, sampledResource;
    rtv->GetResource(&targetResource);
    srv->GetResource(&sampledResource);
    if (!identity(targetResource.Get(), sampledResource.Get()) ||
            !identity(targetResource.Get(), reinterpret_cast<IUnknown*>(current.resource))) return "resource_identity_mismatch";
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(targetResource.As(&texture))) return "not_texture2d";
    texture->GetDesc(&result.texture);
    if (result.texture.Width != 768 || result.texture.Height != 1366 ||
            result.texture.ArraySize != 1 || result.texture.SampleDesc.Count != 1) return "unexpected_texture_description";
    D3D11_RENDER_TARGET_VIEW_DESC rtvDescription {};
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription {};
    rtv->GetDesc(&rtvDescription);
    srv->GetDesc(&srvDescription);
    result.rtvFormat = rtvDescription.Format;
    result.srvFormat = srvDescription.Format;
    const auto typedRgba = [](DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
    // Typeless allocation27 may legally expose typed28/29 views. Preserve and
    // report each view's actual transfer convention; do not infer it from size.
    const bool compatibleFormats = result.texture.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? typedRgba(result.rtvFormat) && typedRgba(result.srvFormat)
        : typedRgba(result.texture.Format) && result.rtvFormat == result.texture.Format &&
            result.srvFormat == result.texture.Format;
    if (!compatibleFormats) return "unexpected_texture_view_formats";
    if (rtvDescription.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D ||
            rtvDescription.Texture2D.MipSlice != 0 ||
            srvDescription.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            srvDescription.Texture2D.MostDetailedMip != 0 || !srvDescription.Texture2D.MipLevels)
        return "unexpected_texture_views";
    ComPtr<ID3D11Device> contextDevice, textureDevice;
    context->GetDevice(&contextDevice);
    texture->GetDevice(&textureDevice);
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !identity(contextDevice.Get(), textureDevice.Get())) return "context_device_mismatch";
    if (isDraw) {
        std::array<ID3D11RenderTargetView*, 8> rawTargets {};
        context->OMGetRenderTargets(8, rawTargets.data(), nullptr);
        std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
        for (unsigned i = 0; i < targets.size(); ++i) targets[i].Attach(rawTargets[i]);
        bool found = false;
        for (unsigned i = 0; i < targets.size(); ++i) {
            if (!targets[i]) continue;
            ComPtr<ID3D11Resource> resource;
            targets[i]->GetResource(&resource);
            if (identity(resource.Get(), targetResource.Get())) {
                if (found) return "multiple_matching_targets";
                if (!identity(targets[i].Get(), rtv)) return "front_output_view_mismatch";
                result.target = i; found = true;
            }
        }
        if (!found) return "front_not_current_output";
        ComPtr<ID3D11BlendState> blend;
        context->OMGetBlendState(&blend, result.factors.data(), &result.mask);
        result.defaultBlend = !blend;
        if (blend) blend->GetDesc(&result.blend);
        else {
            for (auto& rt : result.blend.RenderTarget) {
                rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
                rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
                rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            }
        }
    }
    Source after;
    return resolve(after) && after == current && (!requireFrontOutput || targetBoundFast(after)) ? "resolved" : "native_chain_changed_after_query";
}

// Native final-blit ownership, queried only inside bounded consumer callbacks.
// No native resolver is called: 2F2750 can lazily create an SRV.
struct PresentationChain {
    std::uintptr_t backend = 0, device = 0, swapchain = 0;
    std::uintptr_t windowObject = 0, window = 0;
    std::uintptr_t texture = 0, resource = 0, rtvHolder = 0, srvHolder = 0;
    std::uintptr_t rtv = 0, srv = 0, backbufferRtv = 0;
    bool operator==(const PresentationChain&) const = default;
};

bool presentationOnce(const Source& source, PresentationChain& p) noexcept {
    std::uintptr_t vt = 0, context = 0, textureRtv = 0;
    return read(base, 0x12E17A8, p.backend) && p.backend == source.backend &&
        read(p.backend, 0, vt) && vt == base + kBackendVtable &&
        read(base, 0x12E5818, context) && context == source.context &&
        read(base, 0x12E17A0, p.windowObject) && read(p.windowObject, 0x28, p.window) && p.window &&
        read(p.backend, 0x36FDC0, p.device) && p.device &&
        read(p.backend, 0x36FDD0, p.swapchain) && p.swapchain &&
        read(p.backend, 0x3706A0, p.rtv) && p.rtv &&
        read(p.backend, 0x3706A8, p.backbufferRtv) && p.backbufferRtv &&
        read(p.backend, 0x3706B0, p.texture) && p.texture &&
        read(p.texture, 0x48, p.resource) && p.resource &&
        read(p.texture, 0x18, p.rtvHolder) && read(p.rtvHolder, 0, textureRtv) && textureRtv == p.rtv &&
        read(p.texture, 0x20, p.srvHolder) && read(p.srvHolder, 0, p.srv) && p.srv;
}

bool presentationResolve(const Source& source, PresentationChain& result) noexcept {
    PresentationChain repeated;
    Source current;
    return resolve(current) && current == source && presentationOnce(source, result) &&
        presentationOnce(source, repeated) && result == repeated;
}

bool presentationMainWindow(const PresentationChain& p) noexcept {
    const auto window = reinterpret_cast<HWND>(p.window);
    DWORD process = 0;
    return window && !std::strcmp(registeredD3D11WindowRole(window), "main") &&
        GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId();
}

struct PresentationSample {
    PresentationChain chain;
    const char* status = "disabled";
    std::uintptr_t backbufferResource = 0;
    // -1 means unverified, not a proved mismatch. Publish 0/1 only after all guards.
    int candidateIsPresentation = -1, backendIsMainBackbuffer = -1;
};

const char* samplePresentation(const Source& source, ID3D11Resource* candidate,
        PresentationSample& result) {
    if (ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId()) return "wrong_render_thread";
    PresentationChain before;
    if (!presentationResolve(source, before)) return "presentation_chain_unready";
    result.chain = before;
    if (!presentationMainWindow(before)) return "main_window_unverified";
    // Borrowed pointers are used only after a repeated same-thread owner chain.
    // Every acquired COM reference below dies before this callback returns.
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(source.context);
    auto* swapchain = reinterpret_cast<IDXGISwapChain*>(before.swapchain);
    auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(before.rtv);
    auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(before.srv);
    auto* backbufferRtv = reinterpret_cast<ID3D11RenderTargetView*>(before.backbufferRtv);
    ComPtr<ID3D11Device> contextDevice, swapchainDevice;
    context->GetDevice(&contextDevice);
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            !identity(contextDevice.Get(), reinterpret_cast<IUnknown*>(before.device)) ||
            FAILED(swapchain->GetDevice(IID_PPV_ARGS(&swapchainDevice))) ||
            !identity(contextDevice.Get(), swapchainDevice.Get())) return "presentation_device_mismatch";
    DXGI_SWAP_CHAIN_DESC description {};
    if (FAILED(swapchain->GetDesc(&description)) ||
            description.OutputWindow != reinterpret_cast<HWND>(before.window)) return "swapchain_window_mismatch";
    ComPtr<ID3D11Resource> presentationTarget, presentationInput, nativeBackbuffer;
    rtv->GetResource(&presentationTarget);
    srv->GetResource(&presentationInput);
    backbufferRtv->GetResource(&nativeBackbuffer);
    if (!identity(presentationTarget.Get(), presentationInput.Get()) ||
            !identity(presentationTarget.Get(), reinterpret_cast<IUnknown*>(before.resource)))
        return "presentation_view_resource_mismatch";
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return "get_buffer_failed";
    for (ID3D11Resource* resource : {presentationTarget.Get(), presentationInput.Get(),
            nativeBackbuffer.Get(), static_cast<ID3D11Resource*>(buffer.Get()), candidate}) {
        if (!resource) return "presentation_resource_missing";
        ComPtr<ID3D11Device> device;
        resource->GetDevice(&device);
        if (!identity(contextDevice.Get(), device.Get())) return "presentation_resource_device_mismatch";
    }
    // Distinguish a failed identity query from a proved unequal identity. The
    // general bool identity() helper deliberately conflates those for guards.
    ComPtr<IUnknown> candidateIdentity, presentationIdentity, nativeBufferIdentity, bufferIdentity;
    if (FAILED(candidate->QueryInterface(IID_PPV_ARGS(&candidateIdentity))) || !candidateIdentity ||
            FAILED(presentationTarget->QueryInterface(IID_PPV_ARGS(&presentationIdentity))) || !presentationIdentity ||
            FAILED(nativeBackbuffer->QueryInterface(IID_PPV_ARGS(&nativeBufferIdentity))) || !nativeBufferIdentity ||
            FAILED(buffer->QueryInterface(IID_PPV_ARGS(&bufferIdentity))) || !bufferIdentity)
        return "presentation_identity_query_failed";
    const bool candidateMatch = candidateIdentity.Get() == presentationIdentity.Get();
    const bool bufferMatch = nativeBufferIdentity.Get() == bufferIdentity.Get();
    // A repeated GetBuffer proves that the same buffer is still current; neither
    // dimensions nor a recycled pointer is accepted as ownership evidence.
    ComPtr<ID3D11Texture2D> bufferAgain;
    DXGI_SWAP_CHAIN_DESC descriptionAgain {};
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bufferAgain))) ||
            !identity(buffer.Get(), bufferAgain.Get()) || FAILED(swapchain->GetDesc(&descriptionAgain)) ||
            std::memcmp(&description, &descriptionAgain, sizeof(description))) return "swapchain_changed_during_query";
    PresentationChain after;
    if (!presentationResolve(source, after) || !(after == before) || !presentationMainWindow(after))
        return "presentation_chain_changed_after_query";
    result.backbufferResource = reinterpret_cast<std::uintptr_t>(buffer.Get());
    result.candidateIsPresentation = candidateMatch ? 1 : 0;
    result.backendIsMainBackbuffer = bufferMatch ? 1 : 0;
    return "resolved";
}

struct ConsumerRow {
    PresentationSample presentation;
    Source source;
    std::uintptr_t primitive = 0, caller = 0, shader = 0, outputView = 0, outputResource = 0;
    std::array<std::uint64_t, 2> slots {};
    unsigned outputIndex = 0;
    D3D11_TEXTURE2D_DESC texture {};
    UINT viewportCount = 0, scissorCount = 0, mask = 0;
    std::string viewports, scissors;
    D3D11_BLEND_DESC blend {};
    std::array<float, 4> factors {};
    bool defaultBlend = true, frontIsOutput = false;
};

void writeConsumerRow(const char* event, const ConsumerRow& s) noexcept {
    if (!consumerOutput || consumerOutput->failed) return;
    if (!consumerOutput->file) {
        CreateDirectoryW(L"desktop", nullptr);
        consumerOutput->file = _wfsopen(L"desktop/front-consumers-v3.csv", L"a", _SH_DENYWR);
        if (!consumerOutput->file) { consumerOutput->failed = true; return; }
        if (_fseeki64(consumerOutput->file, 0, SEEK_END)) { consumerOutput->failed = true; return; }
        if (_ftelli64(consumerOutput->file) == 0) std::fputs(
            "schema,pid,tick_ms,thread,interval,clear_serial,primitive_checks,matches,rows,event,primitive,caller_rva,front_helper,front_sprite,front_resource,front_srv,context,ps_slots_lo,ps_slots_hi,ps,output_index,output_rtv,output_resource,output_width,output_height,output_format,front_is_output,viewport_count,viewports,scissor_count,scissors,default_blend,alpha_to_coverage,independent_blend,blend_enabled,src_rgb,dst_rgb,op_rgb,src_alpha,dst_alpha,op_alpha,write_mask,factor_r,factor_g,factor_b,factor_a,sample_mask,front_material,primitive_matches_front_material,owned_only,cached_owned_checks,srv_queries,presentation_enabled,presentation_status,presentation_backend,main_hwnd,presentation_swapchain,presentation_native_texture,presentation_resource,presentation_srv,presentation_rtv,backend_backbuffer_rtv,swapchain_buffer_resource,candidate_is_presentation_texture,backend_rtv_is_exact_main_backbuffer\n", consumerOutput->file);
    }
    const auto& t = threadState;
    const auto& b = s.blend.RenderTarget[s.blend.IndependentBlendEnable ? s.outputIndex : 0];
    const int written = std::fprintf(consumerOutput->file,
        "3,%lu,%llu,%lu,%u,%llu,%u,%u,%u,%s,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%u,%llx,%llx,%u,%u,%u,%u,%u,%s,%u,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%u,%llx,%u,%u,%u,%u,%u,%s,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%d,%d\n",
        GetCurrentProcessId(), GetTickCount64(), GetCurrentThreadId(), t.consumerIntervals,
        t.consumerClear, t.consumerChecks, t.consumerMatches, t.consumerRows, event,
        s.primitive, s.caller, s.source.helper, s.source.sprite, s.source.resource, s.source.srv,
        s.source.context, s.slots[0], s.slots[1], s.shader, s.outputIndex, s.outputView,
        s.outputResource, s.texture.Width, s.texture.Height, static_cast<unsigned>(s.texture.Format),
        s.frontIsOutput ? 1U : 0U, s.viewportCount, s.viewports.c_str(), s.scissorCount, s.scissors.c_str(),
        s.defaultBlend ? 1U : 0U, s.blend.AlphaToCoverageEnable ? 1U : 0U,
        s.blend.IndependentBlendEnable ? 1U : 0U, b.BlendEnable ? 1U : 0U,
        static_cast<unsigned>(b.SrcBlend), static_cast<unsigned>(b.DestBlend), static_cast<unsigned>(b.BlendOp),
        static_cast<unsigned>(b.SrcBlendAlpha), static_cast<unsigned>(b.DestBlendAlpha),
        static_cast<unsigned>(b.BlendOpAlpha), static_cast<unsigned>(b.RenderTargetWriteMask),
        s.factors[0], s.factors[1], s.factors[2], s.factors[3], s.mask,
        s.source.material, s.primitive && s.primitive == s.source.material ? 1U : 0U,
        consumerOwnedOnly ? 1U : 0U, t.consumerOwnedChecks, t.consumerSrvQueries,
        presentationEnabled ? 1U : 0U, s.presentation.status,
        s.presentation.chain.backend, s.presentation.chain.window, s.presentation.chain.swapchain,
        s.presentation.chain.texture, s.presentation.chain.resource, s.presentation.chain.srv,
        s.presentation.chain.rtv, s.presentation.chain.backbufferRtv, s.presentation.backbufferResource,
        s.presentation.candidateIsPresentation, s.presentation.backendIsMainBackbuffer);
    if (written < 0 || std::fflush(consumerOutput->file) || std::ferror(consumerOutput->file)) consumerOutput->failed = true;
}

// Start at a verified native front clear, then examine a whole clear interval
// (subject to the hard check cap), not a time-throttled subset of its draws.
void consumerAfterClear(void* backend) {
    auto& t = threadState;
    if (!t.ready || ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
            !targetBoundFast(t.source, reinterpret_cast<std::uintptr_t>(backend)) ||
            !t.clears || t.clears == t.consumerClear) return;
    if (t.consumerActive) {
        writeConsumerRow(t.consumerRows >= 256 ? "interval_end_row_cap_reached" : "interval_end", {});
        t.consumerActive = false;
    }
    if (t.consumerIntervals >= 24 || (t.consumerClear && t.clears - t.consumerClear < 600)) return;
    t.consumerClear = t.clears;
    ++t.consumerIntervals;
    t.consumerChecks = t.consumerMatches = t.consumerRows = 0;
    t.consumerOwnedChecks = t.consumerSrvQueries = 0;
    t.consumerActive = true;
    writeConsumerRow("interval_begin", {});
}

void observeConsumer(void* primitive, std::uintptr_t caller) {
    auto& t = threadState;
    if (!t.consumerActive || !t.ready || ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId()) return;
    if (++t.consumerChecks > 4096) {
        --t.consumerChecks;
        writeConsumerRow("check_limit", {});
        t.consumerActive = false;
        return;
    }
    // This is the separately allocated sprite+308 object, NOT sprite+6F0.
    // Reject unrelated draws with pointer comparison only; cached equality never
    // authorizes borrowed COM access without the repeated full validation below.
    const auto primitiveAddress = reinterpret_cast<std::uintptr_t>(primitive);
    const bool cachedOwned = primitiveAddress && primitiveAddress == t.source.material;
    if (cachedOwned) ++t.consumerOwnedChecks;
    if (consumerOwnedOnly && !cachedOwned) return;
    if (!drawType(primitive)) return;
    Sample validated;
    // Repeated chain resolution precedes any borrowed COM access; this version
    // deliberately permits the front resource as INPUT rather than output.
    if (std::strcmp(sample(t.source, false, validated, false), "resolved")) {
        writeConsumerRow("source_invalid", {}); t.consumerActive = false; return;
    }
    const auto& source = validated.source;
    if (consumerOwnedOnly && primitiveAddress != source.material) return;
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(source.context);
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> rawViews {};
    ++t.consumerSrvQueries;
    context->PSGetShaderResources(0, static_cast<UINT>(rawViews.size()), rawViews.data());
    std::array<ComPtr<ID3D11ShaderResourceView>, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views;
    for (unsigned i = 0; i < views.size(); ++i) views[i].Attach(rawViews[i]);
    ConsumerRow row;
    for (unsigned i = 0; i < views.size(); ++i) {
        if (!views[i] || !identity(views[i].Get(), reinterpret_cast<IUnknown*>(source.srv))) continue;
        ComPtr<ID3D11Resource> resource;
        views[i]->GetResource(&resource);
        if (identity(resource.Get(), reinterpret_cast<IUnknown*>(source.resource))) row.slots[i / 64] |= std::uint64_t{1} << (i % 64);
    }
    if (!row.slots[0] && !row.slots[1]) return;
    ++t.consumerMatches;
    row.source = source;
    row.primitive = reinterpret_cast<std::uintptr_t>(primitive);
    row.caller = caller >= base && caller - base < 0x6F8000 ? caller - base : 0;
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    row.shader = reinterpret_cast<std::uintptr_t>(shader.Get());
    ComPtr<ID3D11BlendState> blend;
    context->OMGetBlendState(&blend, row.factors.data(), &row.mask);
    row.defaultBlend = !blend;
    if (blend) blend->GetDesc(&row.blend);
    else for (auto& b : row.blend.RenderTarget) {
        b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_ONE;
        b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_ZERO;
        b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports {};
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors {};
    row.viewportCount = static_cast<UINT>(viewports.size());
    row.scissorCount = static_cast<UINT>(scissors.size());
    context->RSGetViewports(&row.viewportCount, viewports.data());
    context->RSGetScissorRects(&row.scissorCount, scissors.data());
    if (row.viewportCount > viewports.size() || row.scissorCount > scissors.size()) return;
    std::ostringstream viewportText, scissorText;
    for (unsigned i = 0; i < row.viewportCount; ++i) {
        const auto& v = viewports[i];
        if (i) viewportText << '|';
        viewportText << v.TopLeftX << ':' << v.TopLeftY << ':' << v.Width << ':' << v.Height << ':' << v.MinDepth << ':' << v.MaxDepth;
    }
    for (unsigned i = 0; i < row.scissorCount; ++i) {
        const auto& s = scissors[i];
        if (i) scissorText << '|';
        scissorText << s.left << ':' << s.top << ':' << s.right << ':' << s.bottom;
    }
    row.viewports = viewportText.str(); row.scissors = scissorText.str();
    std::array<ID3D11RenderTargetView*, 8> rawTargets {};
    context->OMGetRenderTargets(static_cast<UINT>(rawTargets.size()), rawTargets.data(), nullptr);
    std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
    std::array<ComPtr<ID3D11Resource>, 8> resources;
    std::array<D3D11_TEXTURE2D_DESC, 8> descriptions {};
    for (unsigned i = 0; i < targets.size(); ++i) targets[i].Attach(rawTargets[i]);
    ComPtr<ID3D11Device> contextDevice;
    context->GetDevice(&contextDevice);
    for (unsigned i = 0; i < targets.size(); ++i) {
        if (!targets[i]) continue;
        targets[i]->GetResource(&resources[i]);
        if (!resources[i]) return;
        ComPtr<ID3D11Device> device;
        resources[i]->GetDevice(&device);
        if (!identity(device.Get(), contextDevice.Get())) return;
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resources[i].As(&texture))) texture->GetDesc(&descriptions[i]);
    }
    Source after;
    if (!resolve(after) || !(after == source)) {
        writeConsumerRow("source_changed_after_match", {}); t.consumerActive = false; return;
    }
    bool anyOutput = false;
    for (unsigned i = 0; i < targets.size(); ++i) {
        if (!targets[i]) continue;
        anyOutput = true;
        if (t.consumerRows >= 256) continue;
        row.outputIndex = i;
        row.outputView = reinterpret_cast<std::uintptr_t>(targets[i].Get());
        row.outputResource = reinterpret_cast<std::uintptr_t>(resources[i].Get());
        row.texture = descriptions[i];
        row.frontIsOutput = identity(resources[i].Get(), reinterpret_cast<IUnknown*>(source.resource));
        row.presentation = {};
        if (presentationEnabled)
            row.presentation.status = samplePresentation(source, resources[i].Get(), row.presentation);
        ++t.consumerRows;
        writeConsumerRow("front_input_output_candidate", row);
    }
    if (!anyOutput && t.consumerRows < 256) {
        ++t.consumerRows; writeConsumerRow("front_input_no_color_output", row);
    }
}

void writeRow(const char* event, const char* status, const Sample& s) noexcept {
    if (!output || output->failed) return;
    if (!output->file) {
        CreateDirectoryW(L"desktop", nullptr);
        output->file = _wfsopen(L"desktop/front-layer-v2.csv", L"a", _SH_DENYWR);
        if (!output->file) { output->failed = true; return; }
        if (_fseeki64(output->file, 0, SEEK_END)) { output->failed = true; return; }
        if (_ftelli64(output->file) == 0) std::fputs(
            "schema,pid,tick_ms,thread,event,status,clears_observed,primitive_draws_observed,helper,sprite,descriptor,native_texture,resource,rtv,srv,context,width,height,resource_format,rtv_format,srv_format,clear_r,clear_g,clear_b,clear_a,target_index,default_blend,alpha_to_coverage,independent_blend,blend_enabled,src_rgb,dst_rgb,op_rgb,src_alpha,dst_alpha,op_alpha,write_mask,factor_r,factor_g,factor_b,factor_a,sample_mask\n", output->file);
    }
    const auto& b = s.blend.RenderTarget[s.blend.IndependentBlendEnable ? s.target : 0];
    const int written = std::fprintf(output->file,
        "2,%lu,%llu,%lu,%s,%s,%llu,%llu,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%u\n",
        GetCurrentProcessId(), GetTickCount64(), GetCurrentThreadId(), event, status,
        threadState.clears, threadState.draws, s.source.helper, s.source.sprite, s.source.descriptor,
        s.source.texture, s.source.resource, s.source.rtv, s.source.srv, s.source.context,
        s.texture.Width, s.texture.Height, static_cast<unsigned>(s.texture.Format),
        static_cast<unsigned>(s.rtvFormat), static_cast<unsigned>(s.srvFormat),
        s.clear[0], s.clear[1], s.clear[2], s.clear[3], s.target, s.defaultBlend ? 1U : 0U,
        s.blend.AlphaToCoverageEnable ? 1U : 0U, s.blend.IndependentBlendEnable ? 1U : 0U,
        b.BlendEnable ? 1U : 0U, static_cast<unsigned>(b.SrcBlend), static_cast<unsigned>(b.DestBlend),
        static_cast<unsigned>(b.BlendOp), static_cast<unsigned>(b.SrcBlendAlpha), static_cast<unsigned>(b.DestBlendAlpha),
        static_cast<unsigned>(b.BlendOpAlpha), static_cast<unsigned>(b.RenderTargetWriteMask),
        s.factors[0], s.factors[1], s.factors[2], s.factors[3], s.mask);
    if (written < 0 || std::fflush(output->file) || std::ferror(output->file)) output->failed = true;
}

void observeClear(void* backend, const float* color) {
    auto& t = threadState;
    const auto thread = GetCurrentThreadId();
    const auto owner = ownerThread.load(std::memory_order_acquire);
    if (owner && owner != thread) return;
    const auto now = GetTickCount64();
    if (now >= t.nextResolve) {
        t.nextResolve = now + 1000;
        t.ready = resolve(t.source);
        if (!t.ready) {
            auto next = nextUnresolvedReport.load(std::memory_order_relaxed);
            if (now >= next && nextUnresolvedReport.compare_exchange_strong(next, now + 5000)) {
                log_info("bone-eater", "Front observer is receiving native clears; retained slot34 identity chain is not ready");
            }
        }
    }
    if (!t.ready || !targetBoundFast(t.source, reinterpret_cast<std::uintptr_t>(backend))) return;
    ++t.clears;
    if (now < t.nextClear) return;
    t.nextClear = now + 1000;
    Sample s;
    s.source = t.source;
    if (!read(color ? reinterpret_cast<std::uintptr_t>(color) : base + 0x704828, 0, s.clear)) return;
    for (const auto value : s.clear) if (!std::isfinite(value)) return;
    DWORD empty = 0;
    if (!owner && !ownerThread.compare_exchange_strong(empty, thread, std::memory_order_acq_rel) && empty != thread) return;
    const char* status = sample(t.source, false, s);
    writeRow("native_clear", status, s);
}

void observeDraw(void* primitive) {
    auto& t = threadState;
    if (!t.ready || ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
            !targetBoundFast(t.source) || !drawType(primitive)) return;
    ++t.draws;
    const auto now = GetTickCount64();
    if (now < t.nextDraw) return;
    t.nextDraw = now + 100; // At most ten sampled draw states/second.
    Sample s;
    s.source = t.source;
    const char* status = sample(t.source, true, s);
    if (!std::strcmp(status, "resolved")) {
        t.lastResolvedDrawTick = GetTickCount64();
        t.lastResolvedDrawSource = t.source;
        t.lastResolvedDrawClearSerial = t.clears;
    }
    writeRow("post_native_primitive_draw", status, s);
}

bool replaceBmp(const wchar_t* path, UINT width, UINT height,
        const std::vector<unsigned char>& pixels) {
    const std::wstring temporary = std::wstring(path) + L".tmp";
    FILE* file = _wfsopen(temporary.c_str(), L"wb", _SH_DENYRW);
    if (!file) return false;
    BITMAPFILEHEADER header {};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    BITMAPINFOHEADER info {};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
    ok = std::fwrite(&info, sizeof(info), 1, file) == 1 && ok;
    ok = std::fwrite(pixels.data(), pixels.size(), 1, file) == 1 && ok;
    ok = std::fclose(file) == 0 && ok;
    return ok && MoveFileExW(temporary.c_str(), path, MOVEFILE_REPLACE_EXISTING);
}

bool replaceManifest(const char* content, std::size_t bytes) noexcept {
    constexpr auto temporary = L"desktop/captures/front-layer-latest.json.tmp";
    constexpr auto path = L"desktop/captures/front-layer-latest.json";
    FILE* file = _wfsopen(temporary, L"wb", _SH_DENYRW);
    if (!file) return false;
    const bool written = std::fwrite(content, bytes, 1, file) == 1;
    const bool closed = std::fclose(file) == 0;
    return written && closed && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING);
}

struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    bool active = false;
    ~MappedTexture() { if (active) context->Unmap(texture, 0); }
    void unmap() noexcept { if (active) { context->Unmap(texture, 0); active = false; } }
};

const char* capture(const Source& source, Sample& s, ULONGLONG begin) {
    const char* status = sample(source, false, s);
    if (std::strcmp(status, "resolved")) return status;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(reinterpret_cast<IUnknown*>(source.resource)->QueryInterface(IID_PPV_ARGS(&texture))))
        return "capture_source_query_failed";
    auto* context = reinterpret_cast<ID3D11DeviceContext*>(source.context);
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    auto description = s.texture;
    // Only mip0 is captured; CopySubresourceRegion avoids copying any other
    // native mips. Neither source bindings nor its pixels are modified.
    description.MipLevels = 1;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) return "capture_staging_failed";
    const std::size_t rowBytes = static_cast<std::size_t>(description.Width) * 4;
    // Exact validated dimensions bound each allocation to about4MiB.
    std::vector<unsigned char> rgb(rowBytes * description.Height), alpha(rgb.size());
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    MappedTexture lifetime {context, staging.Get()};
    // Explicit diagnostics-only synchronization: Map can wait for the GPU.
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return "capture_map_failed";
    lifetime.active = true;
    if (!mapped.pData || mapped.RowPitch < rowBytes ||
            mapped.RowPitch > std::numeric_limits<std::size_t>::max() / description.Height)
        return "capture_pitch_invalid";
    if (!CreateDirectoryW(L"desktop", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return "capture_directory_failed";
    if (!CreateDirectoryW(L"desktop/captures", nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return "capture_directory_failed";
    // Logical front role, no HWND: this is the retained native resource. View
    // formats28/29 share exact RGBA byte layout even for typeless allocation27.
    observeCaptureAlpha("multidisplay1", nullptr, static_cast<unsigned>(s.srvFormat),
        description.Width, description.Height, mapped.RowPitch, mapped.pData);
    for (UINT y = 0; y < description.Height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) +
            static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* rgbRow = rgb.data() + static_cast<std::size_t>(y) * rowBytes;
        auto* alphaRow = alpha.data() + static_cast<std::size_t>(y) * rowBytes;
        for (UINT x = 0; x < description.Width; ++x) {
            const auto p = static_cast<std::size_t>(x) * 4;
            rgbRow[p] = row[p + 2]; rgbRow[p + 1] = row[p + 1]; rgbRow[p + 2] = row[p]; rgbRow[p + 3] = 255;
            alphaRow[p] = alphaRow[p + 1] = alphaRow[p + 2] = row[p + 3]; alphaRow[p + 3] = 255;
        }
    }
    lifetime.unmap();
    const auto readbackFinished = GetTickCount64();
    Source after;
    if (!resolve(after) || !(after == source)) return "capture_source_changed";
    const auto sequence = threadState.captureSequence + 1;
    const unsigned slot = static_cast<unsigned>(sequence & 1);
    // PID plus launch observation avoids replacing an older run's published
    // pair (including reused PIDs). Two slots bound this run's image cache.
    wchar_t rgbPath[128] {}, alphaPath[128] {};
    std::swprintf(rgbPath, std::size(rgbPath), L"desktop/captures/front-layer-%lu-%llu-%u-rgb.bmp", GetCurrentProcessId(), captureSession, slot);
    std::swprintf(alphaPath, std::size(alphaPath), L"desktop/captures/front-layer-%lu-%llu-%u-alpha.bmp", GetCurrentProcessId(), captureSession, slot);
    if (!replaceBmp(rgbPath, description.Width, description.Height, rgb) ||
            !replaceBmp(alphaPath, description.Width, description.Height, alpha)) return "capture_image_write_failed";
    char manifest[1536] {};
    const int size = std::snprintf(manifest, sizeof(manifest),
        "{\n  \"schema\": 1, \"pid\": %lu, \"thread\": %lu, \"sequence\": %llu,\n"
        "  \"begin_tick_ms\": %llu, \"readback_finished_tick_ms\": %llu, \"publish_tick_ms\": %llu,\n"
        "  \"width\": %u, \"height\": %u, \"resource_format\": %u, \"rtv_format\": %u, \"srv_format\": %u,\n"
        "  \"resource\": \"0x%llx\", \"native_texture\": \"0x%llx\", \"last_resolved_draw_tick_ms\": %llu,\n"
        "  \"native_clears_observed\": %llu, \"native_primitive_draws_observed\": %llu,\n"
        "  \"rgb\": \"front-layer-%lu-%llu-%u-rgb.bmp\", \"alpha\": \"front-layer-%lu-%llu-%u-alpha.bmp\",\n"
        "  \"boundary\": \"prior front contents before next native clear, after observed draws\",\n"
        "  \"pixel_note\": \"raw stored RGB bytes; alpha image is raw alpha grayscale; BMP export alpha is255; no gamma conversion\"\n}\n",
        GetCurrentProcessId(), GetCurrentThreadId(), sequence, begin, readbackFinished, GetTickCount64(),
        description.Width, description.Height, static_cast<unsigned>(description.Format),
        static_cast<unsigned>(s.rtvFormat), static_cast<unsigned>(s.srvFormat), source.resource, source.texture,
        threadState.lastResolvedDrawTick, threadState.clears, threadState.draws,
        GetCurrentProcessId(), captureSession, slot, GetCurrentProcessId(), captureSession, slot);
    if (size <= 0 || static_cast<std::size_t>(size) >= sizeof(manifest) ||
            !replaceManifest(manifest, static_cast<std::size_t>(size))) return "capture_manifest_write_failed";
    threadState.captureSequence = sequence;
    return "capture_saved";
}

void captureBeforeClear(void* backend) {
    auto& t = threadState;
    if (!captureEnabled || !t.ready || ownerThread.load(std::memory_order_acquire) != GetCurrentThreadId() ||
            !targetBoundFast(t.source, reinterpret_cast<std::uintptr_t>(backend))) return;
    const auto now = GetTickCount64();
    if (now < t.nextCapture || !t.lastResolvedDrawTick || now - t.lastResolvedDrawTick > 500 ||
            t.draws <= t.lastCapturedDraws || t.lastResolvedDrawClearSerial != t.clears ||
            !(t.lastResolvedDrawSource == t.source)) return;
    t.lastCapturedDraws = t.draws;
    t.nextCapture = now + 5000; // Also set before allocations to bound failure retries.
    Sample s;
    s.source = t.source;
    const char* status = capture(t.source, s, now);
    t.nextCapture = GetTickCount64() + 5000;
    writeRow("front_capture", status, s);
}

void captureBeforeClearNoThrow(void* backend) noexcept {
    try { captureBeforeClear(backend); } catch (...) {
        threadState.nextCapture = GetTickCount64() + 5000;
    }
}

void observeClearNoThrow(void* backend, const float* color) noexcept {
    try { observeClear(backend, color); } catch (...) {}
}
void observeDrawNoThrow(void* primitive) noexcept {
    try { observeDraw(primitive); } catch (...) {}
}
void consumerAfterClearNoThrow(void* backend) noexcept {
    try { consumerAfterClear(backend); } catch (...) {
        threadState.consumerActive = false;
        if (consumerOutput) consumerOutput->failed = true;
    }
}
void observeConsumerNoThrow(void* primitive, std::uintptr_t caller) noexcept {
    try { observeConsumer(primitive, caller); } catch (...) {
        writeConsumerRow("exception_interval_disabled", {});
        threadState.consumerActive = false;
    }
}
#include "render/native_owned_hud_observer.inl"
#include "render/native_owned_hud_runtime_observer.inl"
#include "render/native_wide_hud.inl"

void __fastcall clearHook(void* backend, const float* color) {
    if (!enabled.load(std::memory_order_acquire) || threadState.inHook) { originalClear(backend, color); return; }
    threadState.inHook = true;
    __try {
        wide_hud::ensureInstalled();
        if(diagnosticFrontEnabled)captureBeforeClearNoThrow(backend);
        originalClear(backend, color);
        if(diagnosticFrontEnabled)observeClearNoThrow(backend, color);
        if (consumerEnabled) consumerAfterClearNoThrow(backend);
    } __finally { threadState.inHook = false; }
}
void __fastcall drawHook(void* primitive) {
    if(owned_hud_runtime_observer::runtime && reinterpret_cast<std::uintptr_t>(primitive)==owned_hud_runtime_observer::observedRearMaterial.load())
        owned_hud_runtime_observer::lifecycle("native-draw",reinterpret_cast<std::uintptr_t>(primitive),reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    if (!enabled.load(std::memory_order_acquire) || threadState.inHook) { originalDraw(primitive); return; }
    threadState.inHook = true;
    __try {
        wide_hud::primitive=reinterpret_cast<std::uintptr_t>(primitive);
        owned_hud_runtime_observer::primitive(primitive, threadState.ready && targetBoundFast(threadState.source));
        originalDraw(primitive);
        if(diagnosticFrontEnabled)observeDrawNoThrow(primitive);
        owned_hud_probe::primitive(primitive, threadState.ready && targetBoundFast(threadState.source));
        if (consumerEnabled) observeConsumerNoThrow(primitive, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    } __finally { wide_hud::primitive=0; threadState.inHook = false; }
}

template<std::size_t N>
bool bytes(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return read(base, rva, actual) && actual == expected;
}
} // namespace

void observeWideHudDevice(ID3D11Device* device) noexcept {
    wide_hud::installShaderObserver(device);
}

void installNativeFrontObserver(void* module) noexcept {
    if (!diagnostics::optionalOutputEnabled() && !wide_hud::requested()) return;
    wchar_t value[4] {};
    if ((!wide_hud::requested() && (GetEnvironmentVariableW(L"BONE_EATER_FRONT_OBSERVE", value, 4) != 1 || value[0] != L'1')) || originalClear) return;
    try {
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) { log_warning("bone-eater", "Front observer rejected module: {}", status); return; }
        base = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> clearEntry {{
            0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
            0x41,0x56,0x41,0x57,0x48,0x83,0xec,0x20,0x4c,0x8b,0x35,0xe9,0x49,0xff,0x00,0x48}};
        constexpr std::array<unsigned char, 22> clearCall {{
            0x48,0x8b,0x43,0x18,0x48,0x8b,0x10,0x49,0x8b,0x06,0x4d,0x8b,0xc7,0x49,0x8b,0xce,0xff,0x90,0x90,0x01,0x00,0x00}};
        constexpr std::array<unsigned char, 24> drawEntry {{
            0x40,0x56,0x48,0x83,0xec,0x40,0xb8,0x06,0x00,0x00,0x00,0x48,0x8b,0xf1,0x66,0x3b,0x41,0x18,0x0f,0x84,0xc0,0x01,0x00,0x00}};
        constexpr std::array<unsigned char, 13> stateCall {{
            0x48,0x8b,0xcf,0xe8,0x3e,0xf6,0xef,0xff,0xe8,0x79,0xb4,0xe8,0xff}};
        constexpr std::array<unsigned char, 33> drawCalls {{
            0x48,0x8b,0x03,0x45,0x33,0xc9,0x45,0x33,0xc0,0x8b,0xd7,0x48,0x8b,0xcb,0xff,0x50,
            0x60,0xeb,0x0e,0x48,0x8b,0x03,0x45,0x33,0xc0,0x8b,0xd7,0x48,0x8b,0xcb,0xff,0x50,0x68}};
        if (!bytes(0x2F0E10, clearEntry) || !bytes(0x2F0EB0, clearCall) || !bytes(0x3F1050, drawEntry) ||
                !bytes(0x3F10BA, stateCall) || !bytes(0x3F11F8, drawCalls)) {
            log_warning("bone-eater", "Front observer rejected native instruction bytes"); return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        diagnosticFrontEnabled=diagnostics::optionalOutputEnabled() && GetEnvironmentVariableW(L"BONE_EATER_FRONT_OBSERVE",value,4)==1 && value[0]==L'1';
        wchar_t captureValue[4] {};
        captureEnabled = diagnosticFrontEnabled && GetEnvironmentVariableW(L"BONE_EATER_FRONT_CAPTURE", captureValue, 4) == 1 &&
            captureValue[0] == L'1';
        wchar_t consumerValue[4] {};
        consumerEnabled = diagnosticFrontEnabled && GetEnvironmentVariableW(L"BONE_EATER_FRONT_CONSUMER_OBSERVE", consumerValue, 4) == 1 &&
            consumerValue[0] == L'1';
        wchar_t presentationValue[4] {};
        presentationEnabled = consumerEnabled &&
            GetEnvironmentVariableW(L"BONE_EATER_FRONT_PRESENT_OBSERVE", presentationValue, 4) == 1 &&
            presentationValue[0] == L'1';
        wchar_t ownedValue[4] {};
        consumerOwnedOnly = GetEnvironmentVariableW(L"BONE_EATER_FRONT_CONSUMER_OWNED_ONLY", ownedValue, 4) == 1 &&
            ownedValue[0] == L'1';
        captureSession = GetTickCount64();
        output = new Output;
        if (consumerEnabled) consumerOutput = new Output;
        originalClear = reinterpret_cast<Clear>(base + 0x2F0E10);
        if (!detour::trampoline_try(originalClear, &clearHook, &originalClear)) return;
        originalDraw = reinterpret_cast<Draw>(base + 0x3F1050);
        if (!detour::trampoline_try(originalDraw, &drawHook, &originalDraw)) return;
        enabled.store(true, std::memory_order_release);
        if (owned_hud_runtime_observer::requested()) owned_hud_runtime_observer::install(base);
        else owned_hud_probe::install(base);
        log_info("bone-eater", "Native HUD route installed: wide={}, metadata={}, capture={}",wideHudRequested(),diagnosticFrontEnabled,captureEnabled);
        if (captureEnabled) log_warning("bone-eater", "Front capture is diagnostics only: GPU Map can stall rendering; at most one attempt per5seconds, before native front clear");
        if (consumerEnabled) log_info("bone-eater", "Read-only front consumer probe v3: owned_only={}, every600front-clears, <=24intervals, <=4096primitive checks and256candidate rows perinterval; output is not certified main", consumerOwnedOnly);
        if (presentationEnabled) log_info("bone-eater", "Read-only presentation identity probe enabled: scoped same-callback references; no graphics writes or retained backbuffers");
    } catch (...) { /* Native rendering remains a pass-through if installation fails. */ }
}
} // namespace bone_eater::render
