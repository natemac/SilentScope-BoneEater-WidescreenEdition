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
python tools/package_release.py
```

The player ZIP is generated under `build/package` with an enclosing `Bone Eater WS-Edition` folder. It includes the runtime, launcher, supplied replacement artwork and notices. It excludes original game data, user calibration, logs and saved settings. Build outputs remain outside version control.

The runtime output is `build/runtime/spicetools/64/Release/BoneEater.exe`; launcher output is `build/launcher/Release/Bone Eater WS-Edition.exe`.

The original game's supported module hashes are recorded in `SUPPORTED_GAME.json`. This beta was developed against those versions; other revisions have not been validated.
