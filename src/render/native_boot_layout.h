#pragma once
namespace bone_eater::render {
// Centered by default. BONE_EATER_BOOT_CENTER=0 disables; observe is read-only.
// Install before game entry, including creation of the widened font canvas.
// Only the pinned nddDiagnosisMode's synchronous font calls are eligible.
void installNativeBootLayout(void* gameModule, void* arkModule) noexcept;
}
