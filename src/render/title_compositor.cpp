#include "render/score_results_identity.h"
#include "render/score_round_identity.h"
#include "render/title_compositor.h"
#include "render/native_viewport.h"
#include "render/title_presentation_policy.h"
#include "render/title_art_rotation.h"
#include "render/phone_card_identity.h"
#include "render/loading_screen_identity.h"
#include "render/menu_landscape_layout.h"
#include "render/score_lobby_identity.h"
#include "render/news_presentation_policy.h"
#include "util/logging.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bone_eater::render {
namespace {
struct Release { template<class T> void operator()(T* p) const { if(p) p->Release(); } };
template<class T> using Com = std::unique_ptr<T,Release>;
template<class T,class F> HRESULT make(Com<T>& p,F f) {
    T* raw=nullptr; HRESULT hr=f(&raw); p.reset(raw); return raw ? hr : E_FAIL;
}
template<class T> bool read(uintptr_t p,T& value) {
    SIZE_T n=0;
    return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),&value,sizeof(value),&n) && n==sizeof(value);
}
template<class T,size_t N> T field(const std::array<unsigned char,N>& b,size_t o) {
    T v{}; std::memcpy(&v,b.data()+o,sizeof(v)); return v;
}
struct Route {
    uintptr_t manager=0,title=0,layout=0;
    unsigned phase=0;
    float alpha=0;
    bool operator==(const Route&) const = default;
};
bool phoneRoute(uintptr_t base,Route& r) {
    r={};r.phase=900;
    return scorePhoneCard(base,[](auto p,auto o,auto& v){return read(p+o,v);},r.title,r.layout,r.alpha);
}
// Observe the verified title lifecycle even when no replacement route draws.
void observeArtRotation(uintptr_t base,TitleArtRotation& rotation) {
    uintptr_t manager=0;std::array<unsigned char,0x48> m{};
    std::array<unsigned char,0x18> t{};
    if(!read(base+0x13DD000,manager)||!read(manager,m)||field<uintptr_t>(m,0)!=base+0x10C6B38)return;
    const auto scene=field<unsigned>(m,0x38);
    constexpr uintptr_t types[]{0,0x10C7278,0x10C7030,0x10C7308,0x10C6FA0,0x10C6E48,0x10C7240,0x10C72B0};
    if(scene<1||scene>7||field<int>(m,0x3C)!=-1)return;
    const auto owner=field<uintptr_t>(m,0x40);
    if(!read(owner,t)||field<uintptr_t>(t,0)!=base+types[scene]||t[8]||t[9])return;
    if(scene!=1){rotation.leave();return;}
    if(field<unsigned>(t,0x10)==1)rotation.observe(owner,field<unsigned>(t,0x14));
}
// Scene lifetime, independent of individual menu animation/layout alpha.
// These typed owners are pre-game menus only; never retain a frame latch.
bool menuBackgroundRoute(uintptr_t base,Route& r) {
    r={};std::array<unsigned char,0x48> m{};std::array<unsigned char,0x18> owner{};
    if(!read(base+0x13DD000,r.manager)||!read(r.manager,m)||field<uintptr_t>(m,0)!=base+0x10C6B38)return false;
    const auto scene=field<unsigned>(m,0x38);
    const uintptr_t type=scene==2?0x10C7030:scene==3?0x10C7308:scene==4?0x10C6FA0:scene==6?0x10C7240:0;
    if(!type)return false;
    const int pending=field<int>(m,0x3C);
    if(pending!=-1&&pending!=2&&pending!=3&&pending!=4&&pending!=6)return false;
    r.title=field<uintptr_t>(m,0x40);
    if(!read(r.title,owner)||field<uintptr_t>(owner,0)!=base+type||owner[9])return false;
    r.phase=700+scene;r.alpha=1;return true;
}
bool newsRoute(uintptr_t base,Route& r) {
    std::array<unsigned char,0x48> m{},t{};
    std::array<unsigned char,0x638> child{};
    std::array<unsigned char,0x70> layout{};
    if(!read(base+0x13DD000,r.manager)||!read(r.manager,m)||
       field<uintptr_t>(m,0)!=base+0x10C6B38||field<unsigned>(m,0x38)!=1||field<int>(m,0x3C)!=-1)return false;
    r.title=field<uintptr_t>(m,0x40);
    if(!read(r.title,t)||field<uintptr_t>(t,0)!=base+0x10C7278||t[8]||t[9]||
       field<unsigned>(t,0x10)!=1||field<unsigned>(t,0x14)!=13)return false;
    auto owner=field<uintptr_t>(t,0x40);
    if(!read(owner,child)||field<uintptr_t>(child,0)!=base+0x10CA770||
       field<unsigned>(child,0x634)!=1||field<unsigned>(child,0x10)>3)return false;
    // Child state 3 has already destroyed its layout, while the title owner
    // is still fading the resident white backing. Keep replacing that frame.
    r.layout=owner;
    uintptr_t resident=0,backing=0,root=0,leaf=0,vt=0;
    std::array<char,32> name{};
    if(!read(base+0x13DCF48,resident)||!read(resident+0x128,backing)||!read(backing,layout)||
       field<uintptr_t>(layout,0)!=base+0x10C9FA8||field<unsigned>(layout,0x64)!=0xBAA||
       !read(backing+0x428,root)||!read(root,vt)||vt!=base+0x10CEB48||
       !read(root+0x18,leaf)||!read(leaf,vt)||vt!=base+0x10CEBA8||
       !read(leaf+0x40,name)||std::memcmp(name.data(),"white\0",6)||!read(leaf+0xBC,r.alpha))return false;
    r.phase=13;return std::isfinite(r.alpha)&&r.alpha>=0&&r.alpha<=1;
}
bool guidanceRoute(uintptr_t base,Route& r) {
    std::array<unsigned char,0x48> manager{};
    std::array<unsigned char,0x20> parent{};
    std::array<unsigned char,0x30> child{};
    std::array<unsigned char,0x70> layout{};
    uintptr_t owner=0;
    if(!read(base+0x13DD000,r.manager)||!read(r.manager,manager)||
       field<uintptr_t>(manager,0)!=base+0x10C6B38||field<unsigned>(manager,0x38)!=3||field<int>(manager,0x3C)!=-1)return false;
    owner=field<uintptr_t>(manager,0x40);
    if(!read(owner,parent)||field<uintptr_t>(parent,0)!=base+0x10C7308||parent[8]||parent[9]||field<unsigned>(parent,0x10)!=2)return false;
    r.title=field<uintptr_t>(parent,0x18);
    if(!read(r.title,child)||field<uintptr_t>(child,0)!=base+0x10CA6F0||child[8]||child[9])return false;
    r.phase=field<unsigned>(child,0x10);if(r.phase<75||r.phase>77)return false;
    r.layout=field<uintptr_t>(child,0x28);
    if(!read(r.layout,layout)||field<uintptr_t>(layout,0)!=base+0x10C9FA8||
       field<unsigned>(layout,0x18)!=2||field<unsigned>(layout,0x64)!=0xB75)return false;
    r.alpha=field<float>(layout,0x58);
    return std::isfinite(r.alpha)&&r.alpha>=0&&r.alpha<=1;
}
bool route(uintptr_t base,Route& r) {
    std::array<unsigned char,0x48> m{};
    std::array<unsigned char,0x30> t{};
    std::array<unsigned char,0x70> l{};
    if(!read(base+0x13DD000,r.manager)||!read(r.manager,m)||
       field<uintptr_t>(m,0)!=base+0x10C6B38||field<unsigned>(m,0x38)!=1||field<int>(m,0x3C)!=-1) return false;
    r.title=field<uintptr_t>(m,0x40);
    if(!read(r.title,t)||field<uintptr_t>(t,0)!=base+0x10C7278||t[8]||t[9]||field<unsigned>(t,0x10)!=1) return false;
    r.phase=field<unsigned>(t,0x14);
    // Exact Title art entry/active/Start exit. Ranking12 and attract movies
    // have the same scene owner, so scene identity alone is insufficient.
    if(r.phase!=6 && r.phase!=7 && r.phase!=8 && r.phase!=15 && r.phase!=16) return false;
    r.layout=field<uintptr_t>(t,0x18);
    if(!read(r.layout,l)||field<uintptr_t>(l,0)!=base+0x10C9FA8||field<unsigned>(l,0x64)!=0xC88) return false;
    r.alpha=field<float>(l,0x58);
    return std::isfinite(r.alpha)&&r.alpha>=0&&r.alpha<=1;
}
// Frame native screens separately from replacement title artwork.
bool framingRoute(uintptr_t base,Route& r) {
    // Earlier route probes may have partially populated this object. Both
    // admission reads must compare only this route's complete identity.
    r={};
    {
        if(scoreLobbyPresentation(base,[](auto p,auto o,auto& v){return read(p+o,v);},r.title,r.layout,r.alpha)) {
            // All live lobby phases use the same presentation. Retain owner,
            // layout and fade identity without rejecting a phase-only advance.
            r.phase=500;return true;
        }
    }

    {
        unsigned phase=0;
        if(landscapeMenuOwner(base,[](auto p,auto o,auto& value){return read(p+o,value);},r.title,phase,true,true)) {
            r.phase=200+phase;r.alpha=1;return true;
        }
    }
    LoadingScreenIdentity loading;
    if(readLoadingScreen(base,[](auto p,auto o,auto& value){return read(p+o,value);},loading)&&loading.phase!=2) {
        r.manager=loading.manager;r.title=loading.scene;r.layout=loading.splash;
        // These states share one presentation. A phase change between the
        // two admission reads must not expose a portrait frame at the handoff.
        r.phase=101;r.alpha=1;return true;
    }
    std::array<unsigned char,0x48> m{};std::array<unsigned char,0x40> t{};
    std::array<unsigned char,0x58> child{};
    if(!read(base+0x13DD000,r.manager)||!read(r.manager,m)||field<uintptr_t>(m,0)!=base+0x10C6B38||
       field<unsigned>(m,0x38)!=1||field<int>(m,0x3C)!=-1)return false;
    r.title=field<uintptr_t>(m,0x40);
    if(!read(r.title,t)||field<uintptr_t>(t,0)!=base+0x10C7278||t[8]||t[9]||field<unsigned>(t,0x10)!=1)return false;
    r.phase=field<unsigned>(t,0x14);r.alpha=1;
    if(r.phase==2){r.layout=field<uintptr_t>(t,0x28);
        return read(r.layout,child)&&field<uintptr_t>(child,0)==base+0x10C7010&&field<unsigned>(child,8)<=23;}
    if(r.phase==12){r.layout=field<uintptr_t>(t,0x38);
        return read(r.layout,child)&&field<uintptr_t>(child,0)==base+0x10C6FF0&&field<unsigned>(child,8)<=3&&child[0x2D]<=1&&child[0x2E]<=1;}
    return false;
}
struct Gpu {
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    Com<ID3D11DeviceContext1> context1;
    Com<ID3DDeviceContextState> pass;
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11Buffer> constants;
    Com<ID3D11SamplerState> sampler;
    Com<ID3D11RasterizerState> raster;
    Com<ID3D11ShaderResourceView> art,menuArt,prompt,guidance,snapshotView,news,phone;
    Com<ID3D11Texture2D> snapshot;
    UINT width=0,height=0,menuWidth=0,menuHeight=0;
    unsigned pairIndex=~0u;
    UINT newsWidth=0,newsHeight=0;
    UINT phoneWidth=0,phoneHeight=0;
};
struct LobbyBackdrop {
    Com<ID3D11Texture2D> texture;
    Com<ID3D11ShaderResourceView> view;
    ID3D11DeviceContext* context=nullptr;
    uintptr_t owner=0;
    bool fresh=false;
};
thread_local LobbyBackdrop lobbyBackdrop;
thread_local LobbyBackdrop roundBackdrop;
thread_local uintptr_t roundTarget=0;
bool texture(Gpu& g,UINT w,UINT h,const void* bytes,Com<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{bytes,w*4,0};Com<ID3D11Texture2D> t;
    return SUCCEEDED(make(t,[&](auto out){return g.device->CreateTexture2D(&d,&data,out);}))&&
        SUCCEEDED(make(view,[&](auto out){return g.device->CreateShaderResourceView(t.get(),nullptr,out);}));
}
bool loadImage(Gpu& g,const wchar_t* name,Com<ID3D11ShaderResourceView>& view,UINT& width,UINT& height) {
    wchar_t exe[32768]{};DWORD n=GetModuleFileNameW(nullptr,exe,32768);
    if(!n||n>=32768) return false;
    std::wstring path(exe,n);path.resize(path.find_last_of(L"\\/")+1);path+=L"desktop\\";path+=name;
    HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(apartment)&&apartment!=RPC_E_CHANGED_MODE) return false;
    struct Apartment { HRESULT hr; ~Apartment(){if(SUCCEEDED(hr))CoUninitialize();} } guard{apartment};
    Com<IWICImagingFactory> factory;Com<IWICBitmapDecoder> decoder;Com<IWICBitmapFrameDecode> frame;Com<IWICFormatConverter> converter;
    if(FAILED(make(factory,[](auto out){return CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(out));}))||
       FAILED(make(decoder,[&](auto out){return factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,out);}))||
       FAILED(make(frame,[&](auto out){return decoder->GetFrame(0,out);}))||FAILED(frame->GetSize(&width,&height))||
       width<320||height<180||width>4096||height>4096||
       FAILED(make(converter,[&](auto out){return factory->CreateFormatConverter(out);}))||
       FAILED(converter->Initialize(frame.get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return false;
    std::vector<unsigned char> pixels(size_t(width)*height*4);
    return SUCCEEDED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(pixels.size()),pixels.data()))&&
       texture(g,width,height,pixels.data(),view);
}
bool loadPair(Gpu& g,unsigned index) {
    if(index>=titleArtPairCount)return false;
    const auto suffix=titleArtSuffix(index);
    Com<ID3D11ShaderResourceView> intro,menu;
    UINT w=0,h=0,mw=0,mh=0;
    if(!loadImage(g,(L"introscreen"+suffix).c_str(),intro,w,h)||
       !loadImage(g,(L"menuscreen"+suffix).c_str(),menu,mw,mh))return false;
    // Commit together, never pair a new title with an older menu texture.
    g.art=std::move(intro);g.menuArt=std::move(menu);
    g.width=w;g.height=h;g.menuWidth=mw;g.menuHeight=mh;g.pairIndex=index;
    return true;
}
bool loadPrompt(Gpu& g) {
    // Draw desktop guidance into its own texture. The user's PNG is unchanged.
    constexpr UINT w=1000,h=200;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
    info.bmiHeader.biHeight=-int(h);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bitmap){DeleteDC(dc);return false;}
    HGDIOBJ previous=SelectObject(dc,bitmap);std::memset(bits,0,w*h*4);
    HFONT font=CreateFontW(52,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!font){SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return false;}
    HGDIOBJ oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
    RECT rect{0,8,int(w),78};DrawTextW(dc,L"PRESS START",-1,&rect,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,oldFont);DeleteObject(font);
    font=CreateFontW(26,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(font){oldFont=SelectObject(dc,font);rect={0,83,int(w),132};DrawTextW(dc,L"FREE PLAY  /  OFFLINE MODE",-1,&rect,DT_CENTER|DT_SINGLELINE|DT_VCENTER);SelectObject(dc,oldFont);DeleteObject(font);}
    font=CreateFontW(28,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!font){SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return false;}
    oldFont=SelectObject(dc,font);rect={0,150,int(w),200};
    DrawTextW(dc,L"BUILD 20261007.3",-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
    SelectObject(dc,oldFont);DeleteObject(font);
    GdiFlush();bool okay=texture(g,w,h,bits,g.prompt);
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return okay;
}
bool loadGuidance(Gpu& g) {
    constexpr UINT w=1056,h=480;
    HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
    info.bmiHeader.biHeight=-int(h);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!bitmap){DeleteDC(dc);return false;}
    auto previous=SelectObject(dc,bitmap);std::memset(bits,0,w*h*4);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
    bool valid=true;
    auto text=[&](const wchar_t* value,int x,int y,int width,int height,int size,bool bold=false) {
        HFONT f=CreateFontW(size,0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if(!f){valid=false;return;}auto old=SelectObject(dc,f);RECT r{x,y,x+width,y+height};
        if(!DrawTextW(dc,value,-1,&r,DT_LEFT|DT_SINGLELINE|DT_VCENTER))valid=false;
        SelectObject(dc,old);DeleteObject(f);
    };
    text(L"DESKTOP CONTROLS",48,20,960,60,38,true);
    const wchar_t* actions[]{L"AIM",L"FIRE / SELECT",L"SCOPE",L"AIM HEIGHT"};
    const wchar_t* keys[]{L"Move mouse",L"Left mouse button",L"Right mouse button / Enter",L"Up / Down arrow keys"};
    for(int i=0;i<4;++i){text(actions[i],48,108+i*53,280,45,27,true);text(keys[i],360,108+i*53,650,45,29);}
    text(L"PRESS START / ENTER TO CONTINUE",48,349,960,50,30,true);
    text(L"Default mouse bindings shown. Custom devices use your configured controls.",48,420,980,35,22);
    GdiFlush();bool okay=valid&&texture(g,w,h,bits,g.guidance);
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return okay;
}
constexpr char shaders[]=R"(
cbuffer Placement:register(b0){float2 target;float2 source;float alpha;float mode;float2 pad;float4 background;};
Texture2D art:register(t0);Texture2D prompt:register(t1);Texture2D guidance:register(t2);Texture2D snapshot:register(t3);Texture2D world:register(t4);SamplerState linearSampler:register(s0);
float4 vsMain(uint i:SV_VertexID):SV_POSITION {return float4(i==0?float2(-1,-1):i==1?float2(-1,3):float2(3,-1),0,1);}
void foreground(float4 p,out float4 result) {
 result=float4(0,0,0,1);
 if(mode>6.5){result=snapshot.Sample(linearSampler,p.xy/target);return;}
 if(mode>5.5) {
   float3 scene=world.Sample(linearSampler,p.xy/target).rgb;
   // Native opaque black fades with the root. Match that fade across the
   // sides, capped at the requested 50% darkness once the lobby is visible.
   float brightness=max(0.5,1-alpha);
   if(p.x<658||p.x>=1263){result=float4(scene*brightness,1);return;}
   float3 panel=snapshot.Sample(linearSampler,p.xy/target).rgb;
   {result=float4(max(panel,scene*brightness),1);return;}
 }
 if(mode>4.5) {
   float2 uv=float2(clamp((p.x-source.x)/source.y,0.005,0.995),clamp(p.y/target.y,0.01,0.97));
   if(p.x<source.x||p.x>=source.x+source.y)uv=float2(0.05,0.95);
   {result=float4(world.Sample(linearSampler,p.xy/target).rgb*snapshot.Sample(linearSampler,uv).rgb,1);return;}
 }
 if(mode>3.5) {
   float2 q=(p.xy-float2(1020,110))/1.3;
   if(all(q>=0)&&all(q<float2(607,640))){result=snapshot.Sample(linearSampler,(float2(657,180)+q)/target);return;}
   // The native choice panels animate through the old header/footer. Do not
   // copy those departing fragments into the separate instruction column.
   if(pad.x>0.5) {
     q=(p.xy-float2(80,270))/1.35;
     if(all(q>=0)&&all(q<float2(607,180))){result=snapshot.Sample(linearSampler,(float2(657,0)+q)/target);return;}
   }
   q=(p.xy-float2(80,810))/1.2;
   if(all(q>=0)&&all(q<float2(607,80))){result=snapshot.Sample(linearSampler,(float2(657,970)+q)/target);return;}
   {result=float4(0,0,0,1);return;}
 }
 if(mode>2.5) {
   // Fit all five ranking rows uniformly. Extend only the empty native left
   // background column into the sides, retaining the native transition fade.
   float scale=min(target.x/pad.x,target.y/pad.y);
   float2 q=(p.xy-(target-pad*scale)*0.5)/scale;
q.x=clamp(q.x,0.5,pad.x-0.5);
   q.y=clamp(q.y,0.5,pad.y-0.5);
   {result=snapshot.Sample(linearSampler,(source+q)/target);return;}
 }
 if(mode>1.5){result=snapshot.Sample(linearSampler,(source+(p.xy/target)*pad)/target);return;}
 if(mode>0.5) {
   float2 uv=(p.xy/target-float2(.225,.23))/float2(.55,.445);
   if(any(uv<0)||any(uv>1)){result=snapshot.Sample(linearSampler,p.xy/target);return;}
   float ink=guidance.Sample(linearSampler,uv).r;
   float3 color=lerp(float3(.035,.065,.08),float3(.92,.97,1),ink);
   if(uv.x<.003||uv.x>.997||uv.y<.006||uv.y>.994||(uv.y>.19&&uv.y<.195))color=float3(.24,.68,.74);
   {result=float4(color*alpha,1);return;}
 }
 float scale=mode<-1.5?max(target.x/source.x,target.y/source.y):min(target.x/source.x,target.y/source.y);float2 extent=source*scale;
 float2 uv=(p.xy-(target-extent)*.5)/extent;
 float3 color=0;if(all(uv>=0)&&all(uv<=1)) color=art.Sample(linearSampler,uv).rgb;
 float2 label=(uv-float2(.51,.83))/float2(.44,.135);
 if(mode>=0&&all(label>=0)&&all(label<=1)) {float ink=prompt.Sample(linearSampler,float2(label.x,label.y*.75)).r;color=lerp(color,float3(1,.96,.88),ink);}
 // Build stamp uses its own atlas row; intro only, no opaque backing panel.
 float2 stamp=(p.xy/target-float2(.015,.947))/float2(.42,.04);
 if(mode>=0&&all(stamp>=0)&&all(stamp<=1)) {
   float ink=prompt.Sample(linearSampler,float2(stamp.x,.75+stamp.y*.25)).r;
   float2 shadow=stamp-float2(2.0,2.0)/(target*float2(.42,.04));
   float shade=all(shadow>=0)&&all(shadow<=1)?prompt.Sample(linearSampler,float2(shadow.x,.75+shadow.y*.25)).r:0;
   color=lerp(color,float3(0,0,0),shade*.85);
   color=lerp(color,float3(.95,.95,.95),ink);
 }
 {result=float4(color*alpha,1);return;}
}
float4 psMain(float4 p:SV_POSITION):SV_TARGET {
 float4 fg=float4(0,0,0,1);foreground(p,fg);
 if(background.w<0.5)return fg;
 // Native menus arrive flattened over black. Keep bright text/art intact,
 // filling their black backdrop without multiplying by the menu fade alpha.
 float scale=max(target.x/background.x,target.y/background.y);
 float2 extent=background.xy*scale;
 float2 uv=(p.xy-(target-extent)*0.5)/extent;
 float3 bg=art.Sample(linearSampler,uv).rgb*background.z;
 float coverage=saturate(max(fg.r,max(fg.g,fg.b)));
 return float4(fg.rgb+bg*(1-coverage),1);
})";
bool initialize(Gpu& g) {
    Com<ID3D11Device1> device1;
    if(FAILED(make(device1,[&](auto out){return g.device->QueryInterface(IID_PPV_ARGS(out));}))||
       FAILED(make(g.context1,[&](auto out){return g.context->QueryInterface(IID_PPV_ARGS(out));})))return false;
    auto level=g.device->GetFeatureLevel();UINT flags=(g.device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)?D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED:0;
    if(FAILED(make(g.pass,[&](auto out){return device1->CreateDeviceContextState(flags,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,out);})))return false;
    HMODULE library=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!library)return false;
    struct Library { HMODULE h;~Library(){FreeLibrary(h);} } compilerGuard{library};
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(library,"D3DCompile"));if(!compile)return false;
    Com<ID3DBlob> vs,ps;
    if(FAILED(make(vs,[&](auto out){return compile(shaders,sizeof(shaders)-1,"DesktopTitle",nullptr,nullptr,"vsMain","vs_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,out,nullptr);}))||
       FAILED(make(ps,[&](auto out){return compile(shaders,sizeof(shaders)-1,"DesktopTitle",nullptr,nullptr,"psMain","ps_4_0",D3DCOMPILE_ENABLE_STRICTNESS,0,out,nullptr);}))||
       FAILED(make(g.vs,[&](auto out){return g.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,out);}))||
       FAILED(make(g.ps,[&](auto out){return g.device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,out);})))return false;
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=48;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;
    return SUCCEEDED(make(g.constants,[&](auto out){return g.device->CreateBuffer(&cb,nullptr,out);}))&&
      SUCCEEDED(make(g.sampler,[&](auto out){return g.device->CreateSamplerState(&s,out);}))&&
      SUCCEEDED(make(g.raster,[&](auto out){return g.device->CreateRasterizerState(&r,out);}))&&loadGuidance(g);
}
}
bool lobbyBackdropRequested() noexcept {
    auto module=GetModuleHandleW(L"gamendd.dll");if(!module)return false;
    uintptr_t owner=0;unsigned phase=0;
    return scoreLobbyOwner(reinterpret_cast<uintptr_t>(module),[](auto p,auto o,auto& v){return read(p+o,v);},owner,phase,true);
}
bool lobbyReticleRequested() noexcept {
    auto module=GetModuleHandleW(L"gamendd.dll");if(!module)return false;
    uintptr_t owner=0;unsigned phase=0;
    return scoreLobbyOwner(reinterpret_cast<uintptr_t>(module),[](auto p,auto o,auto& v){return read(p+o,v);},owner,phase)&&phase>=3&&phase<=7;
}
bool resultsReticleRequested() noexcept {
    auto module=GetModuleHandleW(L"gamendd.dll");
    return module&&scoreResultsReticle(reinterpret_cast<uintptr_t>(module),
        [](auto p,auto o,auto& v){return read(p+o,v);});
}
void captureLobbyBackdrop(ID3D11DeviceContext* context,uintptr_t material,bool frontConsumer) noexcept {
    if(!context||!material||lobbyBackdrop.fresh)return;
    try {
        auto module=GetModuleHandleW(L"gamendd.dll");
        static uintptr_t base=0;if(!base){if(!module||std::strcmp(verifyNativeGameModule(module),"verified"))return;base=reinterpret_cast<uintptr_t>(module);}
        uintptr_t owner=0,layout=0,root=0,node=0,vt=0;float alpha=0;
        if(!scoreLobbyPresentation(base,[](auto p,auto o,auto& v){return read(p+o,v);},owner,layout,alpha))return;
        // The front consumer exists even before/after the artwork root.
        if(!frontConsumer&&(!read(layout+0x428,root)||!read(root,vt)||vt!=base+0x10CEB48||!read(root+0x18,node)))return;
        bool matched=frontConsumer;
        for(unsigned i=0;!matched&&node&&i<128;++i) {
            std::array<char,32> name{};uintptr_t parent=0,renderer=0,draw=0;unsigned selector=~0u;
            if(!read(node+0x10,parent)||parent!=root||!read(node+0x40,name))return;
            if((!std::memcmp(name.data(),"White\0",6)||!std::memcmp(name.data(),"BG\0",3)||!std::memcmp(name.data(),"BG2\0",4))&&
               read(node,vt)&&vt==base+0x10CEBA8&&read(node+0x110,renderer)&&read(renderer,vt)&&vt==base+0x10CC0D8&&
               read(renderer+0x570,selector)&&selector==0&&read(renderer+0x308,draw)&&draw==material){matched=true;break;}
            if(!read(node+0x28,node))return;
        }
        if(!matched)return;
        Com<ID3D11RenderTargetView> output;if(FAILED(make(output,[&](auto p){context->OMGetRenderTargets(1,p,nullptr);return S_OK;})))return;
        Com<ID3D11Resource> resource;if(FAILED(make(resource,[&](auto p){output->GetResource(p);return S_OK;})))return;
        Com<ID3D11Texture2D> source;if(FAILED(make(source,[&](auto p){return resource->QueryInterface(IID_PPV_ARGS(p));})))return;
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        static bool reported=false;if(!reported){reported=true;log_info("bone-eater","Lobby backdrop source {}x{} format {}",d.Width,d.Height,unsigned(d.Format));}
        if(d.Width!=1920||d.Height!=1080||d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1||
           (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS))return;
        auto& capture=lobbyBackdrop;
        if(capture.context!=context){capture={};capture.context=context;}
        D3D11_TEXTURE2D_DESC old{};if(capture.texture)capture.texture->GetDesc(&old);
        if(!capture.texture||old.Format!=d.Format){
            capture.view.reset();capture.texture.reset();d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
            Com<ID3D11Device> device;make(device,[&](auto p){context->GetDevice(p);return S_OK;});
            if(!device||FAILED(make(capture.texture,[&](auto p){return device->CreateTexture2D(&d,nullptr,p);})))return;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
            if(FAILED(make(capture.view,[&](auto p){return device->CreateShaderResourceView(capture.texture.get(),&srv,p);})))return;
        }
        context->CopyResource(capture.texture.get(),source.get());capture.owner=owner;capture.fresh=true;
    }catch(...) {lobbyBackdrop.fresh=false;}
}
bool captureScoreRoundScene(ID3D11DeviceContext* context) noexcept {
 roundBackdrop.fresh=false;
 if(!context)return false;
 try {
  auto module=GetModuleHandleW(L"gamendd.dll");static uintptr_t base=0;
  if(!base){if(!module||std::strcmp(verifyNativeGameModule(module),"verified"))return false;base=reinterpret_cast<uintptr_t>(module);}
  uintptr_t owner=0;unsigned phase=0;
  if(!scoreRoundInfoOwner(base,[](auto p,auto o,auto& v){return read(p+o,v);},owner,phase))return false;
        Com<ID3D11RenderTargetView> output;if(FAILED(make(output,[&](auto p){context->OMGetRenderTargets(1,p,nullptr);return S_OK;})))return false;
        Com<ID3D11Resource> resource;if(FAILED(make(resource,[&](auto p){output->GetResource(p);return S_OK;})))return false;
        Com<ID3D11Texture2D> source;if(FAILED(make(source,[&](auto p){return resource->QueryInterface(IID_PPV_ARGS(p));})))return false;
        D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
        static bool reported=false;if(!reported){reported=true;log_info("bone-eater","Lobby backdrop source {}x{} format {}",d.Width,d.Height,unsigned(d.Format));}
        if(d.Width!=1920||d.Height!=1080||d.SampleDesc.Count!=1||d.ArraySize!=1||d.MipLevels!=1||
           (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS))return false;
        auto& capture=roundBackdrop;
        if(capture.context!=context){capture={};capture.context=context;}
        D3D11_TEXTURE2D_DESC old{};if(capture.texture)capture.texture->GetDesc(&old);
        if(!capture.texture||old.Format!=d.Format){
            capture.view.reset();capture.texture.reset();d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;
            Com<ID3D11Device> device;make(device,[&](auto p){context->GetDevice(p);return S_OK;});
            if(!device||FAILED(make(capture.texture,[&](auto p){return device->CreateTexture2D(&d,nullptr,p);})))return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
            if(FAILED(make(capture.view,[&](auto p){return device->CreateShaderResourceView(capture.texture.get(),&srv,p);})))return false;
        }
        roundTarget=reinterpret_cast<uintptr_t>(output.get());context->CopyResource(capture.texture.get(),source.get());capture.owner=owner;capture.fresh=true;

 return roundBackdrop.fresh;
 }catch(...){roundBackdrop.fresh=false;}return false;
}
bool extendScoreRoundSides(ID3D11DeviceContext* context,ID3D11ShaderResourceView* front,float left,float width) noexcept {
    if(!context||!front||!std::isfinite(left)||!std::isfinite(width)||left<0||width<500||width>800)return false;
    struct ClearCapture {~ClearCapture(){roundBackdrop.fresh=false;}} clearCapture;
    try {
        const auto module=GetModuleHandleW(L"gamendd.dll");
        static uintptr_t base=0;if(!base){if(!module||std::strcmp(verifyNativeGameModule(module),"verified"))return false;base=reinterpret_cast<uintptr_t>(module);}
        uintptr_t owner=0;unsigned state=0;
        if(!scoreRoundInfoOwner(base,[](auto p,auto o,auto& v){return read(p+o,v);},owner,state))return false;
        if(!roundBackdrop.fresh||roundBackdrop.context!=context||roundBackdrop.owner!=owner||!roundBackdrop.view)return false;
        Com<ID3D11RenderTargetView> output;ID3D11RenderTargetView* raw=nullptr;context->OMGetRenderTargets(1,&raw,nullptr);output.reset(raw);if(!output||reinterpret_cast<uintptr_t>(output.get())!=roundTarget)return false;
        Com<ID3D11Resource> resource;ID3D11Resource* rr=nullptr;output->GetResource(&rr);resource.reset(rr);
        Com<ID3D11Texture2D> target;if(FAILED(make(target,[&](auto p){return resource->QueryInterface(IID_PPV_ARGS(p));})))return false;
        D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
        if(td.Width!=1920||td.Height!=1080||td.SampleDesc.Count!=1||(td.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS&&td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM))return false;
        static Gpu gpu;static bool ready=false;
        Com<ID3D11Device> device;ID3D11Device* dev=nullptr;context->GetDevice(&dev);device.reset(dev);
        if(gpu.device.get()!=device.get()){gpu={};ready=false;gpu.device=std::move(device);context->AddRef();gpu.context.reset(context);}
        if(!ready){
            if(!initialize(gpu))return false;
            ready=true;
        }
        ID3DDeviceContextState* previous=nullptr;gpu.context1->SwapDeviceContextState(gpu.pass.get(),&previous);
        struct Restore {ID3D11DeviceContext1* c;Com<ID3DDeviceContextState> p;~Restore(){c->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView* empty[2]{};c->PSSetShaderResources(3,2,empty);c->SwapDeviceContextState(p.get(),nullptr);}} restore{gpu.context1.get(),Com<ID3DDeviceContextState>(previous)};
        std::array<float,12> data{1920,1080,left,width,1,5,0,0};context->UpdateSubresource(gpu.constants.get(),0,nullptr,data.data(),0,0);
        auto cb=gpu.constants.get();auto sampler=gpu.sampler.get();auto out=output.get();
        D3D11_VIEWPORT viewport{0,0,1920,1080,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(gpu.raster.get());
        context->OMSetRenderTargets(1,&out,nullptr);context->OMSetBlendState(nullptr,nullptr,0xffffffff);context->OMSetDepthStencilState(nullptr,0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(gpu.vs.get(),nullptr,0);context->PSSetShader(gpu.ps.get(),nullptr,0);
        context->PSSetConstantBuffers(0,1,&cb);context->PSSetSamplers(0,1,&sampler);context->PSSetShaderResources(3,1,&front);auto scene=roundBackdrop.view.get();context->PSSetShaderResources(4,1,&scene);context->Draw(3,0);return true;
    }catch(...){}
    return false;
}
void composeTitleD3D11Present(IDXGISwapChain* chain,void* main,unsigned flags) noexcept {
    if(!chain||!main||(flags&DXGI_PRESENT_TEST))return;
    try {
        static std::mutex mutex;std::unique_lock lock(mutex,std::try_to_lock);if(!lock)return;
        wchar_t option[4]{};if(GetEnvironmentVariableW(L"BONE_EATER_DESKTOP_TITLE",option,4)==1&&option[0]==L'0')return;
        DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(chain->GetDesc(&desc))||desc.OutputWindow!=main)return;
        static uintptr_t base=0;
        if(!base){auto module=GetModuleHandleW(L"gamendd.dll");if(!module||std::strcmp(verifyNativeGameModule(module),"verified"))return;base=reinterpret_cast<uintptr_t>(module);}
        struct EndLobbyFrame {~EndLobbyFrame(){lobbyBackdrop.fresh=false;}} endLobbyFrame;
        static TitlePresentationPolicy presentation;
        static NewsPresentationPolicy newsPresentation;
        static TitleArtRotation rotation;observeArtRotation(base,rotation);
        Route backgroundBefore,backgroundAfter;
        const bool background=menuBackgroundRoute(base,backgroundBefore);
        bool backgroundOnly=false;
        Route before,after;const bool phone=phoneRoute(base,before);
        const bool news=!phone&&newsRoute(base,before);
        if(!news)newsPresentation.reset();
        const bool guide=!phone&&!news&&guidanceRoute(base,before);
        const bool framing=!phone&&!news&&!guide&&framingRoute(base,before);
        if(phone||news||guide||framing)presentation.reset();
        else {
            if(!route(base,before)){
                presentation.reset();if(!background)return;
                before=backgroundBefore;backgroundOnly=true;
            }
            if(!backgroundOnly&&!presentation.admit(before.title,before.phase,before.alpha))return;
        }
        static Gpu gpu;static bool ready=false;static ULONGLONG retry=0;
        Com<ID3D11Device> device;if(FAILED(make(device,[&](auto out){return chain->GetDevice(IID_PPV_ARGS(out));})))return;
        if(gpu.device.get()!=device.get()){gpu={};gpu.device=std::move(device);ID3D11DeviceContext* c=nullptr;gpu.device->GetImmediateContext(&c);gpu.context.reset(c);ready=false;retry=0;}
        if(!ready){auto now=GetTickCount64();if(now<retry)return;retry=now+10000;ready=initialize(gpu);if(!ready){log_warning("bone-eater","Desktop title unavailable; native title retained");return;}}
        static ULONGLONG artRetry=0;
        if(news&&!gpu.news) {
            auto now=GetTickCount64();if(now<artRetry)return;artRetry=now+10000;
            if(!loadImage(gpu,L"news.png",gpu.news,gpu.newsWidth,gpu.newsHeight))return;
        }
        if(phone&&!gpu.phone) {
            static ULONGLONG phoneRetry=0;
            auto now=GetTickCount64();if(now<phoneRetry)return;phoneRetry=now+10000;
            if(!loadImage(gpu,L"phone.png",gpu.phone,gpu.phoneWidth,gpu.phoneHeight))return;
        }
        const bool titleArt=!phone&&!news&&!guide&&!framing&&!backgroundOnly;
        if(background||titleArt) {
            const unsigned wanted=background?rotation.menu():rotation.title();
            static unsigned failedPair=~0u;static ULONGLONG pairRetry=0;
            if(gpu.pairIndex!=wanted||!gpu.prompt) {
                auto now=GetTickCount64();if(failedPair==wanted&&now<pairRetry)return;
                if((gpu.pairIndex!=wanted&&!loadPair(gpu,wanted))||(!gpu.prompt&&!loadPrompt(gpu))) {
                    failedPair=wanted;pairRetry=now+10000;
                    log_warning("bone-eater","Intro/menu pair {} unavailable; native presentation retained",wanted);return;
                }
                failedPair=~0u;
                log_info("bone-eater","Intro/menu pair {} loaded",wanted);
            }
        }
        Com<ID3D11Texture2D> buffer;if(FAILED(make(buffer,[&](auto out){return chain->GetBuffer(0,IID_PPV_ARGS(out));})))return;
        D3D11_TEXTURE2D_DESC d{};buffer->GetDesc(&d);if(!d.Width||!d.Height||d.SampleDesc.Count!=1)return;
        if(framing||background||guide){
            if(d.Width!=1920||d.Height!=1080||d.ArraySize!=1||d.MipLevels!=1)return;
            D3D11_TEXTURE2D_DESC old{};if(gpu.snapshot)gpu.snapshot->GetDesc(&old);
            if(!gpu.snapshot||old.Format!=d.Format){gpu.snapshotView.reset();gpu.snapshot.reset();auto copy=d;
                copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;copy.Usage=D3D11_USAGE_DEFAULT;copy.CPUAccessFlags=copy.MiscFlags=0;
                if(FAILED(make(gpu.snapshot,[&](auto out){return gpu.device->CreateTexture2D(&copy,nullptr,out);}))||
                   FAILED(make(gpu.snapshotView,[&](auto out){return gpu.device->CreateShaderResourceView(gpu.snapshot.get(),nullptr,out);})))return;
            }
        }
        Com<ID3D11RenderTargetView> view;
        if(FAILED(make(view,[&](auto out){return gpu.device->CreateRenderTargetView(buffer.get(),nullptr,out);}))||
            !(phone?phoneRoute(base,after):backgroundOnly?menuBackgroundRoute(base,after):news?newsRoute(base,after):guide?guidanceRoute(base,after):framing?framingRoute(base,after):route(base,after)))return;
        // Native elapsed can advance between reads. Revalidate ownership but
        // use the latest alpha, avoiding native portrait flashes on timer ticks.
        if(phone){before.alpha=after.alpha;}
        if(before!=after)return;
        if(background&&(!menuBackgroundRoute(base,backgroundAfter)||backgroundBefore!=backgroundAfter))return;
        if(before.phase>=500&&before.phase<600&&(!lobbyBackdrop.fresh||lobbyBackdrop.owner!=before.title||lobbyBackdrop.context!=gpu.context.get()||!lobbyBackdrop.view))return;
        ID3DDeviceContextState* previous=nullptr;gpu.context1->SwapDeviceContextState(gpu.pass.get(),&previous);
        struct Restore { ID3D11DeviceContext1* c;Com<ID3DDeviceContextState> p;~Restore(){c->OMSetRenderTargets(0,nullptr,nullptr);ID3D11ShaderResourceView* empty[5]{};c->PSSetShaderResources(0,5,empty);c->SwapDeviceContextState(p.get(),nullptr);} } restore{gpu.context1.get(),Com<ID3DDeviceContextState>(previous)};
        auto c=gpu.context.get();std::array<float,8> data{float(d.Width),float(d.Height),float(gpu.width),float(gpu.height),before.alpha,guide?1.f:0.f,0,0};
        if(news)data={float(d.Width),float(d.Height),float(gpu.newsWidth),float(gpu.newsHeight),
            newsPresentation.alpha(before.layout,GetTickCount64(),before.alpha),-1,0,0};
        if(phone)data={float(d.Width),float(d.Height),float(gpu.phoneWidth),float(gpu.phoneHeight),before.alpha,-2,0,0};
        if(framing){
            // Native artwork occupies the 607-pixel portrait strip. Uniform
            // enlargement retains proportions and native fades already in RGB.
            data=before.phase>=200?std::array<float,8>{1920,1080,657,180,1,4,
                (before.phase==202||before.phase==209||before.phase==212||before.phase==217||before.phase==303||before.phase==404)?1.f:0.f,0}:
                before.phase==2?std::array<float,8>{1920,1080,657,140,1,2,607,607.f*9/16}:
                before.phase>=100?std::array<float,8>{1920,1080,657,135,1,3,607,745}:
                std::array<float,8>{1920,1080,657,0,1,3,607,568};
            if(before.phase>=500&&before.phase<600)data={1920,1080,0,0,before.alpha,6,0,0};
        }
        if(backgroundOnly)data={1920,1080,0,0,1,7,0,0};
        if(framing||background||guide)c->CopyResource(gpu.snapshot.get(),buffer.get());
        std::array<float,12> constants{};std::copy(data.begin(),data.end(),constants.begin());
        if(background){constants[8]=float(gpu.menuWidth);constants[9]=float(gpu.menuHeight);constants[10]=0.30f;constants[11]=1;}
        c->UpdateSubresource(gpu.constants.get(),0,nullptr,constants.data(),0,0);
        auto cb=gpu.constants.get();auto sampler=gpu.sampler.get();ID3D11ShaderResourceView* sources[]{phone?gpu.phone.get():news?gpu.news.get():background?gpu.menuArt.get():gpu.art.get(),gpu.prompt.get(),gpu.guidance.get(),gpu.snapshotView.get()};auto output=view.get();
        D3D11_VIEWPORT viewport{0,0,float(d.Width),float(d.Height),0,1};c->RSSetViewports(1,&viewport);c->RSSetState(gpu.raster.get());
        c->OMSetRenderTargets(1,&output,nullptr);c->OMSetBlendState(nullptr,nullptr,0xFFFFFFFF);c->OMSetDepthStencilState(nullptr,0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(gpu.vs.get(),nullptr,0);c->PSSetShader(gpu.ps.get(),nullptr,0);
        c->PSSetConstantBuffers(0,1,&cb);c->PSSetSamplers(0,1,&sampler);c->PSSetShaderResources(0,4,sources);auto worldView=data[5]==6?lobbyBackdrop.view.get():nullptr;c->PSSetShaderResources(4,1,&worldView);c->Draw(3,0);
        if(titleArt&&before.alpha>0)rotation.presented();
    }catch(...){/* Preserve native presentation on any unsupported path. */}
}
}
