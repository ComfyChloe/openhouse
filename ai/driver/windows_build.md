# Windows x64 / MinGW build

## Deliverables

`scripts/build-windows.sh` builds and packages:

* `build-win64/openvr/lighthouse/bin/win64/driver_lighthouse.dll`;
* `liblh.dll`, `lhviz.exe`, `lhtool.exe`, `lhcapture.exe`;
* `dist/lh-windows-x64.zip` — stripped binaries, dependency notices, hashes and
  an optional backup/restore installer.

The driver DLL is self-contained: Ceres, glog/gflags, hidapi, zlib and compiler
runtimes are statically linked. PE import inspection checks that it needs only
Windows system/UCRT libraries, not Linux libraries or separate MinGW DLLs.
The standalone apps share `apps/liblh.dll`; keep the apps directory together.
Windows 10/11 x64 with the Universal CRT is the intended target.

## Building on Linux

Required tools: MinGW-w64, vcpkg, Git, CMake, Ninja, Python 3, and the OpenVR driver
header (the platform-neutral SDK header at `/usr/include/openvr` works here).
No additional compiler/packages were needed on the current machine.

```
scripts/build-windows.sh
```

The system vcpkg executable initially had no ports checkout. The script uses
`$LH_VCPKG_ROOT`, or a dedicated `$XDG_CACHE_HOME/openhouse/vcpkg` checkout (default
`~/.cache/openhouse/vcpkg`). It installs the pinned `vcpkg.json` dependencies and test/
viewer features for `x64-mingw-static`, then builds with up to 16 threads.
`cmake/mingw-w64.cmake` prevents accidentally linking host Linux libraries.
`OPENVR_INCLUDE_DIR` and `VCPKG_MAX_CONCURRENCY` can override the defaults.
Use a dedicated vcpkg root: its installed dependency set is managed by the manifest.

The vcpkg manifest pins baseline `a5f736224e85b32e25a8ffc52562333180396642`:
Ceres 2.2 (Schur specialization, no SuiteSparse/LAPACK dependency), Eigen 5.0.1,
hidapi, zlib, nlohmann-json; optional GLFW and GTest. Our solvers use dense Ceres
solvers, so the omitted sparse backends are not required. vcpkg app-local DLL
copying is disabled because this is a static-dependency cross-build.

For repackaging an existing build:

```
python3 scripts/package-windows.py build-win64 "$VCPKG_ROOT/installed/x64-mingw-static"
```

`BUILD.json` records source commit/dirty status, compiler, pinned dependency
baseline, artifact hashes and PE imports. Do not mistake an artifact built from a
dirty tree for that commit alone.

## Windows installation

Close SteamVR. Extract the ZIP completely. In PowerShell from the extracted folder:

```
powershell -ExecutionPolicy Bypass -File .\Install-Driver.ps1 Install
# Nonstandard location:
powershell -ExecutionPolicy Bypass -File .\Install-Driver.ps1 Install -SteamVRPath "D:\SteamLibrary\steamapps\common\SteamVR"
# Restore:
powershell -ExecutionPolicy Bypass -File .\Install-Driver.ps1 Restore
```

The installer checks for running hosts, backs up the original as
`driver_lighthouse.dll.stock`, and verifies the installed file hash. Its path
lookup uses OpenVR's runtime path file, then Steam's registry path. It has **not**
been executed on a native Windows machine.

Manual installation is also possible: back up and replace only
`SteamVR\drivers\lighthouse\bin\win64\driver_lighthouse.dll`. Keep SteamVR's existing
manifest, models, input profiles, and hand assets. No HMD display support is
provided, just as in the Linux build. The Linux `.so` and installer are untouched.

## Platform handling

`src/platform/platform.hpp` handles:

* room/settings files in `%APPDATA%\lh`, with `%USERPROFILE%` fallback;
* Steam's HKCU `Software\Valve\Steam\SteamPath` location;
* assets relative to the installed module, covering another Steam library;
* replacing an existing saved file using `MoveFileExW` with replacement semantics
  (never delete the old file first);
* process IDs and the capture tool's `vrserver.exe` exclusion check.

Overrides: `LH_STEAM_ROOT`, `LH_STEAM_CONFIG_DIR`, `STEAMVR_PATH`, `LH_HAND_POSES`,
`LH_REFERENCE_DB`, `LH_ROOM_MAP`, `LH_STEAM_DB`. The capture tool has nonblocking
Windows console/redirected-input handling. HID access uses hidapi's Windows backend.
SteamVR host room/chaperone files are never rewritten by our alignment code.

## Validation and limits

**Verified under Wine with an isolated prefix, not native Windows:**

* all 8 available Windows CTest suites pass (Linux original-binary oracle tests
  are intentionally excluded);
* Windows `LoadLibrary`/`GetProcAddress` mock host loads the DLL, registers the
  Index controller, receives valid replay poses and skeleton/input updates;
* room persistence, replacing an existing file, and platform path overrides;
* simulated controller power-cycle replay: a fresh protocol/clock/filter epoch
  reacquires tracking without discarding the shared room;
* Windows visualizer startup/render/replay/screenshot smoke test;
* stripped package dependency/import inspection.

To run the tests via Wine, configure `CMAKE_CROSSCOMPILING_EMULATOR` as a semicolon-
separated command list, for example `/usr/bin/env;WINEPREFIX=/path/to/isolated-prefix;
WINEDEBUG=-all;/usr/bin/wine` (without a newline). Then run:

```
xvfb-run -a ctest --test-dir build-win64 --output-on-failure -j4
```

Set `LH_HAND_POSES` to the installed Valve GLB's Windows/Wine path to enable
asset-dependent skeletal checks. Native Windows SteamVR, physical Windows USB,
haptics, power-off and reconnect still need hardware testing. Wine passing does
not prove native hardware correctness or tracking parity with Valve's driver.

## Size

Release build (`scripts/build-windows.sh`): no debug info, `-ffunction-sections
-fdata-sections`, `--gc-sections` and `-s`, with release-only dependencies from the
overlay triplet `cmake/triplets/x64-mingw-static-release.cmake`. The driver DLL is
7.3 MiB (was 163 MiB unstripped RelWithDebInfo). Ceres is 4.3 MiB of it, 2.5 MiB of
which are the `schur` fixed-size specializations used by the room solver's
DENSE_SCHUR; they are kept for solver speed.
