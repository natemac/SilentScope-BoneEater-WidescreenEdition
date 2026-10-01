# Camera policy tests

<!-- current-update:start -->
> Current update: **DE97 installed: HUD seam/name and top-five ranking corrections plus ranking notice cleanup; 28 rendering suites pass. Beta readiness work continues; aiming and full-flow checks remain open**.
> See the [current update checkpoint](../../CURRENT_UPDATE.md) for the installed build,
> verified results and next work. Older observations below remain historical evidence.
<!-- current-update:end -->

`vertical_pan_policy_tests.cpp` is a standalone, deterministic C++17 test
executable. No game modules, graphics system, test framework or network access
are required. This verifies control logic only; it cannot verify original-game
camera hooks, target accuracy or the physical feel of a lightgun.

The 11 cases cover fixed framing, deadzone continuity and symmetry, an analytic
rate/exponential transition, timestep subdivision at 20/60/120/144 Hz,
scope-hold/release, mode changes while held, atomic rejection, scene reset,
configuration validation, tiny steps, and 10,000 deterministic stress updates
checking angle/speed bounds and no overshoot.

`framing_math_tests.cpp` adds nine cases for the stateless look-at pitch helper:
pitch sign/angle, target distance and W preservation, exact neutral output,
authored-up orthogonalization, translation/scale invariance, finite/roll/pitch
validation, degenerate and pole-crossing rejection, float output precision, a
measured native camera tuple, and script motion continuing while the policy
offset is held. Repeating a commit with the same authored baseline does not
compound the pitch. These tests perform no game-memory reads or writes.

Example from the repository root with a C++17 compiler available:

```text
c++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc src/camera/vertical_pan_policy.cpp tests/camera/vertical_pan_policy_tests.cpp -o camera_policy_tests
```

The standalone CMake target also supports an installed Visual Studio toolchain:

```text
cmake -S tests/camera -B build/camera-tests -G "Visual Studio 17 2022" -A x64
cmake --build build/camera-tests --config Release --parallel 2
ctest --test-dir build/camera-tests -C Release --output-on-failure --verbose
```

For MSVC in an initialized developer shell:

```text
cl /nologo /std:c++17 /EHsc /W4 /Isrc src/camera/vertical_pan_policy.cpp tests/camera/vertical_pan_policy_tests.cpp /Fe:camera_policy_tests.exe
```

Run the resulting executable; exit code zero requires all cases to pass. Keep
build outputs outside `original game` and `reference`.

Verified on 26 September 2026 with MSVC 19.44.35222.0, Windows SDK 10.0.26100.0,
and the Release x64 CMake targets: clean build, 11 policy cases plus nine framing
math cases passed, zero failures. CTest registers two executables. This verifies
isolated policy/math behavior only; no game process was launched for these tests.
