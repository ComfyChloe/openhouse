# Agent handoff: start here

This folder is the working knowledge for AI agents continuing this project. Human-facing
documentation is written separately by the owner; do not add Markdown outside `ai/`.

## The project

* **lhdrvrw / liblighthouse** (repository root): a reverse-engineered, improvable C++20
  reimplementation of Valve's Lighthouse 1.0 tracking (`driver_lighthouse`). It ships as a
  drop-in OpenVR driver (`driver_lighthouse.so` / `.dll`) for SteamVR and for Monado's
  `steamvr_lh`, plus tools (capture, replay, viewer). The owner runs it under Monado +
  WiVRn with a Quest headset (alignment tools aligns the Quest with the Lighthouse room),
  in VRChat and Beat Saber, on Linux and Windows SteamVR.
* **lhbs** (`basestation_firmware/`): a readable reimplementation of the HTC Lighthouse 1.0
  base station firmware (NXP LPC11U37), with an optical multi-station mode for more than
  two bases (TDM time slots).

Goals and constraints (from the owner):
* Tracking algorithms and constants start from the original (oracle-tested), but use
  normal libraries (Ceres, Eigen, hidapi, nlohmann-json). Readable code with documented
  provenance (stock addresses, constants), not bit-for-bit transliteration.
* Long-term aim: **better than stock SteamVR tracking**. Improvements live outside the
  stock filter core (`DeviceTracker`, `SetImprovements`, env switches for A/B), so the
  oracle tests keep checking the stock behaviour.
* USB transport stays separate from tracking (`lh::IInputSink`); tracking must not
  depend on hidapi.
* Device roles come from each device's JSON config (`device_class`), never from the
  hardware: the owner maps some Index controllers and wands as generic trackers.
* Only tracked devices matter (controllers, trackers, bases); no HMD display work.
* Lighthouse 1.0 only. LH2 was researched and set aside: `driver/lh1_as_lh2_research.md`.
* **Commit only when the owner asks.** Ask for hardware captures when a real-device test
  is needed; don't guess hardware behaviour.

## Where things are

| Path | What |
|---|---|
| `src/transport/usb` | HID transport, report/wireless decoding, raw capture (`.lhraw`) |
| `src/tracking` | sync/sweep disambiguation (stock TDM, `disambiguator_multi` for lhbs multi), pose solver, the stock EKF (`direct_filter`, `lighthouse_direct`) |
| `src/runtime` | `Session`, `DeviceTracker` (improvements), room map and room solver |
| `src/openvr` | the OpenVR driver (devices, inputs, skeleton, host frame, room publisher) |
| `src/platform` | OS helpers (paths, precise sleep) |
| `apps/` | `lhtool` (replay/session/track), `lhcapture` (recorder), `lhviz` (viewer), `lhhaptics`, `lhidentify` |
| `tests/` | unit, runtime, OpenVR component/mock-host, oracle tests (load the original binary) |
| `tools/` | `eval_replay.sh/.py` (tracking scorer), RE helpers |
| `scripts/` | Linux install (`steamvr-driver.sh`), packaging, Windows cross-build |
| `basestation_firmware` | the firmware (`src/`, `test/`, `Makefile`); `tools/` flashing and console. `fw/`, `patches/`, `re/` are local only |
| `re/ghidra_scripts` | headless Ghidra scripts. The rest of `re/` (project, decompiles) is local only |

Not in git, on purpose: Valve's binaries, decompiled code, HTC firmware images and flash
dumps, `libsurvive/` (reference only: LH2 polynomials and decode), recordings.

## Build, test, install

```sh
cmake -S . -B build-linux -G Ninja && cmake --build build-linux -j$(nproc)
ctest --test-dir build-linux -j4 --output-on-failure      # 10 tests; oracle tests skip without fixtures
scripts/steamvr-driver.sh install build-linux/openvr/lighthouse/bin/linux64/driver_lighthouse.so
```

Windows: MSVC builds are preferred (native SteamVR). The project builds with the VS 2026
generator and vcpkg static triplets; `triplets/x64-windows-static-avx2.cmake` builds every
dependency with AVX2 (Eigen's alignment must match across all of them, or the heap is
corrupted). OpenVR headers: any `openvr_driver.h` (2.15.6 used). The MinGW cross-build
(`scripts/build-windows.sh`, `driver/windows_build.md`) also works. On Windows the stock
`driver_lighthouse.dll` is replaced in `SteamVR\drivers\lighthouse\bin\win64` (keep the stock
one as `.stock`).

## Evaluating tracking changes

Measure, don't reason. Replay captures with `tools/eval_replay.sh CAPTURE OUT 50 8 --room
COPY_OF_ROOMMAP` (parallel windows, prints valid/coast/lost %, jumps, jitter). **Always replay
against a copy of the room map** (`~/.config/lh/roommap.json`, Windows
`%APPDATA%\lh\roommap.json`); a replay can rewrite the map it is given. The script calls
`build/lhtool`; point it at your build directory.

Captures (owner's machine; ask for them if absent):
* `~/lh-captures/vrchat-01`: 10 min VRChat, 5 devices: the main regression capture.
* `~/lh-captures/testswing`: 24 s of violent swings. `~/lh-captures/ms4-01`: bases on/off.
* `/mnt/lh-captures/beatsaber-01`, `-01-2`, `-01-3`: fast Beat Saber play. Fly-aways
  reproduce only from some starting points: replay each with `lhtool session ... --from F`
  for F in 0 120 200 230 260 and count "out of bounds" resets. Beat Saber is the owner's
  top priority: a change must keep these clean.

Debug switches (env): `LH_DEBUG_UPDATE[=all]` (large filter updates and frame rejections),
`LH_DEBUG_AXIS` (out-of-sequence sweeps), `LH_DEBUG_MULTI`, `LH_DEBUG_SOLVER`,
`LH_DEBUG_REJECT`. A/B: `LH_STOCK_FILTER`, `LH_NO_SWING`, `LH_NO_FRAME_GATE`. Recording
inside the host: `LH_CAPTURE=DIR` or `capture` under `driver_lighthouse` in
`steamvr.vrsettings`.

## The owner's setup

Four LH1 bases on lhbs multi-station: slots 1-4 = E236DC40 (master), A101B835, D7B1AD76,
89AE90F2 (older 15-diode unit). 89AE90F2's calibration was once wiped by HTC's
"calibration-rescue-244" firmware and restored from a flash backup: **never suggest that
firmware**. Devices: Index controllers (LHR-13DA81D1 right, LHR-8F9BE6F2 left), trackers,
an Index HMD used only as a sensor.

## State of the work (2026-10-22)

Recently done (details in `driver/tracking_improvements.md`):
* Beat Saber fly-aways fixed: slave bases' mislabelled sync axis is dropped in
  `disambiguator_multi`, and `DirectFilter::Update` rejects whole frames that would throw
  the device more than 10 cm (owner chose to apply it in violent motion too).
* Room solver: exact Schur-complement base covariance instead of `ceres::Covariance`,
  hand-derived `AngleCost` Jacobian, solve backoff; fixed a fatal Ceres abort.
* Windows: `IVRDriverInput_005` for buttons (see below), precise sleeps
  (`platform::SleepFor`), MSVC build.

Open:
* **Slave firmware sends the wrong sync axis** (up to 10 % of a slave's sweeps, now dropped
  by the driver): root cause in lhbs slave timing, needs a base on the console.
  `firmware/MULTISTATION.md`. Slaves also still need reflashing with the latest build.
* Live check of the input path under Monado: the driver logs `input components through
  IVRDriverInput_005` or `_004`; confirm buttons work.
* The Windows build installed on the owner's PC predates the latest fixes (axis drop,
  frame gate rework, solver crash fix): rebuild it there.
* `driver/PROGRESS.md` and `driver/closing_the_gap.md`: remaining parity work with the
  stock tracker.

## Hard-won lessons

* **Windows SteamVR ignores boolean inputs sent through `IVRDriverInput_004`** (accepted,
  never applied: no buttons, no dashboard). Keep component calls on `ComponentInput()`,
  which asks for `_005` like the stock DLL. When an OpenVR call behaves differently on
  Windows, check which interface version the stock DLL requests before suspecting the
  compiler (it wasn't MinGW).
* Windows `sleep_for` rounds up to the 15.6 ms timer tick; use `platform::SleepFor`.
* vcpkg's Ceres has no SuiteSparse: avoid `ceres::Covariance` (its sparse QR fallback was
  87 % of the CPU).
* Monado's `steamvr_lh` doesn't type-check input components, so Windows-only bugs hide
  on Linux. Check Monado's `steamvr_lh` source for what it implements before relying on
  an OpenVR host feature.
* Replays from different starting points follow different trajectories (and GCC vs MSVC
  differ too): a single replay proving "clean" means little.

## Index of the documents

`driver/` (the driver and library):
* Overview: `SYSTEM_OVERVIEW.md` (end to end), `ARCHITECTURE.md` (the original's design),
  `usage.md` (build/run/API), `PROGRESS.md`, `VALIDATION.md`, `closing_the_gap.md`.
* Transport: `usb_protocol.md`, `wireless_protocol.md`, `capture.md`, `source_layer.md`.
* Tracking: `disambiguator_tdm.md`, `multistation_plan.md`, `pose_solver.md`,
  `lighthouse_direct.md`, `state_vector.md`, `measurement_models.md`, `sensor_type.md`,
  `constants.md`, `tracking_improvements.md` (every change beyond stock, with numbers).
* Runtime and host: `lh_runtime.md`, `runtime.md`, `interfaces.md`, `openvr_driver.md`,
  `windows_build.md`.
* Research: `lh1_as_lh2_research.md`.

`firmware/` (lhbs): `usage.md` (build, flash, console), `FIRMWARE_OVERVIEW.md`,
`HARDWARE.md` (pin map, peripherals, verification log), `MULTISTATION.md` (TDM slots,
open issues).

`reverse_engineering.md`: the Ghidra workspace for `driver_lighthouse.so` (local only).
