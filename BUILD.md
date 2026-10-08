# Build and package

Use Windows x64, Visual Studio 2022 C++ Build Tools (including the Windows SDK and MASM), CMake and Python 3 for packaging. Original game data is needed to play, not to compile.

The pinned, patched Spice2x source and its source dependencies are vendored. Do not apply the historical patches again. Run these commands in PowerShell from this repository:

```powershell
$sourceRoot = (Get-Location).Path.Replace('\','/')
cmake -S vendor/spice2x/src/spice2x -B build/runtime -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PROJECT_spicetools_INCLUDE=$sourceRoot/cmake/BoneEaterRuntime.cmake"
cmake --build build/runtime --config Release --target spicetools_spice64 --parallel 4
cmake -S src/launcher -B build/launcher -G "Visual Studio 17 2022" -A x64
cmake --build build/launcher --config Release --parallel 4
ctest --test-dir build/launcher -C Release --output-on-failure
cmake -S tests/render -B build/render -G "Visual Studio 17 2022" -A x64
cmake --build build/render --config Release --parallel 4
ctest --test-dir build/render -C Release --output-on-failure
cmake -S tests/input -B build/input -G "Visual Studio 17 2022" -A x64
cmake --build build/input --config Release --parallel 4
ctest --test-dir build/input -C Release --output-on-failure
python tools/package_release.py
```

The player ZIP is generated as `build/package/BoneEater-WSEdition.zip`, with no enclosing folder. Its four top-level entries are `game/`, `docs/`, `launch-settings.json`, and `Bone Eater WS-Edition.exe`. The README, build manifest, provenance and license notices are under `docs/`. Version information stays in the README and intro screen rather than the archive name. Extract into a short path such as `C:\BE`. It includes the runtime, launcher, supplied replacement artwork and notices. It excludes original game data, user calibration, logs and saved settings. Build outputs remain outside version control.

The runtime output is `build/runtime/spicetools/64/Release/BoneEater.exe`; launcher output is `build/launcher/Release/Bone Eater WS-Edition.exe`.

The original game's supported module hashes are recorded in `SUPPORTED_GAME.json`. This beta was developed against those versions; other revisions have not been validated.

BUILD 20261007.3 is a beta release. Five launcher suites cover settings/calibration/migration/display recovery; input has 12 suites and rendering has 35. The user confirmed affected Sinden aiming and display switching/restoration beyond the Denon setup; other hardware combinations remain unverified. The launcher and runtime must be deployed together because both participate in the private user-data layout.

Manual display diagnostic: build/launcher/Release/display_probe.exe reads actual active source/target timings. --test-1080p-scaled-session exercises the current launcher policy (1080p desktop, retained output timing) for ten seconds then restores without starting the game. --test-1080p and --test-1080p-59 deliberately exercise the older native-signal switch; --list-modes is read-only. Run with the game closed. Desktop API success cannot establish physical receiver/display visibility.
