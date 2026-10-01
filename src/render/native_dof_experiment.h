#pragma once
#include <cstdint>

namespace bone_eater::render {
// Requires the shared35E800 callback observer. Default-off, startup-only
// BONE_EATER_DOF_MAIN_OFF=1. Two local-register decisions, no native data writes.
void installNativeDofExperiment(void* module) noexcept;
void beginNativeDofExperiment(void* bloom, void* descriptor, std::uintptr_t caller) noexcept;
// MUST run in the shared callback's SEH finally, including native exceptions.
void endNativeDofExperiment(bool nativeCompleted) noexcept;
// A nested native callback is transparent even if its caller is unexpected.
// The outer certificate is restored only by the shared hook's SEH finally.
std::uint64_t suspendNativeDofExperiment() noexcept;
void resumeNativeDofExperiment(std::uint64_t active) noexcept;
}
