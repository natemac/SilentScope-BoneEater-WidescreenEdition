#include "render/owned_hud_replay.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bone_eater::render {
namespace {
using Microsoft::WRL::ComPtr;
bool sameDevice(ID3D11DeviceContext* context, ID3D11Device* expected) noexcept {
    ComPtr<ID3D11Device> actual;
    if (!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    context->GetDevice(&actual);
    return actual.Get()==expected;
}
bool dimensions(ID3D11RenderTargetView* target, UINT width, UINT height, DXGI_FORMAT format) noexcept {
    if (!target) return false;
    D3D11_RENDER_TARGET_VIEW_DESC view {}; target->GetDesc(&view);
    if (view.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice!=0 || view.Format!=format) return false;
    ComPtr<ID3D11Resource> resource; target->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc {}; texture->GetDesc(&desc);
    return desc.Width==width && desc.Height==height && desc.SampleDesc.Count==1 && desc.ArraySize==1;
}
bool sameViewport(const D3D11_VIEWPORT& a,const D3D11_VIEWPORT& b) noexcept {
    return a.TopLeftX==b.TopLeftX && a.TopLeftY==b.TopLeftY && a.Width==b.Width &&
        a.Height==b.Height && a.MinDepth==b.MinDepth && a.MaxDepth==b.MaxDepth;
}
struct SavedTargets {
    ID3D11DeviceContext* context;
    std::array<ComPtr<ID3D11RenderTargetView>,8> targets;
    ComPtr<ID3D11DepthStencilView> depth;
    std::array<D3D11_VIEWPORT,16> viewports {};
    std::array<D3D11_RECT,16> scissors {};
    UINT viewportCount=16, scissorCount=16;
    bool changed=false;
    explicit SavedTargets(ID3D11DeviceContext* c):context(c) {
        std::array<ID3D11RenderTargetView*,8> raw {};
        context->OMGetRenderTargets(8,raw.data(),&depth);
        for(std::size_t i=0;i<8;++i) targets[i].Attach(raw[i]);
        context->RSGetViewports(&viewportCount,viewports.data());
        context->RSGetScissorRects(&scissorCount,scissors.data());
    }
    void restore() noexcept {
        if(!changed) return;
        std::array<ID3D11RenderTargetView*,8> raw {};
        for(std::size_t i=0;i<8;++i) raw[i]=targets[i].Get();
        context->OMSetRenderTargets(8,raw.data(),depth.Get());
        context->RSSetViewports(viewportCount,viewports.data());
        context->RSSetScissorRects(scissorCount,scissors.data());
        changed=false;
    }
    ~SavedTargets(){restore();}
};
bool noUnorderedOrStreamOutput(ID3D11DeviceContext* context) noexcept {
    std::array<ID3D11UnorderedAccessView*,8> views {};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,views.data());
    bool clear=true;
    for(auto* p:views) if(p){clear=false;p->Release();}
    std::array<ID3D11Buffer*,4> buffers {};
    context->SOGetTargets(4,buffers.data());
    for(auto* p:buffers) if(p){clear=false;p->Release();}
    return clear;
}
}

bool OwnedHudReplay::prepare(ID3D11DeviceContext* context, DXGI_FORMAT format) noexcept {
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return false;
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    if(device_.Get()==device.Get() && format_==format && texture_ && composeState_) return true;
    if(moved_) return false; // Never destroy a layer containing suppressed indices.
    ComPtr<ID3D11Device1> device1; ComPtr<ID3D11DeviceContext1> context1;
    if(!device || device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_0 ||
       FAILED(device.As(&device1)) || FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    D3D11_TEXTURE2D_DESC td {}; td.Width=1920;td.Height=1080;td.MipLevels=1;td.ArraySize=1;
    td.Format=format;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target; ComPtr<ID3D11ShaderResourceView> view;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&texture)) || FAILED(device->CreateRenderTargetView(texture.Get(),nullptr,&target)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(),nullptr,&view))) return false;
    ComPtr<ID3DDeviceContextState> state;
    const auto level=device->GetFeatureLevel(); D3D_FEATURE_LEVEL chosen;
    UINT flags=(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
    if(FAILED(device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&state))) return false;
    const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!compiler) return false;
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));
    const char source[]="Texture2D layer:register(t0); float4 vs(uint i:SV_VertexID):SV_Position {return float4(i==2?3:-1,i==1?-3:1,0,1);} float4 ps(float4 p:SV_Position):SV_Target {return layer.Load(int3(p.xy,0));}";
    ComPtr<ID3DBlob> vs,ps,error;
    const bool compiled=compile && SUCCEEDED(compile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&error)) &&
        SUCCEEDED(compile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&error));
    FreeLibrary(compiler);
    if(!compiled) return false;
    ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
    if(FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel))) return false;
    D3D11_BLEND_DESC blend {}; auto& b=blend.RenderTarget[0]; b.BlendEnable=TRUE;
    b.SrcBlend=D3D11_BLEND_ZERO;b.DestBlend=D3D11_BLEND_SRC_COLOR;b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ZERO;b.DestBlendAlpha=D3D11_BLEND_ONE;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=7;
    ComPtr<ID3D11BlendState> multiply; if(FAILED(device->CreateBlendState(&blend,&multiply))) return false;
    D3D11_DEPTH_STENCIL_DESC ds {}; ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> noDepth; if(FAILED(device->CreateDepthStencilState(&ds,&noDepth))) return false;
    D3D11_RASTERIZER_DESC rs {};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;if(FAILED(device->CreateRasterizerState(&rs,&rasterizer))) return false;
    device_=device;texture_=texture;target_=target;view_=view;composeState_=state;
    vertex_=vertex;pixel_=pixel;multiply_=multiply;noDepth_=noDepth;rasterizer_=rasterizer;format_=format;
    return true;
}

bool OwnedHudReplay::begin(ID3D11DeviceContext* context,std::uint64_t frame,DXGI_FORMAT format) noexcept {
    if(!frame || moved_ || !prepare(context,format)) return false;
    frame_=frame;ownerThread_=GetCurrentThreadId();
    const float white[4]={1,1,1,1};context->ClearRenderTargetView(target_.Get(),white);
    return true;
}

bool OwnedHudReplay::replay(ID3D11DeviceContext* context,std::uint64_t frame,
        ID3D11RenderTargetView* front,const D3D11_VIEWPORT& viewport,
        const OwnedHudDrawPlan& plan,UINT total,const std::array<OwnedHudTransform,3>& transforms,
        bool certified,OwnedHudDrawIndexed draw,void* user) noexcept {
    if(!certified || !draw || !frame || frame!=frame_ || ownerThread_!=GetCurrentThreadId() ||
       !sameDevice(context,device_.Get()) || !target_ || !plan.replacement || plan.frame!=frame || !plan.batch.valid() ||
       !plan.count || plan.count>plan.ranges.size() || plan.totalQuads!=total/6 ||
       !total || total%6 || total>OwnedHudQuadSelection::maxQuads*6 ||
       viewport.TopLeftX!=0 || viewport.TopLeftY!=0 || viewport.Width!=768 || viewport.Height!=1366 ||
       !dimensions(front,768,1366,format_)) return false;
    UINT next=0;bool selected=false;
    std::array<D3D11_VIEWPORT,3> mapped {};
    for(std::size_t i=0;i<plan.count;++i) {
        const auto& r=plan.ranges[i];const auto g=static_cast<unsigned>(r.destination);
        if(!r.indexCount || r.firstIndex!=next || r.indexCount%6 || r.indexCount>total-next || g>3) return false;
        next+=r.indexCount;
        if(g==3) continue;
        selected=true;const auto& t=transforms[g];
        if(!std::isfinite(t.scale)||t.scale<=0||t.scale>4||!std::isfinite(t.x)||!std::isfinite(t.y)) return false;
        mapped[g]={t.x,t.y,viewport.Width*t.scale,viewport.Height*t.scale,viewport.MinDepth,viewport.MaxDepth};
        const auto& v=mapped[g];
        if(v.TopLeftX < -32768 || v.TopLeftY < -32768 || v.TopLeftX+v.Width>32767 || v.TopLeftY+v.Height>32767) return false;
    }
    if(next!=total || !selected) return false;
    SavedTargets saved(context);
    if(saved.viewportCount!=1 || !sameViewport(saved.viewports[0],viewport) || saved.targets[0].Get()!=front || saved.scissorCount>1) return false;
    for(std::size_t i=1;i<8;++i) if(saved.targets[i]) return false;
    D3D11_PRIMITIVE_TOPOLOGY topology;context->IAGetPrimitiveTopology(&topology);
    ComPtr<ID3D11Buffer> indices;DXGI_FORMAT indexFormat;UINT offset;
    context->IAGetIndexBuffer(&indices,&indexFormat,&offset);
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !indices || indexFormat!=DXGI_FORMAT_R16_UINT || offset!=0) return false;
    D3D11_BUFFER_DESC ib {};indices->GetDesc(&ib);if(ib.ByteWidth<total*2) return false;
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    context->GSGetShader(&gs,nullptr,nullptr);context->HSGetShader(&hs,nullptr,nullptr);context->DSGetShader(&ds,nullptr,nullptr);
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    context->VSGetShader(&vertex,nullptr,nullptr);context->PSGetShader(&pixel,nullptr,nullptr);
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue;context->GetPredication(&predicate,&predicateValue);
    if(!vertex || !pixel || gs || hs || ds || predicate || !noUnorderedOrStreamOutput(context)) return false;
    ComPtr<ID3D11DepthStencilState> depthState;UINT stencil;context->OMGetDepthStencilState(&depthState,&stencil);
    if(saved.depth) {D3D11_DEPTH_STENCIL_DESC desc {};if(!depthState) return false;depthState->GetDesc(&desc);if(desc.DepthEnable||desc.StencilEnable)return false;}
    ComPtr<ID3D11RasterizerState> raster;context->RSGetState(&raster);
    D3D11_RASTERIZER_DESC rd {};if(raster)raster->GetDesc(&rd);
    if(rd.ScissorEnable && saved.scissorCount!=1) return false;
    // Every fallible preparation has completed before the first suppressed quad.
    // Callback must be the native nonthrowing DrawIndexed dispatch, not a policy.
    for(std::size_t i=0;i<plan.count;++i) {
        const auto& r=plan.ranges[i];const auto g=static_cast<unsigned>(r.destination);
        if(g==3) saved.restore();
        else {
            auto* target=target_.Get();context->OMSetRenderTargets(1,&target,nullptr);
            context->RSSetViewports(1,&mapped[g]);
            if(rd.ScissorEnable) {
                const auto& s=saved.scissors[0];const auto& t=transforms[g];
                const auto edge=[](double v){return static_cast<LONG>(std::clamp(v,0.0,1920.0));};
                D3D11_RECT rect {edge(std::floor(s.left*t.scale+t.x)),
                    static_cast<LONG>(std::clamp(std::floor(s.top*t.scale+t.y),0.f,1080.f)),
                    edge(std::ceil(s.right*t.scale+t.x)),
                    static_cast<LONG>(std::clamp(std::ceil(s.bottom*t.scale+t.y),0.f,1080.f))};
                context->RSSetScissorRects(1,&rect);
            }
            saved.changed=true;
        }
        draw(user,r.indexCount,r.firstIndex,0);
    }
    moved_=true;
    return true;
}

bool OwnedHudReplay::compose(ID3D11DeviceContext* context,std::uint64_t frame,ID3D11RenderTargetView* presentation) noexcept {
    if(!pending(frame) || ownerThread_!=GetCurrentThreadId() || !sameDevice(context,device_.Get()) ||
       !dimensions(presentation,1920,1080,format_)) return false;
    SavedTargets saved(context);
    if(saved.targets[0].Get()!=presentation || saved.viewportCount!=1 ||
        !sameViewport(saved.viewports[0],D3D11_VIEWPORT{0,0,1920,1080,0,1})) return false;
    for(std::size_t i=1;i<8;++i) if(saved.targets[i]) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if(FAILED(context->QueryInterface(IID_PPV_ARGS(&context1)))) return false;
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(composeState_.Get(),&previous);
    context->OMSetRenderTargets(1,&presentation,nullptr);
    const D3D11_VIEWPORT v {0,0,1920,1080,0,1};context->RSSetViewports(1,&v);context->RSSetState(rasterizer_.Get());
    context->OMSetBlendState(multiply_.Get(),nullptr,0xffffffff);context->OMSetDepthStencilState(noDepth_.Get(),0);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(),nullptr,0);context->PSSetShader(pixel_.Get(),nullptr,0);
    auto* view=view_.Get();context->PSSetShaderResources(0,1,&view);
    context->Draw(3,0);
    // Drop native output references from the reusable isolated state before
    // swapping it out: never retain a presentation/backbuffer across callbacks.
    context->OMSetRenderTargets(0,nullptr,nullptr);
    context1->SwapDeviceContextState(previous.Get(),nullptr);
    moved_=false;
    return true;
}
}
