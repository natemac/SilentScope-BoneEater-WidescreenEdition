#pragma once

struct IDXGISwapChain;

namespace bone_eater::render {

// Register creation-time titles from the game's CreateWindowEx hooks. Present
// uses this cached identity without synchronously messaging the window thread.
// Re-registering a recycled HWND replaces/clears any previous slot identity.
void registerD3D11Window(void* window, const char* title) noexcept;
void registerD3D11Window(void* window, const wchar_t* title) noexcept;
// Stable literal slot name; no title messaging or string allocation.
const char* registeredD3D11WindowRole(void* window) noexcept;

// Observation only: does not change graphics state, window placement, input,
// Present arguments, or frame-pacing settings. knownMainWindow is NDD_MAIN_WINDOW
// (a HWND, passed as void* to keep this interface independent of Windows headers).
// Records metadata changes and five-second callback-rate summaries in
// desktop/graphics.csv beneath the game's working directory. Counts describe
// Present hook invocations, not confirmed displayed frames.
// Set BONE_EATER_GRAPHICS_TELEMETRY=0 before launch to disable this observer.
// Set BONE_EATER_CAPTURE=1 for diagnostic BMP copies of each game's backbuffer
// in desktop/captures. Starts after 30 callbacks, at most once per five seconds.
// Capture is disabled by default and is not a compositor implementation.
// Opt-in readback may stall the GPU and is intended only for visual diagnosis.
void observeD3D11Present(IDXGISwapChain* swapchain, void* knownMainWindow,
                         unsigned int syncInterval, unsigned int presentFlags,
                         const char* presentApi) noexcept;

// Called only after a FAILED native Present/Present1. Synchronously appends and
// flushes at most 32 failure records per process to desktop/present-failures-v1.log
// before the game's native caller can exit on its HRESULT. Queries only; never
// replaces the result, resets the device, captures pixels, or retains COM state.
void observeD3D11PresentFailure(IDXGISwapChain* swapchain, void* knownMainWindow,
                               unsigned int syncInterval, unsigned int presentFlags,
                               const char* presentApi, long result) noexcept;

}  // namespace bone_eater::render
