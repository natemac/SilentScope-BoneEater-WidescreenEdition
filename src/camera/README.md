# Vertical framing experiment

<!-- current-update:start -->
> Current update: **DE97 installed: HUD seam/name and top-five ranking corrections plus ranking notice cleanup; 28 rendering suites pass. Beta readiness work continues; aiming and full-flow checks remain open**.
> See the [current update checkpoint](../../CURRENT_UPDATE.md) for the installed build,
> verified results and next work. Older observations below remain historical evidence.
<!-- current-update:end -->

`--aim-framing` now connects this policy to the verified native main camera.
The adapter pitches the final look-at target before the native scope ray is
calculated, restores the wrapper's authored target after the commit, and reuses
the certified offset at the later main-camera commit. It leaves the native
committed target pitched for rendering and targeting. Main and authored target
objects were independently verified as distinct in the tested scene.

The PID33328 six-phase probe confirmed approximately +6 degrees, a constant
held offset while aim moved from top to bottom, approximately -6 degrees after
release, and return to neutral. Independent analysis checked 28 settled policy
samples and 44 paired native commits. A separate scoped shot with negative
pitch registered a hit. This is bounded stationary-encounter evidence: scripted
camera motion, recoil, culling, stalls, and physical comfort still need tests.
See `review/CAMERA_ENGINE_FINDINGS.md` and `review/framing-33328.json`.

The portable policy described below remains separate from its native adapter.

## Framing comparisons to test

Keeping `--native-wide` but omitting `--native-vertical-fov` is a useful second
candidate: it retains the authored horizontal coverage and crops vertically.
The pitch control can then reveal high/low targets without exposing as much
peripheral geometry. This is a separate experiment, not a universal fix.

For the observed zoomed Stage00 camera in PID20184, full-height preservation
measures about35.67° horizontal by20.52° vertical. The narrower option predicts
12.10° by6.82°; an eight-degree pan in each direction sweeps a22.82° vertical
union. For the earlier45° authored horizontal camera, the corresponding sweep
would cover only42.23° of the original70.54° vertical view. Coverage therefore
depends on the native camera, and the full-height option must remain available.
These are angular calculations; live target accessibility, culling, cinematic
composition and comfortable gun movement still need comparison.

Portable C++17 control logic for comparing two candidate widescreen camera
behaviors. This module does not load the game, change projection matrices,
perform targeting, or establish a working Windows port. The defaults are
prototype values, not measured Bone Eater camera settings.

| Mode | Requested player pan |
|---|---|
| `FixedWide` | Zero; a nonzero existing pan eases back to zero. |
| `AimReframe` | A vertical angular offset driven by calibrated main-view aim outside a central deadzone. |

`VerticalPanPolicy` owns only that offset. The engine adapter must compose it
with the current original scripted camera on every update. Scope hold freezes
the player offset, **not** the original camera animation, its FOV, recoil or
other native behavior. The correct local rotation axis/order must be established
in the engine; this module does not assume that adding Euler angles is valid.

The input is device/main-view aim intent (`normalizedY`: top 0, bottom 1).
Never feed engine-resolved aim, projected world targets, the scope image's
coordinates, or coordinates changed by this pan back into the policy. That
creates a camera/aim feedback loop. The proposed dependency is:

1. Select and calibrate input; retain main-view aim intent.
2. Update this policy from that intent and the scope-hold action.
3. Combine its offset with the latest native scripted camera.
4. Build the final main view/projection and resolve native aim using that view.
5. Use corresponding resolved aim for native shots, scope and markers.

The adapter follows the verified early-main-commit, scope-ray, late-main-commit
ordering in this game build. Scope lock does not prevent the player from aiming
or firing while held. The input passed to the policy is the accepted cabinet
coordinate converted back to desktop intent, before any camera projection.

## API and behavior

Include `camera/vertical_pan_policy.h` with `src` on the include path, and compile
`vertical_pan_policy.cpp`. The module depends only on the C++ standard library.

```cpp
bone_eater::camera::VerticalPanPolicy pan;
bone_eater::camera::VerticalPanInput input;
input.aim.normalizedY = calibratedMainAimY;
input.mode = bone_eater::camera::FramingMode::AimReframe;
input.scopeHeld = scopeHeld;
const auto result = pan.update(input, simulationDeltaSeconds);
// Consume result.state.offsetRadians only after handling result.status.
// It is a separate player offset, never an absolute/frozen camera pose.
```

Positive offset means look upward; adapt the sign to the verified engine basis.
The default maximum excursion is eight degrees, deadzone is the central 20% of
the viewport, response time is 0.12 seconds, and speed is capped at 20 degrees
per second. A constant target follows a rate-limited first-order response:

`d(offset)/dt = clamp((requested - offset) / responseSeconds, -maxSpeed, maxSpeed)`

The implementation analytically advances the constant-speed and exponential
segments. Constant-intent behavior therefore matches across timestep
subdivisions, up to floating-point error, without frame-count-based tuning.
Changing input between samples can still produce sample-rate differences.

Scope hold freezes the current offset immediately, including the first accepted
held update. Release continues from that position toward the current requested
offset. It never resets to the latest target or replays stored motion. Switching
to `FixedWide` requests neutral; while scope is held the freeze takes priority,
then release eases back. These rules guarantee positional continuity within the
configured speed bound, not continuous angular velocity at hold/release.

Nonfinite/out-of-range aim, nonfinite/negative/over-limit `dt`, and unknown modes
return a failure status without modifying any state. Zero elapsed time returns
`NoTimeElapsed` without consuming a lock transition. The default allowed `dt`
maximum is 0.10 seconds. The caller must handle long stalls/focus loss explicitly
rather than silently clamp elapsed time or replay a large stale input interval.
Input/action lifecycle handling remains outside this module.

Configuration is fixed for an instance. Construction throws
`std::invalid_argument` for invalid values: angle must be in `[0, pi/2)`,
deadzone in `[0, 1)`, and response, speed and maximum delta must be finite and
positive. All angles use radians. A zero angle cap intentionally disables pan.
Do not replace the instance to change settings during gameplay without a
separately designed state transition.

`resetScene()` explicitly returns to neutral and clears the lock/target. It is
the one intentional discontinuity, suitable only for a scene/aim
reinitialization boundary. Do not call it when entering or leaving the scope,
on every camera animation change, or to conceal an input conversion error.

The production choice between fixed framing and aim-driven reframing still
requires a real encounter comparison: visibility of vertically authored action,
target accessibility, scope stability, and physical-gun comfort. No mode here
claims that existing assets/culling/aim limits already support widescreen.
