#include "render/native_owned_hud_runtime.h"
#include <map>
#include <vector>
#include <string>
#include <cassert>
#include <thread>
#include <iostream>
using namespace bone_eater::render;
namespace {
std::map<std::uintptr_t,std::vector<unsigned char>> memory;
constexpr std::uintptr_t base=0x180000000, manager=0x10000, owner=0x20000, root=0x30000;
constexpr std::uintptr_t imageNative=0x800000, fontNative=0x801000, wrapper=0x802000;
constexpr std::uintptr_t timerNative=0x803000,timerMaterial=0x812000;
constexpr std::uintptr_t imageMaterial=0x810000, fontMaterial=0x811000, camera=0x820000;
std::uintptr_t gui(std::size_t i) { return 0x100000+i*0x1000; }
std::uintptr_t adapter(std::size_t i) { return 0x600000+i*0x1000; }
bool bytes(std::uintptr_t p,void* out,std::size_t length) noexcept {
    auto it=memory.upper_bound(p);
    if (it==memory.begin()) return false;
    --it;
    if (p-it->first>it->second.size() || length>it->second.size()-(p-it->first)) return false;
    std::memcpy(out,it->second.data()+p-it->first,length); return true;
}
template<class T> void put(std::uintptr_t p,std::size_t offset,T value) {
    auto it=memory.upper_bound(p+offset); assert(it!=memory.begin()); --it;
    assert(p+offset-it->first+sizeof(T)<=it->second.size());
    std::memcpy(it->second.data()+p+offset-it->first,&value,sizeof(T));
}
void name(std::uintptr_t p,const char* value) {
    assert(std::strlen(value)<32);
    std::array<char,32> text {}; std::memcpy(text.data(),value,std::strlen(value)); put(p,0x40,text);
}
void fixture() {
    memory.clear();
    memory[base].resize(0x1400000); memory[manager].resize(0x200); memory[owner].resize(0x2200);
    memory[root].resize(0x200);
    for (auto p:{imageNative,fontNative,wrapper,imageMaterial,fontMaterial,camera}) memory[p].resize(0x600);
    put(base,0x13DCF78,manager); put(manager,0,base+0x10CA298); put(manager,0x48,3u); put(manager,0xD0,owner);
    put(owner,0,base+0x10CA1D8); put(owner,0x48,3u); put(owner,0xE0,4u); put(owner,0x140,root);
    put(base,0x13DB5B0,camera); put(root,0,base+0x10CEBA8); put(root,8,root); put(root,0x3C,4u); name(root,"root_upper");
    for (auto native:{imageNative,fontNative}) {
        put(native,0,base+0x10CC0D8); put(native,0x1A0,0x800000u);
        put(native,0x1A8,camera); put(native,0x570,2u);
        put(native,0x308,native==imageNative ? imageMaterial : fontMaterial);
    }
    put(wrapper,0x18,fontNative); put(wrapper,0x90,2u);
    for (auto material:{imageMaterial,fontMaterial}) {
        put(material,0,base+0x706570); put(material,0x10,material+0x100000);
        put(material,0x18,std::uint16_t{1});
    }
    std::map<std::string,std::uintptr_t> named {{"root_upper",root}};
    for (std::size_t i=0;i<ownedHudNodeCount;++i) named[ownedHudNodes[i].name]=gui(i);
    std::map<std::uintptr_t,std::vector<std::uintptr_t>> children;
    for (std::size_t i=0;i<ownedHudNodeCount;++i) {
        const auto& spec=ownedHudNodes[i];
        memory[gui(i)].resize(0x200); memory[adapter(i)].resize(0x100);
        put(gui(i),0,base+(spec.font ? 0x10CEAE8 : 0x10CEBA8)); put(gui(i),8,gui(i));
        put(gui(i),0x3C,4u); put(gui(i),0x30,adapter(i)); put(gui(i),0x10,named.at(spec.parent));
        put(gui(i),0x110,spec.font ? wrapper : imageNative); put(gui(i),0x12C,2u); name(gui(i),spec.name);
        put(adapter(i),0,base+(spec.font ? 0x10CEC58 : 0x10CEC08));
        put(adapter(i),0x40,spec.font ? wrapper : imageNative);
        children[named.at(spec.parent)].push_back(gui(i));
    }
    for (const auto& pair:children) {
        put(pair.first,0x18,pair.second.front());
        for (std::size_t i=1;i<pair.second.size();++i) put(pair.second[i-1],0x28,pair.second[i]);
    }
    const std::pair<unsigned,unsigned> anchors[] {{0x148,0},{0x150,1},{0x158,2},{0x160,23},{0x168,24},{0x170,25},
        {0x188,47},{0x1B8,46},{0x1C0,11},{0x1C8,12},{0x1D0,13},{0x1D8,14},{0x1E0,15},{0x1E8,10},
        {0x200,6},{0x208,5},{0x210,4},{0x218,9},{0x220,8},{0x228,7},{0x230,19},
        {0x288,37},{0x290,38},{0x298,39},{0x2A0,40},{0x2A8,41},{0x2B0,42},{0x2B8,43},{0x2C0,44},{0x2C8,45},
        {0x6A8,27},{0x6B0,28},{0x6B8,29},{0x1C60,2},{0x1C68,3},{0x1C70,17},{0x1C78,18},
        {0x1C88,25},{0x1C90,26},{0x1C98,32},{0x1CA0,33}};
    for (auto a:anchors) put(owner,a.first,gui(a.second));
}
void reset(OwnedHudRuntime& runtime,std::uintptr_t material) {
    const auto ticket=runtime.resetBefore(material); assert(ticket.material==material);
    put(material,0x40,std::uint64_t{}); runtime.resetAfter(ticket);
}
void append(OwnedHudRuntime& runtime,std::size_t node) {
    const auto ticket=runtime.beforeSubmit(adapter(node)); assert(ticket.node==node);
    std::uintptr_t native=0,material=0;assert(bytes(adapter(node)+0x40,&native,8));
    if(ownedHudNodes[node].font)native=fontNative;assert(bytes(native+0x308,&material,8));
    put(material,0x44,ticket.first+1); put(material,0x40,ticket.bytes+0x70);
    runtime.afterSubmit(ticket);
}
void ready(OwnedHudRuntime& runtime) {
    fixture(); runtime.initialize(base,&bytes);
    reset(runtime,fontMaterial); reset(runtime,imageMaterial); assert(runtime.active());
    for (std::size_t i=0;i<ownedHudNodeCount;++i) append(runtime,i);
}
constexpr std::uintptr_t guiManager=0xa00000,sortList=0xb00000,sortCells=0xb10000;
constexpr std::uint64_t hiddenMask=0x3060603fc;
void admissionFixture(OwnedHudRuntime& runtime) {
    fixture(); runtime.initialize(base,&bytes);
    memory[guiManager].resize(0x14000); memory[sortList].resize(0x1000); memory[sortCells].resize(0x1000);
    put(base,0x13DCFE8,guiManager); put(guiManager,0,base+0x10CE680); put(guiManager,8,base+0x10CE6A8);
    put(guiManager+8,0x138B8,static_cast<int>(ownedHudNodeCount));
    unsigned admitted=0;
    for (std::size_t i=0;i<ownedHudNodeCount;++i) {
        put(guiManager+8,0x28+i*0x10,OwnedHudAdmission::Record{i,gui(i)});
        if (hiddenMask&(std::uint64_t{1}<<i)) continue;
        put(sortList,admitted*8,sortCells+admitted*8); put(sortCells,admitted*8,gui(i)); ++admitted;
    }
    assert(admitted==41);
    reset(runtime,fontMaterial); reset(runtime,imageMaterial);
}
void packetTests(OwnedHudRuntime& runtime,bool separateTimer=false) {
    admissionFixture(runtime);
    if(separateTimer){
        memory[timerNative]=memory[imageNative];memory[timerMaterial]=memory[imageMaterial];
        put(timerNative,0x308,timerMaterial);put(timerMaterial,0x10,timerMaterial+0x100000);
        for(unsigned i=49;i<=53;++i){put(gui(i),0x110,timerNative);put(adapter(i),0x40,timerNative);}
        reset(runtime,timerMaterial);reset(runtime,imageMaterial);
    }
    const auto token=runtime.admissionBegin(guiManager+8);runtime.admissionCapture(token,sortList,41);
    for(std::size_t i=0;i<ownedHudNodeCount;++i)if(!(hiddenMask&(std::uint64_t{1}<<i)))append(runtime,i);
    unsigned imageCount=0;assert(bytes(imageMaterial+0x44,&imageCount,4));
    put(imageMaterial,0x44,imageCount+1);put(imageMaterial,0x40,(imageCount+1)*112); // Shared native overlay.
    constexpr std::uintptr_t record=0xc00000,native=0xc01000,rear=0xc02000;
    memory[record].resize(0x100);memory[native].resize(0x400);memory[rear].resize(0x100);
    put(record,0x40,native);put(native,0x308,rear);put(rear,0x10,std::uintptr_t{0xc03000});
    for(unsigned i=0;i<2;++i){auto ticket=runtime.beforeRear(record,i);assert(ticket.index==i);
        put(rear,0x44,i+1);put(rear,0x40,(i+1)*112);runtime.afterRear(ticket);}
    put(rear,0x44,7u);put(rear,0x40,7u*112); // Other native quads share the rear material.
    std::vector<std::uintptr_t> materials{imageMaterial,fontMaterial,rear};
    if(separateTimer)materials.push_back(timerMaterial);
    std::array<std::vector<unsigned char>,4> uploaded;
    for(unsigned i=0;i<materials.size();++i){unsigned n=0;assert(bytes(materials[i]+0x40,&n,4));
        auto data=std::uintptr_t{0xd00000}+i*0x10000;
        memory[data]=std::vector<unsigned char>(n,static_cast<unsigned char>(i+1));uploaded[i]=memory[data];
        put(materials[i],0x38,data+n);
    }
    runtime.admissionFinish(token,true);
    auto packet=runtime.matchUpload(rear,uploaded[2]);assert(packet&&packet->rear.total==7);
    auto animated=uploaded[0];animated.back()^=0x40;
    assert(!runtime.matchUpload(imageMaterial,animated));
    auto matched=runtime.resolveUpload(imageMaterial,animated);
    assert(matched.certificate==packet&&matched.packet&&matched.packet!=packet);
    assert(OwnedHudRuntime::mappingEquivalent(packet,matched.packet,imageMaterial));
    assert(runtime.publishPacket(matched.certificate,[]()noexcept{return true;}));
    assert(!runtime.publishPacket(matched.packet,[]()noexcept{return true;})); // Mapping is not an owner certificate.
    assert(runtime.matchUpload(imageMaterial,uploaded[0],packet->frame)==packet);
    assert(runtime.matchUpload(fontMaterial,uploaded[1],packet->frame)==packet);
    if(separateTimer)assert(runtime.matchUpload(timerMaterial,uploaded[3],packet->frame)==packet);
    // CPU reset precedes GPU consumption. Immutable bytes survive that reset.
    runtime.resetBefore(rear);reset(runtime,imageMaterial);
    assert(runtime.matchUpload(rear,uploaded[2],packet->frame)==packet);
    assert(runtime.publishPacket(packet,[]()noexcept{return true;}));
    uploaded[2][0]^=1;assert(!runtime.matchUpload(rear,uploaded[2],packet->frame));
    assert(!runtime.matchUpload(imageMaterial,uploaded[0],packet->frame+100));
    put(owner,0x48,2u);assert(!runtime.publishPacket(packet,[]()noexcept{return true;}));
    put(owner,0x48,3u);
    reset(runtime,fontMaterial);
    if(separateTimer)reset(runtime,timerMaterial);
    const auto nextToken=runtime.admissionBegin(guiManager+8);runtime.admissionCapture(nextToken,sortList,41);
    for(std::size_t i=0;i<ownedHudNodeCount;++i)if(!(hiddenMask&(std::uint64_t{1}<<i)))append(runtime,i);
    put(imageMaterial,0x44,imageCount+1);put(imageMaterial,0x40,(imageCount+1)*112);
    // Identical upload bytes cannot distinguish conflicting ownership ranges.
    // Insert an unowned quad before the two owned rear quads in the next frame.
    put(rear,0x44,1u);put(rear,0x40,112u);
    for(unsigned i=0;i<2;++i){auto ticket=runtime.beforeRear(record,i);assert(ticket.index==i);
        put(rear,0x44,i+2);put(rear,0x40,(i+2)*112);runtime.afterRear(ticket);}
    put(rear,0x44,7u);put(rear,0x40,7u*112);
    memory[0xd00000][0]^=0x20; // A changing image upload identifies this CPU frame.
    runtime.admissionFinish(nextToken,true);
    assert(runtime.latestPacketFrame()!=packet->frame);
    uploaded[2][0]^=1;
    assert(!runtime.matchUpload(rear,uploaded[2]));
    assert(!runtime.matchUpload(rear,uploaded[2],packet->frame));
    std::array<OwnedHudRuntime::UploadProof,2> joint{{{rear,uploaded[2]},{imageMaterial,uploaded[0]}}};
    assert(runtime.matchUploads(joint)==packet);
    joint[1].bytes=memory[0xd00000];
    auto next=runtime.matchUploads(joint);assert(next&&next->frame!=packet->frame);
    // The native front pass may use frame N while the rear pass uses N+1.
    // Both actual uploads are independently certified; publication requires
    // one unchanged owner tree, not an artificial common CPU timestamp.
    std::array<OwnedHudRuntime::Packet,2> adjacent{{packet,next}};
    assert(runtime.publishPackets(adjacent,[]()noexcept{return true;}));
    put(owner,0x48,2u);
    assert(!runtime.publishPackets(adjacent,[]()noexcept{return true;}));
    put(owner,0x48,3u);
    adjacent[1].reset();
    assert(!runtime.publishPackets(adjacent,[]()noexcept{return true;}));
    assert(OwnedHudRuntime::mappingEquivalent(packet,next,imageMaterial));
    assert(!OwnedHudRuntime::mappingEquivalent(packet,next,rear));
    // An independently matching but incompatible timer cannot be combined.
    if(separateTimer){auto badTimer=uploaded[3];badTimer[0]^=1;
        std::array<OwnedHudRuntime::UploadProof,3> mixed{{joint[0],joint[1],{timerMaterial,badTimer}}};
        assert(!runtime.matchUploads(mixed));}
    runtime.initialize(base,&bytes);assert(!runtime.matchUpload(imageMaterial,uploaded[0]));
}
void admissionTests(OwnedHudRuntime& runtime) {
    // Native font adapter resets during manager scan, before first-sort capture.
    admissionFixture(runtime);
    reset(runtime,fontMaterial);
    auto firstSort=runtime.admissionBegin(guiManager+8); assert(firstSort);
    runtime.admissionCapture(firstSort,sortList,41);
    for (std::size_t i=0;i<ownedHudNodeCount;++i)
        if (!(hiddenMask&(std::uint64_t{1}<<i))) append(runtime,i);
    runtime.admissionFinish(firstSort,true);
    assert(runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);
    for (int scenario=0;scenario<9;++scenario) {
        admissionFixture(runtime);
        const auto token=runtime.admissionBegin(guiManager+8); assert(token);
        runtime.admissionCapture(token,sortList,41);
        assert(!runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);
        for (std::size_t i=0;i<ownedHudNodeCount;++i)
            if (!(hiddenMask&(std::uint64_t{1}<<i))) append(runtime,i);
        if (scenario==1) put(guiManager+8,0x28,OwnedHudAdmission::Record{100,gui(0)});
        if (scenario==2) append(runtime,2); //Conflicts with omitted node.
        if (scenario==3) runtime.admissionCapture(token,sortList,41); //Duplicate sort.
        if (scenario==4) assert(!runtime.admissionBegin(guiManager+8)); //Nested pass.
        if (scenario==5) reset(runtime,imageMaterial); //Epoch changed mid-manager.
        runtime.admissionFinish(token,scenario!=6);
        if (scenario>=1 && scenario<=6) {
            assert(!runtime.active()); continue;
        }
        auto plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
        assert(plan.replacement && runtime.stillCurrent(plan));
        const auto d=runtime.diagnostics();
        assert(d.admissionMask==hiddenMask && !d.admissionToken);
        assert(d.selection.missing[0]==0 && d.selection.missing[1]==0 && d.selection.missing[2]==0);
        if (scenario==7) runtime.beforeSubmit(adapter(2)); //Late conflict revokes existing plan.
        if (scenario==8) runtime.admissionBegin(guiManager+8); //Repeated pass revokes certificate.
        if (scenario>=7) assert(!runtime.stillCurrent(plan));
    }
}
}
void sharedOverlayUploadTests() {
    OwnedHudRuntime::RenderBatch batch{1,2,std::vector<unsigned char>(5*112)};
    for(unsigned q=0;q<5;++q)std::fill_n(batch.vertices.begin()+q*112,112,static_cast<unsigned char>(q+1));
    OwnedHudDrawPlan plan;plan.replacement=true;plan.batch={1,1,2};plan.totalQuads=5;plan.count=5;
    plan.ranges[0]={0,6,OwnedHudGroup::Left};plan.ranges[1]={6,6,OwnedHudGroup::Count};
    plan.ranges[2]={12,6,OwnedHudGroup::Center};plan.ranges[3]={18,6,OwnedHudGroup::Count};
    plan.ranges[4]={24,6,OwnedHudGroup::Right};
    auto upload=batch.vertices;
    upload[112]^=0x20;upload[3*112+51]^=0x40; // Native overlays animate independently.
    assert(OwnedHudRuntime::matchesOwnedUpload(batch,plan,upload));
    for(unsigned q:{0u,2u,4u}){auto changed=upload;changed[q*112+27]^=1;
        assert(!OwnedHudRuntime::matchesOwnedUpload(batch,plan,changed));}
    auto reordered=upload;std::swap_ranges(reordered.begin(),reordered.begin()+112,reordered.begin()+4*112);
    assert(!OwnedHudRuntime::matchesOwnedUpload(batch,plan,reordered));
    OwnedHudDrawPlan rebased;
    assert(OwnedHudRuntime::rebaseOwnedUpload(batch,plan,reordered,rebased));
    assert(rebased.ranges[0].destination==OwnedHudGroup::Right&&rebased.ranges[4].destination==OwnedHudGroup::Left);
    auto changedOwned=reordered;changedOwned[2*112]^=1;
    assert(!OwnedHudRuntime::rebaseOwnedUpload(batch,plan,changedOwned,rebased));
    auto duplicate=reordered;std::copy_n(duplicate.begin(),112,duplicate.begin()+112);
    assert(!OwnedHudRuntime::rebaseOwnedUpload(batch,plan,duplicate,rebased));
    auto ambiguous=batch;std::copy_n(ambiguous.vertices.begin(),112,ambiguous.vertices.begin()+4*112);
    assert(!OwnedHudRuntime::rebaseOwnedUpload(ambiguous,plan,ambiguous.vertices,rebased));
    auto shortened=upload;shortened.resize(4*112);
    assert(!OwnedHudRuntime::matchesOwnedUpload(batch,plan,shortened));
    auto invalid=plan;invalid.ranges[2].firstIndex=6;
    assert(!OwnedHudRuntime::matchesOwnedUpload(batch,invalid,upload));
    invalid=plan;invalid.batch.vertexBuffer=3;
    assert(!OwnedHudRuntime::matchesOwnedUpload(batch,invalid,upload));
    invalid=plan;for(auto& r:invalid.ranges)r.destination=OwnedHudGroup::Count;
    assert(!OwnedHudRuntime::matchesOwnedUpload(batch,invalid,upload));
}
int main() {
    sharedOverlayUploadTests();
    OwnedHudRuntime runtime;
    admissionTests(runtime);
    ready(runtime);
    auto plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
    assert(plan.replacement && plan.totalQuads==54 && runtime.stillCurrent(plan));
    auto font=runtime.prepareDraw(fontMaterial,true,{true,true,true},true);
    assert(font.replacement && font.totalQuads==1 && font.ranges[0].destination==OwnedHudGroup::Center);
    assert(runtime.stillCurrent(font));
    const auto inFlight=runtime.resetBefore(imageMaterial);
    assert(inFlight.material==imageMaterial && !runtime.active() && !runtime.stillCurrent(plan));
    assert(!runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);
    put(imageMaterial,0x40,std::uint64_t{}); runtime.resetAfter(inFlight);
    assert(runtime.active() && !runtime.stillCurrent(plan));
    reset(runtime,imageMaterial);
    assert(!runtime.stillCurrent(plan) && !runtime.stillCurrent(font));

    fixture(); runtime.initialize(base,&bytes);
    reset(runtime,fontMaterial); reset(runtime,imageMaterial);
    for (std::size_t i=0;i<ownedHudNodeCount;++i) {
        if (i==0) { const auto zero=runtime.beforeSubmit(adapter(i)); runtime.afterSubmit(zero); }
        else append(runtime,i);
    }
    plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
    assert(plan.replacement && plan.totalQuads==53 && runtime.stillCurrent(plan));

    ready(runtime);
    put(gui(19),0x10,gui(0)); //Unlimited sibling reparented after submit.
    assert(!runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);

    ready(runtime);
    put(owner,0x1C60,gui(3)); //Known alias must match the actual satellite root.
    assert(!runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);

    ready(runtime);
    plan=runtime.prepareDraw(imageMaterial,true,{false,true,true},true);
    assert(plan.replacement);
    for (std::size_t i=0;i<plan.count;++i) assert(plan.ranges[i].destination!=OwnedHudGroup::Left);

    fixture(); runtime.initialize(base,&bytes);
    reset(runtime,fontMaterial); reset(runtime,imageMaterial);
    // Worker-thread submits are published to the separate render thread.
    std::thread worker([&] { for (std::size_t i=0;i<ownedHudNodeCount;++i) append(runtime,i); });
    worker.join();
    plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
    assert(plan.replacement && runtime.stillCurrent(plan));

    ready(runtime);
    plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
    auto ignored=runtime.beforeSubmit(adapter(0)); //Submission after sealing invalidates that group.
    assert(ignored.node==ownedHudNodeCount && !runtime.stillCurrent(plan));

    ready(runtime);
    put(wrapper,0x90,0u);
    assert(!runtime.prepareDraw(imageMaterial,true,{true,true,true},true).replacement);

    ready(runtime);
    plan=runtime.prepareDraw(imageMaterial,true,{true,true,true},true);
    put(imageMaterial,0x44,55u);
    assert(!runtime.stillCurrent(plan));

    fixture(); runtime.initialize(base,&bytes);
    put(gui(18),0x28,gui(18)); //Cycle in the owner tree must fail bounded traversal.
    reset(runtime,imageMaterial);
    assert(!runtime.active());
    ready(runtime);
    constexpr std::uintptr_t rearRecord=0xB00000,rearNative=0xB01000,rearMaterial=0xB02000,rearVertex=0xB03000;
    memory[rearRecord].resize(0x100);memory[rearNative].resize(0x400);memory[rearMaterial].resize(0x100);
    put(rearRecord,0x40,rearNative);put(rearNative,0x308,rearMaterial);put(rearMaterial,0x10,rearVertex);
    for(unsigned i=0;i<2;++i){auto ticket=runtime.beforeRear(rearRecord,i);assert(ticket.index==i);
        put(rearMaterial,0x44,i+1);put(rearMaterial,0x40,(i+1)*0x70);runtime.afterRear(ticket);}
    auto rear=runtime.rearPair();assert(rear.seen==3&&rear.total==2&&rear.first[0]==0&&rear.first[1]==1);
    std::array<OwnedHudDrawPlan,2> plans{runtime.prepareDraw(imageMaterial,true,{true,true,true},true),runtime.prepareDraw(fontMaterial,true,{true,true,true},true)};
    unsigned publications=0;
    assert(runtime.publishCurrent(plans,rear,[&]()noexcept{++publications;return true;}));assert(publications==1);
    put(rearMaterial,0x10,rearVertex+8);
    assert(!runtime.publishCurrent(plans,rear,[&]()noexcept{++publications;return true;}));assert(publications==1);
    put(rearMaterial,0x10,rearVertex);runtime.resetBefore(rearMaterial);
    assert(!runtime.rearPair().frame&&!runtime.publishCurrent(plans,rear,[]()noexcept{return true;}));
    packetTests(runtime);
    packetTests(runtime,true);
    std::cout << "native owned HUD runtime tests passed\n";
}
