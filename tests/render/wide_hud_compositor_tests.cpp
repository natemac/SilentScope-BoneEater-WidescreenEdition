#include "render/wide_hud_compositor.h"
#include <d3dcompiler.h>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace bone_eater::render;
using Microsoft::WRL::ComPtr;
struct Surface {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> target;
};
Surface makeSurface(ID3D11Device* device,UINT w,UINT h) {
    Surface out; D3D11_TEXTURE2D_DESC desc {};
    desc.Width=w;desc.Height=h;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&out.texture)));
    assert(SUCCEEDED(device->CreateRenderTargetView(out.texture.Get(),nullptr,&out.target)));
    return out;
}
std::array<unsigned char,4> pixel(ID3D11Device* device,ID3D11DeviceContext* context,Surface& source,UINT x,UINT y) {
    D3D11_TEXTURE2D_DESC desc {};source.texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));
    context->CopyResource(staging.Get(),source.texture.Get());
    D3D11_MAPPED_SUBRESOURCE map {};assert(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)));
    std::array<unsigned char,4> out;
    const auto* address=static_cast<const unsigned char*>(map.pData)+map.RowPitch*y+4*x;
    std::copy(address,address+4,out.begin());context->Unmap(staging.Get(),0);return out;
}
struct Dispatch { ID3D11DeviceContext* context; unsigned calls=0; };
void draw(void* opaque,UINT count,UINT first,INT base) {
    auto& d=*static_cast<Dispatch*>(opaque);++d.calls;d.context->DrawIndexed(count,first,base);
}
int main() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;D3D_FEATURE_LEVEL chosen;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,&chosen,&context)));
    auto front=makeSurface(device.Get(),768,1366),presentation=makeSurface(device.Get(),1920,1080);
    const char shader[]="struct V{float4 p:SV_Position;float4 c:COLOR;};V vs(uint i:SV_VertexID){V o;uint j=i%4;o.p=float4(j==1||j==2?1:-1,j<2?1:-1,0,1);o.c=float4(i<4?1:0,i<4?0:1,0,.5);return o;}float4 ps(V i):SV_Target{return i.c;}";
    ComPtr<ID3DBlob> vs,ps,error;
    assert(SUCCEEDED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)));
    assert(SUCCEEDED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> fragment;
    assert(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
    assert(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&fragment)));
    context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(fragment.Get(),nullptr,0);
    const unsigned short indexData[]={0,1,2,0,2,3,4,5,6,4,6,7};
    D3D11_BUFFER_DESC ib{};ib.ByteWidth=sizeof(indexData);ib.Usage=D3D11_USAGE_IMMUTABLE;ib.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{indexData,0,0};ComPtr<ID3D11Buffer> indices;
    assert(SUCCEEDED(device->CreateBuffer(&ib,&data,&indices)));
    context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;assert(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)));context->RSSetState(raster.Get());
    D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ZERO;b.DestBlendAlpha=D3D11_BLEND_ONE;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
    ComPtr<ID3D11BlendState> alpha;assert(SUCCEEDED(device->CreateBlendState(&bd,&alpha)));context->OMSetBlendState(alpha.Get(),nullptr,~0u);
    auto* native=front.target.Get();context->OMSetRenderTargets(1,&native,nullptr);
    const D3D11_VIEWPORT narrow{0,0,768,1366,0,1};context->RSSetViewports(1,&narrow);
    const float white[4]={1,1,1,1};context->ClearRenderTargetView(native,white);
    WideHudCompositor compositor;assert(compositor.clearFront(context.Get(),native,white));
    Dispatch dispatch{context.Get()};
    // Two overlapping half-alpha draws. Replaying all front draws in order
    // must produce (.5,.75,.25), not separate-layer multiply (.5,.5,.25).
    const WideHudRange ranges[]={{0,6,{0,0,1,1}},{6,6,{0,0,1,1}}};
    assert(compositor.mirrorFront(context.Get(),native,ranges,draw,&dispatch));
    const auto untouched=pixel(device.Get(),context.Get(),front,100,100);assert(untouched[0]==255&&untouched[1]==255);
    auto* output=presentation.target.Get();context->OMSetRenderTargets(1,&output,nullptr);
    const D3D11_VIEWPORT wide{0,0,1920,1080,0,1};context->RSSetViewports(1,&wide);
    const float scene[4]={1,1,1,.7f};context->ClearRenderTargetView(output,scene);
    assert(!compositor.publish(context.Get(),output)); // Rear branch absent.
    assert(compositor.forkScene(context.Get(),output));
    assert(compositor.publish(context.Get(),output));
    const auto result=pixel(device.Get(),context.Get(),presentation,100,100);
    assert(std::abs(int(result[0])-128)<=2&&std::abs(int(result[1])-192)<=2&&std::abs(int(result[2])-64)<=2&&std::abs(int(result[3])-179)<=2);
    const auto outside=pixel(device.Get(),context.Get(),presentation,1800,100);assert(outside[0]==255&&outside[1]==255);
    ComPtr<ID3D11RenderTargetView> current;context->OMGetRenderTargets(1,&current,nullptr);assert(current.Get()==output);
    ComPtr<ID3D11VertexShader> restored;context->VSGetShader(&restored,nullptr,nullptr);assert(restored.Get()==vertex.Get());
    // A failed branch leaves native pixels untouched, including later draws.
    context->ClearRenderTargetView(output,scene);assert(compositor.forkScene(context.Get(),output));
    const WideHudRange rear[]={{0,6,{0,0,.5f,.5f}}};
    assert(compositor.mirrorScene(context.Get(),output,rear,draw,&dispatch));
    compositor.invalidate();assert(!compositor.publish(context.Get(),output));
    const auto fallback=pixel(device.Get(),context.Get(),presentation,100,100);assert(fallback[0]==255&&fallback[1]==255);
    // Production publication uses the original consumer shader and executes
    // its native alpha write once, then copies only the alternate RGB.
    context->OMSetRenderTargets(1,&native,nullptr);context->RSSetViewports(1,&narrow);
    assert(compositor.clearFront(context.Get(),native,white));
    assert(compositor.mirrorFront(context.Get(),native,ranges,draw,&dispatch));
    context->OMSetRenderTargets(1,&output,nullptr);context->RSSetViewports(1,&wide);
    context->ClearRenderTargetView(output,scene);assert(compositor.forkScene(context.Get(),output));
    const char consumerShader[]="Texture2D t:register(t0);float4 ps(float4 p:SV_Position):SV_Target{return float4(t.Load(int3(p.xy,0)).rgb,.25);}";
    ComPtr<ID3DBlob> consumerCode;assert(SUCCEEDED(D3DCompile(consumerShader,sizeof(consumerShader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&consumerCode,&error)));
    ComPtr<ID3D11PixelShader> consumerPixel;assert(SUCCEEDED(device->CreatePixelShader(consumerCode->GetBufferPointer(),consumerCode->GetBufferSize(),nullptr,&consumerPixel)));
    context->PSSetShader(consumerPixel.Get(),nullptr,0);
    ComPtr<ID3D11ShaderResourceView> nativeSource;assert(SUCCEEDED(device->CreateShaderResourceView(front.texture.Get(),nullptr,&nativeSource)));
    auto* source=nativeSource.Get();context->PSSetShaderResources(0,1,&source);
    b.SrcBlend=D3D11_BLEND_ZERO;b.DestBlend=D3D11_BLEND_SRC_COLOR;b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_ZERO;
    ComPtr<ID3D11BlendState> multiply;assert(SUCCEEDED(device->CreateBlendState(&bd,&multiply)));context->OMSetBlendState(multiply.Get(),nullptr,~0u);
    const WideHudRange consumer{0,6,{}};assert(compositor.publishNative(context.Get(),output,consumer,draw,&dispatch));
    const auto production=pixel(device.Get(),context.Get(),presentation,100,100);
    assert(std::abs(int(production[0])-128)<=2&&std::abs(int(production[1])-192)<=2&&std::abs(int(production[2])-64)<=2&&std::abs(int(production[3])-64)<=1);
    ComPtr<ID3D11ShaderResourceView> restoredSource;context->PSGetShaderResources(0,1,&restoredSource);assert(restoredSource.Get()==nativeSource.Get());
    auto makeDepth=[&](UINT width,UINT height){ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC td{};
        td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        assert(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&texture)));ComPtr<ID3D11DepthStencilView> view;
        assert(SUCCEEDED(device->CreateDepthStencilView(texture.Get(),nullptr,&view)));return view;};
    auto frontDepth=makeDepth(768,1366),sceneDepth=makeDepth(1920,1080);
    context->PSSetShader(fragment.Get(),nullptr,0);context->OMSetBlendState(alpha.Get(),nullptr,~0u);context->OMSetDepthStencilState(nullptr,0);
    context->OMSetRenderTargets(1,&native,frontDepth.Get());context->RSSetViewports(1,&narrow);
    context->ClearRenderTargetView(native,white);context->ClearDepthStencilView(frontDepth.Get(),D3D11_CLEAR_DEPTH,1,0);
    assert(compositor.clearFront(context.Get(),native,white));assert(compositor.primeFrontDepth(context.Get(),frontDepth.Get(),D3D11_CLEAR_DEPTH,1,0));
    assert(compositor.mirrorFront(context.Get(),native,ranges,draw,&dispatch));
    context->OMSetRenderTargets(1,&output,sceneDepth.Get());context->RSSetViewports(1,&wide);
    context->ClearRenderTargetView(output,scene);context->ClearDepthStencilView(sceneDepth.Get(),D3D11_CLEAR_DEPTH,1,0);
    assert(compositor.forkScene(context.Get(),output));context->PSSetShader(consumerPixel.Get(),nullptr,0);
    context->PSSetShaderResources(0,1,&source);context->OMSetBlendState(multiply.Get(),nullptr,~0u);
    assert(compositor.publishNative(context.Get(),output,consumer,draw,&dispatch));
    const auto depthOrdered=pixel(device.Get(),context.Get(),presentation,100,100);
    // LESS rejects the second coplanar quad; removing depth would turn it green.
    assert(depthOrdered[0]==255&&std::abs(int(depthOrdered[1])-128)<=2&&std::abs(int(depthOrdered[2])-128)<=2);
    // A relocated widget can cross the old portrait clip boundary. Transform
    // clip coordinates before rasterization, retaining native color varyings.
    const char outsideShader[]="struct V{float4 p:SV_Position;float4 c:COLOR;};V vs(uint i:SV_VertexID){V o;uint j=i%4;o.p=float4(j==1||j==2?1.2:-1.2,j<2?1:-1,0,1);o.c=float4(1,0,0,.5);return o;}";
    ComPtr<ID3DBlob> outsideCode;assert(SUCCEEDED(D3DCompile(outsideShader,sizeof(outsideShader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&outsideCode,&error)));
    ComPtr<ID3D11VertexShader> outsideVertex;assert(SUCCEEDED(device->CreateVertexShader(outsideCode->GetBufferPointer(),outsideCode->GetBufferSize(),nullptr,&outsideVertex)));
    recordWideHudVertexShader(outsideVertex.Get(),outsideCode->GetBufferPointer(),outsideCode->GetBufferSize());
    context->VSSetShader(outsideVertex.Get(),nullptr,0);context->PSSetShader(fragment.Get(),nullptr,0);
    context->OMSetRenderTargets(1,&native,nullptr);context->RSSetViewports(1,&narrow);context->OMSetBlendState(alpha.Get(),nullptr,~0u);
    assert(compositor.clearFront(context.Get(),native,white));
    const WideHudRange unclipped{0,6,{80,0,1,1},true};
    assert(compositor.mirrorFront(context.Get(),native,{&unclipped,1},draw,&dispatch));
    ComPtr<ID3D11GeometryShader> restoredGeometry;context->GSGetShader(&restoredGeometry,nullptr,nullptr);assert(!restoredGeometry);
    context->OMSetRenderTargets(1,&output,nullptr);context->RSSetViewports(1,&wide);context->ClearRenderTargetView(output,scene);
    assert(compositor.forkScene(context.Get(),output));assert(compositor.publish(context.Get(),output));
    const auto edge=pixel(device.Get(),context.Get(),presentation,50,100);
    assert(edge[0]==255&&std::abs(int(edge[1])-128)<=2&&std::abs(int(edge[2])-128)<=2);
    // Regression: legacy half-pixel vertices must not leave a black clear
    // column at the right portrait boundary when mirrored to a wider target.
    const char legacyShader[]="struct V{float4 p:SV_Position;float4 c:COLOR;};V vs(uint i:SV_VertexID){V o;uint j=i%4;o.p=float4((j==1||j==2?1:-1)*(1-1.0/768),(j<2?1:-1)+1.0/1366,0,1);o.c=1;return o;}";
    ComPtr<ID3DBlob> legacyCode;assert(SUCCEEDED(D3DCompile(legacyShader,sizeof(legacyShader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&legacyCode,&error)));
    ComPtr<ID3D11VertexShader> legacyVertex;assert(SUCCEEDED(device->CreateVertexShader(legacyCode->GetBufferPointer(),legacyCode->GetBufferSize(),nullptr,&legacyVertex)));
    context->VSSetShader(legacyVertex.Get(),nullptr,0);context->PSSetShader(fragment.Get(),nullptr,0);context->OMSetBlendState(nullptr,nullptr,~0u);
    context->OMSetRenderTargets(1,&native,nullptr);context->RSSetViewports(1,&narrow);
    const float black[4]{};assert(compositor.clearFront(context.Get(),native,black));
    const WideHudRange legacyRange{0,6,wideHudOriginal};assert(compositor.mirrorFront(context.Get(),native,{&legacyRange,1},draw,&dispatch));
    context->OMSetRenderTargets(1,&output,nullptr);context->RSSetViewports(1,&wide);context->ClearRenderTargetView(output,scene);
    assert(compositor.forkScene(context.Get(),output));assert(compositor.publish(context.Get(),output));
    for(UINT x: {654u,655u,656u,657u,658u,1260u,1261u,1262u,1263u,1264u,1265u,1266u,1267u,1268u}){const auto seam=pixel(device.Get(),context.Get(),presentation,x,1000);assert(seam[0]==255&&seam[1]==255&&seam[2]==255);}
    std::cout<<"WARP complete front overlap, paired publication, discarded branch and pipeline restoration passed\n";
}
