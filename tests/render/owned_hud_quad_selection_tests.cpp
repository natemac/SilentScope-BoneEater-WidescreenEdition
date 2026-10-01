#include "render/owned_hud_quad_selection.h"
#include <cassert>
#include <iostream>
using namespace bone_eater::render;
constexpr OwnedHudBatchKey batch {1,2,3};
constexpr std::array<bool,3> backing {true,true,true};
void all(OwnedHudQuadSelection& s, std::size_t omit=ownedHudNodeCount) {
    s.begin(1);
    for (std::size_t i=0;i<ownedHudNodeCount;++i)
        if (i!=omit) assert(s.observe(1,i,batch,static_cast<unsigned>(i*2),1));
}
int main() {
    OwnedHudQuadSelection s;
    all(s);
    s.seal(1,true,true,backing,true);
    assert(!s.ready(OwnedHudGroup::Left)); // Complete batch preflight required.
    s.validateBatch(batch,batch,120,true);
    assert(s.ready(OwnedHudGroup::Left) && s.ready(OwnedHudGroup::Right) && s.ready(OwnedHudGroup::Center));
    auto p=s.plan(1,batch,120);
    assert(p.replacement && p.count==ownedHudNodeCount*2);
    unsigned indices=0;
    for (std::size_t i=0;i<p.count;++i) {
        assert(p.ranges[i].firstIndex==indices);
        indices+=p.ranges[i].indexCount;
    }
    assert(indices==720); // Exact partition: nothing duplicated or omitted.
    assert(!s.plan(2,batch,120).replacement);
    assert(!s.plan(1,{1,3,3},120).replacement);

    all(s,19); // Unlimited-ammo sibling missing invalidates whole left group.
    s.seal(1,true,true,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left) && s.ready(OwnedHudGroup::Right));
    for (const auto& r:s.plan(1,batch,120).ranges) assert(r.indexCount==0 || r.destination!=OwnedHudGroup::Left);

    all(s);
    assert(!s.observe(1,0,batch,95,1)); // Duplicate submission is not double counted.
    s.seal(1,true,true,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left));

    s.begin(1);
    for (std::size_t i=0;i<ownedHudNodeCount;++i)
        assert(s.observe(1,i,batch,i==23 ? 0 : static_cast<unsigned>(i),1));
    s.seal(1,true,true,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left) && !s.ready(OwnedHudGroup::Right));

    all(s); s.seal(1,true,true,backing,false); s.validateBatch(batch,batch,120,true);
    assert(s.ready(OwnedHudGroup::Left) && !s.ready(OwnedHudGroup::Center));
    all(s); s.seal(1,true,true,{false,true,true},true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left) && s.ready(OwnedHudGroup::Center));

    all(s); s.seal(1,true,true,backing,true); s.validateBatch(batch,{1,4,3},120,true);
    assert(!s.plan(1,batch,120).replacement); // Material reset fails entire frame selection.
    all(s); s.seal(1,true,true,backing,true); s.validateBatch(batch,batch,120,false);
    assert(!s.plan(1,batch,120).replacement); // Arbitrary IB must not use ordinal*6.
    all(s); s.seal(1,true,false,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.plan(1,batch,120).replacement);

    s.begin(2);
    assert(!s.observe(1,0,batch,0,1));
    s.seal(2,true,true,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left));
    s.begin(3);
    assert(!s.observe(3,0,batch,0,0)); // Hidden appearance is not no-submit proof.
    s.begin(3);
    for (std::size_t i=0;i<ownedHudNodeCount;++i) assert(s.observe(3,i,{},0,0,true));
    s.seal(3,true,true,backing,true);
    assert(s.ready(OwnedHudGroup::Center) && !s.plan(3,batch,120).replacement);

    s.begin(4);
    assert(!s.observe(4,0,batch,OwnedHudQuadSelection::maxQuads,1));
    s.begin(4);
    assert(!s.observe(4,0,batch,0,2)); // Image record append must be single quad.
    s.begin(4);
    assert(s.observe(4,46,batch,20,8)); // Separately certified text could emit a span.

    // One semantic group spans two material buffers. Never replace the first
    // before verifying the other; a reset in the other invalidates both.
    s.begin(5);
    constexpr OwnedHudBatchKey other {4,5,6};
    for (std::size_t i=0;i<ownedHudNodeCount;++i)
        assert(s.observe(5,i,i==1 ? other : batch,static_cast<unsigned>(i),1));
    s.seal(5,true,true,backing,true); s.validateBatch(batch,batch,120,true);
    assert(!s.ready(OwnedHudGroup::Left));
    s.validateBatch(other,{4,6,6},120,true);
    assert(!s.ready(OwnedHudGroup::Left));
    std::cout << "owned HUD quad selection tests passed\n";
}
