#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <span>
#include <array>

namespace bone_eater::render {
struct WideHudMapping { float x=0,y=0,sx=1,sy=1; };
struct WideHudRange { UINT first=0,count=0; WideHudMapping mapping; bool expandClip=false; };
// Records the real shader signature; never guesses the native varyings.
void recordWideHudVertexShader(ID3D11VertexShader*,const void*,SIZE_T) noexcept;
using WideHudDispatch=void(*)(void*,UINT,UINT,INT);

// A speculative color branch. Original native calls always execute. Only a
// complete front + rear pair can publish, at the original front consumer.
// All methods run synchronously on the immediate context's render thread.
class WideHudCompositor {
public:
    bool clearFront(ID3D11DeviceContext*,ID3D11RenderTargetView*,const float*) noexcept;
    void clearDepth(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8) noexcept;
    bool primeFrontDepth(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8) noexcept;
    void depthOutput(std::uintptr_t resource,bool front,bool scene) noexcept {
        if(resource==nativeFrontDepthResource_&&!front)invalidateFront();
        if(resource==nativeSceneDepthResource_&&!scene)invalidateScene();
    }
    void depthResourceWrite(std::uintptr_t resource) noexcept {
        if(resource==nativeFrontDepthResource_)invalidateFront();
        if(resource==nativeSceneDepthResource_)invalidateScene();
    }
    bool mirrorFront(ID3D11DeviceContext*,ID3D11RenderTargetView*,std::span<const WideHudRange>,
                     WideHudDispatch,void*) noexcept;
    bool forkScene(ID3D11DeviceContext*,ID3D11RenderTargetView*) noexcept;
    bool mirrorScene(ID3D11DeviceContext*,ID3D11RenderTargetView*,std::span<const WideHudRange>,
                     WideHudDispatch,void*) noexcept;
    bool publish(ID3D11DeviceContext*,ID3D11RenderTargetView*) noexcept;
    bool publishNative(ID3D11DeviceContext*,ID3D11RenderTargetView*,const WideHudRange&,
                       WideHudDispatch,void*) noexcept;
    void invalidate() noexcept { frontReady_=sceneReady_=false; }
    void invalidateFront() noexcept { frontReady_=false; }
    void invalidateScene() noexcept { sceneReady_=false; }
    bool frontReady() const noexcept { return frontReady_; }
    bool sceneReady() const noexcept { return sceneReady_; }
    unsigned failure() const noexcept { return failure_; }
private:
    using Texture=Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Target=Microsoft::WRL::ComPtr<ID3D11RenderTargetView>;
    bool prepare(ID3D11DeviceContext*) noexcept;
    bool mirror(ID3D11DeviceContext*,ID3D11RenderTargetView*,ID3D11RenderTargetView*,
                UINT,UINT,std::span<const WideHudRange>,WideHudDispatch,void*) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Texture front_,scene_;
    Target frontTarget_,sceneTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frontView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneView_;
    Texture frontDepth_,sceneDepth_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> frontDepthView_,sceneDepthView_;
    std::uintptr_t nativeFrontDepth_=0,nativeSceneDepth_=0;
    std::uintptr_t nativeFrontDepthResource_=0,nativeSceneDepthResource_=0;
    UINT frontDepthCleared_=0;
    bool prepareDepth(ID3D11DeviceContext*,ID3D11DepthStencilView*,bool) noexcept;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> multiply_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> copyRgb_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> noDepth_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mappingConstants_;
    DWORD thread_=0;
    bool frontReady_=false,sceneReady_=false;
    unsigned failure_=0;
};
inline constexpr float wideHudFit=1080.f/1366.f;
inline constexpr float wideHudLeft=(1920.f-768.f*wideHudFit)*.5f;
inline constexpr WideHudMapping wideHudOriginal {wideHudLeft,0,wideHudFit,wideHudFit};
inline constexpr WideHudMapping wideHudGroups[3] {{60,0,1,1},{1092,0,1,1},{576,0,1,1}};
}
