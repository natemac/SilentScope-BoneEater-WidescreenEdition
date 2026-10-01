#include "render/wide_battle_backing.h"
#include <cassert>
#include <limits>
using namespace bone_eater::render;
namespace {
bool near(double a,double b) {return std::fabs(a-b)<.001;}
const std::array<RearIlluminationPose,2> source {{{{0,-43},{1,1}},{{400,-43},{1,1}}}};
WideBattleBackingPlan plan() { WideBattleBackingPlan p; assert(planWideBattleBacking({},source,p)); return p; }
const OwnedHudBatchKey batch {10,20,30};
WideBattleBackingFrame submitted() {
    const auto p=plan(); WideBattleBackingFrame f; f.begin(1,2,p,true);
    assert(f.submitted(1,0,batch,0,p.poses[0],true,true));
    assert(f.submitted(1,1,batch,1,p.poses[1],true,true)); return f;
}
}
int main() {
    auto p=plan();
    assert(near(p.poses[0].position[0]-.1,0));
    assert(near(p.poses[1].position[0]-.1,960));
    assert(near(p.poses[1].position[0]-.1+400*p.poses[1].scale[0],1920));
    assert(near(p.poses[0].position[1]-.1,1366./1367.*(-43+.4)));
    assert(near(p.poses[0].scale[1],1366./1367.));
    // Native parent animation remains affine; authored dimensions, UVs and
    // alpha are outside this API and cannot accidentally be rewritten.
    auto animated=source; for(auto& v:animated) {v.position[0]-=800;v.position[1]-=400;}
    WideBattleBackingPlan hidden; assert(planWideBattleBacking({},animated,hidden));
    assert(near(p.poses[0].position[0]-hidden.poses[0].position[0],1920));
    assert(near(p.poses[0].position[1]-hidden.poses[0].position[1],400*1366./1367.));
    auto unchanged=p;
    assert(!planWideBattleBacking({},p.poses,p)); // No double application.
    assert(p.valid && p.poses[0].position==unchanged.poses[0].position);
    auto bad=source; bad[1].position[0]+=1; assert(!planWideBattleBacking({},bad,p));
    bad=source; bad[0].position[0]=std::numeric_limits<float>::quiet_NaN(); assert(!planWideBattleBacking({},bad,p));
    WideBattleBackingLayout wrong; wrong.width=1280; assert(!planWideBattleBacking(wrong,source,p));
    wrong={}; wrong.right=2000; assert(!planWideBattleBacking(wrong,source,p));
    auto f=submitted(); assert(!f.ready(1,2,{}));
    f.consumed(1,batch,batch,2,true); assert(f.ready(1,2,{}));
    assert((f.groups(1,2,{})==std::array<bool,3>{true,true,true}));
    assert(!f.ready(2,2,{})); assert(!f.ready(1,3,{}));
    wrong={};wrong.frontScale=.9f;assert(!f.ready(1,2,wrong));
    // Incomplete or reset source pair must never authorize a relocated group.
    f.begin(1,2,plan(),true); assert(f.submitted(1,0,batch,0,plan().poses[0],true,true));
    f.consumed(1,batch,batch,2,true);assert(!f.ready(1,2,{}));
    f=submitted();f.consumed(1,batch,{10,21,30},2,true);assert(!f.ready(1,2,{}));
    f=submitted();f.consumed(1,batch,batch,1,true);assert(!f.ready(1,2,{}));
    f=submitted();f.consumed(1,batch,batch,2,false);assert(!f.ready(1,2,{}));
    f.begin(1,2,plan(),true);assert(!f.submitted(1,0,batch,0,plan().poses[0],true,false));
    assert(!f.submitted(1,1,batch,1,plan().poses[1],true,true));
    f.begin(1,2,plan(),true);assert(f.submitted(1,0,batch,0,plan().poses[0],true,true));
    assert(!f.submitted(1,1,batch,0,plan().poses[1],true,true));
    f.begin(1,2,plan(),false);assert(!f.submitted(1,0,batch,0,plan().poses[0],true,true));
}
