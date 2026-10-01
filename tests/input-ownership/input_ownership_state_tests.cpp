#include "input/input_ownership_state.h"
#include <iostream>
#include <stdexcept>
using namespace bone_eater::input;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
InputOwnershipState ready() {
    InputOwnershipState state;
    const auto id = state.begin(99, true);
    state.conversion(id, {{960,540}}, true);
    state.finish(id, true, true, {{960,540}}, 1000);
    return state;
}
int main() {
    unsigned groups = 0;
    auto state = ready(); state.scopeInvocation();
    CHECK(state.match(99, {{960,540}}, 1100) == OwnershipMatch::Completed); ++groups;
    CHECK(state.match(99, {{960,540}}, 1101) == OwnershipMatch::Stale);
    CHECK(state.match(99, {{960,540}}, 999) == OwnershipMatch::Stale); ++groups;
    CHECK(state.match(100, {{960,540}}, 1000) == OwnershipMatch::WrongInput);
    CHECK(state.match(99, {{959,540}}, 1000) == OwnershipMatch::ChangedPoint); ++groups;
    state.scopeInvocation(); CHECK(state.match(99, {{960,540}}, 1000) == OwnershipMatch::RepeatedScope); ++groups;
    state = ready(); auto id = state.begin(99,true);
    CHECK(!state.completed && state.match(99,{{960,540}},1000) == OwnershipMatch::Updating);
    state.finish(id,true,true,{{960,540}},1001); CHECK(!state.completed); ++groups;
    state = {}; id = state.begin(99,true); state.conversion(id,{{960,540}},false);
    state.finish(id,true,true,{{960,540}},1000); CHECK(!state.completed); ++groups;
    for (const bool returned : {false,true}) {
        state = {}; id = state.begin(99,true); state.conversion(id,{{960,540}},true);
        state.finish(id,returned,!returned,{{960,540}},1000); CHECK(!state.completed);
    } ++groups;
    state = {}; id = state.begin(99,true); state.conversion(id,{{960,540}},true);
    state.conversion(id,{{960,540}},true); state.finish(id,true,true,{{960,540}},1000);
    CHECK(!state.completed && state.conversions == 2); ++groups;
    state = {}; id = state.begin(99,true); state.conversion(id,{{0.0f,540}},true);
    state.finish(id,true,true,{{-0.0f,540}},1000); CHECK(!state.completed); ++groups;
    for (const OwnershipPoint point : {OwnershipPoint{{NAN,10}}, OwnershipPoint{{10,INFINITY}},
            OwnershipPoint{{1921,20}}, OwnershipPoint{{10,-1}}}) {
        state = {}; id = state.begin(99,true); state.conversion(id,point,true);
        state.finish(id,true,true,point,1000); CHECK(!state.completed);
    } ++groups;
    state = {}; const auto outer = state.begin(99,true); state.conversion(outer,{{960,540}},true);
    const auto inner = state.begin(99,true); state.conversion(inner,{{960,540}},true);
    state.finish(inner,true,true,{{960,540}},1000); CHECK(!state.completed && state.updating);
    const auto another = state.begin(99,true); state.conversion(another,{{960,540}},true);
    state.finish(another,true,true,{{960,540}},1000); CHECK(!state.completed);
    state.finish(outer,true,true,{{960,540}},1000); CHECK(!state.completed && !state.updating);
    id = state.begin(99,true); state.conversion(id,{{960,540}},true);
    state.finish(id,true,true,{{960,540}},1000); CHECK(state.completed); ++groups;
    state = ready(); id = state.begin(99,false); state.conversion(id,{{960,540}},true);
    state.finish(id,true,true,{{960,540}},1000); CHECK(!state.completed); ++groups;
    state = ready(); state.serial = UINT64_MAX; CHECK(state.begin(99,true) == 0 && !state.completed);
    state.conversion(1,{{960,540}},true); CHECK(!state.completed); ++groups;
    std::cout << groups << " ownership state groups passed\n";
}
