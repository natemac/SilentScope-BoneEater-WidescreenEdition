#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace bone_eater::render {

using NativeWindowShutdown = void (*)();

// Call immediately after the source wrapper creates and records the NDD main
// ASKA window, on that window's owner thread. WM_CLOSE and Escape are intercepted;
// the callback is queued once away from window dispatch. The callback must
// remain valid for the process lifetime (production uses launcher::shutdown).
// No termination, keyboard hook or auxiliary-window policy is added here.
bool installNativeMainWindowExit(HWND window, NativeWindowShutdown shutdown) noexcept;

} // namespace bone_eater::render
