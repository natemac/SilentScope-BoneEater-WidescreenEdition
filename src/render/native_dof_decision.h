#pragma once
#include <cstddef>
#include <cstdint>

namespace bone_eater::render {
// Offsets are consumed by native_dof_stubs.asm. This is OUR per-thread memory;
// the stubs never store into camera parameters or native renderer objects.
struct DofDecision {
    std::uint64_t active = 0;
    std::uintptr_t camera = 0, parameter = 0, processor = 0, combiner = 0;
    std::uint64_t setupVisits = 0, combineVisits = 0, setupChanged = 0, combineChanged = 0;
    std::uint64_t setupFlags = 0, combineFlags = 0;
};
static_assert(sizeof(DofDecision) == 88);
static_assert(offsetof(DofDecision, active) == 0 && offsetof(DofDecision, camera) == 8);
static_assert(offsetof(DofDecision, parameter) == 16 && offsetof(DofDecision, processor) == 24);
static_assert(offsetof(DofDecision, combiner) == 32 && offsetof(DofDecision, setupVisits) == 40);
static_assert(offsetof(DofDecision, combineVisits) == 48 && offsetof(DofDecision, setupChanged) == 56);
static_assert(offsetof(DofDecision, combineChanged) == 64 && offsetof(DofDecision, setupFlags) == 72);
static_assert(offsetof(DofDecision, combineFlags) == 80);
}

extern "C" {
extern __declspec(thread) bone_eater::render::DofDecision bone_eater_dof_decision;
extern void* bone_eater_dof_setup_resume;
extern void* bone_eater_dof_combine_resume;
void bone_eater_dof_setup_stub();
void bone_eater_dof_combine_stub();
}
