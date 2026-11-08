# Agent handoff: start here

This folder is the working knowledge for AI agents continuing this project. Human-facing
documentation is the root `README.md`, written by the owner (in their voice: casual,
short). Don't add other Markdown outside `ai/`.

## The project

* **openhouse / liblighthouse** (repository root): a reverse-engineered, improvable C++20
  reimplementation of Valve's Lighthouse 1.0 and 2.0 tracking (`driver_lighthouse`). It ships as a
  drop-in OpenVR driver (`driver_lighthouse.so` / `.dll`) for SteamVR and for Monado's
  `steamvr_lh`, plus tools (capture, replay, viewer). The owner runs it under Monado +
  WiVRn with a Quest headset (a separate tool aligns the Quest with the Lighthouse room),
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
  oracle tests keep checking the stock behaviour. `LH_STOCK_FILTER` turns the
  improvements off for comparisons; add per-change switches locally while testing and
  remove them before the change lands (the release has none).
* USB transport stays separate from tracking (`lh::IInputSink`); tracking must not
  depend on hidapi.
* Device roles come from each device's JSON config (`device_class`), never from the
  hardware: the owner maps some Index controllers as generic trackers. Vive wands are
  controllers (`ControllerType::ViveWand`, libsurvive button map; untested on hardware).
* Headsets: tracked by us; their display comes from SteamVR's stock driver loaded at
  runtime for the headset only (`src/openvr/stock_hmd.cpp`). Never ship Valve's files.
* Lighthouse 1.0 and 2.0 both work (`driver/lh2_support.md`). Making LH1 bases emit
  LH2-style light was researched and set aside: `driver/lh1_as_lh2_research.md`.
* Release hygiene: no Valve/HTC text or data in the repo (stock log wording, firmware
  descriptions), no stderr debug prints (use DEBUG-level log messages), the name is
  openhouse everywhere.
* **Commit only when the owner asks.** Ask for hardware captures when a real-device test
  is needed; don't guess hardware behaviour.

## Where things are

| Path | What |
|---|---|
| `src/transport/usb` | HID transport, report/wireless decoding, raw capture (`.lhraw`) |
| `src/tracking` | sync/sweep disambiguation (stock TDM, `disambiguator_multi` for lhbs multi), pose solver, the stock EKF (`direct_filter`, `lighthouse_direct`) |
| `src/runtime` | `Session`, `DeviceTracker` (improvements), room map and room solver |
| `src/openvr` | the OpenVR driver (devices, inputs, skeleton, host frame, room publisher, stock HMD, pairing, telemetry server) |
| `src/common` | small shared pieces (loopback sockets) |
| `src/platform` | OS helpers (paths, precise sleep) |
| `apps/` | `openhouse-viewer` (live status of the running driver), `lhtool` (replay/session/track), `lhcapture` (recorder), `lhviz` (developer viewer), `lhhaptics`, `lhidentify` |
| `tests/` | unit, runtime, OpenVR component/mock-host, oracle tests (load the original binary) |
| `tools/` | `eval_replay.sh/.py` (tracking scorer), RE helpers |
| `scripts/` | Linux install (`steamvr-driver.sh`), packaging (`package-driver.py`, `package-windows.py`, `package-firmware.py`), Windows cross-build |
| `basestation_firmware` | the firmware (`src/`, `test/`, `Makefile`); `tools/` flashing and console; `installer/` the end-user installers (Linux sh, Windows ps1/bat). `fw/`, `patches/`, `re/`, `bsfirmwares/`, `backups/` are local only |
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
`build-linux/lhtool` (the one Linux build directory).

Captures (owner's machine; ask for them if absent):
* `~/lh-captures/vrchat-01`: 10 min VRChat, 5 devices: the main regression capture.
* `~/lh-captures/testswing`: 24 s of violent swings. `~/lh-captures/ms4-01`: bases on/off.
* `/mnt/lh-captures/beatsaber-01`, `-01-2`, `-01-3`: fast Beat Saber play. Fly-aways
  reproduce only from some starting points: replay each with `lhtool session ... --from F`
  for F in 0 120 200 230 260 and count "out of bounds" resets. Beat Saber is the owner's
  top priority: a change must keep these clean.

Logs and diagnostics: every message has a level (DEBUG/INFO/WARN/ERR). Replays print
them with `lhtool session` (`"level"` field); live, the host log gets INFO+ unless
`LH_LOG_LEVEL=DEBUG`, and `openhouse-viewer` shows everything. Recording inside the host:
`LH_CAPTURE=DIR` or `capture` under `driver_lighthouse` in `steamvr.vrsettings`; the
recording has `driver.jsonl` (what the driver did and logged) next to `input.lhraw`.

## The owner's setup

Four LH1 bases on lhbs multi-station: slots 1-4 = E236DC40 (master), A101B835, D7B1AD76,
89AE90F2 (older 15-diode unit). 89AE90F2's calibration was once wiped by HTC's
"calibration-rescue-244" firmware and restored from a flash backup: **never suggest that
firmware**. Devices: Index controllers (LHR-13DA81D1 right, LHR-8F9BE6F2 left), trackers,
a Vive headset (LHR-0704E07A). Quest Pro through WiVRn for VRChat.
Alignment tools can read the room from `/openhouse-room-<uid>` (`include/lh/room_shm.h`;
copies of that header in other projects must stay identical).

## State of the work (2026-10-25)

Done recently:
* Release clean-up: renamed to openhouse, debug/A-B switches removed, log levels,
  copied Valve text removed, README written.
* `openhouse-viewer` + driver telemetry (`src/openvr/telemetry_server.*`).
* Room locks (well-placed bases only move on their accelerometer), stale-room detection
  (turned origin -> new room), transport messages in the session log.
* Headset hybrid (stock display, our pose), SteamVR pairing menu, roles, hand slots,
  power-off on exit, configs read from devices per session.
* lhbs firmware installer (`basestation_firmware/installer`); the Windows script is
  untested on Windows.

Open:
* A base seen well by only one device (the owner's channel 3, `D7B1AD76`, which mostly
  sees one corner) never reaches the lock and gets relearned from poor views; the still
  headset tracking from it moves with it (`test2still`). Next: weight keyframes by how
  well the device sees both bases, or require two devices before re-placing.
* Fly-aways right after a reacquire or a base newly used from the room map (friend's
  `live3-4`, 2.0): the contradiction check now catches them within ~1 s; the cause (the
  relationship the filter takes) is open.
* Windows: test the viewer, the firmware installer and the telemetry port on real
  Windows.

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
* LH2: `lh2_support.md` (pipeline, results, open items). Research: `lh1_as_lh2_research.md`.

`firmware/` (lhbs): `usage.md` (build, flash, console; end users: `basestation_firmware/installer`), `FIRMWARE_OVERVIEW.md`,
`HARDWARE.md` (pin map, peripherals, verification log), `MULTISTATION.md` (TDM slots,
open issues).

`reverse_engineering.md`: the Ghidra workspace for `driver_lighthouse.so` (local only).
