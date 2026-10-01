#pragma once
#include "render/owned_hud_quad_selection.h"
#include "render/gui_font_identity.h"
#include "render/owned_hud_admission.h"
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <deque>
#include <span>

namespace bone_eater::render {
// Native hooks live in the integration. All reads are injected so identity and
// lifecycle decisions can be exercised without dereferencing process pointers.
class OwnedHudRuntime {
public:
    using ReadBytes=bool(*)(std::uintptr_t,void*,std::size_t) noexcept;
    struct ResetTicket { std::uintptr_t material=0; std::uint64_t generation=0; bool anchor=false,timer=false; };
    struct SubmitTicket {
        std::size_t node=ownedHudNodeCount;
        std::uint64_t frame=0;
        OwnedHudBatchKey batch;
        std::uint32_t first=0, bytes=0;
    };
    struct RearTicket { std::uint64_t frame=0; std::uintptr_t material=0,vertex=0; unsigned index=2,first=0,bytes=0; };
    struct RearPair { std::uint64_t frame=0; std::uintptr_t material=0,vertex=0; std::array<unsigned,2> first{}; unsigned seen=0,total=0; };
    RearTicket beforeRear(std::uintptr_t record,unsigned index) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        RearTicket t;std::uintptr_t native=0;
        if(index>1 || !activeUnlocked() || rearRejectedFrame_==frame_ || !read(record,0x40,native) || !read(native,0x308,t.material) ||
           !read(t.material,0x10,t.vertex)||!t.vertex||!read(t.material,0x44,t.first)||
           !read(t.material,0x40,t.bytes)||t.first>=OwnedHudQuadSelection::maxQuads||t.bytes!=t.first*0x70)return {};
        t.frame=frame_;t.index=index;return t;
    }
    void afterRear(const RearTicket& t) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(t.index>1)return;
        unsigned count=0,bytes=0;std::uintptr_t vertex=0;
        if(!activeUnlocked()||t.frame!=frame_||!read(t.material,0x44,count)||count!=t.first+1||
           !read(t.material,0x40,bytes)||bytes!=t.bytes+0x70||!read(t.material,0x10,vertex)||vertex!=t.vertex){rear_={};rearRejectedFrame_=frame_;rearReason_=1;return;}
        if(rear_.frame!=frame_)rear_={frame_,t.material,t.vertex};
        if(rear_.material!=t.material||rear_.vertex!=t.vertex||(rear_.seen&(1u<<t.index))){rear_={};rearRejectedFrame_=frame_;rearReason_=2;return;}
        rearReason_=0;rear_.first[t.index]=t.first;rear_.seen|=1u<<t.index;
    }
    struct RearDiagnostics {RearPair raw;std::uint64_t current=0;bool active=false;unsigned reason=0;};
    RearDiagnostics rearDiagnostics() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return {rear_,frame_,activeUnlocked(),rearReason_};}
    RearPair rearPair() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);return rearUnlocked();
    }
    bool ownsRearMaterial(std::uintptr_t material) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return material&&material==rear_.material&&rear_.frame==frame_&&rear_.seen==3;
    }
    // Hold the same mutex as reset/submit/admission through the final GPU
    // publication. A copied certificate alone cannot authorize a later draw.
    template<class Publish> bool publishCurrent(const std::array<OwnedHudDrawPlan,2>& plans,
            const RearPair& rear,Publish&& publish) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now=rearUnlocked();
        if(!now.frame||plans[0].batch.material==plans[1].batch.material||now.frame!=rear.frame||now.material!=rear.material||now.vertex!=rear.vertex||
           now.first!=rear.first||now.total!=rear.total||!currentUnlocked(plans[0])||!currentUnlocked(plans[1]))return false;
        for(unsigned i=0;i<3;++i)if(!selection_.ready(static_cast<OwnedHudGroup>(i)))return false;
        return publish();
    }
    struct RenderBatch {std::uintptr_t material=0,vertex=0;std::vector<unsigned char> vertices;};
    struct RenderPacket {std::uint64_t frame=0;std::array<OwnedHudDrawPlan,3> plans;RearPair rear;std::array<RenderBatch,4> batches;};
    using Packet=std::shared_ptr<const RenderPacket>;
    // A shared image material also contains native overlays that keep their
    // original mapping. Their animation must not invalidate byte-identical HUD
    // ranges. Preserve the certified ordinals, count and every selected byte.
    static bool matchesOwnedUpload(const RenderBatch& batch,const OwnedHudDrawPlan& plan,
            std::span<const unsigned char> bytes) noexcept {
        if(!plan.replacement||!plan.batch.valid()||plan.batch.material!=batch.material||
           plan.batch.vertexBuffer!=batch.vertex||!plan.totalQuads||plan.totalQuads>2048||
           bytes.size()!=plan.totalQuads*112u||batch.vertices.size()!=bytes.size()||
           !plan.count||plan.count>plan.ranges.size())return false;
        unsigned next=0,owned=0;
        for(unsigned i=0;i<plan.count;++i){const auto& r=plan.ranges[i];
            if(r.firstIndex!=next||!r.indexCount||r.indexCount%6||
               r.indexCount>plan.totalQuads*6-next||unsigned(r.destination)>unsigned(OwnedHudGroup::Count))return false;
            if(r.destination!=OwnedHudGroup::Count){
                const auto begin=next/6*112,length=r.indexCount/6*112;
                if(std::memcmp(bytes.data()+begin,batch.vertices.data()+begin,length))return false;
                owned+=r.indexCount;
            }
            next+=r.indexCount;
        }
        return owned&&next==plan.totalQuads*6;
    }
    static bool rebaseOwnedUpload(const RenderBatch& batch,const OwnedHudDrawPlan& plan,
            std::span<const unsigned char> bytes,OwnedHudDrawPlan& output) noexcept {
        // Validate the complete source partition before using any range offset.
        if(!matchesOwnedUpload(batch,plan,batch.vertices)||bytes.size()!=batch.vertices.size())return false;
        std::array<OwnedHudGroup,2048> source{},destination{};
        std::array<bool,2048> used{};
        for(unsigned i=0;i<plan.count;++i){const auto& r=plan.ranges[i];
            for(unsigned q=r.firstIndex/6;q<(r.firstIndex+r.indexCount)/6;++q)source[q]=r.destination;
        }
        for(unsigned q=0;q<plan.totalQuads;++q){
            bool found=false;unsigned selected=plan.totalQuads;
            auto group=OwnedHudGroup::Count;
            for(unsigned j=0;j<plan.totalQuads;++j){
                if(std::memcmp(bytes.data()+q*112,batch.vertices.data()+j*112,112))continue;
                if(found&&source[j]!=group)return false; // Identical bytes with conflicting ownership.
                found=true;group=source[j];
                if(!used[j]&&selected==plan.totalQuads)selected=j;
            }
            destination[q]=group;
            if(group!=OwnedHudGroup::Count){if(selected==plan.totalQuads)return false;used[selected]=true;}
        }
        for(unsigned q=0;q<plan.totalQuads;++q)if(source[q]!=OwnedHudGroup::Count&&!used[q])return false;
        auto rebased=plan;rebased.count=0;
        for(unsigned q=0;q<plan.totalQuads;++q){
            if(rebased.count&&rebased.ranges[rebased.count-1].destination==destination[q])rebased.ranges[rebased.count-1].indexCount+=6;
            else {if(rebased.count==rebased.ranges.size())return false;
                rebased.ranges[rebased.count++]={q*6,6,destination[q]};}
        }
        output=rebased;return true;
    }
    std::uint64_t latestPacketFrame() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return packets_.empty()?0:packets_.back().data->frame;}
    bool renderMaterial(std::uintptr_t material) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& p:packets_)for(const auto& b:p.data->batches)if(b.material==material)return true;
        return false;
    }
    Packet matchUpload(std::uintptr_t material,std::span<const unsigned char> bytes,std::uint64_t frame=0) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        Packet result,first;unsigned firstSlot=4;
        for(auto p=packets_.rbegin();p!=packets_.rend();++p)for(unsigned slot=0;slot<4;++slot){const auto& b=p->data->batches[slot];
            if(b.material!=material||b.vertices.size()!=bytes.size())continue;
            if(!std::equal(bytes.begin(),bytes.end(),b.vertices.begin()))continue;
            if(first&&!sameMapping(*first,firstSlot,*p->data,slot))return {};
            if(!first){first=p->data;firstSlot=slot;}
            if(!result&&(!frame||p->data->frame==frame))result=p->data;
        }return result;
    }
    static bool mappingEquivalent(const Packet& a,const Packet& b,std::uintptr_t material) noexcept {
        if(!a||!b||!material)return false;
        for(unsigned i=0;i<4;++i)if(a->batches[i].material==material)
            for(unsigned j=0;j<4;++j)if(b->batches[j].material==material)return sameMapping(*a,i,*b,j);
        return false;
    }
    struct UploadProof {std::uintptr_t material=0;std::span<const unsigned char> bytes;};
    struct UploadDiagnostic {unsigned candidates=0,differences=~0u,offset=0,slot=4,destination=4,matchingQuads=0,unmatchedDestinations=0,ownedMatches=0;std::uint64_t frame=0;};
    UploadDiagnostic diagnoseUpload(std::uintptr_t material,std::span<const unsigned char> bytes) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);UploadDiagnostic result;
        for(const auto& p:packets_)for(unsigned slot=0;slot<4;++slot){const auto& b=p.data->batches[slot];
            if(b.material!=material||b.vertices.size()!=bytes.size()||bytes.empty())continue;
            ++result.candidates;unsigned count=0,first=0;
            for(unsigned i=0;i<bytes.size();++i)if(bytes[i]!=b.vertices[i]){if(!count)first=i;++count;}
            if(count<=result.differences){result.differences=count;result.offset=first;result.slot=slot;result.frame=p.data->frame;result.destination=4;
                result.matchingQuads=0;std::array<bool,2048> used{};
                for(unsigned q=0;q<bytes.size()/112;++q)for(unsigned j=0;j<bytes.size()/112;++j)
                    if(!used[j]&&std::memcmp(bytes.data()+q*112,b.vertices.data()+j*112,112)==0){used[j]=true;++result.matchingQuads;break;}
                result.unmatchedDestinations=0;result.ownedMatches=0;
                if(slot<3)for(unsigned i=0;i<p.data->plans[slot].count;++i){const auto& r=p.data->plans[slot].ranges[i];
                    if(first/112*6>=r.firstIndex&&first/112*6<r.firstIndex+r.indexCount)result.destination=unsigned(r.destination);
                    for(unsigned q=r.firstIndex/6;q<(r.firstIndex+r.indexCount)/6;++q){
                        if(!used[q])result.unmatchedDestinations|=1u<<unsigned(r.destination);
                        else if(r.destination!=OwnedHudGroup::Count)++result.ownedMatches;
                    }}}
        }return result;
    }
    struct MatchedUpload {Packet certificate,packet;};
    MatchedUpload resolveUpload(std::uintptr_t material,std::span<const unsigned char> bytes) const noexcept {
        if(auto exact=matchUpload(material,bytes))return {exact,exact};
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            Packet certificate;std::shared_ptr<RenderPacket> mapped;
            for(auto p=packets_.rbegin();p!=packets_.rend();++p){const auto& b=p->data->batches[0];
                if(b.material!=material||b.vertices.size()!=bytes.size())continue;
                auto plan=p->data->plans[0];
                if(!matchesOwnedUpload(b,plan,bytes)&&!rebaseOwnedUpload(b,plan,bytes,plan))continue;
                if(mapped){auto candidate=*p->data;candidate.plans[0]=plan;
                    if(!sameMapping(*mapped,0,candidate,0))return {};}
                else {certificate=p->data;mapped=std::make_shared<RenderPacket>(*p->data);
                    mapped->plans[0]=plan;mapped->batches[0].vertices.assign(bytes.begin(),bytes.end());}
            }
            return {certificate,mapped};
        } catch(...) {return {};}
    }
    // Intersect actual uploads, rather than pinning the newest packet matching
    // a static label before a changing timer/rear batch arrives. Only a common
    // immutable CPU packet is required only by callers requesting a joint match.
    // Native rendering uses independent matchUpload certificates instead.
    Packet matchUploads(std::span<const UploadProof> uploads) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(uploads.empty()||uploads.size()>4)return {};
        Packet result;
        std::array<unsigned,4> firstSlots{};
        for(auto p=packets_.rbegin();p!=packets_.rend();++p){
            std::array<unsigned,4> slots{};bool matches=true;
            for(unsigned i=0;i<uploads.size();++i){slots[i]=4;const auto& u=uploads[i];
                if(!u.material||u.bytes.empty()){matches=false;break;}
                for(unsigned j=0;j<4;++j){const auto& b=p->data->batches[j];
                    if(b.material==u.material&&b.vertices.size()==u.bytes.size()&&std::equal(u.bytes.begin(),u.bytes.end(),b.vertices.begin())){slots[i]=j;break;}}
                if(slots[i]==4){matches=false;break;}
            }
            if(!matches)continue;
            if(result){for(unsigned i=0;i<uploads.size();++i)if(!sameMapping(*result,firstSlots[i],*p->data,slots[i]))return {};}
            else {result=p->data;firstSlots=slots;}
        }
        return result;
    }
    template<class Publish> bool publishPacket(const Packet& packet,Publish&& publish) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!packet)return false;
        for(const auto& p:packets_)if(p.data==packet){Snapshot repeated;
            if(!collect(repeated)||!(repeated==p.identity))return false;
            return publish();
        }return false;
    }
    // Native front and rear passes can consume adjacent CPU generations. Each
    // uploaded batch must have its own exact immutable certificate, and every
    // certificate must still describe the same live owner tree.
    template<class Publish> bool publishPackets(std::span<const Packet> packets,Publish&& publish) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(packets.empty()||packets.size()>4)return false;
        Snapshot repeated;if(!collect(repeated))return false;
        for(const auto& packet:packets){if(!packet)return false;
            const auto found=std::find_if(packets_.begin(),packets_.end(),[&](const auto& p){return p.data==packet;});
            if(found==packets_.end()||!(found->identity==repeated))return false;
        }
        return publish();
    }
    struct Diagnostics {
        std::uint64_t frame=0,imageEpoch=0,fontEpoch=0; bool active=false,sealed=false;
        unsigned collectStage=0; std::size_t node=0;
        OwnedHudQuadSelection::Diagnostics selection;
        std::uint64_t admissionMask=0,admissionToken=0;
        unsigned admissionStage=0;
    };
    Diagnostics diagnostics() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return {frame_,imageEpoch_,fontEpoch_,activeUnlocked(),sealed_,collectStage_,collectNode_,selection_.diagnostics(),admissionMask_,admissionToken_,admissionStage_};
    }
    void initialize(std::uintptr_t module, ReadBytes reader) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        base_=module; reader_=reader;packets_.clear();
        serial_=frame_=imageEpoch_=fontEpoch_=timerEpoch_=0; fontEpochMaterial_=timerEpochMaterial_=0;
        imageResets_=fontResets_=timerResets_=0;
        admission_={}; submittedMask_=0;
        abortUnlocked(); disabled_=false;
    }
    std::uint64_t frame() const noexcept { std::lock_guard<std::mutex> lock(mutex_); return frame_; }
    bool active() const noexcept { std::lock_guard<std::mutex> lock(mutex_); return activeUnlocked(); }
    bool ownsMaterial(std::uintptr_t material) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_.valid && (material==snapshot_.imageMaterial || material==snapshot_.fontMaterial || material==snapshot_.timerMaterial);
    }
    // Whole-manager and first-sort observation. All provisional/certified state
    // shares the submit/render mutex; no native list pointers survive capture.
    std::uint64_t admissionBegin(std::uintptr_t manager) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeUnlocked()) return 0;
        try {
            admissionStage_=1;
            if (sealed_ || admissionToken_) { abortUnlocked(); return 0; }
            std::vector<OwnedHudAdmission::Record> registry;
            Snapshot repeated;
            admissionStage_=2;
            if (!collect(repeated) || !(repeated==snapshot_)) { abortUnlocked(); return 0; }
            admissionStage_=3;
            if (!readOwnedHudRegistry(base_,manager,reader_,registry)) { abortUnlocked(); return 0; }
            admissionStage_=4;
            admissionToken_=admission_.begin(admissionStamp(),registry,admissionSelected());
            if (!admissionToken_) { abortUnlocked(); return 0; }
            admissionManager_=manager;
            admissionStage_=5;
            for (std::size_t i=0;i<ownedHudNodeCount;++i)
                if (submittedMask_&(std::uint64_t{1}<<i)) admission_.submitted(snapshot_.nodes[i].gui);
            return admissionToken_;
        } catch (...) { abortUnlocked(); return 0; }
    }
    void admissionCapture(std::uint64_t token,std::uintptr_t list,int count) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!token) return;
        try {
            if (!activeUnlocked()) { abortUnlocked(); return; } //Preserve reset failure stage.
            admissionStage_=61;
            if (token!=admissionToken_ || sealed_) { abortUnlocked(); return; }
            std::vector<std::uintptr_t> admitted;
            admissionStage_=64;
            if (!readOwnedHudAdmitted(list,count,reader_,admitted)) { abortUnlocked(); return; }
            if (!admission_.capture(token,admitted)) {
                admissionStage_=70+admission_.captureFailure(); abortUnlocked(); return;
            }
            admissionStage_=7;
        } catch (...) { abortUnlocked(); }
    }
    void admissionFinish(std::uint64_t token,bool normalReturn) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!token) return;
        try {
            if (activeUnlocked()) admissionStage_=8;
            if (!normalReturn || token!=admissionToken_ || !activeUnlocked() || sealed_) { abortUnlocked(); return; }
            std::vector<OwnedHudAdmission::Record> registry;
            Snapshot repeated;
            const bool stable=collect(repeated) && repeated==snapshot_ &&
                readOwnedHudRegistry(base_,admissionManager_,reader_,registry);
            const auto proof=admission_.finish(token,admissionStamp(),registry,admissionSelected(),stable);
            admissionToken_=0; admissionManager_=0;
            admissionStage_=9;
            if (!proof.valid || (proof.omitted&submittedMask_)) { abortUnlocked(); return; }
            admissionMask_=proof.omitted;
            admissionStage_=10;
            for (std::size_t i=0;i<ownedHudNodeCount;++i)
                // Null adapters already received an explicit zero at reset.
                if ((proof.omitted&(std::uint64_t{1}<<i)) && snapshot_.nodes[i].record)
                    selection_.observe(frame_,i,{},0,0,true);
            freezePacket();
        } catch (...) { abortUnlocked(); }
    }
    // Call before/after the original material reset329310. All non-anchor
    // materials pass through; a known font reset advances only its own epoch.
    ResetTicket resetBefore(std::uintptr_t material) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(material==rear_.material){rear_={};rearRejectedFrame_=frame_;rearReason_=3;}
        std::uintptr_t owner=0, root=0, anchor=0;
        std::uint32_t group=0;
        if (ownerIdentity(owner,root,group) && read(owner,0x148,anchor)) {
            std::uintptr_t native=0, candidate=0, vt=0;
            if (read(anchor,0,vt) && vt==base_+0x10CEBA8 && read(anchor,0x110,native) &&
                    read(native,0x308,candidate) && candidate==material) {
                ++imageResets_;
                cancelAdmission();
                admissionStage_=0;
                if (++serial_==0) { abortUnlocked(); return {}; }
                return {material,serial_,true};
            }
        }
        // The native timer atlas has its own reset/upload lifecycle. Resolve it
        // through the independently owned shooting-range root, never by atlas.
        std::uintptr_t timerRoot=0,child=0;
        if(owner && read(owner,0x188,timerRoot) && read(timerRoot,0x18,child)) {
            for(unsigned i=0;child && i<16;++i) {
                Node node;
                if(!readNode(child,node) || node.parent!=timerRoot)break;
                if(!std::strcmp(node.name.data(),"lcd_bt_timenum_3")) {
                    if(node.material==material) {
                        ++timerResets_;
                        if(admissionToken_ || admissionMask_) {admissionStage_=21;abortUnlocked();}
                        if(++serial_==0){abortUnlocked();return {};}
                        return {material,serial_,false,true};
                    }
                    break;
                }
                if(!read(child,0x28,child))break;
            }
        }
        std::uintptr_t gui=0;
        GuiFontIdentity font;
        if (owner && read(owner,0x1B8,gui) && readPlayerNameIdentity(base_,owner,gui,1,
                [this](auto p,auto o,auto& v) noexcept { return read(p,o,v); },font) &&
                material==font.material) {
            ++fontResets_;
            if (admissionToken_ || admissionMask_) { admissionStage_=20; abortUnlocked(); }
            if (++serial_==0) { abortUnlocked(); return {}; }
            return {material,serial_,false};
        }
        return {};
    }
    void resetAfter(ResetTicket ticket) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ticket.material) return;
        auto& resets=ticket.anchor ? imageResets_ : ticket.timer ? timerResets_ : fontResets_;
        if (resets!=1) { if (resets) --resets; abortUnlocked(); return; }
        --resets;
        std::uint64_t counters=1;
        if (!read(ticket.material,0x40,counters) || counters) { abortUnlocked(); return; }
        if(ticket.timer) {
            timerEpoch_=ticket.generation;timerEpochMaterial_=ticket.material;
            if(sealed_)selection_.invalidate(OwnedHudGroup::Center);
            return;
        }
        if (!ticket.anchor) {
            fontEpoch_=ticket.generation;
            fontEpochMaterial_=ticket.material;
            if (sealed_) selection_.invalidate(OwnedHudGroup::Center);
            return;
        }
        if(rear_.seen)rearReason_=4;
        frame_=ticket.generation;
        imageEpoch_=ticket.generation;
        sealed_=false;
        disabled_=false;
        selection_.begin(frame_);
        snapshot_={};
        Snapshot current;
        if (!collect(current) || current.imageMaterial!=ticket.material) { abortUnlocked(); return; }
        snapshot_=current;
        submittedMask_=0;
        if (fontEpochMaterial_!=snapshot_.fontMaterial) fontEpoch_=0;
        if(timerEpochMaterial_!=snapshot_.timerMaterial)timerEpoch_=0;
        // The font must have been observed resetting while its identity was
        // current. A first-frame font with no known epoch stays native.
        if (!fontEpoch_) selection_.invalidate(OwnedHudGroup::Center);
        for (std::size_t i=0;i<ownedHudNodeCount;++i)
            if (!snapshot_.nodes[i].record)
                selection_.observe(frame_,i,{},0,0,true); //1E5560 returns on null+30.
    }
    SubmitTicket beforeSubmit(std::uintptr_t record) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        SubmitTicket result;
        if (!activeUnlocked()) return result;
        for (std::size_t i=0;i<ownedHudNodeCount;++i) {
            const auto& node=snapshot_.nodes[i];
            if (!record || record!=node.record) continue;
            const auto bit=std::uint64_t{1}<<i;
            submittedMask_|=bit;
            admission_.submitted(node.gui);
            if (admissionMask_&bit) { abortUnlocked(); return result; }
            if (sealed_) { selection_.invalidate(ownedHudNodes[i].group,OwnedHudQuadSelection::LateMutation); return result; }
            Node current;
            if (!readNode(node.gui,current) || !(current==node) || !ownerStillCurrent()) {
                selection_.invalidate(ownedHudNodes[i].group,OwnedHudQuadSelection::Identity); return result;
            }
            const auto epoch=epochFor(node.material);
            std::uintptr_t vertex=0;
            if (!epoch || !read(node.material,0x10,vertex) || !vertex) {
                selection_.invalidate(ownedHudNodes[i].group,OwnedHudQuadSelection::VertexUnavailable); return {};
            }
            if (!read(node.material,0x44,result.first) || !read(node.material,0x40,result.bytes) ||
                    result.first>=OwnedHudQuadSelection::maxQuads || result.bytes!=result.first*0x70) {
                selection_.invalidate(ownedHudNodes[i].group,OwnedHudQuadSelection::CounterState); return {};
            }
            result.node=i; result.frame=frame_;
            result.batch={node.material,epoch,vertex};
            return result;
        }
        return result;
    }
    void afterSubmit(const SubmitTicket& ticket) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ticket.node>=ownedHudNodeCount) return;
        const auto group=ownedHudNodes[ticket.node].group;
        std::uint32_t count=0, bytes=0;
        std::uintptr_t vertex=0;
        const auto epoch=epochFor(ticket.batch.material);
        if (!activeUnlocked() || sealed_ || frame_!=ticket.frame || epoch!=ticket.batch.epoch ||
                !read(ticket.batch.material,0x10,vertex) || vertex!=ticket.batch.vertexBuffer) {
            selection_.invalidate(group,OwnedHudQuadSelection::EpochChanged); return;
        }
        Node current;
        if (!readNode(snapshot_.nodes[ticket.node].gui,current) || !(current==snapshot_.nodes[ticket.node]) ||
                !ownerStillCurrent()) { selection_.invalidate(group,OwnedHudQuadSelection::Identity); return; }
        if (!read(ticket.batch.material,0x44,count) || !read(ticket.batch.material,0x40,bytes)) {
            selection_.invalidate(group,OwnedHudQuadSelection::AppendDelta); return;
        }
        if (count==ticket.first && bytes==ticket.bytes)
            selection_.observe(frame_,ticket.node,{},0,0,true); //Observed native call emitted no quad.
        else if (count==ticket.first+1 && bytes==ticket.bytes+0x70)
            selection_.observe(frame_,ticket.node,ticket.batch,ticket.first,1);
        else selection_.invalidate(group,OwnedHudQuadSelection::AppendDelta);
    }
    // Call once the native primitive has been prepared, before originalDraw.
    // A returned replacement plan is valid only for this synchronous draw and
    // the supplied current frame. GPU integration rechecks bound VB/IB/context.
    OwnedHudDrawPlan prepareDraw(std::uintptr_t material, bool targetReady,
                                const std::array<bool,3>& backingReady,
                                bool fontRouteCertified) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!activeUnlocked() || admissionToken_) return {};
        std::size_t slot=material==snapshot_.imageMaterial ? 0 :
            material==snapshot_.fontMaterial ? 1 : material==snapshot_.timerMaterial ? 2 : 3;
        if (slot==3) return {};
        if (!sealed_) {
            Snapshot repeated;
            const bool stable=collect(repeated) && repeated==snapshot_;
            selection_.seal(frame_,stable,targetReady,backingReady,fontRouteCertified);
            sealed_=true;
            preflight(snapshot_.imageMaterial,imageEpoch_);
            if (snapshot_.fontMaterial) preflight(snapshot_.fontMaterial,fontEpoch_);
            preflightTimer();
        }
        if (!targetReady || !ownerStillCurrent()) { abortUnlocked(); return {}; }
        OwnedHudBatchKey key;
        std::uint32_t count=0;
        if (!batchNow(material,epochFor(material),key,count)) return {};
        return selection_.plan(frame_,key,count);
    }
    // Repeated identical native draws receive the same partition. Returning
    // an unchanged original draw after one replacement would resurrect the
    // removed placement. The integration certifies the intended output pass.
    void abort() noexcept { std::lock_guard<std::mutex> lock(mutex_); abortUnlocked(); }
    bool stillCurrent(const OwnedHudDrawPlan& plan) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentUnlocked(plan);
    }
private:
    bool currentUnlocked(const OwnedHudDrawPlan& plan) const noexcept {
        if (!activeUnlocked() || !plan.replacement || plan.frame!=frame_) return false;
        const auto epoch=epochFor(plan.batch.material);
        OwnedHudBatchKey current; std::uint32_t count=0;
        if (!ownerStillCurrent() || !batchNow(plan.batch.material,epoch,current,count) ||
                !(current==plan.batch) || count!=plan.totalQuads) return false;
        for (std::size_t i=0;i<plan.count;++i)
            if (plan.ranges[i].destination!=OwnedHudGroup::Count && !selection_.ready(plan.ranges[i].destination)) return false;
        return true;
    }
    RearPair rearUnlocked() const noexcept {
        auto r=rear_;std::uintptr_t vertex=0;unsigned bytes=0;
        if(!activeUnlocked()||r.frame!=frame_||r.seen!=3||r.first[0]==r.first[1]||
           !read(r.material,0x10,vertex)||vertex!=r.vertex||!read(r.material,0x44,r.total)||
           !read(r.material,0x40,bytes)||bytes!=r.total*0x70||r.total>OwnedHudQuadSelection::maxQuads||
           r.first[0]>=r.total||r.first[1]>=r.total)return {};
        return r;
    }
    RearPair rear_;
    unsigned rearReason_=0;
    std::uint64_t rearRejectedFrame_=0;
    bool activeUnlocked() const noexcept { return snapshot_.valid && frame_ && !disabled_ && !imageResets_ && !fontResets_ && !timerResets_; }
    void cancelAdmission() noexcept {
        admission_.cancel(); admissionToken_=admissionMask_=0; admissionManager_=0;
    }
    void abortUnlocked() noexcept { cancelAdmission(); snapshot_={}; selection_={}; rear_={};rearReason_=5;rearRejectedFrame_=0; disabled_=true; sealed_=false; }
    OwnedHudAdmission::Stamp admissionStamp() const noexcept { return {frame_,imageEpoch_,fontEpoch_}; }
    OwnedHudAdmission::Selected admissionSelected() const noexcept {
        OwnedHudAdmission::Selected result{};
        for (std::size_t i=0;i<result.size();++i) result[i]=snapshot_.nodes[i].gui;
        return result;
    }
    struct Node {
        std::uintptr_t gui=0, parent=0, record=0, wrapper=0, native=0, material=0;
        std::uint32_t group=0;
        std::array<char,32> name {};
        bool font=false;
        bool operator==(const Node&) const = default;
    };
    struct Snapshot {
        bool valid=false;
        std::uintptr_t owner=0, root=0, imageMaterial=0, fontMaterial=0, timerMaterial=0;
        std::uint32_t group=0;
        std::array<Node,ownedHudNodeCount> nodes {};
        bool operator==(const Snapshot&) const = default;
    };
    static bool sameMapping(const RenderPacket& a,unsigned ai,const RenderPacket& b,unsigned bi) noexcept {
        if(ai!=bi)return false;
        if(ai==3){
            auto x=a.rear.first,y=b.rear.first;
            if(x[1]<x[0])std::swap(x[0],x[1]);
            if(y[1]<y[0])std::swap(y[0],y[1]);
            return x==y&&a.rear.total==b.rear.total;
        }
        const auto& x=a.plans[ai];const auto& y=b.plans[bi];
        if(x.count!=y.count||x.totalQuads!=y.totalQuads)return false;
        for(std::size_t i=0;i<x.count;++i)if(x.ranges[i].firstIndex!=y.ranges[i].firstIndex||
            x.ranges[i].indexCount!=y.ranges[i].indexCount||x.ranges[i].destination!=y.ranges[i].destination)return false;
        return true;
    }
    struct StoredPacket {std::shared_ptr<RenderPacket> data;Snapshot identity;};
    std::deque<StoredPacket> packets_;
    bool captureBatch(RenderBatch& batch,std::uintptr_t material,unsigned count) {
        unsigned bytes=0,actual=0;std::uintptr_t end=0;
        if(!count||count>2048||!read(material,0x44,actual)||actual!=count||!read(material,0x40,bytes)||bytes!=count*112||
           !read(material,0x38,end)||end<bytes||!read(material,0x10,batch.vertex)||!batch.vertex)return false;
        batch.material=material;batch.vertices.resize(bytes);
        return reader_(end-bytes,batch.vertices.data(),bytes);
    }
    void freezePacket() {
        const auto rear=rearUnlocked();if(!rear.frame)return;
        Snapshot repeated;if(!collect(repeated)||!(repeated==snapshot_))return;
        if(!sealed_){selection_.seal(frame_,true,true,{true,true,true},true);sealed_=true;
            preflight(snapshot_.imageMaterial,imageEpoch_);preflight(snapshot_.fontMaterial,fontEpoch_);preflightTimer();}
        for(unsigned i=0;i<3;++i)if(!selection_.ready(static_cast<OwnedHudGroup>(i)))return;
        auto p=std::make_shared<RenderPacket>();p->frame=frame_;p->rear=rear;
        for(unsigned i=0;i<3;++i){auto material=i==0?snapshot_.imageMaterial:i==1?snapshot_.fontMaterial:snapshot_.timerMaterial;
            if(i==2){unsigned n=0;if(!material)continue;if(!read(material,0x44,n))return;if(!n)continue;}
            OwnedHudBatchKey key;unsigned count=0;
            if(!batchNow(material,epochFor(material),key,count))return;
            p->plans[i]=selection_.plan(frame_,key,count);
            if(!p->plans[i].replacement||!captureBatch(p->batches[i],material,count))return;
        }
        if(!captureBatch(p->batches[3],rear.material,rear.total))return;
        if(!packets_.empty()&&packets_.back().data->frame==frame_)return;
        packets_.push_back({p,snapshot_});while(packets_.size()>64)packets_.pop_front();
    }
    template<typename T> bool read(std::uintptr_t owner,std::size_t offset,T& out) const noexcept {
        return reader_ && owner && owner<=std::numeric_limits<std::uintptr_t>::max()-offset &&
            owner+offset<=std::numeric_limits<std::uintptr_t>::max()-sizeof(out) &&
            reader_(owner+offset,&out,sizeof(out));
    }
    bool ownerIdentity(std::uintptr_t& owner,std::uintptr_t& root,std::uint32_t& group) const noexcept {
        std::uintptr_t manager=0, vt=0;
        std::uint32_t state=0;
        return read(base_,0x13DCF78,manager) && read(manager,0,vt) && vt==base_+0x10CA298 &&
            read(manager,0x48,state) && state==3 && read(manager,0xD0,owner) &&
            read(owner,0,vt) && vt==base_+0x10CA1D8 && read(owner,0x48,state) && state==3 &&
            read(owner,0xE0,group) && group>0 && group<32 && read(owner,0x140,root) && root;
    }
    bool ownerStillCurrent() const noexcept {
        std::uintptr_t owner=0,root=0; std::uint32_t group=0;
        return ownerIdentity(owner,root,group) && owner==snapshot_.owner &&
            root==snapshot_.root && group==snapshot_.group;
    }
    bool readNode(std::uintptr_t gui,Node& out) const noexcept {
        std::uintptr_t vt=0,self=0,back=0,camera=0,slot=0;
        std::uint32_t selector=0;
        out.gui=gui;
        if (!read(gui,0,vt) || (vt!=base_+0x10CEBA8 && vt!=base_+0x10CEAE8) ||
                !read(gui,8,self) || self!=gui || !read(gui,0x10,out.parent) ||
                !read(gui,0x3C,out.group) || !read(gui,0x40,out.name) ||
                !std::memchr(out.name.data(),0,out.name.size()) || !read(gui,0x30,out.record)) return false;
        out.font=vt==base_+0x10CEAE8;
        if (!out.record) return !out.font; // Only proven image no-submit branch.
        if (!read(gui,0x110,out.native)) return false;
        if (out.font) {
            out.wrapper=out.native;
            if (!read(out.wrapper,0x18,out.native)) return false;
        }
        if (!read(out.record,0,vt) || vt!=base_+(out.font ? 0x10CEC58 : 0x10CEC08) ||
                !read(out.record,0x40,back) || back!=(out.font ? out.wrapper : out.native) ||
                !read(out.native,0,vt) || (vt!=base_+0x10CC0D8 && vt!=base_+0x10CD1E8) ||
                (out.font && vt!=base_+0x10CC0D8) || !read(out.native,0x570,selector) || selector!=2 ||
                !read(out.native,0x1A8,camera) || !read(base_,0x13DB5B0,slot) || !camera || camera!=slot ||
                !read(out.native,0x308,out.material) || !read(out.material,0,vt) || vt!=base_+0x706570) return false;
        return true;
    }
    bool collect(Snapshot& out) const noexcept {
        collectStage_=1; collectNode_=0;
        if (!ownerIdentity(out.owner,out.root,out.group)) return false;
        collectStage_=2;
        std::array<std::uintptr_t,256> pending {},visited {};
        std::array<bool,ownedHudNodeCount> found {};
        std::size_t count=1,seen=0;
        pending[0]=out.root;
        while (count) {
            const auto gui=pending[--count];
            if (!gui || seen==visited.size()) return false;
            for (std::size_t i=0;i<seen;++i) if (visited[i]==gui) return false;
            visited[seen++]=gui;
            std::uintptr_t vt=0,self=0,child=0,next=0,parent=0;
            std::uint32_t group=0;
            std::array<char,32> name {};
            if (!read(gui,0,vt) || (vt!=base_+0x10CEBA8 && vt!=base_+0x10CEAE8 && vt!=base_+0x10CEB48) ||
                    !read(gui,8,self) || self!=gui || !read(gui,0x3C,group) || group!=out.group ||
                    !read(gui,0x10,parent) || !read(gui,0x18,child) || !read(gui,0x28,next) ||
                    !read(gui,0x40,name) || !std::memchr(name.data(),0,32)) return false;
            if (gui==out.root && std::strcmp(name.data(),"root_upper")) return false;
            // Do not traverse root_upper's siblings outside the certified tree.
            if (next && gui!=out.root) {
                std::uintptr_t nextParent=0;
                if (!read(next,0x10,nextParent) || nextParent!=parent || count==pending.size()) return false;
                pending[count++]=next;
            }
            if (child) {
                std::uintptr_t childParent=0;
                if (!read(child,0x10,childParent) || childParent!=gui || count==pending.size()) return false;
                pending[count++]=child;
            }
            for (std::size_t i=0;i<ownedHudNodeCount;++i) if (!std::strcmp(name.data(),ownedHudNodes[i].name)) {
                if (found[i] || !readNode(gui,out.nodes[i]) || out.nodes[i].font!=ownedHudNodes[i].font) return false;
                found[i]=true;
            }
        }
        for (std::size_t i=0;i<ownedHudNodeCount;++i) {
            collectStage_=3; collectNode_=i;
            if (!found[i]) return false;
            const auto& node=out.nodes[i];
            std::uintptr_t parent=out.root;
            if (std::strcmp(ownedHudNodes[i].parent,"root_upper")) {
                parent=0;
                for (std::size_t j=0;j<ownedHudNodeCount;++j)
                    if (!std::strcmp(ownedHudNodes[j].name,ownedHudNodes[i].parent)) parent=out.nodes[j].gui;
            }
            if (!parent || node.parent!=parent) return false;
            for (std::size_t j=0;j<i;++j)
                if (node.record && node.record==out.nodes[j].record) return false;
            if (!node.record) continue;
            if (node.font) out.fontMaterial=node.material;
            else if(i>=49 && i<=53 && node.material!=out.imageMaterial) {
                if(!out.timerMaterial)out.timerMaterial=node.material;
                else if(node.material!=out.timerMaterial)return false;
            }
            else if (!out.imageMaterial) out.imageMaterial=node.material;
            else if (node.material!=out.imageMaterial) return false; //Measured BattleLcd shared batch only.
        }
        // Independently anchor the tree to all known owner fields, including
        // documented aliases (which deliberately point to the same tree node).
        struct Anchor { std::size_t offset,node; };
        constexpr Anchor anchors[] {{0x148,0},{0x150,1},{0x158,2},{0x160,23},{0x168,24},{0x170,25},
            {0x188,47},{0x1B8,46},{0x1C0,11},{0x1C8,12},{0x1D0,13},{0x1D8,14},{0x1E0,15},{0x1E8,10},
            {0x200,6},{0x208,5},{0x210,4},{0x218,9},{0x220,8},{0x228,7},{0x230,19},
            {0x288,37},{0x290,38},{0x298,39},{0x2A0,40},{0x2A8,41},{0x2B0,42},{0x2B8,43},{0x2C0,44},{0x2C8,45},
            {0x6A8,27},{0x6B0,28},{0x6B8,29},{0x1C60,2},{0x1C68,3},{0x1C70,17},{0x1C78,18},
            {0x1C88,25},{0x1C90,26},{0x1C98,32},{0x1CA0,33}};
        for (const auto& a:anchors) {
            collectStage_=4; collectNode_=a.node;
            std::uintptr_t pointer=0;
            if (!read(out.owner,a.offset,pointer) || pointer!=out.nodes[a.node].gui) return false;
        }
        if (!out.imageMaterial || !out.fontMaterial || out.imageMaterial==out.fontMaterial || (out.timerMaterial && out.timerMaterial==out.fontMaterial)) return false;
        GuiFontIdentity font;
        collectStage_=5; collectNode_=46;
        if (!readPlayerNameIdentity(base_,out.owner,out.nodes[46].gui,frame_ ? frame_ : 1,
                [this](auto p,auto o,auto& v) noexcept { return read(p,o,v); },font) ||
                font.adapter!=out.nodes[46].record || font.wrapper!=out.nodes[46].wrapper ||
                font.renderer!=out.nodes[46].native || font.material!=out.fontMaterial) return false;
        std::uintptr_t owner=0,root=0; std::uint32_t group=0;
        collectStage_=6;
        out.valid=ownerIdentity(owner,root,group) && owner==out.owner && root==out.root && group==out.group;
        return out.valid;
    }
    bool batchNow(std::uintptr_t material,std::uint64_t epoch,OwnedHudBatchKey& key,std::uint32_t& count) const noexcept {
        std::uintptr_t vertex=0,index=0,vt=0;
        std::uint16_t type=0;
        // Null explicit IB requires the separately verified native shared quad
        // IB at DrawIndexed. The runtime does not invent an arbitrary IB map.
        if (!epoch || !read(material,0,vt) || vt!=base_+0x706570 ||
                !read(material,0x10,vertex) || !vertex || !read(material,8,index) || index ||
                !read(material,0x18,type) || type!=1 || !read(material,0x44,count) ||
                !count || count>OwnedHudQuadSelection::maxQuads) return false;
        key={material,epoch,vertex}; return true;
    }
    std::uint64_t epochFor(std::uintptr_t material) const noexcept {
        return material==snapshot_.imageMaterial?imageEpoch_:material==snapshot_.fontMaterial?fontEpoch_:
            material && material==snapshot_.timerMaterial?timerEpoch_:0;
    }
    void preflightTimer() noexcept {
        if(!snapshot_.timerMaterial)return;
        unsigned count=0;
        if(!read(snapshot_.timerMaterial,0x44,count)){selection_.invalidate(OwnedHudGroup::Center);return;}
        // A fully omitted timer needs no upload. Its nodes still require the
        // manager's explicit no-submission proof.
        if(count)preflight(snapshot_.timerMaterial,timerEpoch_);
    }
    void preflight(std::uintptr_t material,std::uint64_t epoch) noexcept {
        OwnedHudBatchKey current;
        std::uint32_t count=0;
        if (!batchNow(material,epoch,current,count)) {
            if (material==snapshot_.imageMaterial) {
                selection_.invalidate(OwnedHudGroup::Left); selection_.invalidate(OwnedHudGroup::Right);
            }
            selection_.invalidate(OwnedHudGroup::Center); return;
        }
        selection_.validateBatch(current,current,count,true);
    }
    ReadBytes reader_=nullptr;
    mutable std::mutex mutex_;
    std::uintptr_t base_=0;
    std::uint64_t serial_=0,frame_=0,imageEpoch_=0,fontEpoch_=0,timerEpoch_=0;
    std::uintptr_t fontEpochMaterial_=0,timerEpochMaterial_=0;
    bool disabled_=false,sealed_=false;
    unsigned imageResets_=0,fontResets_=0,timerResets_=0;
    mutable unsigned collectStage_=0;
    mutable std::size_t collectNode_=0;
    Snapshot snapshot_;
    OwnedHudQuadSelection selection_;
    OwnedHudAdmission admission_;
    std::uint64_t admissionToken_=0,admissionMask_=0,submittedMask_=0;
    std::uintptr_t admissionManager_=0;
    unsigned admissionStage_=0;
};
} // namespace bone_eater::render
