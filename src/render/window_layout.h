#pragma once

namespace bone_eater::render {

// Diagnostic desktop arrangement only. Does not change render targets,
// projection, input coordinates, visibility, z-order, or foreground focus.
// Enabled only by diagnostic/wide/wide-monitor before process startup. The
// opt-in wide-monitor mode fits the main window within the whole monitor;
// it does not change display mode, window styles, z-order or auxiliary policy.
bool diagnosticWindowLayoutEnabled() noexcept;

// Call after sampled texture metadata is refreshed, not on every draw.
// role is "main", "multidisplay0", or "multidisplay1". Dimensions must come
// from the actual backbuffer texture, not the window or saved cabinet layout.
void observeDiagnosticWindow(void* window, const char* role,
                             unsigned int textureWidth,
                             unsigned int textureHeight) noexcept;

// Existing game MoveWindow/SetWindowPos hooks use cached placements to prevent
// cabinet coordinates from undoing the diagnostic layout. No placement is
// returned before the main output and this output have been observed.
bool diagnosticWindowPlacement(void* window, int& x, int& y,
                                int& width, int& height) noexcept;

} // namespace bone_eater::render
