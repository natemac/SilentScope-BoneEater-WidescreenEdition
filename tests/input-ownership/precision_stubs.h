#pragma once
#include "input/native_precision_bypass.h"
namespace precision_fixture {
inline bool ready=false;
inline unsigned enters=0,leaves=0;
}
namespace bone_eater::input {
bool nativePrecisionRequested() noexcept { return false; }
bool prepareNativePrecisionBypass(void*) noexcept { return false; }
void nativePrecisionScopeInstalled(void*) noexcept {}
void nativePrecisionAimInstalled(void*) noexcept {}
bool nativePrecisionReady() noexcept { return precision_fixture::ready; }
void recordNativePrecisionDomain(std::uint64_t,std::uintptr_t,const render::NativeHudGeometry&) noexcept {}
NativePrecisionScopeToken beginNativePrecisionScope(std::uintptr_t,std::uintptr_t,float,const InputOwnershipState&,bool) noexcept {
    ++precision_fixture::enters; return {};
}
void finishNativePrecisionScope(NativePrecisionScopeToken,bool) noexcept { ++precision_fixture::leaves; }
void observeNativePrecisionRay(std::uintptr_t,float,float,bool,bool,bool) noexcept {}
}
