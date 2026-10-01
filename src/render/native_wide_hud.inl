// Included after the collector adapter, inside the front observer namespace.
namespace wide_hud {
bool requested() noexcept {
    return wideHudRequested();
}
using Indexed=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using Plain=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using ClearTarget=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*);
Indexed indexed=nullptr;Plain plain=nullptr;ClearTarget clearTarget=nullptr;
std::atomic<bool> installed{false};
ID3D11DeviceContext* hookedContext=nullptr; // Identity only; never dereferenced outside native callback.
thread_local std::uintptr_t primitive=0;
thread_local bool internal=false;
thread_local bool omitPlayerName=false;
thread_local bool captureLobbyThisFrame=false;
std::atomic<unsigned> activeQueries{0};
std::atomic<unsigned> foreignWrites{0};
struct Frame {
    WideHudCompositor gpu;
    Source source;
    PresentationChain presentation;
    std::array<OwnedHudDrawPlan,3> plans{};
    OwnedHudRuntime::RearPair rear;
    OwnedHudRuntime::Packet packet;
    struct BufferProof {ComPtr<ID3D11Resource> resource;std::uintptr_t material=0;std::vector<unsigned char> bytes;};
    std::array<BufferProof,4> buffers;
    std::array<OwnedHudRuntime::RenderBatch,4> receipts;
    std::array<OwnedHudRuntime::Packet,4> certificates;
    std::array<OwnedHudRuntime::Packet,4> mappedPackets;
    struct PendingMap {ComPtr<ID3D11Resource> resource;void* data=nullptr;std::uintptr_t material=0;unsigned bytes=0;} mapped;

    unsigned rearDraws=0,frontDraws=0,sceneDraws=0;
    unsigned stage=0;
    unsigned attempts=0,published=0,packetMiss=0;
    std::uintptr_t missMaterial=0;unsigned missCount=0;
    unsigned foreignSerial=0;
    std::uintptr_t lastDepthClear=0;
    std::uintptr_t lastDepthResource=0;
    UINT depthFlags=0;FLOAT depthValue=1;UINT8 stencilValue=0;
    ULONGLONG nextLog=0;
};
thread_local Frame* frame=nullptr;
struct Dispatch { ID3D11DeviceContext* context; bool indexed; INT base; };
void dispatch(void* user,UINT count,UINT first,INT) {
    const auto& d=*static_cast<Dispatch*>(user);
    if(omitPlayerName)return;
    if(d.indexed)indexed(d.context,count,first,d.base);else plain(d.context,count,first);
}
bool nativeDispatch(std::uintptr_t caller,bool isIndexed) noexcept {
    // primitive is set only around the signature-checked native DrawPrimitive
    // hook. D3D11 sometimes forwards that synchronous call through its own
    // executable code (observed at d3d11+154BE9), so its return address need not
    // be the game's direct COM call site. Never admit arbitrary external code.
    if(!primitive)return false;
    if(caller==base+(isIndexed?0x3F1209:0x3F1219))return true;
    const auto d3d11=GetModuleHandleW(L"d3d11.dll");
    HMODULE owner=nullptr;MEMORY_BASIC_INFORMATION region{};
    if(!d3d11||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(caller),&owner)||owner!=d3d11||
       VirtualQuery(reinterpret_cast<LPCVOID>(caller),&region,sizeof(region))!=sizeof(region)||
       region.AllocationBase!=d3d11||region.Type!=MEM_IMAGE||region.State!=MEM_COMMIT||
       (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto protection=region.Protect&0xff;
    return protection==PAGE_EXECUTE||protection==PAGE_EXECUTE_READ||
        protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
}
bool quadBuffers(ID3D11DeviceContext* c,std::uintptr_t material,std::uintptr_t expected,UINT total) noexcept {
    std::uintptr_t wrapper=0,allocation=0,vb=0,ibWrapper=0,ibAllocation=0,ib=0,explicitIndex=1;
    std::uint16_t type=0;
    if(!read(material,0x10,wrapper)||wrapper!=expected||!read(wrapper,8,allocation)||!read(allocation,0x48,vb)||
       !read(material,8,explicitIndex)||explicitIndex||!read(material,0x18,type)||type!=1||
       !read(base,0x13CC1E0,ibWrapper)||!read(ibWrapper,0,ibAllocation)||!read(ibAllocation,0x48,ib))return false;
    ComPtr<ID3D11Buffer> vertex,index;UINT stride=0,offset=0,indexOffset=0;DXGI_FORMAT format;
    c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);c->IAGetIndexBuffer(&index,&format,&indexOffset);
    D3D11_PRIMITIVE_TOPOLOGY topology;c->IAGetPrimitiveTopology(&topology);
    if(reinterpret_cast<std::uintptr_t>(vertex.Get())!=vb||reinterpret_cast<std::uintptr_t>(index.Get())!=ib||
       stride!=28||offset||indexOffset||format!=DXGI_FORMAT_R16_UINT||topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return false;
    D3D11_BUFFER_DESC v{},i{};vertex->GetDesc(&v);index->GetDesc(&i);
    return total&&total%6==0&&total/6<=OwnedHudQuadSelection::maxQuads&&v.ByteWidth>=total/6*112&&i.ByteWidth>=total*2;
}
void report(bool published) noexcept {
    if(!diagnostics::optionalOutputEnabled())return;
    auto& f=*frame;++f.attempts;f.published+=published;const auto now=GetTickCount64();if(now<f.nextLog)return;f.nextLog=now+1000;
    log_info("bone-eater","Wide HUD interval attempts={} published={} packet_misses={} miss_material={:x} miss_count={}",f.attempts,f.published,f.packetMiss,f.missMaterial,f.missCount);f.attempts=f.published=f.packetMiss=0;
    log_info("bone-eater","Wide HUD packet latest={} selected={} uploaded={},{},{}",owned_hud_runtime_observer::runtime->latestPacketFrame(),f.packet?f.packet->frame:0,f.buffers[0].bytes.size(),f.buffers[1].bytes.size(),f.buffers[2].bytes.size());
    const auto diagnostic=owned_hud_runtime_observer::runtime->diagnostics();
    log_info("bone-eater","Wide HUD collector active={} collect={} node={} admission={} reasons={},{},{}",diagnostic.active,diagnostic.collectStage,diagnostic.node,diagnostic.admissionStage,diagnostic.selection.reasons[0],diagnostic.selection.reasons[1],diagnostic.selection.reasons[2]);
    auto rd=owned_hud_runtime_observer::runtime->rearDiagnostics();
    log_info("bone-eater","Wide HUD rear ledger raw_frame={} current={} seen={} active={} reason={} material={:x}",rd.raw.frame,rd.current,rd.raw.seen,rd.active,rd.reason,rd.raw.material);
    log_info("bone-eater","Wide HUD frame={} publish={} stage={} gpu_failure={} front_ready={} scene_ready={} front_draws={} scene_draws={} rear_draws={} plans={},{} rear_seen={} rear_material={:x}",
        f.rear.frame,published,f.stage,f.gpu.failure(),f.gpu.frontReady(),f.gpu.sceneReady(),f.frontDraws,f.sceneDraws,f.rearDraws,
        f.plans[0].replacement,f.plans[1].replacement,f.rear.seen,f.rear.material);
}
void bufferWrite(ID3D11Resource* resource) noexcept {
    if(!frame)return;
    for(auto& b:frame->buffers)if(b.resource.Get()==resource){b.resource.Reset();b.bytes.clear();b.material=0;}
}
OwnedHudRuntime::Packet drawPacket(ID3D11DeviceContext* c,std::uintptr_t material,UINT count) noexcept {
    if(!frame||!material||!count||count%6)return {};
    ComPtr<ID3D11Buffer> vertex;UINT stride=0,offset=0;c->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);
    if(!vertex||stride!=28||offset)return {};
    for(const auto& b:frame->buffers)if(b.resource.Get()==vertex.Get()&&b.material==material&&b.bytes.size()==count/6*112){
        auto& f=*frame;
        auto receipt=std::find_if(f.receipts.begin(),f.receipts.end(),[&](const auto& r){return r.material==material;});
        if(receipt!=f.receipts.end()&&receipt->vertices!=b.bytes)return {};
        if(receipt==f.receipts.end())receipt=std::find_if(f.receipts.begin(),f.receipts.end(),[](const auto& r){return !r.material;});
        if(receipt==f.receipts.end())return {};
        auto match=owned_hud_runtime_observer::runtime->resolveUpload(material,b.bytes);
        auto result=match.packet;
        if(!result){++f.packetMiss;f.missMaterial=material;f.missCount=count;
            if(diagnostics::optionalOutputEnabled()){static unsigned rows=0;if(rows++<64){
                const auto reason=owned_hud_runtime_observer::runtime->diagnoseUpload(material,b.bytes);
                log_info("bone-eater","Wide HUD upload miss candidates={} differences={} offset={} slot={} destination={} frame={} count={} matching_quads={} unmatched_destinations={} owned_matches={}",reason.candidates,reason.differences,reason.offset,reason.slot,reason.destination,reason.frame,count,reason.matchingQuads,reason.unmatchedDestinations,reason.ownedMatches);
            }}return {};}
        unsigned slot=0;while(slot<4&&result->batches[slot].material!=material)++slot;
        if(slot==4)return {};
        if(f.mappedPackets[slot]&&!OwnedHudRuntime::mappingEquivalent(f.mappedPackets[slot],result,material))return {};
        if(!receipt->material)*receipt={material,0,b.bytes};
        f.certificates[slot]=match.certificate;f.mappedPackets[slot]=result;
        f.packet=result;
        return result;
    }++frame->packetMiss;frame->missMaterial=material;frame->missCount=count;return {};
}
bool draw(ID3D11DeviceContext* c,UINT count,UINT first,INT baseVertex,bool isIndexed,std::uintptr_t caller) noexcept {
    if(internal||!installed.load())return false;
    if(diagnostics::optionalOutputEnabled()&&primitive&&primitive==owned_hud_runtime_observer::observedRearMaterial.load()&&c!=hookedContext){
        static unsigned rows=0;if(rows++<8)log_info("bone-eater","Wide HUD other context rear context={:x} expected={:x} type={} count={} thread={}",reinterpret_cast<std::uintptr_t>(c),reinterpret_cast<std::uintptr_t>(hookedContext),unsigned(c->GetType()),count,GetCurrentThreadId());
    }
    if(c!=hookedContext)return false;
    if(diagnostics::optionalOutputEnabled()&&primitive&&primitive==owned_hud_runtime_observer::observedRearMaterial.load()){
        static ULONGLONG due=0;auto now=GetTickCount64();if(now>=due){due=now+1000;
        const auto diagnostic=owned_hud_runtime_observer::runtime->diagnostics();
    log_info("bone-eater","Wide HUD collector active={} collect={} node={} admission={} reasons={},{},{}",diagnostic.active,diagnostic.collectStage,diagnostic.node,diagnostic.admissionStage,diagnostic.selection.reasons[0],diagnostic.selection.reasons[1],diagnostic.selection.reasons[2]);
    auto rd=owned_hud_runtime_observer::runtime->rearDiagnostics();
        ComPtr<ID3D11RenderTargetView> out;c->OMGetRenderTargets(1,&out,nullptr);
        log_info("bone-eater","Wide HUD observed rear primitive={:x} count={} caller={:x} target={:x} ledger_frame={} current={} seen={} active={} reason={} queries={} frame_tls={}",primitive,count,caller-base,reinterpret_cast<std::uintptr_t>(out.Get()),rd.raw.frame,rd.current,rd.raw.seen,rd.active,rd.reason,activeQueries.load(),frame!=nullptr);
    }}
    if(!frame){foreignWrites.fetch_add(1);return false;}
    const auto priorDepthClear=frame->lastDepthClear;frame->lastDepthClear=0;
    if(activeQueries.load()||frame->foreignSerial!=foreignWrites.load()){frame->gpu.invalidate();frame->buffers={};frame->foreignSerial=foreignWrites.load();return false;}
    if(!frame->gpu.frontReady()&&!frame->gpu.sceneReady()&&primitive!=frame->source.material&&
       !owned_hud_runtime_observer::runtime->renderMaterial(primitive))return false;
    internal=true;
    auto& f=*frame;bool replaced=false;
    try {
        auto* runtime=owned_hud_runtime_observer::runtime;
        ComPtr<ID3D11RenderTargetView> output;c->OMGetRenderTargets(1,&output,nullptr);
        std::array<ID3D11RenderTargetView*,8> outputs{};c->OMGetRenderTargets(8,outputs.data(),nullptr);
        for(unsigned i=0;i<outputs.size();++i)if(outputs[i]){
            ComPtr<ID3D11Resource> resource;outputs[i]->GetResource(&resource);const auto id=reinterpret_cast<std::uintptr_t>(resource.Get());
            if(id==f.source.resource&&(i||reinterpret_cast<std::uintptr_t>(outputs[i])!=f.source.rtv))f.gpu.invalidateFront();
            if(id==f.presentation.resource&&(i||reinterpret_cast<std::uintptr_t>(outputs[i])!=f.presentation.rtv))f.gpu.invalidateScene();
            outputs[i]->Release();
        }
        const auto target=reinterpret_cast<std::uintptr_t>(output.Get());
        if(diagnostics::optionalOutputEnabled()&&runtime->ownsRearMaterial(primitive)){static ULONGLONG due=0;const auto now=GetTickCount64();if(now>=due){due=now+1000;
            ComPtr<ID3D11Resource> rr;if(output)output->GetResource(&rr);ComPtr<ID3D11Texture2D> tt;D3D11_TEXTURE2D_DESC td{};if(rr&&SUCCEEDED(rr.As(&tt)))tt->GetDesc(&td);
            log_info("bone-eater","Wide HUD rear draw frame={} primitive={:x} target={:x} presentation={:x} front={:x} size={}x{} count={} caller={:x}",runtime->frame(),primitive,target,f.presentation.rtv,f.source.rtv,td.Width,td.Height,count,caller-base);
        }}

        ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);
        if(depth){ComPtr<ID3D11Resource> resource;depth->GetResource(&resource);
            f.gpu.depthOutput(reinterpret_cast<std::uintptr_t>(resource.Get()),target==f.source.rtv,target==f.presentation.rtv);}
        Dispatch d{c,isIndexed,baseVertex};
        const bool nativeCall=nativeDispatch(caller,isIndexed);
        if(target==f.source.rtv && f.gpu.frontReady()) {
            f.gpu.primeFrontDepth(c,depth.Get(),reinterpret_cast<std::uintptr_t>(depth.Get())==priorDepthClear?f.depthFlags:0,f.depthValue,f.stencilValue);
            ++f.frontDraws;f.stage=10;
            std::array<WideHudRange,ownedHudNodeCount*2+1> ranges{};std::size_t n=1;
            ranges[0]={first,count,wideHudOriginal};
            if(runtime->renderMaterial(primitive)) {
                const auto packet=drawPacket(c,primitive,count);
                OwnedHudDrawPlan plan;unsigned planSlot=3;
                if(packet)for(unsigned i=0;i<3;++i)if(packet->plans[i].batch.material==primitive){plan=packet->plans[i];planSlot=i;}
                f.stage=11;
                if(!nativeCall||!isIndexed||first||baseVertex||!plan.replacement||count!=plan.totalQuads*6||
                   !quadBuffers(c,primitive,plan.batch.vertexBuffer,count)){
                    if(diagnostics::optionalOutputEnabled()){static ULONGLONG next=0;const auto now=GetTickCount64();if(now>=next){next=now+1000;
                        log_info("bone-eater","Wide HUD front guard native={} indexed={} first={} base={} plan={} total={} count={} buffers={} caller={:x} module={:x}",nativeCall,isIndexed,first,baseVertex,plan.replacement,plan.totalQuads,count,quadBuffers(c,primitive,plan.batch.vertexBuffer,count),caller,base);
                    }}f.gpu.invalidateFront();}
                else {
                    n=plan.count;
                    for(std::size_t i=0;i<n;++i){const auto& r=plan.ranges[i];const auto g=unsigned(r.destination);
                        ranges[i]={r.firstIndex,r.indexCount,g<3?wideHudGroups[g]:wideHudOriginal,g<3};}
                    if(planSlot==3)f.gpu.invalidateFront();else f.plans[planSlot]=plan;
                }
            }
            if(f.gpu.frontReady()&&!f.gpu.mirrorFront(c,output.Get(),{ranges.data(),n},dispatch,&d)){
                static unsigned rejected=0;if(diagnostics::optionalOutputEnabled()&&rejected++<12)log_info("bone-eater","Wide HUD front mirror rejected material={:x} failure={}",primitive,f.gpu.failure());
            }
        } else if(target==f.presentation.rtv) {
            const auto packet=runtime->renderMaterial(primitive)?drawPacket(c,primitive,count):OwnedHudRuntime::Packet{};
            const auto rear=packet?packet->rear:OwnedHudRuntime::RearPair{};
            if(nativeCall&&primitive==f.source.material) {
                f.stage=30;
                Source repeated;PresentationSample p;ComPtr<ID3D11Resource> destination;output->GetResource(&destination);
                ComPtr<ID3D11ShaderResourceView> input;c->PSGetShaderResources(0,1,&input);
                ComPtr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;c->OMGetBlendState(&blend,factors,&mask);
                D3D11_BLEND_DESC desc{};if(blend)blend->GetDesc(&desc);const auto& b=desc.RenderTarget[0];
                const auto geometry=readNativeHudGeometry();
                const bool multiply=blend&&b.BlendEnable&&b.SrcBlend==D3D11_BLEND_ZERO&&b.DestBlend==D3D11_BLEND_SRC_COLOR&&b.BlendOp==D3D11_BLEND_OP_ADD;
                const bool complete=f.plans[0].replacement&&f.plans[1].replacement&&f.certificates[0]&&
                    (!f.certificates[0]->plans[2].replacement||f.plans[2].replacement);
                if(nativeBattleBackgroundHealthy()&&f.rearDraws==1 && complete&&
                   count==6&&first==0&&baseVertex==0&&isIndexed&&geometry.valid&&geometry.sprite==f.source.sprite&&
                   geometry.left==wideHudLeft&&geometry.top==0&&geometry.width==768*wideHudFit&&geometry.height==1080&&
                   f.plans[0].batch.material!=f.plans[1].batch.material&&multiply&&
                   reinterpret_cast<std::uintptr_t>(input.Get())==f.source.srv&&resolve(repeated)&&repeated==f.source&&
                   !std::strcmp(samplePresentation(f.source,destination.Get(),p),"resolved")&&p.candidateIsPresentation==1&&p.backendIsMainBackbuffer==1) {
                    const float scale=1920.f/geometry.width;
                    const WideHudRange consumer{0,count,{-geometry.left*scale,0,scale,1}};
                    std::array<OwnedHudRuntime::Packet,4> certificates{};unsigned certificateCount=0;
                    for(const auto& certificate:f.certificates)if(certificate)certificates[certificateCount++]=certificate;
                    replaced=runtime->publishPackets({certificates.data(),certificateCount},[&]() noexcept{return f.gpu.publishNative(c,output.Get(),consumer,dispatch,&d);});
                }
                if(!replaced&&diagnostics::optionalOutputEnabled()){static ULONGLONG next=0;const auto now=GetTickCount64();if(now>=next){next=now+1000;
                    log_info("bone-eater","Wide HUD fallback complete={} packet={} latest={} front={} scene={} failure={} draws={},{},{} plans={},{},{}",complete,f.packet?f.packet->frame:0,runtime->latestPacketFrame(),f.gpu.frontReady(),f.gpu.sceneReady(),f.gpu.failure(),f.frontDraws,f.sceneDraws,f.rearDraws,f.plans[0].replacement,f.plans[1].replacement,f.plans[2].replacement);
                }}
                report(replaced);f.gpu.invalidate();f.rear={};f.packet.reset();f.receipts={};f.certificates={};f.mappedPackets={};
                // The presentation texture can be populated by a copy rather
                // than a clear. The consumer closes this capture interval in
                // either case; duplicate detection must not span later frames.
                f.rearDraws=f.sceneDraws=0;
            } else {
                std::array<WideHudRange,5> ranges{};std::size_t n=1;ranges[0]={first,count,{}};
                if(rear.frame&&primitive==rear.material) {
                    f.stage=20;
                    ++f.rearDraws;
                    if(f.rearDraws!=1||!nativeCall||!isIndexed||first||baseVertex||count!=rear.total*6||!quadBuffers(c,primitive,rear.vertex,count)||f.gpu.sceneReady()) {
                        if(diagnostics::optionalOutputEnabled())log_info("bone-eater","Wide HUD rear guard native={} caller={:x} module={:x} buffers={} ready={} draws={}",nativeCall,caller,base,quadBuffers(c,primitive,rear.vertex,count),f.gpu.sceneReady(),f.rearDraws);
                        f.gpu.invalidateScene();
                    } else if(f.gpu.forkScene(c,output.Get())) {
                        f.rear=rear;f.sceneDraws=0;n=0;UINT next=0;
                        if(diagnostics::optionalOutputEnabled()){
                            static bool recorded=false;
                            if(!recorded){recorded=true;for(const auto& batch:packet->batches)if(batch.material==rear.material)
                                for(unsigned leaf=0;leaf<2;++leaf)for(unsigned v=0;v<4;++v){
                                    const auto offset=rear.first[leaf]*112u+v*28u;
                                    if(offset+28<=batch.vertices.size()){
                                        std::array<float,7> values{};std::memcpy(values.data(),batch.vertices.data()+offset,28);
                                        log_info("bone-eater","HUD seam vertex leaf={} vertex={} x={} y={} z={} u={} v={}",leaf,v,values[0],values[1],values[2],values[5],values[6]);
                                    }
                                }
                            }
                        }
                        auto order=rear.first;if(order[1]<order[0])std::swap(order[0],order[1]);
                        // Undo the accepted narrow alignment, then stretch only
                        // the pure illumination pair. Foreground glyphs are uniform.
                        constexpr float sx=2.4f/(768.f*wideHudFit/769.f);
                        constexpr WideHudMapping mapping{-.96f-sx*wideHudLeft,0,sx,1366.f/1080.f};
                        auto rightMapping=mapping;
                        // The native commit leaves a small gap between these
                        // two certified illumination quads. At landscape scale
                        // it can straddle a pixel center and expose a dark line.
                        // Join their actual uploaded edges without overlapping
                        // translucent pixels or changing any foreground glyph.
                        bool joined=false;
                        for(const auto& batch:packet->batches)if(batch.material==rear.material){
                            float translation=0;
                            if(hudSeamTranslation(batch.vertices,rear.first[0],rear.first[1],sx,translation)){
                                rightMapping.x+=translation;joined=true;
                            }
                        }
                        if(!joined)f.gpu.invalidateScene();
                        for(auto ordinal:order){const auto start=ordinal*6;
                            if(start>next)ranges[n++]={next,start-next,{}};
                            ranges[n++]={start,6,ordinal==rear.first[1]?rightMapping:mapping};next=start+6;}
                        if(next<count)ranges[n++]={next,count-next,{}};
                    }
                }
                if(f.gpu.sceneReady()){++f.sceneDraws;f.gpu.mirrorScene(c,output.Get(),{ranges.data(),n},dispatch,&d);}
            }
        }
    } catch (...) { f.gpu.invalidate(); }
    internal=false;return replaced;
}

// VA-02B: discard only the identified pale world backing, before post-processing.
// This never changes native data, HUD/movie targets, or the auxiliary scene.
bool suppressBriefingPaleQuad(ID3D11DeviceContext* c,UINT count,UINT first,INT baseVertex,uintptr_t caller) noexcept {
    if(internal||c!=hookedContext||count!=6||first||baseVertex||!primitive||!nativeDispatch(caller,true))return false;
    uintptr_t vt=0,wrap=0,cpu=0,allocation=0,nativeBuffer=0,state=0,manager=0;
    unsigned scene=0,pending=0;std::uint16_t kind=1;
    if(!read(primitive,0,vt)||vt!=base+0x706178||!read(primitive,0x18,kind)||kind!=0||
       !read(base,0x13DD000,manager)||!read(manager,0,vt)||vt!=base+0x10C6B38||
       !read(manager,0x38,scene)||scene!=5||!read(manager,0x3C,pending)||pending!=0xffffffffu||
       !read(primitive,0x10,wrap)||!read(wrap,0x20,cpu)||!read(wrap,8,allocation)||
       !read(allocation,0x48,nativeBuffer)||!read(primitive,0x40,state))return false;
    std::array<unsigned char,112> vertices{};std::array<unsigned char,176> material{};
    if(!read(cpu,0,vertices)||!read(state,0,material)||!matchesBriefingPaleAsset(vertices,material))return false;
    ComPtr<ID3D11Buffer> v;UINT stride=0,offset=0;c->IAGetVertexBuffers(0,1,&v,&stride,&offset);
    if(!v||stride!=28||offset||reinterpret_cast<uintptr_t>(v.Get())!=nativeBuffer)return false;
    D3D11_BUFFER_DESC vd{};v->GetDesc(&vd);if(vd.ByteWidth!=112)return false;
    ComPtr<ID3D11RenderTargetView> view;c->OMGetRenderTargets(1,&view,nullptr);if(!view)return false;
    ComPtr<ID3D11Resource> resource;view->GetResource(&resource);ComPtr<ID3D11Texture2D> target;
    if(FAILED(resource.As(&target)))return false;
    D3D11_TEXTURE2D_DESC td{};target->GetDesc(&td);
    if(td.Width!=1920||td.Height!=1080||td.SampleDesc.Count!=1||td.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;
    ComPtr<ID3D11ShaderResourceView> input;c->PSGetShaderResources(0,1,&input);if(!input)return false;
    input->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;if(FAILED(resource.As(&texture)))return false;
    texture->GetDesc(&td);if(td.Width!=512||td.Height!=512||td.Format!=DXGI_FORMAT_BC1_TYPELESS)return false;
    static bool reported=false;if(!reported){reported=true;log_info("bone-eater","Briefing pale backing removed: exact packed asset and main scene target matched");}
    return true;
}
// Preserve CPU collection and draw receipts while omitting only the dedicated
// player-name font batch from native and mirrored front targets.
bool playerNameDraw(ID3D11DeviceContext* c,UINT count,UINT first,INT vertex,uintptr_t caller) noexcept {
    if(internal||c!=hookedContext||count!=6||first||vertex||!nativeDispatch(caller,true))return false;
    uintptr_t manager=0,owner=0,gui=0;GuiFontIdentity identity;
    if(!read(base,0x13DCF78,manager)||!read(manager,0xD0,owner)||!read(owner,0x1B8,gui)||
       !readPlayerNameIdentity(base,owner,gui,1,[](auto p,auto o,auto& v)noexcept{return read(p,o,v);},identity)||
       primitive!=identity.material)return false;
    Source source;if(!resolve(source)||source.context!=reinterpret_cast<uintptr_t>(c))return false;
    ComPtr<ID3D11RenderTargetView> target;c->OMGetRenderTargets(1,&target,nullptr);
    if(reinterpret_cast<uintptr_t>(target.Get())!=source.rtv)return false;
    static bool reported=false;if(!reported){reported=true;log_info("bone-eater","Gameplay player name hidden: dedicated font batch verified");}
    return true;
}
void captureLobbyDraw(ID3D11DeviceContext* c,std::uintptr_t caller,bool isIndexed) {
    if(captureLobbyThisFrame&&!internal&&c==hookedContext&&nativeDispatch(caller,isIndexed)) {
        Source source;PresentationSample presentation;
        ComPtr<ID3D11RenderTargetView> output;c->OMGetRenderTargets(1,&output,nullptr);
        ComPtr<ID3D11Resource> resource;if(output)output->GetResource(&resource);
        if(resource&&resolve(source)&&!std::strcmp(samplePresentation(source,resource.Get(),presentation),"resolved")&&
           presentation.candidateIsPresentation==1&&presentation.backendIsMainBackbuffer==1) {
            ComPtr<ID3D11ShaderResourceView> input;c->PSGetShaderResources(0,1,&input);
            const bool frontConsumer=primitive==source.material&&reinterpret_cast<uintptr_t>(input.Get())==source.srv;
            internal=true;captureLobbyBackdrop(c,primitive,frontConsumer);internal=false;
        }
    }
}
void STDMETHODCALLTYPE indexedHook(ID3D11DeviceContext* c,UINT count,UINT first,INT baseVertex) {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    captureLobbyDraw(c,caller,true);
    if(suppressBriefingPaleQuad(c,count,first,baseVertex,caller))return;
    if(!internal&&c==hookedContext&&count==6&&!first&&!baseVertex&&nativeDispatch(caller,true)){
        RankingNoticeIdentity notice;
        if(readRankingNotice(base,[](auto p,auto o,auto& v)noexcept{return read(p,o,v);},notice)&&primitive==notice.material)return;
    }
    bool roundCaptured=false;ComPtr<ID3D11ShaderResourceView> roundInput;float roundLeft=0,roundWidth=0;
    if(!internal&&c==hookedContext&&count==6&&!first&&!baseVertex&&nativeDispatch(caller,true)) {
        Source source;
        if(resolve(source)&&primitive==source.material) {
            ComPtr<ID3D11ShaderResourceView> input;c->PSGetShaderResources(0,1,&input);
            ComPtr<ID3D11BlendState> blend;FLOAT factors[4];UINT mask;c->OMGetBlendState(&blend,factors,&mask);
            D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
            const auto& b=bd.RenderTarget[0];const auto geometry=readNativeHudGeometry();
            if(geometry.valid&&geometry.sprite==source.sprite&&input&&
               reinterpret_cast<uintptr_t>(input.Get())==source.srv&&blend&&b.BlendEnable&&
               b.SrcBlend==D3D11_BLEND_ZERO&&b.DestBlend==D3D11_BLEND_SRC_COLOR&&b.BlendOp==D3D11_BLEND_OP_ADD) {
                internal=true;roundCaptured=captureScoreRoundScene(c);internal=false;
                if(roundCaptured){roundInput=input;roundLeft=geometry.left;roundWidth=geometry.width;}
            }
        }
    }
    const bool prior=omitPlayerName;omitPlayerName=playerNameDraw(c,count,first,baseVertex,caller);
    if(!draw(c,count,first,baseVertex,true,caller)&&!omitPlayerName)indexed(c,count,first,baseVertex);
    omitPlayerName=prior;
    if(roundCaptured){internal=true;extendScoreRoundSides(c,roundInput.Get(),roundLeft,roundWidth);internal=false;}

}
void STDMETHODCALLTYPE plainHook(ID3D11DeviceContext* c,UINT count,UINT first) {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    captureLobbyDraw(c,caller,false);
    if(!draw(c,count,first,0,false,caller))plain(c,count,first);
}
void STDMETHODCALLTYPE clearTargetHook(ID3D11DeviceContext* c,ID3D11RenderTargetView* target,const FLOAT* color) {
    if(!internal&&installed.load()&&c==hookedContext&&!frame)foreignWrites.fetch_add(1);
    if(!internal&&installed.load()&&c==hookedContext&&frame){internal=true;auto& f=*frame;
        Source source;PresentationChain presentation;
        if(resolve(source)&&source.context==reinterpret_cast<std::uintptr_t>(c)&&presentationResolve(source,presentation)) {
            if(!(source==f.source)||!(presentation==f.presentation))f.gpu.invalidate();
            f.source=source;f.presentation=presentation;
            ComPtr<ID3D11Resource> resource;target->GetResource(&resource);const auto id=reinterpret_cast<std::uintptr_t>(resource.Get());
            if(id==source.resource)f.gpu.invalidateFront();
            if(id==presentation.resource)f.gpu.invalidateScene();
            if(reinterpret_cast<std::uintptr_t>(target)==source.rtv){
                captureLobbyThisFrame=lobbyBackdropRequested();
                DWORD expectedThread=0;ownerThread.compare_exchange_strong(expectedThread,GetCurrentThreadId());
                // The certified native route starts with the front clear.
                // Discard unfinished prior intervals even if a transition
                // skipped their final consumer; never strand old receipts.
                f.packet.reset();f.receipts={};f.certificates={};f.mappedPackets={};f.rear={};f.rearDraws=f.sceneDraws=0;f.gpu.invalidateScene();
                f.plans={};f.frontDraws=0;f.gpu.clearFront(c,target,color);
                ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);
                if(depth&&reinterpret_cast<std::uintptr_t>(depth.Get())==f.lastDepthClear)f.gpu.clearDepth(c,depth.Get(),f.depthFlags,f.depthValue,f.stencilValue);}
            if(reinterpret_cast<std::uintptr_t>(target)==presentation.rtv){f.gpu.invalidateScene();f.rear={};f.rearDraws=f.sceneDraws=0;}
        } else f.gpu.invalidate();
        internal=false;
    }
    clearTarget(c,target,color);
}
void unsupported(ID3D11DeviceContext* c) noexcept {
    if(!internal&&installed.load()&&c==hookedContext){if(frame){frame->gpu.invalidate();frame->buffers={};frame->lastDepthClear=0;}else foreignWrites.fetch_add(1);}
}
// Anything outside the certified overlay draw/clear route discards the branch.
using IndexedInstanced=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
using Instanced=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
using Auto=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using Indirect=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT);
using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
using Region=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
using Update=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
using Resolve=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,ID3D11Resource*,UINT,DXGI_FORMAT);
using Execute=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11CommandList*,BOOL);
IndexedInstanced indexedInstanced=nullptr;Instanced instanced=nullptr;Auto autoDraw=nullptr;
Indirect indexedIndirect=nullptr,plainIndirect=nullptr;
Copy copy=nullptr;Region region=nullptr;Update update=nullptr;Resolve resolveResource=nullptr;Execute execute=nullptr;
void STDMETHODCALLTYPE indexedInstancedHook(ID3D11DeviceContext*c,UINT a,UINT b,UINT d,INT e,UINT f){unsupported(c);indexedInstanced(c,a,b,d,e,f);}
void STDMETHODCALLTYPE instancedHook(ID3D11DeviceContext*c,UINT a,UINT b,UINT d,UINT e){unsupported(c);instanced(c,a,b,d,e);}
void STDMETHODCALLTYPE autoHook(ID3D11DeviceContext*c){unsupported(c);autoDraw(c);}
void STDMETHODCALLTYPE indexedIndirectHook(ID3D11DeviceContext*c,ID3D11Buffer*b,UINT o){unsupported(c);indexedIndirect(c,b,o);}
void STDMETHODCALLTYPE plainIndirectHook(ID3D11DeviceContext*c,ID3D11Buffer*b,UINT o){unsupported(c);plainIndirect(c,b,o);}
void writeResource(ID3D11DeviceContext*c,ID3D11Resource*r) noexcept {
    if(!internal&&c==hookedContext)bufferWrite(r);
    if(!internal&&installed.load()&&c==hookedContext&&!frame)foreignWrites.fetch_add(1);
    if(!internal&&installed.load()&&c==hookedContext&&frame){const auto id=reinterpret_cast<std::uintptr_t>(r);
        if(id==frame->lastDepthResource)frame->lastDepthClear=0;
        if(id==frame->source.resource)frame->gpu.invalidateFront();
        if(id==frame->presentation.resource)frame->gpu.invalidateScene();}
    if(!internal&&installed.load()&&c==hookedContext&&frame)frame->gpu.depthResourceWrite(reinterpret_cast<std::uintptr_t>(r));
}
void STDMETHODCALLTYPE copyHook(ID3D11DeviceContext*c,ID3D11Resource*d,ID3D11Resource*s){writeResource(c,d);copy(c,d,s);}
void STDMETHODCALLTYPE regionHook(ID3D11DeviceContext*c,ID3D11Resource*d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource*s,UINT ss,const D3D11_BOX*b){writeResource(c,d);region(c,d,sub,x,y,z,s,ss,b);}
void STDMETHODCALLTYPE updateHook(ID3D11DeviceContext*c,ID3D11Resource*d,UINT sub,const D3D11_BOX*b,const void*s,UINT row,UINT depth){writeResource(c,d);update(c,d,sub,b,s,row,depth);}
void STDMETHODCALLTYPE resolveHook(ID3D11DeviceContext*c,ID3D11Resource*d,UINT sub,ID3D11Resource*s,UINT ss,DXGI_FORMAT f){writeResource(c,d);resolveResource(c,d,sub,s,ss,f);}
void STDMETHODCALLTYPE executeHook(ID3D11DeviceContext*c,ID3D11CommandList*l,BOOL restore){unsupported(c);execute(c,l,restore);}
using Query=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Asynchronous*);
using DepthClear=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8);
DepthClear depthClear=nullptr;
void STDMETHODCALLTYPE depthClearHook(ID3D11DeviceContext*c,ID3D11DepthStencilView*v,UINT flags,FLOAT depth,UINT8 stencil){
    if(!internal&&installed.load()&&c==hookedContext){if(frame){internal=true;
        frame->lastDepthClear=reinterpret_cast<std::uintptr_t>(v);frame->depthFlags=flags;frame->depthValue=depth;frame->stencilValue=stencil;
        ComPtr<ID3D11Resource> resource;v->GetResource(&resource);frame->lastDepthResource=reinterpret_cast<std::uintptr_t>(resource.Get());
        frame->gpu.clearDepth(c,v,flags,depth,stencil);internal=false;
    }else foreignWrites.fetch_add(1);}
    depthClear(c,v,flags,depth,stencil);
}
using DispatchCompute=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT);
using ClearUint=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const UINT*);
using ClearFloat=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11UnorderedAccessView*,const FLOAT*);
using Mips=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11ShaderResourceView*);
using ClearView=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11View*,const FLOAT*,const D3D11_RECT*,UINT);
using Region1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*,UINT);
using Update1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*,UINT,const D3D11_BOX*,const void*,UINT,UINT,UINT);
using DiscardResource=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11Resource*);
using DiscardView=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11View*);
using DiscardView1=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3D11View*,const D3D11_RECT*,UINT);
Query beginQuery=nullptr,endQuery=nullptr;DispatchCompute compute=nullptr;Indirect computeIndirect=nullptr;
ClearUint clearUint=nullptr;ClearFloat clearFloat=nullptr;Mips mips=nullptr;ClearView clearView=nullptr;
Region1 region1=nullptr;Update1 update1=nullptr;DiscardResource discardResource=nullptr;DiscardView discardView=nullptr;DiscardView1 discardView1=nullptr;
bool drawQuery(ID3D11Asynchronous* a) noexcept {
    ComPtr<ID3D11Query> query;if(FAILED(a->QueryInterface(IID_PPV_ARGS(&query))))return true;
    D3D11_QUERY_DESC d{};query->GetDesc(&d);return d.Query!=D3D11_QUERY_EVENT&&d.Query!=D3D11_QUERY_TIMESTAMP&&d.Query!=D3D11_QUERY_TIMESTAMP_DISJOINT;
}
void STDMETHODCALLTYPE beginHook(ID3D11DeviceContext*c,ID3D11Asynchronous*a){if(c==hookedContext&&drawQuery(a)){activeQueries.fetch_add(1);unsupported(c);}beginQuery(c,a);}
void STDMETHODCALLTYPE endHook(ID3D11DeviceContext*c,ID3D11Asynchronous*a){endQuery(c,a);if(c==hookedContext&&drawQuery(a)){auto n=activeQueries.load();if(n)activeQueries.fetch_sub(1);else unsupported(c);}}
void STDMETHODCALLTYPE computeHook(ID3D11DeviceContext*c,UINT x,UINT y,UINT z){unsupported(c);compute(c,x,y,z);}
void STDMETHODCALLTYPE computeIndirectHook(ID3D11DeviceContext*c,ID3D11Buffer*b,UINT o){unsupported(c);computeIndirect(c,b,o);}
void viewWrite(ID3D11DeviceContext*c,ID3D11View*v) noexcept {ComPtr<ID3D11Resource> r;v->GetResource(&r);writeResource(c,r.Get());}
void STDMETHODCALLTYPE clearUintHook(ID3D11DeviceContext*c,ID3D11UnorderedAccessView*v,const UINT*x){viewWrite(c,v);clearUint(c,v,x);}
void STDMETHODCALLTYPE clearFloatHook(ID3D11DeviceContext*c,ID3D11UnorderedAccessView*v,const FLOAT*x){viewWrite(c,v);clearFloat(c,v,x);}
void STDMETHODCALLTYPE mipsHook(ID3D11DeviceContext*c,ID3D11ShaderResourceView*v){viewWrite(c,v);mips(c,v);}
void STDMETHODCALLTYPE clearViewHook(ID3D11DeviceContext1*c,ID3D11View*v,const FLOAT*x,const D3D11_RECT*r,UINT n){viewWrite(c,v);clearView(c,v,x,r,n);}
void STDMETHODCALLTYPE region1Hook(ID3D11DeviceContext1*c,ID3D11Resource*d,UINT sub,UINT x,UINT y,UINT z,ID3D11Resource*s,UINT ss,const D3D11_BOX*b,UINT flags){writeResource(c,d);region1(c,d,sub,x,y,z,s,ss,b,flags);}
void STDMETHODCALLTYPE update1Hook(ID3D11DeviceContext1*c,ID3D11Resource*d,UINT sub,const D3D11_BOX*b,const void*s,UINT row,UINT depth,UINT flags){writeResource(c,d);update1(c,d,sub,b,s,row,depth,flags);}
void STDMETHODCALLTYPE discardResourceHook(ID3D11DeviceContext1*c,ID3D11Resource*d){writeResource(c,d);discardResource(c,d);}
void STDMETHODCALLTYPE discardViewHook(ID3D11DeviceContext1*c,ID3D11View*v){viewWrite(c,v);discardView(c,v);}
void STDMETHODCALLTYPE discardView1Hook(ID3D11DeviceContext1*c,ID3D11View*v,const D3D11_RECT*r,UINT n){viewWrite(c,v);discardView1(c,v,r,n);}
using MapBuffer=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
using UnmapBuffer=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT);
MapBuffer mapBuffer=nullptr;UnmapBuffer unmapBuffer=nullptr;
HRESULT STDMETHODCALLTYPE mapHook(ID3D11DeviceContext*c,ID3D11Resource*r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE*out){
    if(!internal&&c==hookedContext&&type!=D3D11_MAP_READ)writeResource(c,r);
    const auto result=mapBuffer(c,r,sub,type,flags,out);
    if(diagnostics::optionalOutputEnabled()&&!internal&&c==hookedContext&&owned_hud_runtime_observer::runtime->renderMaterial(primitive)){static unsigned n=0;if(n++<12)log_info("bone-eater","Wide HUD upload map material={:x} type={} result={:x}",primitive,unsigned(type),unsigned(result));}
    if(!internal&&c==hookedContext&&frame&&SUCCEEDED(result)&&sub==0&&type==D3D11_MAP_WRITE_DISCARD&&out&&out->pData&&
       owned_hud_runtime_observer::runtime->renderMaterial(primitive)){
        ComPtr<ID3D11Buffer> buffer;D3D11_BUFFER_DESC desc{};std::uint16_t count=0;
        if(SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&buffer)))){buffer->GetDesc(&desc);
            if((desc.BindFlags==D3D11_BIND_VERTEX_BUFFER)&&read(primitive,0x1a,count)&&count&&count<=2048&&desc.ByteWidth>=count*112u)
                frame->mapped={r,out->pData,primitive,count*112u};
        }
    }return result;
}
void STDMETHODCALLTYPE unmapHook(ID3D11DeviceContext*c,ID3D11Resource*r,UINT sub){
    if(!internal&&c==hookedContext&&frame&&frame->mapped.resource.Get()==r&&sub==0){
        auto& m=frame->mapped;
        try {std::vector<unsigned char> bytes(m.bytes);
            if(owned_hud_runtime_observer::readBytes(reinterpret_cast<std::uintptr_t>(m.data),bytes.data(),bytes.size())){
                auto& entries=frame->buffers;auto slot=std::find_if(entries.begin(),entries.end(),[&](const auto& b){return b.material==m.material;});
                if(slot==entries.end())slot=std::find_if(entries.begin(),entries.end(),[](const auto& b){return !b.material;});
                if(slot!=entries.end())*slot={r,m.material,std::move(bytes)};
            }
        }catch(...){}
        m={};
    }
    unmapBuffer(c,r,sub);
}
template<class T> bool hook(void** table,unsigned slot,T callback,T& original) noexcept {
    original=reinterpret_cast<T>(table[slot]);return detour::trampoline_try(original,callback,&original);
}
using CreateVertex=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11VertexShader**);
CreateVertex createVertex=nullptr;
HRESULT STDMETHODCALLTYPE createVertexHook(ID3D11Device* d,const void* bytes,SIZE_T length,ID3D11ClassLinkage* linkage,ID3D11VertexShader** out){
    const auto result=createVertex(d,bytes,length,linkage,out);
    if(SUCCEEDED(result)&&out&&*out&&!linkage)recordWideHudVertexShader(*out,bytes,length);
    return result;
}
bool installShaderObserver(ID3D11Device* device) noexcept {
    if(!requested()||!device)return false;
    static std::mutex mutex;std::lock_guard<std::mutex> lock(mutex);
    if(createVertex)return true;
    const bool ok=hook(*reinterpret_cast<void***>(device),12,&createVertexHook,createVertex);
    if(!ok)createVertex=nullptr;
    return ok;
}
void ensureInstalled() noexcept {
    static bool attempted=false;
    if(attempted||!requested()||!owned_hud_runtime_observer::installed.load())return;
    std::uintptr_t context=0;if(!read(base,0x12E5818,context)||!context)return;
    attempted=true;hookedContext=reinterpret_cast<ID3D11DeviceContext*>(context);
    auto** table=*reinterpret_cast<void***>(hookedContext);
    ComPtr<ID3D11Device> device;hookedContext->GetDevice(&device);
    if(!installShaderObserver(device.Get()))return;
    if(!hook(table,14,&mapHook,mapBuffer)||!hook(table,15,&unmapHook,unmapBuffer)||!hook(table,12,&indexedHook,indexed)||!hook(table,13,&plainHook,plain)||!hook(table,50,&clearTargetHook,clearTarget)||
       !hook(table,20,&indexedInstancedHook,indexedInstanced)||!hook(table,21,&instancedHook,instanced)||
       !hook(table,38,&autoHook,autoDraw)||!hook(table,39,&indexedIndirectHook,indexedIndirect)||!hook(table,40,&plainIndirectHook,plainIndirect)||
       !hook(table,46,&regionHook,region)||!hook(table,47,&copyHook,copy)||!hook(table,48,&updateHook,update)||
       !hook(table,57,&resolveHook,resolveResource)||!hook(table,58,&executeHook,execute)) {
        log_warning("bone-eater","Wide HUD context hook incomplete; native rendering retained");return;
    }
    ComPtr<ID3D11DeviceContext1> c1;
    if(FAILED(hookedContext->QueryInterface(IID_PPV_ARGS(&c1))))return;
    auto** table1=*reinterpret_cast<void***>(c1.Get());
    if(!hook(table,53,&depthClearHook,depthClear)||!hook(table,27,&beginHook,beginQuery)||!hook(table,28,&endHook,endQuery)||
       !hook(table,41,&computeHook,compute)||!hook(table,42,&computeIndirectHook,computeIndirect)||
       !hook(table,51,&clearUintHook,clearUint)||!hook(table,52,&clearFloatHook,clearFloat)||!hook(table,54,&mipsHook,mips)||
       !hook(table1,115,&region1Hook,region1)||!hook(table1,116,&update1Hook,update1)||
       !hook(table1,117,&discardResourceHook,discardResource)||!hook(table1,118,&discardViewHook,discardView)||
       !hook(table1,132,&clearViewHook,clearView)||!hook(table1,133,&discardView1Hook,discardView1)) {
        log_warning("bone-eater","Wide HUD extended write hooks incomplete; native rendering retained");return;
    }
    frame=new Frame;installed.store(true);
    log_info("bone-eater","Wide HUD transactional compositor installed: complete front and paired presentation branch");
}
} // namespace wide_hud
