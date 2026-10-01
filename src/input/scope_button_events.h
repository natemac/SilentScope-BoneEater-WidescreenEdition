#pragma once
#include "input/scope_control.h"
#include <vector>
#include <windows.h>

namespace bone_eater::input {
void configureScopeButtonEvents(const std::vector<int>& keys) noexcept;
// Call only from the already-verified active main-window subclass. Observation
// never consumes the Windows message; native menu/trigger handling continues.
void observeScopeButtonMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Legacy mouse/keyboard path only. Root supplies verified scene/source/focus
// context; request.buttonsHeld is replaced by the event stream's held state.
ScopeControlSnapshot consumeScopeButtonEvents(const ScopeControlInput& request) noexcept;
void resetScopeButtonEvents() noexcept;
} // namespace bone_eater::input
