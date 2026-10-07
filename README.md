# Silent Scope: Bone Eater — Widescreen Edition

A Windows adaptation of Silent Scope: Bone Eater for a single 16:9 screen, with mouse-compatible lightgun controls. **First public beta — BUILD 20260930.2.** Original game files are required and are not included.

## What changed

- Single-screen widescreen presentation for Story and Score Attack, with a movable composited scope.
- Refined angled scope lens, with the original circular option available in JSON.
- Full-width aiming in the Score Attack lobby and results, corrected Start-button hit mapping and bullet-hole placement.
- Menu/transition flash fixes and cleanup of leftover cabinet player labels, backplates and lower score gauges.
- Twelve intro images in shuffled order: every image appears once per set, with no immediate repeat. Starting the game keeps the matching menu background at 30% opacity while the menus fade over it.
- Full-screen phone notice, refreshed artwork and an intro build label.
- A portable launcher that initializes inherited cabinet gun calibration to the full input range on first launch, with a backup.

Story and Score Attack have been tested during development. This beta still needs broader lightgun, display and long-session testing.

## Install and play

1. Download the **player ZIP** from Releases and extract it into a fresh folder.
2. Inside `game`, read **ADD GAME FILES HERE.md**. Copy your original `arkdata`, `conf`, `data`, `modules` and `prop` folders there. Keep the included `BoneEater.exe` and `desktop` folder.
3. Run **Bone Eater WS-Edition.exe** in the outer folder.

Use mouse-compatible lightgun output. Left mouse fires; right mouse or Enter controls the scope. Do not manually bind Gun X/Y. Change `scope.shape` in `launch-settings.json` to `angled` or `circle`, then restart to compare. Alt+F4 exits.

First launch backs up the copied calibration XML/CRC to `game/desktop/calibration-initialization-r3` and initializes full-range values. Your original game folder and lightgun driver calibration remain untouched. Later launches preserve calibration changes. Do not recopy the original `conf` over an initialized installation: that can restore the old cabinet calibration.

## launch-settings.json

Edit `launch-settings.json` beside **Bone Eater WS-Edition.exe** to customize the view and scope. Close the game, save the file, then launch again: settings are read at startup. **The values below are the shipped defaults and recommended starting settings for Widescreen Edition**, including mouse-compatible lightguns. Sensitivity and smoothing can be tuned to your hardware and preference.

```json
{
  "schema_version": 1,
  "view": "balanced125",
  "main_dof_off": false,
  "input_profile": null,
  "scope": {
    "mode": "toggle_hold",
    "bindings": ["ENTER", "RBUTTON"],
    "hold_ms": 250,
    "low_gain": 0.25,
    "high_gain": 0.1,
    "low_smoothing_ms": 35,
    "high_smoothing_ms": 55,
    "shape": "angled"
  }
}
```

### Recommended defaults versus original arcade behavior

**There is no complete original-arcade preset in this JSON.** The launcher keeps the single-screen widescreen layout, aiming fixes and other edition changes enabled. The original cabinet used a separate scope display and portrait main display, so its layout and calibration cannot be restored by selecting a scope shape or control mode.

| Setting | Default / recommended for this edition | Original arcade comparison |
|---|---|---|
| `schema_version` | `1` | Launcher file format only; no arcade equivalent. |
| `view` | `"balanced125"` | Both available choices are widescreen framing presets. Neither restores the original portrait camera/layout; there is no `"original"` value. |
| `main_dof_off` | `false` | Preserves the game's native main-view depth-of-field behavior. This is also the choice for original blur behavior. |
| `input_profile` | `null` | Desktop mouse/lightgun input is an adaptation. An original cabinet gun/calibration setup is not a selectable profile in this file. |
| `scope.mode` | `"toggle_hold"` | `"legacy"` leaves scope control to the original game path instead of the added tap/hold controller. It does not recreate the cabinet's physical scope hardware. |
| `scope.bindings` | `["ENTER", "RBUTTON"]` | Desktop bindings for the added controller, not original cabinet button settings. They do not configure that controller when mode is `"legacy"`. |
| `scope.hold_ms` | `250` | Added tap/hold threshold; no original arcade value. Unused by the added controller in `"legacy"` mode. |
| `scope.low_gain` / `scope.high_gain` | `0.25` / `0.1` | Added scoped-motion tuning for `"toggle_hold"`; these are not measured original cabinet sensitivity values. `"legacy"` bypasses that tuning path. |
| `scope.low_smoothing_ms` / `scope.high_smoothing_ms` | `35` / `55` | Added scoped-motion smoothing for `"toggle_hold"`; no equivalent original cabinet values are provided. `"legacy"` bypasses that tuning path. |
| `scope.shape` | `"angled"` | Approximates the original scope display's shaped rectangular outline. `"circle"` is an alternative presentation, not an original-arcade preset. Both remain composited desktop lenses. |

**To compare native scope controls:** keep the full default file above, change only `scope.mode` to `"legacy"`, and restart. Keep `main_dof_off` at `false` and `scope.shape` at `"angled"` for native blur and the arcade-inspired lens outline. This is a control comparison within Widescreen Edition, not a complete cabinet restoration. Change the mode back to `"toggle_hold"` to return to the recommended desktop controls. Do not restore old cabinet calibration files for this comparison.

### View and input

| Setting | Shipped value | What it does |
|---|---|---|
| `schema_version` | `1` | Settings-file format version. Leave this at `1`; it is not the game build number. |
| `view` | `"balanced125"` | Main-view framing. Choose `"balanced125"` for the standard view or `"closer150"` for a tighter, closer view. |
| `main_dof_off` | `false` | Keeps the game's main-view depth-of-field blur. Set to `true` to disable that blur. |
| `input_profile` | `null` | Uses normal mouse-compatible input without a custom device profile. Advanced users can supply a path to an input-profile JSON file; most mouse/lightgun users should leave this as `null`. This is not the cabinet calibration file. |
| `scope` | Object shown above | Groups the scope appearance and control settings described below. |

For a custom `input_profile`, relative paths start inside the `game` folder, not beside the launcher. Absolute paths also work. Use forward slashes in JSON paths (for example, `"C:/Profiles/my-input.json"`) or escape each backslash as `\\`.

### Scope

| Setting inside `scope` | Shipped value | What it does |
|---|---|---|
| `mode` | `"toggle_hold"` | Tap to toggle the scope on/off. Hold for higher magnification; release returns to lower magnification. `"legacy"` uses the original scope-control behavior instead of this tap/hold controller. |
| `bindings` | `["ENTER", "RBUTTON"]` | Buttons that operate the tap/hold scope control. Either Enter **or** right mouse works; these are alternatives, not a combination. A lightgun button can be mapped to one of these inputs. |
| `hold_ms` | `250` | How long to hold before higher magnification engages, in milliseconds. Smaller values engage it sooner. Accepts whole numbers from `100` to `1000`. |
| `low_gain` | `0.25` | Scoped aiming sensitivity at lower magnification. Lower values give slower, finer movement; higher values give faster movement. Accepts values greater than `0` through `1`. |
| `high_gain` | `0.1` | Scoped aiming sensitivity at higher magnification, with the same range as `low_gain`. This changes movement sensitivity, not the zoom amount. |
| `low_smoothing_ms` | `35` | Aiming smoothing at lower magnification. Higher values reduce jitter but make movement feel less immediate. Accepts `0`–`250` milliseconds; `0` disables added smoothing. |
| `high_smoothing_ms` | `55` | Aiming smoothing at higher magnification, with the same range and tradeoff. |
| `shape` | `"angled"` | Lens outline: `"angled"` uses the shaped rectangular lens; `"circle"` uses a circular lens. This changes the visual mask, not calibration or magnification. |

Bindings accept 1–8 distinct uppercase names: `ENTER`, `SPACE`, `LBUTTON` (left mouse), `RBUTTON` (right mouse), `MBUTTON` (middle mouse), `XBUTTON1`/`XBUTTON2` (extra mouse buttons), or a single `A`–`Z` or `0`–`9`. Left mouse normally fires, so choosing `LBUTTON` for scope would share that trigger.

For a first adjustment, change just one setting: try `"circle"` to compare lens shapes, reduce a gain for finer scoped aiming, or reduce smoothing for a more immediate response. Keep a copy of the original settings. The guide above covers every field in the shipped file; optional advanced adaptive aiming is not enabled in that file.

Use valid JSON: double quotes around names/text, lowercase `true`/`false`/`null`, and no comments or trailing commas. Unknown names, duplicate settings and out-of-range values are rejected. Keep the full example when editing rather than deleting settings to reset them: omitted scope fields can use different fallback values.

## Known bugs and limits

- **Intermittent extra scope window:** one test session flickered and displayed the original scope window in the upper-right alongside the moving lens. Restarting resolved it. The cause is not yet confirmed; no mid-game reset shortcut is included. If it recurs, save `game/desktop/game.log` before restarting and include it in a bug report.
- Some lobby instruction text may be clipped. Older notice/tutorial artwork and the intermediate portrait scoreboard still need presentation review.
- Full campaign, extended stability and all lightgun/display combinations are not certified. The top-center hit replay is intentional.
- Complete local score/profile persistence and online account/ranking support are not provided by this release.

## Planned features

- English dubbed audio integration.
- Persistent local score keeping, high scores and player progress.
- Investigation of the intermittent scope-window issue and a guarded mid-game recovery shortcut.
- Further presentation cleanup and hardware testing.

These are planned work, not included features or release-date promises.

## How this was made

Built on the GPL-licensed Spice2x runtime, this project adapts the original game's camera, window, input and HUD behavior rather than recreating the game. Work proceeded through small changes, runtime traces, screenshots, automated checks and repeated Story/Score Attack playtests. Scope compositing, widescreen hit mapping and menu artwork were refined separately so accepted gameplay could be preserved. Fresh-install beta testing then exposed inherited cabinet calibration, which the launcher now handles explicitly.

Developed by **natemac** with **ChatGPT-6 Astra**. The shared creation conversations are available in [CREATION.md](CREATION.md).

## Source and credits

See [BUILD.md](BUILD.md) to build the included source, run checks and package the player ZIP. The vendored Spice2x source already contains the integration changes; do not apply the patches again. [SOURCE.lock.json](SOURCE.lock.json) records upstream provenance.

Runtime/source licensing and upstream notices are retained in [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt). Original game content is supplied separately by the player. Game names and artwork retain their respective ownership; this is an unofficial adaptation.
