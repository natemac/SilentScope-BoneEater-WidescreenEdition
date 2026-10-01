#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <wrl/client.h>
#include "render/owned_hud_quad_selection.h"
#include <array>
#include <cstdint>

namespace bone_eater::render {
// Affine mapping in native front-viewport pixel coordinates. Uniform scale
// preserves glyph proportions. Layout/ownership certification belongs to caller.
struct OwnedHudTransform { float scale=0, x=0, y=0; };
using OwnedHudDrawIndexed = void(*)(void* user, UINT count, UINT first, INT base);

// Render-thread-only component. Owns only its texture/device state, never native
// backbuffers, game objects or a context across calls. False means no native
// indices were consumed; caller executes its unchanged DrawIndexed exactly once.
class OwnedHudReplay {
public:
    bool begin(ID3D11DeviceContext* context, std::uint64_t frame, DXGI_FORMAT format) noexcept;
    bool replay(ID3D11DeviceContext* context, std::uint64_t frame,
                ID3D11RenderTargetView* certifiedFront, const D3D11_VIEWPORT& certifiedViewport,
                const OwnedHudDrawPlan& plan, UINT totalIndices,
                const std::array<OwnedHudTransform,3>& transforms,
                bool certifiedConsumerAndBacking, OwnedHudDrawIndexed draw, void* user) noexcept;
    // Call at the verified native front-consumer boundary, AFTER its normal
    // multiply. Expected target must be the exact current presentation resource,
    // 1920x1080, single sample. Never call this from a generic Present overlay.
    bool compose(ID3D11DeviceContext* context, std::uint64_t frame,
                 ID3D11RenderTargetView* certifiedPresentation) noexcept;
    bool pending(std::uint64_t frame) const noexcept { return frame && frame==frame_ && moved_; }
    // Refuse to discard suppressed pixels before their certified consumer.
    bool end() noexcept { if(moved_) return false; frame_=0; ownerThread_=0; return true; }
private:
    bool prepare(ID3D11DeviceContext* context, DXGI_FORMAT format) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> composeState_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> multiply_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
    std::uint64_t frame_=0;
    DWORD ownerThread_=0;
    bool moved_=false;
};
}
