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
