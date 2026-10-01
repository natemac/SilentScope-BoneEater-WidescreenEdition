#include "render/owned_hud_admission.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
using namespace bone_eater::render;
using Policy=OwnedHudAdmission;
namespace {
const Policy::Stamp stamp{10,10,9};
Policy::Selected selected() {
    Policy::Selected ids{};
    for (std::size_t i=0;i<ids.size();++i) ids[i]=0x10000+i*0x100;
    return ids;
}
std::vector<Policy::Record> registry() {
    std::vector<Policy::Record> records;
    for (auto id:selected()) records.push_back({id+1,id});
    records.push_back({123,0x90000}); return records;
}
std::vector<std::uintptr_t> admitted() {
    // Reproduce BAD1's actual fourteen missing alternative nodes.
    constexpr std::uint64_t missing=0x3060603fc;
    std::vector<std::uintptr_t> ids;
    const auto all=selected();
    for (std::size_t i=0;i<all.size();++i) if (!(missing&(std::uint64_t{1}<<i))) ids.push_back(all[i]);
    ids.push_back(0x90000); return ids;
}
void policyTests() {
    const auto ids=selected(); const auto records=registry(); const auto drawn=admitted();
    {
        Policy p; const auto token=p.begin(stamp,records,ids); assert(token);
        assert(p.capture(token,drawn));
        p.submitted(ids[0]); //An actually admitted node may submit normally.
        const auto result=p.finish(token,stamp,records,ids,true);
        assert(result.valid && result.omitted==0x3060603fc && result.selected==ids);
        assert(!p.finish(token,stamp,records,ids,true).valid);
        assert(!p.begin(stamp,records,ids)); //No second pass in same frame.
    }
    for (int failure=0;failure<12;++failure) {
        Policy p; const auto token=p.begin(stamp,records,ids); assert(token);
        auto changedRecords=records; auto changedIds=ids; auto changedStamp=stamp;
        auto list=drawn; bool identitiesCurrent=true;
        switch(failure) {
        case 0: break; //No first sort observed.
        case 1: list.push_back(list[0]); assert(!p.capture(token,list)); break;
        case 2: list[0]=0xdead; assert(!p.capture(token,list)); break;
        case 3: assert(!p.capture(token,{})); break;
        case 4: assert(p.capture(token,list)); assert(!p.capture(token,list)); break;
        case 5: p.submitted(ids[2]); assert(!p.capture(token,list)); break;
        case 6: assert(p.capture(token,list)); p.submitted(ids[2]); break;
        case 7: assert(p.capture(token,list)); ++changedStamp.fontEpoch; break;
        case 8: assert(p.capture(token,list)); std::swap(changedRecords[0],changedRecords[1]); break;
        case 9: assert(p.capture(token,list)); ++changedRecords.back().key; break;
        case 10: assert(p.capture(token,list)); identitiesCurrent=false; break;
        case 11: assert(p.capture(token,list)); ++changedIds[2]; break;
        }
        assert(!p.finish(token,changedStamp,changedRecords,changedIds,identitiesCurrent).valid);
    }
    for (int failure=0;failure<6;++failure) {
        Policy p; auto records2=records; auto ids2=ids; auto stamp2=stamp;
        switch(failure) {
        case 0: records2.back().gui=records2[0].gui; break;
        case 1: records2[0].gui=0; break;
        case 2: ids2[1]=ids2[0]; break;
        case 3: ids2[0]=0xbeef; break;
        case 4: stamp2.imageEpoch=0; break;
        case 5: records2.resize(Policy::maxEntries+1); break;
        }
        assert(!p.begin(stamp2,records2,ids2));
    }
    {
        Policy p; const auto token=p.begin(stamp,records,ids); assert(token);
        assert(p.capture(token,drawn)); assert(!p.begin({11,11,11},records,ids));
        assert(!p.finish(token,stamp,records,ids,true).valid); //Reentry poisons outer invocation.
        const Policy::Stamp next{12,12,12}; const auto nextToken=p.begin(next,records,ids); assert(nextToken);
        assert(!p.capture(token,drawn)); //Stale asynchronous observation poisons new invocation.
        assert(!p.finish(nextToken,next,records,ids,true).valid);
    }
    {
        Policy p; auto all=std::vector<std::uintptr_t>(ids.begin(),ids.end());
        const auto token=p.begin(stamp,records,ids); assert(p.capture(token,all));
        const auto result=p.finish(token,stamp,records,ids,true);
        assert(result.valid && !result.omitted); //All selected nodes admitted.
    }
}
std::map<std::uintptr_t,std::vector<unsigned char>> memory;
template<class T> void put(std::uintptr_t p,T value) {
    auto& bytes=memory[p]; bytes.resize(sizeof(value)); std::memcpy(bytes.data(),&value,sizeof(value));
}
bool read(std::uintptr_t p,void* out,std::size_t size) {
    const auto it=memory.find(p);
    if (it==memory.end() || it->second.size()!=size) return false;
    std::memcpy(out,it->second.data(),size); return true;
}
void readerTests() {
    constexpr std::uintptr_t base=0x180000000,complete=0x50000,incoming=complete+8,list=0x80000,cells=0xa0000;
    const auto records=registry();
    put(base+0x13DCFE8,complete); put(complete,base+0x10CE680); put(incoming,base+0x10CE6A8);
    put(incoming+0x138B8,static_cast<int>(records.size()));
    for (std::size_t i=0;i<records.size();++i) put(incoming+0x28+i*0x10,records[i]);
    std::vector<Policy::Record> got;
    assert(readOwnedHudRegistry(base,incoming,read,got) && got==records);
    assert(!readOwnedHudRegistry(base,complete,read,got) && got.empty());
    put(incoming,base+0x10CE680);
    assert(!readOwnedHudRegistry(base,incoming,read,got));
    put(incoming,base+0x10CE6A8);
    int countReads=0;
    auto changedCount=[&](std::uintptr_t p,void* out,std::size_t size) {
        if (p==incoming+0x138B8 && ++countReads==2) return false;
        return read(p,out,size);
    };
    assert(!readOwnedHudRegistry(base,incoming,changedCount,got));
    for (int invalidCount:{-1,0,4999,5000}) {
        put(incoming+0x138B8,invalidCount);
        assert(!readOwnedHudRegistry(base,incoming,read,got));
    }
    const auto drawn=admitted();
    for (std::size_t i=0;i<drawn.size();++i) { put(list+i*8,cells+i*8); put(cells+i*8,drawn[i]); }
    std::vector<std::uintptr_t> result;
    assert(readOwnedHudAdmitted(list,static_cast<int>(drawn.size()),read,result) && result==drawn);
    put(list,std::uintptr_t{0});
    assert(!readOwnedHudAdmitted(list,static_cast<int>(drawn.size()),read,result) && result.empty());
    assert(!readOwnedHudAdmitted(std::numeric_limits<std::uintptr_t>::max()-3,2,read,result));
    assert(!readOwnedHudAdmitted(list,4999,read,result));
}
}
int main() { policyTests(); readerTests(); std::cout<<"HUD admission policy and guarded native reads passed\n"; }
