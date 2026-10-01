#include "render/owned_hud_replay.h"
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
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
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
    auto front=makeSurface(device.Get(),768,1366), presentation=makeSurface(device.Get(),1920,1080);
    const char shader[]="struct V{float4 p:SV_Position;float4 c:COLOR;};V vs(uint i:SV_VertexID){V o;uint j=i%4;o.p=float4(float(i/4)+(j==1||j==2?1.0:0.0)-1.0,j<2?1:-1,0,1);o.c=float4(i<4?.5:1,i<4?1:.5,1,1);return o;}float4 ps(V i):SV_Target{return i.c;}";
    ComPtr<ID3DBlob> vs,ps,error;assert(SUCCEEDED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error)));
    assert(SUCCEEDED(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> fragment;
    assert(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
    assert(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&fragment)));
    context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(fragment.Get(),nullptr,0);
    const unsigned short indexData[]={0,1,2,0,2,3,4,5,6,4,6,7};
    D3D11_BUFFER_DESC ib {};ib.ByteWidth=sizeof(indexData);ib.Usage=D3D11_USAGE_IMMUTABLE;ib.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data {indexData,0,0};ComPtr<ID3D11Buffer> indices;
    assert(SUCCEEDED(device->CreateBuffer(&ib,&data,&indices)));
    context->IASetIndexBuffer(indices.Get(),DXGI_FORMAT_R16_UINT,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_RASTERIZER_DESC rd {};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;assert(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)));context->RSSetState(raster.Get());
    auto* rtv=front.target.Get();context->OMSetRenderTargets(1,&rtv,nullptr);
    const D3D11_VIEWPORT viewport {0,0,768,1366,0,1};context->RSSetViewports(1,&viewport);
    const float white[4]={1,1,1,1};context->ClearRenderTargetView(rtv,white);
    OwnedHudReplay replay;assert(replay.begin(context.Get(),1,DXGI_FORMAT_R8G8B8A8_UNORM));
    OwnedHudDrawPlan plan;plan.replacement=true;plan.count=2;plan.frame=1;plan.batch={1,1,1};plan.totalQuads=2;
    plan.ranges[0]={0,6,OwnedHudGroup::Left};plan.ranges[1]={6,6,OwnedHudGroup::Count};
    std::array<OwnedHudTransform,3> transforms {};transforms[0]={.5,0,0};
    Dispatch dispatch {context.Get()};
    assert(!replay.replay(context.Get(),2,rtv,viewport,plan,12,transforms,true,draw,&dispatch));
    auto invalid=plan;invalid.ranges[1].firstIndex=7;
    assert(!replay.replay(context.Get(),1,rtv,viewport,invalid,12,transforms,true,draw,&dispatch));
    assert(!replay.replay(context.Get(),1,rtv,viewport,plan,12,transforms,false,draw,&dispatch));
    assert(dispatch.calls==0);
    assert(replay.replay(context.Get(),1,rtv,viewport,plan,12,transforms,true,draw,&dispatch));
    assert(dispatch.calls==2 && replay.pending(1));
    assert(!replay.end());
    assert(!replay.begin(context.Get(),2,DXGI_FORMAT_R8G8B8A8_UNORM));
    ComPtr<ID3D11RenderTargetView> current;context->OMGetRenderTargets(1,&current,nullptr);assert(current.Get()==rtv);
    D3D11_VIEWPORT actual {};UINT count=1;context->RSGetViewports(&count,&actual);assert(actual.Width==768 && actual.Height==1366);
    const auto untouched=pixel(device.Get(),context.Get(),front,128,200);
    const auto original=pixel(device.Get(),context.Get(),front,640,200);
    assert(untouched[0]==255 && untouched[1]==255 && original[0]==255 && std::abs(int(original[1])-128)<=1);
    auto* output=presentation.target.Get();context->OMSetRenderTargets(1,&output,nullptr);
    const float background[4]={.8f,.6f,.4f,.7f};context->ClearRenderTargetView(output,background);
    const D3D11_VIEWPORT wide {0,0,1920,1080,0,1};context->RSSetViewports(1,&wide);
    assert(replay.compose(context.Get(),1,output));assert(!replay.pending(1));assert(!replay.compose(context.Get(),1,output));
    ComPtr<ID3D11VertexShader> restored;context->VSGetShader(&restored,nullptr,nullptr);assert(restored.Get()==vertex.Get());
    ComPtr<ID3D11Buffer> restoredIndices;DXGI_FORMAT indexFormat;UINT indexOffset;
    context->IAGetIndexBuffer(&restoredIndices,&indexFormat,&indexOffset);assert(restoredIndices.Get()==indices.Get());
    context->OMGetRenderTargets(1,current.ReleaseAndGetAddressOf(),nullptr);assert(current.Get()==output);
    const auto relocated=pixel(device.Get(),context.Get(),presentation,128,200);
    const auto outside=pixel(device.Get(),context.Get(),presentation,900,900);
    assert(std::abs(int(relocated[0])-102)<=2 && std::abs(int(relocated[1])-153)<=2 && std::abs(int(relocated[3])-179)<=2);
    assert(std::abs(int(outside[0])-204)<=2 && std::abs(int(outside[1])-153)<=2);
    replay.end();assert(replay.begin(context.Get(),2,DXGI_FORMAT_R8G8B8A8_UNORM));
    std::cout<<"WARP selective replay pixels, fallback gates, native gaps and pipeline restoration passed\n";
}
