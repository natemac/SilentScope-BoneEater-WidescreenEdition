#pragma once

#include <cstdint>

namespace bone_eater::render {

// Returns the static string "verified" only for the exact supported gamendd.dll
// file hash and x64 PE image. Validation is cached for the current module base;
// later calls check the loaded module identity and readable PE header. This does
// not pin the module or validate a hook ABI/prologue/caller on its caller's behalf.
// Requires bcrypt.lib. No game function calls or memory writes occur.
const char* verifyNativeGameModule(void* module) noexcept;

struct NativeViewportSnapshot {
    bool valid = false;
    const char* status = "unresolved";
    std::uintptr_t moduleBase = 0;
    std::uintptr_t windowObject = 0;
    std::uintptr_t frameBufferObject = 0;
    void* window = nullptr;
    std::int32_t clientWidth = 0;
    std::int32_t clientHeight = 0;
    std::int32_t osClientWidth = 0;
    std::int32_t osClientHeight = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::uint32_t logicalWidth = 0;
    std::uint32_t logicalHeight = 0;
    std::uint32_t resourceWidth = 0;
    std::uint32_t resourceHeight = 0;
};

// Snapshot the native main-window fit rectangle and FrameBuffer dimensions.
// expectedMainWindow must be the registered ASKA HWND. Safe self-process reads,
// exact-module/FrameBuffer-vftable gates, HWND/client checks, and repeated critical
// fields reject unresolved or concurrently changing geometry. Coordinates are
// native CLIENT pixels; map to backbuffer using the actual osClient dimensions.
// Native WM_SIZE dimensions can differ from the actual HWND client dimensions.
// This API neither interprets cabinet calibration nor transforms input/aim.
NativeViewportSnapshot readNativeMainViewport(void* expectedMainWindow) noexcept;

// Call only when diagnostic recording is requested. Throttled, shared-read CSV.
void observeNativeMainViewport(void* expectedMainWindow) noexcept;

} // namespace bone_eater::render
