#include "render/native_dof_decision.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using bone_eater::render::DofDecision;
extern "C" {
void dof_test_resume();
__declspec(thread) DofDecision bone_eater_dof_decision {};
void* bone_eater_dof_setup_resume = reinterpret_cast<void*>(&dof_test_resume);
void* bone_eater_dof_combine_resume = reinterpret_cast<void*>(&dof_test_resume);
}
struct RegisterState {
    std::array<std::uint64_t, 15> gpr;
    std::uint64_t flags;
    std::array<std::array<std::uint64_t, 2>, 16> xmm;
    std::uint64_t beforeRsp, afterRsp;
    std::uint32_t mxcsr;
};
static_assert(offsetof(RegisterState, flags) == 120 && offsetof(RegisterState, xmm) == 128);
static_assert(offsetof(RegisterState, beforeRsp) == 384 && offsetof(RegisterState, mxcsr) == 400);
extern "C" void dof_test_setup(const RegisterState*, RegisterState*);
extern "C" void dof_test_combine(const RegisterState*, RegisterState*);
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
constexpr unsigned rax = 0, rcx = 1, rsi = 5, rdi = 6, r12 = 11, r13 = 12;
constexpr std::uint64_t testedFlags = 0xCD5; // OF, DF, SF, ZF, AF, PF, CF.

RegisterState state(unsigned seed, unsigned bits) {
    RegisterState s {};
    for (unsigned i = 0; i < s.gpr.size(); ++i) s.gpr[i] = 0xABCDEF9000000000ULL + seed * 0x10000ULL + i * 0x123ULL;
    for (unsigned i = 0; i < s.xmm.size(); ++i) s.xmm[i] = {0x1122334400000000ULL + seed + i, 0xFFEEDDCCBBAA0000ULL + seed * 19 + i};
    s.flags = 0x202 | (bits & testedFlags);
    s.mxcsr = 0x1F80;
    return s;
}
DofDecision certificate(const RegisterState& s, bool setup) {
    DofDecision d;
    d.active = 1;
    d.camera = s.gpr[setup ? rax : rcx];
    d.parameter = s.gpr[setup ? r12 : rax];
    d.processor = s.gpr[rdi];
    d.combiner = s.gpr[rsi];
    return d;
}
void execute(bool setup, const RegisterState& s, bool match) {
    const auto before = bone_eater_dof_decision;
    RegisterState result {};
    if (setup) dof_test_setup(&s, &result); else dof_test_combine(&s, &result);
    auto expected = s.gpr;
    if (match) expected[setup ? r13 : r12] &= ~std::uint64_t(8);
    CHECK(result.gpr == expected);
    CHECK(result.xmm == s.xmm);
    CHECK((result.flags & testedFlags) == (s.flags & testedFlags));
    CHECK(result.beforeRsp == result.afterRsp);
    CHECK(result.mxcsr == s.mxcsr);
    const auto& d = bone_eater_dof_decision;
    CHECK(d.active == before.active && d.camera == before.camera && d.parameter == before.parameter);
    CHECK(d.processor == before.processor && d.combiner == before.combiner);
    const bool change = match && (s.gpr[setup ? r13 : r12] & 8);
    CHECK(d.setupVisits == before.setupVisits + (match && setup));
    CHECK(d.combineVisits == before.combineVisits + (match && !setup));
    CHECK(d.setupChanged == before.setupChanged + (change && setup));
    CHECK(d.combineChanged == before.combineChanged + (change && !setup));
    CHECK(d.setupFlags == (match && setup ? s.gpr[r13] & 255 : before.setupFlags));
    CHECK(d.combineFlags == (match && !setup ? s.gpr[r12] & 255 : before.combineFlags));
}
int main() {
    unsigned groups = 0;
    for (bool setup : {true, false}) {
        auto s = state(1, testedFlags);
        s.gpr[setup ? r13 : r12] |= 8;
        for (auto active : {0ULL, 2ULL, ~0ULL}) {
            bone_eater_dof_decision = certificate(s, setup);
            bone_eater_dof_decision.active = active;
            execute(setup, s, false);
        }
        ++groups;
        for (unsigned field = 0; field < 3; ++field) {
            bone_eater_dof_decision = certificate(s, setup);
            if (field == 0) ++bone_eater_dof_decision.camera;
            if (field == 1) ++bone_eater_dof_decision.parameter;
            if (field == 2 && setup) ++bone_eater_dof_decision.processor;
            if (field == 2 && !setup) ++bone_eater_dof_decision.combiner;
            execute(setup, s, false);
        }
        ++groups;
        // Every low-byte combination, status-flag pattern and XMM/GPR sentinel.
        for (unsigned value = 0; value < 256; ++value) {
            s = state(value + 2, value * 0x117);
            s.gpr[setup ? r13 : r12] = (s.gpr[setup ? r13 : r12] & ~255ULL) | value;
            bone_eater_dof_decision = certificate(s, setup);
            execute(setup, s, true);
        }
        ++groups;
        // The certificate names a real parameter block. No byte in it changes.
        std::array<unsigned char, 128> parameter;
        for (unsigned i = 0; i < parameter.size(); ++i) parameter[i] = static_cast<unsigned char>(i);
        const auto before = parameter;
        s = state(400, testedFlags);
        s.gpr[setup ? r12 : rax] = reinterpret_cast<std::uintptr_t>(parameter.data());
        bone_eater_dof_decision = certificate(s, setup);
        execute(setup, s, true);
        CHECK(parameter == before);
        ++groups;
    }
    const auto mainCertificate = bone_eater_dof_decision;
    std::atomic<unsigned> ready {0};
    auto worker = [&](unsigned seed, bool setup) {
        CHECK(bone_eater_dof_decision.active == 0);
        auto s = state(seed, testedFlags);
        s.gpr[setup ? r13 : r12] |= 8;
        bone_eater_dof_decision = certificate(s, setup);
        ++ready;
        while (ready.load() != 2) std::this_thread::yield();
        for (unsigned i = 0; i < 10000; ++i) execute(setup, s, true);
        CHECK((setup ? bone_eater_dof_decision.setupVisits : bone_eater_dof_decision.combineVisits) == 10000);
        CHECK((setup ? bone_eater_dof_decision.combineVisits : bone_eater_dof_decision.setupVisits) == 0);
    };
    std::thread first(worker, 711, true), second(worker, 933, false);
    first.join(); second.join();
    CHECK(std::memcmp(&bone_eater_dof_decision, &mainCertificate, sizeof(mainCertificate)) == 0);
    ++groups;
    std::printf("%u register/flags/stack/TLS fixture groups passed\n", groups);
}
