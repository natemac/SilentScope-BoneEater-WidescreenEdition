#include "render/wide_hud_compositor.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
namespace bone_eater::render {
namespace {
using Microsoft::WRL::ComPtr;
HMODULE shaderCompiler() noexcept {static HMODULE module=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);return module;}
constexpr GUID signatureKey{0x563cf982,0xbd18,0x443d,{0xa1,0xde,0x21,0x50,0x99,0x74,0xb2,0x11}};
constexpr GUID geometryKey{0x563cf982,0xbd18,0x443d,{0xa1,0xde,0x21,0x50,0x99,0x74,0xb2,0x12}};
bool clipShader(ID3D11Device* device,ID3D11VertexShader* vertex,ComPtr<ID3D11GeometryShader>& out) {
    if(!vertex)return false;
    UINT size=sizeof(ID3D11GeometryShader*);ID3D11GeometryShader* cached=nullptr;
    if(SUCCEEDED(vertex->GetPrivateData(geometryKey,&size,&cached))&&cached){out.Attach(cached);return true;}
    size=0;if(FAILED(vertex->GetPrivateData(signatureKey,&size,nullptr))||!size||size>8192)return false;
    std::vector<char> source(size);
    if(FAILED(vertex->GetPrivateData(signatureKey,&size,source.data())))return false;
    ComPtr<ID3DBlob> code,error;
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(shaderCompiler(),"D3DCompile"));
    if(!compile||FAILED(compile(source.data(),size,nullptr,nullptr,nullptr,"gs","gs_5_0",0,0,&code,&error))||
       FAILED(device->CreateGeometryShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&out)))return false;
    vertex->SetPrivateDataInterface(geometryKey,out.Get());return true;
}
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
    return desc.Width==width && desc.Height==height && desc.SampleDesc.Count==1 && desc.ArraySize==1&&
        desc.Usage==D3D11_USAGE_DEFAULT&&desc.CPUAccessFlags==0&&!(desc.BindFlags&D3D11_BIND_UNORDERED_ACCESS);
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
    ComPtr<ID3D11Buffer> geometryConstants;
    bool changed=false;
    explicit SavedTargets(ID3D11DeviceContext* c):context(c) {
        std::array<ID3D11RenderTargetView*,8> raw {};
        context->OMGetRenderTargets(8,raw.data(),&depth);
        for(std::size_t i=0;i<8;++i) targets[i].Attach(raw[i]);
        context->RSGetViewports(&viewportCount,viewports.data());
        context->RSGetScissorRects(&scissorCount,scissors.data());
        context->GSGetConstantBuffers(0,1,&geometryConstants);
    }
    void restore() noexcept {
        if(!changed) return;
        std::array<ID3D11RenderTargetView*,8> raw {};
        for(std::size_t i=0;i<8;++i) raw[i]=targets[i].Get();
        context->OMSetRenderTargets(8,raw.data(),depth.Get());
        context->RSSetViewports(viewportCount,viewports.data());
        context->RSSetScissorRects(scissorCount,scissors.data());
        context->GSSetShader(nullptr,nullptr,0);
        auto* constants=geometryConstants.Get();context->GSSetConstantBuffers(0,1,&constants);
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

void recordWideHudVertexShader(ID3D11VertexShader* shader,const void* bytes,SIZE_T length) noexcept {
    if(!shader||!bytes||length>1024*1024)return;
    try {
        const auto reflect=reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(shaderCompiler(),"D3DReflect"));
        ComPtr<ID3D11ShaderReflection> reflection;
        if(!reflect||FAILED(reflect(bytes,length,IID_PPV_ARGS(&reflection))))return;
        D3D11_SHADER_DESC desc{};if(FAILED(reflection->GetDesc(&desc))||desc.OutputParameters>16)return;
        std::string fields,position;
        for(UINT i=0;i<desc.OutputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC p{};
            if(FAILED(reflection->GetOutputParameterDesc(i,&p))||!p.SemanticName||
               p.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32||p.Stream||!p.Mask)return;
            const std::string semantic=p.SemanticName;
            if(semantic.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)return;
            unsigned components=0;for(unsigned mask=p.Mask;mask;mask>>=1)++components;
            const auto field="v"+std::to_string(i);
            if(p.SystemValueType==D3D_NAME_POSITION){if(components!=4||!position.empty())return;position=field;}
            else if(p.SystemValueType!=D3D_NAME_UNDEFINED||
                    (semantic!="TEXCOORD"&&semantic!="COLOR"))return;
            fields+="float"+std::to_string(components)+" "+field+":"+semantic+std::to_string(p.SemanticIndex)+";";
        }
        if(position.empty())return;
        const auto source="cbuffer Map:register(b0){float4 m;}struct V{"+fields+"};[maxvertexcount(3)]void gs(triangle V input[3],inout TriangleStream<V> output){[unroll]for(uint i=0;i<3;++i){V v=input[i];v."+position+".xy=v."+position+".xy*m.xy+v."+position+".ww*m.zw;output.Append(v);}output.RestartStrip();}";
        shader->SetPrivateData(signatureKey,UINT(source.size()),source.data());
    }catch(...){}
}
bool WideHudCompositor::prepare(ID3D11DeviceContext* context) noexcept {
    constexpr auto format=DXGI_FORMAT_R8G8B8A8_UNORM;
    if(!context || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        (format!=DXGI_FORMAT_R8G8B8A8_UNORM && format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return false;
    ComPtr<ID3D11Device> device; context->GetDevice(&device);
    if(device_.Get()==device.Get() && front_ && state_) return true;
    invalidate();
    frontDepth_.Reset();sceneDepth_.Reset();frontDepthView_.Reset();sceneDepthView_.Reset();
    nativeFrontDepth_=nativeSceneDepth_=nativeFrontDepthResource_=nativeSceneDepthResource_=0;
    frontDepthCleared_=0;
    mappingConstants_.Reset();
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
    D3D11_BLEND_DESC copyBlend{};copyBlend.RenderTarget[0].RenderTargetWriteMask=7;
    ComPtr<ID3D11BlendState> copyRgb;if(FAILED(device->CreateBlendState(&copyBlend,&copyRgb)))return false;
    D3D11_DEPTH_STENCIL_DESC ds {}; ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> noDepth; if(FAILED(device->CreateDepthStencilState(&ds,&noDepth))) return false;
    D3D11_RASTERIZER_DESC rs {};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rasterizer;if(FAILED(device->CreateRasterizerState(&rs,&rasterizer))) return false;
    device_=device;front_=texture;frontTarget_=target;frontView_=view;state_=state;
    scene_.Reset();sceneTarget_.Reset();sceneView_.Reset();copyRgb_=copyRgb;thread_=GetCurrentThreadId();
    vertex_=vertex;pixel_=pixel;multiply_=multiply;noDepth_=noDepth;rasterizer_=rasterizer;
    return true;
}

bool WideHudCompositor::clearFront(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,const float* color) noexcept {
    frontReady_=false;failure_=1;
    if(!color || !prepare(c) || !dimensions(native,768,1366,DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> c1;
    if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1)))) return false;
    // Native target binds are deferred until the primitive is prepared. The
    // context can still expose the previous pass's DSV at ClearRTV time.
    nativeFrontDepth_=nativeFrontDepthResource_=0;frontDepthCleared_=0;
    const float white[4]={1,1,1,1};c->ClearRenderTargetView(frontTarget_.Get(),white);
    // Clear only pixels inside the fitted native canvas. Rounding the right
    // edge outward left a black column beyond the native sprite coverage.
    const D3D11_RECT footprint {LONG(std::ceil(wideHudLeft)),0,LONG(std::floor(wideHudLeft+768*wideHudFit)),1080};
    c1->ClearView(frontTarget_.Get(),color,&footprint,1);
    frontReady_=true;failure_=0;return true;
}
bool WideHudCompositor::primeFrontDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* native,UINT flags,FLOAT depth,UINT8 stencil) noexcept {
    if(!frontReady_)return false;
    if(nativeFrontDepth_)return nativeFrontDepth_==reinterpret_cast<std::uintptr_t>(native);
    if(!native)return true;
    if(!prepareDepth(c,native,true)){invalidateFront();return false;}
    nativeFrontDepth_=reinterpret_cast<std::uintptr_t>(native);
    if(flags)clearDepth(c,native,flags,depth,stencil);
    return true;
}
bool WideHudCompositor::prepareDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* native,bool front) noexcept {
    ComPtr<ID3D11Resource> resource;native->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))return false;
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);D3D11_DEPTH_STENCIL_VIEW_DESC v{};native->GetDesc(&v);
    if(d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1||d.Usage!=D3D11_USAGE_DEFAULT||d.CPUAccessFlags||
       v.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||v.Texture2D.MipSlice||v.Flags||
       (!front&&(d.Width!=1920||d.Height!=1080)))return false;
    auto& owned=front?frontDepth_:sceneDepth_;auto& view=front?frontDepthView_:sceneDepthView_;
    D3D11_TEXTURE2D_DESC old{};if(owned)owned->GetDesc(&old);
    if(!owned||old.Format!=d.Format){owned.Reset();view.Reset();d.Width=1920;d.Height=1080;
        d.BindFlags=D3D11_BIND_DEPTH_STENCIL;d.MiscFlags=0;
        if(FAILED(device_->CreateTexture2D(&d,nullptr,&owned))||FAILED(device_->CreateDepthStencilView(owned.Get(),&v,&view)))return false;
    }
    if(!front)c->CopyResource(owned.Get(),resource.Get());
    (front?nativeFrontDepthResource_:nativeSceneDepthResource_)=reinterpret_cast<std::uintptr_t>(resource.Get());
    return true;
}
void WideHudCompositor::clearDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* native,UINT flags,FLOAT depth,UINT8 stencil) noexcept {
    const auto id=reinterpret_cast<std::uintptr_t>(native);
    ComPtr<ID3D11Resource> resource;native->GetResource(&resource);const auto rid=reinterpret_cast<std::uintptr_t>(resource.Get());
    if(rid==nativeFrontDepthResource_&&id!=nativeFrontDepth_)invalidateFront();
    if(rid==nativeSceneDepthResource_&&id!=nativeSceneDepth_)invalidateScene();
    if(frontReady_&&id==nativeFrontDepth_&&frontDepthView_){c->ClearDepthStencilView(frontDepthView_.Get(),flags,depth,stencil);frontDepthCleared_|=flags;}
    if(sceneReady_&&id==nativeSceneDepth_&&sceneDepthView_)c->ClearDepthStencilView(sceneDepthView_.Get(),flags,depth,stencil);
}

bool WideHudCompositor::forkScene(ID3D11DeviceContext* c,ID3D11RenderTargetView* native) noexcept {
    sceneReady_=false;failure_=2;
    if(!prepare(c) || thread_!=GetCurrentThreadId() || !sameDevice(c,device_.Get()) || !native) return false;
    ComPtr<ID3D11Resource> resource;native->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    D3D11_RENDER_TARGET_VIEW_DESC v{};native->GetDesc(&v);
    if(d.Width!=1920 || d.Height!=1080 || d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 ||
       d.Usage!=D3D11_USAGE_DEFAULT||d.CPUAccessFlags||(d.BindFlags&D3D11_BIND_UNORDERED_ACCESS)||
       v.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D || v.Texture2D.MipSlice ||
       (v.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && v.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return false;
    ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);
    nativeSceneDepth_=reinterpret_cast<std::uintptr_t>(depth.Get());
    if(depth&&!prepareDepth(c,depth.Get(),false))return false;
    D3D11_TEXTURE2D_DESC old{};if(scene_)scene_->GetDesc(&old);
    if(!scene_ || old.Format!=d.Format) {
        scene_.Reset();sceneTarget_.Reset();sceneView_.Reset();d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.MiscFlags=0;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=v.Format;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        if(FAILED(device_->CreateTexture2D(&d,nullptr,&scene_)) ||
           FAILED(device_->CreateRenderTargetView(scene_.Get(),&v,&sceneTarget_))||
           FAILED(device_->CreateShaderResourceView(scene_.Get(),&sv,&sceneView_))) return false;
    }
    c->CopyResource(scene_.Get(),resource.Get());sceneReady_=true;failure_=0;return true;
}

bool WideHudCompositor::mirror(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,
        ID3D11RenderTargetView* target,UINT width,UINT height,std::span<const WideHudRange> ranges,
        WideHudDispatch draw,void* user) noexcept {
    failure_=3;
    if(!draw || ranges.empty() || thread_!=GetCurrentThreadId() || !sameDevice(c,device_.Get())) return false;
    SavedTargets saved(c);
    if(saved.targets[0].Get()!=native || saved.viewportCount!=1 || saved.scissorCount>1 ||
       !sameViewport(saved.viewports[0],{0,0,float(width),float(height),0,1})) return false;
    for(unsigned i=1;i<8;++i)if(saved.targets[i])return false;
    failure_=4;
    ComPtr<ID3D11DepthStencilState> depth;UINT stencil=0;c->OMGetDepthStencilState(&depth,&stencil);
    ID3D11DepthStencilView* alternateDepth=nullptr;
    if(saved.depth){D3D11_DEPTH_STENCIL_DESC d{};if(depth)depth->GetDesc(&d);else{d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}
        const bool front=target==frontTarget_.Get();const auto expected=front?nativeFrontDepth_:nativeSceneDepth_;
        failure_=41;if(reinterpret_cast<std::uintptr_t>(saved.depth.Get())!=expected)return false;
        failure_=42;if(front&&d.DepthEnable&&!(frontDepthCleared_&D3D11_CLEAR_DEPTH))return false;
        failure_=43;if(front&&d.StencilEnable&&!(frontDepthCleared_&D3D11_CLEAR_STENCIL))return false;
        alternateDepth=front?frontDepthView_.Get():sceneDepthView_.Get();if(!alternateDepth)return false;
    }
    ComPtr<ID3D11GeometryShader> gs;ComPtr<ID3D11HullShader> hs;ComPtr<ID3D11DomainShader> ds;
    c->GSGetShader(&gs,nullptr,nullptr);c->HSGetShader(&hs,nullptr,nullptr);c->DSGetShader(&ds,nullptr,nullptr);
    ComPtr<ID3D11Predicate> pred;BOOL value;c->GetPredication(&pred,&value);
    failure_=44;if(gs||hs||ds)return false;failure_=45;if(pred)return false;
    failure_=46;if(!noUnorderedOrStreamOutput(c))return false;
    // Sampling the native destination would make the alternate branch diverge.
    ComPtr<ID3D11Resource> destination;native->GetResource(&destination);
    std::array<ID3D11ShaderResourceView*,128> inputs{};
    bool feedback=false;
    for(unsigned stage=0;stage<2;++stage){
        if(stage==0)c->PSGetShaderResources(0,128,inputs.data());else c->VSGetShaderResources(0,128,inputs.data());
        for(auto* input:inputs)if(input){ComPtr<ID3D11Resource> r;input->GetResource(&r);const auto id=reinterpret_cast<std::uintptr_t>(r.Get());
            feedback|=r.Get()==destination.Get()||id==nativeFrontDepthResource_||id==nativeSceneDepthResource_;input->Release();}
    }
    failure_=47;if(feedback)return false;
    failure_=5;
    ComPtr<ID3D11RasterizerState> raster;c->RSGetState(&raster);D3D11_RASTERIZER_DESC rd{};
    if(raster)raster->GetDesc(&rd);
    if(rd.ScissorEnable && saved.scissorCount!=1)return false;
    for(const auto& r:ranges){const auto& m=r.mapping;
        if(!r.count || !std::isfinite(m.x)||!std::isfinite(m.y)||!std::isfinite(m.sx)||!std::isfinite(m.sy)||
           m.sx<=0||m.sy<=0||m.sx>4||m.sy>4||m.x < -20000||m.y < -20000||
           m.x+width*m.sx>30000||m.y+height*m.sy>30000)return false;
    }
    ComPtr<ID3D11GeometryShader> transform;
    if(std::any_of(ranges.begin(),ranges.end(),[](const auto& r){return r.expandClip;})){
        ComPtr<ID3D11VertexShader> vertex;UINT classes=0;c->VSGetShader(&vertex,nullptr,&classes);
        D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);
        failure_=48;
        if(classes||topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST||!clipShader(device_.Get(),vertex.Get(),transform))return false;
        if(!mappingConstants_){D3D11_BUFFER_DESC d{};d.ByteWidth=16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device_->CreateBuffer(&d,nullptr,&mappingConstants_)))return false;}
    }
    saved.changed=true;c->OMSetRenderTargets(1,&target,alternateDepth);
    for(const auto& r:ranges){const auto& m=r.mapping;
        const D3D11_VIEWPORT viewport=r.expandClip?D3D11_VIEWPORT{0,0,1920,1080,0,1}:D3D11_VIEWPORT{m.x,m.y,width*m.sx,height*m.sy,0,1};c->RSSetViewports(1,&viewport);
        if(r.expandClip){const float ax=width*m.sx/1920.f,ay=height*m.sy/1080.f;
            const float map[4]{ax,ay,2*m.x/1920.f+ax-1,1-2*m.y/1080.f-ay};
            c->UpdateSubresource(mappingConstants_.Get(),0,nullptr,map,0,0);auto* constants=mappingConstants_.Get();
            c->GSSetConstantBuffers(0,1,&constants);c->GSSetShader(transform.Get(),nullptr,0);
        }else c->GSSetShader(nullptr,nullptr,0);
        if(rd.ScissorEnable){const auto& s=saved.scissors[0];
            const bool full=r.expandClip&&s.left==0&&s.top==0&&s.right==LONG(width)&&s.bottom==LONG(height);
            const D3D11_RECT rect=full?D3D11_RECT{0,0,1920,1080}:D3D11_RECT{LONG(std::clamp(std::floor(s.left*m.sx+m.x),0.f,1920.f)),
                LONG(std::clamp(std::floor(s.top*m.sy+m.y),0.f,1080.f)),
                LONG(std::clamp(std::ceil(s.right*m.sx+m.x),0.f,1920.f)),
                LONG(std::clamp(std::ceil(s.bottom*m.sy+m.y),0.f,1080.f))};c->RSSetScissorRects(1,&rect);
        }
        draw(user,r.count,r.first,0);
    }
    failure_=0;return true;
}
bool WideHudCompositor::mirrorFront(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,
        std::span<const WideHudRange> r,WideHudDispatch draw,void* user) noexcept {
    if(!frontReady_)return false;
    frontReady_=mirror(c,native,frontTarget_.Get(),768,1366,r,draw,user);return frontReady_;
}
bool WideHudCompositor::mirrorScene(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,
        std::span<const WideHudRange> r,WideHudDispatch draw,void* user) noexcept {
    if(!sceneReady_)return false;
    sceneReady_=mirror(c,native,sceneTarget_.Get(),1920,1080,r,draw,user);return sceneReady_;
}
bool WideHudCompositor::publish(ID3D11DeviceContext* c,ID3D11RenderTargetView* native) noexcept {
    failure_=6;
    if(!frontReady_||!sceneReady_||thread_!=GetCurrentThreadId()||!sameDevice(c,device_.Get()))return false;
    ComPtr<ID3D11Resource> destination;native->GetResource(&destination);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(destination.As(&texture)))return false;
    D3D11_TEXTURE2D_DESC d{},s{};texture->GetDesc(&d);scene_->GetDesc(&s);
    if(d.Width!=s.Width||d.Height!=s.Height||d.Format!=s.Format||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1)return false;
    ComPtr<ID3D11DeviceContext1> c1;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1))))return false;
    ComPtr<ID3DDeviceContextState> previous;c1->SwapDeviceContextState(state_.Get(),&previous);
    auto* output=sceneTarget_.Get();c->OMSetRenderTargets(1,&output,nullptr);
    const D3D11_VIEWPORT viewport{0,0,1920,1080,0,1};c->RSSetViewports(1,&viewport);c->RSSetState(rasterizer_.Get());
    c->OMSetBlendState(multiply_.Get(),nullptr,~0u);c->OMSetDepthStencilState(noDepth_.Get(),0);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex_.Get(),nullptr,0);c->PSSetShader(pixel_.Get(),nullptr,0);
    auto* input=frontView_.Get();c->PSSetShaderResources(0,1,&input);c->Draw(3,0);
    c->OMSetRenderTargets(0,nullptr,nullptr);
    c->CopyResource(destination.Get(),scene_.Get());
    c1->SwapDeviceContextState(previous.Get(),nullptr);
    invalidate();failure_=0;return true;
}
bool WideHudCompositor::publishNative(ID3D11DeviceContext* c,ID3D11RenderTargetView* native,
        const WideHudRange& consumer,WideHudDispatch draw,void* user) noexcept {
    if(!frontReady_||!sceneReady_)return false;
    ComPtr<ID3D11Resource> destination;native->GetResource(&destination);
    ComPtr<ID3D11Texture2D> texture;if(FAILED(destination.As(&texture)))return false;
    D3D11_TEXTURE2D_DESC d{},s{};texture->GetDesc(&d);scene_->GetDesc(&s);
    if(d.Width!=s.Width||d.Height!=s.Height||d.Format!=s.Format||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1)return false;
    ComPtr<ID3D11DeviceContext1> c1;if(FAILED(c->QueryInterface(IID_PPV_ARGS(&c1))))return false;
    ComPtr<ID3D11DepthStencilView> nativeDepth;c->OMGetRenderTargets(0,nullptr,&nativeDepth);
    ComPtr<ID3D11Resource> depthDestination;
    if(nativeSceneDepth_){if(reinterpret_cast<std::uintptr_t>(nativeDepth.Get())!=nativeSceneDepth_)return false;nativeDepth->GetResource(&depthDestination);}
    ComPtr<ID3D11ShaderResourceView> previous;c->PSGetShaderResources(0,1,&previous);
    auto* input=frontView_.Get();c->PSSetShaderResources(0,1,&input);
    const bool composed=mirror(c,native,sceneTarget_.Get(),1920,1080,{&consumer,1},draw,user);
    input=previous.Get();c->PSSetShaderResources(0,1,&input);
    if(!composed){invalidate();return false;}
    // The native shader, UVs, vertex color, blend and sampler performed the
    // multiply. Only the certified quad's viewport and source were replaced.
    // Keep the exact native consumer's alpha, including the untouched margins.
    // Every fallible operation is complete before this original draw.
    draw(user,consumer.count,consumer.first,0);
    ComPtr<ID3DDeviceContextState> saved;c1->SwapDeviceContextState(state_.Get(),&saved);
    c->OMSetRenderTargets(1,&native,nullptr);
    if(depthDestination)c->CopyResource(depthDestination.Get(),sceneDepth_.Get());
    const D3D11_VIEWPORT viewport{0,0,1920,1080,0,1};c->RSSetViewports(1,&viewport);c->RSSetState(rasterizer_.Get());
    c->OMSetBlendState(copyRgb_.Get(),nullptr,~0u);c->OMSetDepthStencilState(noDepth_.Get(),0);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex_.Get(),nullptr,0);c->PSSetShader(pixel_.Get(),nullptr,0);
    input=sceneView_.Get();c->PSSetShaderResources(0,1,&input);c->Draw(3,0);
    c->OMSetRenderTargets(0,nullptr,nullptr);c1->SwapDeviceContextState(saved.Get(),nullptr);
    invalidate();return true;
}
} // namespace bone_eater::render

