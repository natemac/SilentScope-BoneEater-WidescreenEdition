# D3D11 output observation and compositor integration

<!-- current-update:start -->
> Current update: **DE97 installed: HUD seam/name and top-five ranking corrections plus ranking notice cleanup; 28 rendering suites pass. Beta readiness work continues; aiming and full-flow checks remain open**.
> See the [current update checkpoint](../../CURRENT_UPDATE.md) for the installed build,
> verified results and next work. Older observations below remain historical evidence.
<!-- current-update:end -->

Current source increment: `owned_hud_admission.h` provides tested provisional
admission evidence and guarded native readers. It is now wired to the opt-in runtime
observer and live-verified after correcting native font-reset ordering. See [implementation details](../../review/HUD_ADMISSION_IMPLEMENTATION.md).
The full render regression passes 23/23; wider rendering remains disabled.

Current integration checkpoint (26 September evening): native main resources
are 1920x1080, with optional native vertical-FOV preservation. `-2Display`
uses the original multiply blend to combine the front LCD and scene. Optional
front/rear fitting preserves the portrait menus together inside the wide canvas;
HUD reflow and direct cinematic artwork are not finished.

The compositor now follows the exact final native ScopeCamera ray coordinates,
maps them through the measured native fit/OS client/backbuffer geometry, and
gates on script Scope Off plus fresh input, native ray and source texture.
`desktop/scope-v3.csv` records these distinct coordinate domains. The original
raw-intent placement described in the historical prototype section below has
been replaced. The final ray is a centered screen-plane construction transformed
by the native camera, not a general inverse of arbitrary projection changes.

`--desktop-reticle` adds a small GPU crosshair while scope is released. Scope
hold replaces it with the real circular scope image. Initial live draws and
scoped target hits passed; exact BattleUI suppression now removes the large
red aiming circle and lines in gameplay. Edge/ruler artwork remains. The ready callback means a recent successful draw
submission, not a guarantee for the next frame. Every rejection leaves native
presentation available.

`--native-input-desktop` removes verified ARK viewport margins after native
device calibration and separately inverse-maps menu input. A five-point live
test reached all four exact logical corners; center error was below0.5logical
pixel from native quantization. This establishes coordinate propagation, not
physical gun accuracy. See `review/desktop-aim-30076.json`.

`--park-auxiliary` is an optional monitored offscreen-window experiment with
automatic rollback on occlusion/cadence failure. It is default off. PID33328
parked successfully through menus, then restored when gameplay dropped from
about54 to15 callbacks/sec. The slowdown's cause is not isolated; this is not
yet an accepted single-window mode. Read `review/AUXILIARY_WINDOWS_FINDINGS.md` before enabling.
Composition alone does not mean the auxiliary window can safely stop rendering.

`d3d11_diagnostics.cpp` observes each non-test `Present` / `Present1` callback
before the upstream debug overlay. Add it to the standalone target, then apply
`patches/d3d11-diagnostics.patch` to the pinned source checkout. The patch is
guarded by `BONE_EATER_STANDALONE` and does not alter an upstream baseline build.

## What observation establishes

The current screenshots show **ASKA changing from the white attention screen to
3D gameplay**. White pixels are content, not evidence of a rendering failure.
Auxiliary windows are classified only by their complete title index; the title
`Aska MultiDisplay[0](multipssID:33)` is `multidisplay0`, and index 1 is
`multidisplay1`. Those names are output slots, not declarations that a slot is
the scope. Verify the content of both during scope use before assigning a role.

`desktop/graphics.csv` records:

- HWND, title, main-window reference, and swapchain address;
- DXGI descriptor size and actual backbuffer texture size separately from the
  window's client size;
- texture format and sample count;
- D3D11 device address, adapter LUID/name, and observed Present thread;
- callback counts and intervals, with a summary approximately every 5 seconds.

Counts represent hook callbacks, not successful presents, screen refreshes, or
GPU execution times. Metadata is sampled at most once per second unless the
swapchain descriptor changes. A game that invokes a swapchain from different
threads can have more threads than the sampled `present_thread` reports.
Addresses are meaningful only within one process/session; a destroyed chain's
address can be reused. No COM objects or backbuffers are retained by telemetry.

Set `BONE_EATER_GRAPHICS_TELEMETRY=0` before starting the game to disable CSV
recording and BMP capture. Window arrangement and separately enabled camera
sampling remain independent. Set `BONE_EATER_CAPTURE=1` to opt into CPU-readback BMP diagnostics;
the first capture follows 30 callbacks, then each chain is captured at most once
every 5 seconds. Files are atomically replaced under `desktop/captures` and named
from output slot and HWND. Metadata failures and unsupported formats are recorded
as capture events. Captures can stall the GPU and are not a shipping compositor.
This option also requires telemetry to remain enabled.

## Diagnostic window arrangement

`window_layout.cpp` is enabled only by `BONE_EATER_LAYOUT=diagnostic`. Add the
source to the standalone target and apply `patches/window-layout.patch` to the
vendor checkout. After refreshing metadata and classifying a window, call:

```cpp
observeDiagnosticWindow(observed.window, roleName(observed.role),
    observed.textureWidth, observed.textureHeight);
```

The main ASKA window fits in the left 60% of its initially observed monitor's
work area. Auxiliary slots 0 and 1 fit in the right column, stacked top/bottom.
Each client area preserves the measured texture aspect to pixel-rounding
accuracy. Nonclient frame dimensions and DPI are accounted for. The layout does
not activate, minimize, hide, or reorder windows. It leaves an already-minimized
window alone. Placement changes run through a short in-process work item, so
the render callback cannot synchronously enter game window-resize handlers.
The cached geometry also overrides subsequent game cabinet-placement requests.

Metadata-driven layout checks are throttled to approximately one second, except
when a new output or changed texture size requires an update. Matching windows
produce no placement calls. The standalone patch disables the upstream generic
800x480 coercion of the last-created NDD auxiliary; the upstream baseline build
retains its original behavior. This is an inspection layout, not 16:9 camera
conversion or scope compositing.

The layout code never calls `ResizeBuffers` or changes projection. Native client
resizing still produces normal Windows resize notifications, which the game may
handle. Verify actual texture dimensions in CSV after arranging the windows
before claiming that the game's render-target sizes remained unchanged.

## Opt-in scope compositor prototype

The captured runtime session established that MultiDisplay[0] is the actual
800x480 scope image and MultiDisplay[1] is the 768x1366 portrait UI. All three
outputs used one D3D11 device and Present thread. `scope_compositor.cpp` implements
that measured path, enabled only by `BONE_EATER_SCOPE_OVERLAY=1`. Add its source
to the standalone target and apply `patches/scope-compositor.patch` after the
diagnostics patch. The window-registration patch is also required.

The pass copies the scope output on the GPU and draws a circular overlay at the
shared cabinet aim sample while scope intent is held. Its source is the centered
480x480 square of the 800x480 texture: UV x=0.2..0.8, y=0..1. Both image axes are
scaled equally and the optical center remains at UV (0.5,0.5). It does not stretch
the full landscape image into a circle. Overlay diameter is initially 36% of the
main backbuffer height; its center stays at the exact aim, with border clipping.
Original windows remain visible.

The independent pass uses D3D11.1 context-state swapping to restore graphics
pipeline state; it inherits the device's required single-threaded creation flag.
This follows Microsoft's [context-state creation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-createdevicecontextstate)
and [state-swap](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate)
contracts. Pipeline isolation does not synchronize the engine's other threads or
isolate active asynchronous queries. The prototype relies on the measured common
render thread; its mutex protects compositor bookkeeping only.

Input and source frames older than 250 ms suppress drawing. Unrecognized source
dimensions/formats, different devices, unavailable context-state support, and
partial/scroll Present1 calls do not draw. `desktop/scope.csv` reports the reason
and GPU copy/draw submission counters. Those counters do not establish successful
screen presentation or native optical activation. Observe active gameplay and
scope hold/release before claiming a working magnified view. The shader clips
some original scope HUD content outside its circular crop; HUD integration is a
separate task. No camera aspect or field of view changes are implemented here.

## Further compositor integration

First use a gameplay capture to identify the real scope image and its useful UV
rectangle. Compare device addresses and adapter LUIDs for that output and ASKA.
Also compare actual buffer dimensions with window dimensions to distinguish
rendering geometry from desktop-window stretching. Preserve the original output
windows while measuring: occlusion or minimization may affect rendering.

For a **shared D3D11 device**, the intended path is:

1. Immediately before the scope output's present, copy its current backbuffer
   into an owned shader-resource texture on the same device. Resolve multisample
   input first if necessary. Copy the rendered scope rather than synthesizing a
   zoomed crop from the main image.
2. Immediately before ASKA's debug overlay/present, draw the newest scope texture
   through a circular mask in an independent render pass. Preserve all game
   pipeline state; do not rely on ImGui's debug-overlay visibility or frame
   lifetime. No staging texture, CPU pixel copy, or forced GPU wait belongs here.
3. Feed the pass the same normalized, filtered aim sample used by game input.
   Scope activation changes visibility only. It must not recalibrate the aim,
   recompute the cursor from camera-transformed coordinates, or move the scope
   center to keep its border on screen. Border clipping preserves exact aim.
4. On resize, release every reference to swapchain-owned backbuffers and RTVs
   before `ResizeBuffers`; rebuild dependent state after success. Recreate owned
   resources when source size/format or device identity changes, and suppress
   presentation of stale scope frames after reset or source loss.

For **different devices on the same adapter**, use an explicit GPU-shared texture
bridge with compatible creation flags, handles, and producer/consumer
synchronization. Passing one device's shader-resource view to another is invalid.
Different adapters require a separately measured design; a CPU screenshot loop
would not satisfy the natural, low-latency scope requirement.

The compositor makes the second image usable inside a single output. It does
not prove a 16:9 game camera, uncropped HUD, matching hit rays, or acceptable
portrait-authored encounter framing. Those require camera/input integration and
the fixed-wide versus aim-pan playtest described in `PORT_PLAN.md`.
