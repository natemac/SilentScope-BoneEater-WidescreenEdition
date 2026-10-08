# BUILD 20261007.3 — beta release

## Bug fixes

- **Fresh-install setup:** creates missing cabinet NVRAM from the supplied original template, with valid CRC, completed calibration and full 0–4095 axis ranges. A manual first run of the original executable is no longer needed.
- **Split crosshairs / restricted aiming from a missing cabinet marker:** checks `user/conf/raw/dx` on every launch and recreates it if missing or renamed. A zero-byte file is valid. An inaccessible file stops launch with a clear warning instead of continuing with the wrong cabinet layout.
- **High-DPI and 4K desktop support:** enables physical-pixel DPI handling. With `force_1080p` enabled, the launcher uses a 1920×1080 desktop while retaining the working physical display signal through GPU scaling, then restores the captured display configuration on exit. This avoids the native 1080p HDMI switch that lost signal on the tested Denon setup.
- **Separate writable configuration:** `user/` holds NVRAM, raw cabinet storage, controls and main logs. Existing supplied `game/conf` settings migrate once; supplied conf/prop files are preserved. The native bookkeeping mount also uses private storage. Calibration changes are reported; `--repair-calibration` backs up and repairs the private calibration explicitly.

## Scope controls

Tapping toggles 2× scope in both options. Holding engages 10× after `hold_ms`.

- **Option 2 — default:** `"hold_release": "exit"` closes the scope when the hold is released.
- **Option 1:** `"hold_release": "lower"` returns to 2× when released.

Set this inside `scope` in `launch-settings.json` with `"mode": "toggle_hold"`, then restart. Omitting `hold_release` now defaults to `exit`; an explicit `lower` remains supported. Legacy mode bypasses the tap/hold controller.

## Packaging and upgrade

Download **BoneEater-WSEdition.zip**. It contains `game/`, `docs/`, `launch-settings.json` and `Bone Eater WS-Edition.exe` directly at its root, without an enclosing folder. Build information is in the README and intro screen. Original game files are required and are not included.

Extract to a short path such as `C:\BE`, add the original game folders listed in `game/ADD GAME FILES HERE.md`, and launch the outer **Bone Eater WS-Edition.exe**. A short ZIP filename cannot compensate for a deeply nested extraction location. When upgrading, retain your own `user/` folder; do not distribute it. Keep the companion launcher running for display restoration.

## Confirmed testing and resolved reports

- User confirmed Option 2 in **Story and Score Attack**: hold for 10×, release to exit, then tap to reopen 2×.
- User confirmed affected **Sinden guns reach all four screen edges**.
- User confirmed the **1080p desktop change and restoration on additional display setups**, beyond the accepted Denon test.
- The reported **checkerboard/grid was caused by incomplete original game files** and resolved when the affected player used a complete working set. No renderer fix was needed for that report; exact missing filenames were not identified.
- All 52 automated suites passed (5 launcher, 12 input, 35 rendering). Launcher/input regression tests cover setup, calibration, display restoration, both scope policies, timing boundaries and configuration transport. An isolated bookkeeping test preserved all 41 supplied conf/prop file hashes while writing private state.

## Still open

- [Issue #1](https://github.com/natemac/SilentScope-BoneEater-WidescreenEdition/issues/1): `ess.dll` startup crash. The report contains a 162-character module-path warning; a controlled short-path retest is still needed. Path length is a lead, not a confirmed fix.
- [Issue #2](https://github.com/natemac/SilentScope-BoneEater-WidescreenEdition/issues/2): original scope window/focus during Sky/final-boss sequences. No fix or mid-game scope reset is claimed.
- Extended stability and other hardware/display combinations still need testing. User confirmations do not establish compatibility with every gun, driver or monitor.

Randomized paired intro/menu artwork, accepted scope shapes, lobby/bullet alignment and Score Attack menu/results fixes remain included. Dubbed audio integration and local score/profile keeping remain planned features.
