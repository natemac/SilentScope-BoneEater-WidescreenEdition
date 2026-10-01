#include "render/lobby_impact.h"
#include "render/lobby_impact_policy.h"
#include "render/native_viewport.h"
#include "util/logging.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <memory>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <vector>
namespace bone_eater::render {
namespace {
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
    Com<ID3D11Texture2D> atlas;
    Com<ID3D11ShaderResourceView> atlasView;
    Com<ID3D11VertexShader> vertexShader;
    Com<ID3D11PixelShader> pixelShader;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11BlendState> blend;
    Com<ID3D11RasterizerState> rasterizer;
    bool pipelineReady=false;
};
struct Pending {
    std::mutex mutex;
    ULONGLONG readyAt=0;
    std::uintptr_t owner=0,gun=0;
    std::array<std::uintptr_t,10> adapters{};
    std::array<LobbyImpact,10> poses{};
    std::array<bool,10> queued{};
};
// Trampolines may outlive static destruction.
Pending& pending(){static auto* p=new Pending;return *p;}
struct Read {
    template<class T> bool operator()(std::uintptr_t p,std::uintptr_t o,T& value) const noexcept {
        SIZE_T done=0;return p&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p+o),&value,sizeof(value),&done)&&done==sizeof(value);
    }
};
bool requested(){static const bool v=[] {wchar_t s[4]{};return GetEnvironmentVariableW(L"BONE_EATER_DESKTOP_RETICLE",s,4)==1&&s[0]==L'1';}();return v;}
bool ownerAt(std::uintptr_t base,std::uintptr_t& owner,std::uintptr_t& gun){
    unsigned phase=0;std::uintptr_t resident=0,vt=0;
    return scoreLobbyOwner(base,Read{},owner,phase)&&phase>=3&&phase<=7&&
        Read{}(base,0x13DCF48,resident)&&Read{}(resident,0x10,gun)&&Read{}(gun,0,vt)&&vt==base+0x10CA460;
}
constexpr char kShaders[]=R"(
cbuffer Placement : register(b0) { float4 pose; float4 rotation; float4 content; float4 target; };
Texture2D atlas : register(t0); SamplerState texSampler : register(s0);
struct Vertex {float4 position:SV_POSITION; float2 uv:TEXCOORD0;};
Vertex vsMain(uint id:SV_VertexID) {
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float2 q=(corners[id]-.5)*pose.zw;
    q=float2(q.x*rotation.x-q.y*rotation.y,q.x*rotation.y+q.y*rotation.x)+pose.xy;
    float2 pixel=content.xy+q/float2(1920,1080)*content.zw;
    Vertex v; v.position=float4(pixel.x/target.x*2-1,1-pixel.y/target.y*2,0,1);
    v.uv=lerp(float2(1,1)/512,float2(319,279)/512,corners[id]);return v;
}
float4 psMain(Vertex v):SV_TARGET {float4 c=atlas.Sample(texSampler,v.uv);c.a*=rotation.z;return c;}
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
    constants.ByteWidth = 64;
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
    raster.ScissorEnable = TRUE;
    hr = createCom(gpu.rasterizer, [&](ID3D11RasterizerState** out) {
        return gpu.device->CreateRasterizerState(&raster, out);
    });
    gpu.pipelineReady = SUCCEEDED(hr);
    return hr;
}

bool loadAtlas(Resources& gpu){
    wchar_t exe[32768]{};const auto count=GetModuleFileNameW(nullptr,exe,32768);if(!count||count>=32768)return false;
    auto path=std::filesystem::path(exe).parent_path()/L"data/ui/guncrosschair/guncrosschair.aif";
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in||in.tellg()!=1049184)return false;
    std::vector<unsigned char> bytes(1049184);in.seekg(0);if(!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return false;
    std::uint64_t hash=14695981039346656037ull;for(auto c:bytes){hash^=c;hash*=1099511628211ull;}
    if(hash!=0x8d0c50c6f8389221ull)return false;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=512;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{bytes.data()+0x260,512*4,0};
    return SUCCEEDED(createCom(gpu.atlas,[&](auto** out){return gpu.device->CreateTexture2D(&desc,&data,out);}))&&
        SUCCEEDED(createCom(gpu.atlasView,[&](auto** out){return gpu.device->CreateShaderResourceView(gpu.atlas.get(),nullptr,out);}));
}
struct StateGuard {
    ID3D11DeviceContext1* context;Com<ID3DDeviceContextState> prior;
    StateGuard(Resources& gpu):context(gpu.context1.get()){ID3DDeviceContextState* p=nullptr;context->SwapDeviceContextState(gpu.passState.get(),&p);prior.reset(p);}
    ~StateGuard(){context->SwapDeviceContextState(prior.get(),nullptr);}
};
}
bool captureLobbyImpact(std::uintptr_t base,std::uintptr_t adapter) noexcept {
    if(!requested())return false;
    try {
        auto& p=pending();std::lock_guard lock(p.mutex);auto now=GetTickCount64();
        if(!p.readyAt||now-p.readyAt>100)return false;
        unsigned i=0;while(i<10&&p.adapters[i]!=adapter)++i;if(i==10)return false;
        std::uintptr_t owner=0,gun=0;if(!ownerAt(base,owner,gun)||owner!=p.owner||gun!=p.gun)return false;
        LobbyImpact pose{};if(!lobbyImpactNode(base,gun,i,adapter,Read{},pose))return false;
        p.poses[i]=pose;p.queued[i]=true;
        static unsigned reported=0;if(reported++<3)log_info("bone-eater","Lobby impact replacement x={} y={} alpha={}",pose.x,pose.y,pose.alpha);
        return true;
    }catch(...){return false;}
}
void composeLobbyImpact(IDXGISwapChain* chain,void* mainWindow,unsigned flags) noexcept {
    if(!requested()||!chain||!mainWindow||(flags&DXGI_PRESENT_TEST))return;
    DXGI_SWAP_CHAIN_DESC swap{};if(FAILED(chain->GetDesc(&swap))||swap.OutputWindow!=mainWindow)return;
    auto& p=pending();
    try {
        static auto* mutex=new std::mutex;std::lock_guard gpuLock(*mutex);
        static auto* gpuPtr=new Resources;auto& gpu=*gpuPtr;
        std::array<LobbyImpact,10> poses{};std::array<bool,10> queued{};std::uintptr_t previousOwner=0;
        {std::lock_guard lock(p.mutex);previousOwner=p.owner;poses=p.poses;queued=p.queued;p.queued.fill(false);p.readyAt=0;}
        const auto viewport=readNativeMainViewport(mainWindow);
        if(!viewport.valid||viewport.logicalWidth!=1920||viewport.logicalHeight!=1080||viewport.osClientWidth<=0||viewport.osClientHeight<=0)return;
        const auto base=viewport.moduleBase;std::uintptr_t owner=0,gun=0;
        if(!ownerAt(base,owner,gun))return;
        if(previousOwner!=owner)queued.fill(false);
        Com<ID3D11Device> device;if(FAILED(createCom(device,[&](auto** out){return chain->GetDevice(IID_PPV_ARGS(out));})))return;
        if(gpu.device.get()!=device.get()){
            gpu={};gpu.device=std::move(device);ID3D11DeviceContext* context=nullptr;gpu.device->GetImmediateContext(&context);gpu.context.reset(context);
        }
        static ULONGLONG retryAt=0;
        if(!gpu.pipelineReady||!gpu.atlasView){
            if(GetTickCount64()<retryAt)return;retryAt=GetTickCount64()+5000;
            if(!gpu.pipelineReady&&FAILED(createPipeline(gpu)))return;
            if(!gpu.atlasView&&!loadAtlas(gpu))return;
            log_info("bone-eater","Lobby impact original atlas and isolated pipeline ready");
        }
        Com<ID3D11Texture2D> back;if(FAILED(createCom(back,[&](auto** out){return chain->GetBuffer(0,IID_PPV_ARGS(out));})))return;
        D3D11_TEXTURE2D_DESC d{};back->GetDesc(&d);if(!d.Width||!d.Height||d.SampleDesc.Count!=1)return;
        Com<ID3D11RenderTargetView> view;if(FAILED(createCom(view,[&](auto** out){return gpu.device->CreateRenderTargetView(back.get(),nullptr,out);})))return;
        std::array<std::uintptr_t,10> adapters{};
        for(unsigned i=0;i<10;++i){std::uintptr_t node=0;if(Read{}(gun,0x1A0+8*i,node))Read{}(node,0x30,adapters[i]);}
        const float scaleX=float(d.Width)/viewport.osClientWidth,scaleY=float(d.Height)/viewport.osClientHeight;
        const float left=viewport.left*scaleX,top=viewport.top*scaleY,width=(viewport.right-viewport.left)*scaleX,height=(viewport.bottom-viewport.top)*scaleY;
        if(width<=0||height<=0)return;
        {
            StateGuard restore(gpu);auto* c=gpu.context.get();
            auto* rt=view.get();c->OMSetRenderTargets(1,&rt,nullptr);c->OMSetBlendState(gpu.blend.get(),nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);
            D3D11_VIEWPORT vp{0,0,float(d.Width),float(d.Height),0,1};c->RSSetViewports(1,&vp);
            D3D11_RECT clip{LONG(left),LONG(top),LONG(left+width),LONG(top+height)};c->RSSetScissorRects(1,&clip);c->RSSetState(gpu.rasterizer.get());
            c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->VSSetShader(gpu.vertexShader.get(),nullptr,0);c->PSSetShader(gpu.pixelShader.get(),nullptr,0);
            auto* constants=gpu.constants.get();c->VSSetConstantBuffers(0,1,&constants);c->PSSetConstantBuffers(0,1,&constants);
            auto* texture=gpu.atlasView.get();auto* sampler=gpu.sampler.get();c->PSSetShaderResources(0,1,&texture);c->PSSetSamplers(0,1,&sampler);
            for(unsigned i=0;i<10;++i)if(queued[i]){
                const auto& q=poses[i];float values[16]={q.x,q.y,q.width,q.height,std::cos(q.angle),std::sin(q.angle),q.alpha,0,left,top,width,height,float(d.Width),float(d.Height),0,0};
                c->UpdateSubresource(constants,0,nullptr,values,0,0);c->Draw(6,0);
            }
        }
        std::uintptr_t checkOwner=0,checkGun=0;if(!ownerAt(base,checkOwner,checkGun)||checkOwner!=owner||checkGun!=gun)return;
        {std::lock_guard lock(p.mutex);p.owner=owner;p.gun=gun;p.adapters=adapters;p.readyAt=GetTickCount64();}
    }catch(...){std::lock_guard lock(p.mutex);p.readyAt=0;p.queued.fill(false);}
}
}
