# Validation plan and capture requirements

Passing tests does not establish whole-driver tracking equivalence. Current
fixtures are mostly stationary and the full original filter worker is not yet
instantiated by the oracle harness.

## Automated checks

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/lh_oracle_tests --gtest_filter='*SampleHistory*:*DirectFilter*:*PoseSolver*'
./build/lhtool track captures/controller_static_10s.lhraw > /tmp/poses.jsonl
xvfb-run -a ./build/lhviz --replay captures/<session>/input.lhraw --screenshot frame.ppm 10
```

Also verify a tracking-only build (`LH_BUILD_USB=OFF`) and an installed external
consumer using `find_package(lighthouse CONFIG REQUIRED)` / `lh::tracking`.
These are development checks, not accuracy benchmarks.

## Generic user capture

The preferred tool is now `./build/lhcapture`: move/occlude the controller freely
and press Ctrl-C when done. No prescribed sequence is required. Give back the whole
printed session directory. See [capture.md](capture.md).

## Optional separate recordings for targeted regression

With SteamVR closed, LH1 bases on, and the right controller awake, capture each
case separately if desired. Move the controller, not the HMD, if that is more convenient.
Stay within the normal safe play area; no fast or extreme movements are required.

```sh
./build/lhtool capture captures/controller_stationary_visible_30s.lhraw 30
./build/lhtool capture captures/controller_slow_translation_30s.lhraw 30
./build/lhtool capture captures/controller_slow_rotation_30s.lhraw 30
./build/lhtool capture captures/controller_occlusion_recovery_30s.lhraw 30
```

- Stationary: place the controller where both bases see several sensors.
- Translation: gentle movement across the space at roughly constant orientation.
- Rotation: gentle rotation about several axes while keeping the origin fairly still.
- Occlusion/recovery: briefly block line of sight without moving the bases, then
  restore it. Include stationary periods before and after.

`.lhraw` stores endpoint configuration and raw HID reports, allowing re-decoding
without hardware. Keep the raw files; pose JSON alone cannot validate parsers.
These commands **do not record the original tracker's output poses** and cannot
by themselves establish pose parity.

## Still required for an equivalence claim

1. Instantiate the original full tracker/config/source/DB interfaces in a replay
   harness and feed it the exact same events, configurations, and scheduling.
2. Compare bootstrap decisions, active base slots, update acceptance, state,
   covariance, timing, and poses through motion and recovery—not just static poses.
3. Validate config geometry/IMU-frame transforms and learned DB initialization
   against original functions, including factory vs learned calibration fallback.
4. Port/verify the remaining IMU-only, slot replacement, database, and transport
   startup/reconnect policies. See runtime.md for the current list.
5. Add a live reference-pose recorder/comparator with correct IMU↔head/grip and
   primary-base↔SteamVR-world transforms and time alignment. Do not simply subtract
   our IMU-origin pose from a SteamVR head/grip pose. A fitted static alignment can
   conceal frame/calibration errors, so motion fixtures are necessary.

No live reference comparison was performed during this continuation. Existing
SteamVR configuration files were read, not modified.

## First motion baseline (2026-10-16)

`lhcapture` session `captures/session-20261016-142931Z-147083/` (94 s, controller
moved/hidden/fast-moved/one-base-hidden) is now wired into the oracle suite:

- `Captures/TdmDiff` and `Captures/WirelessDiff` compare the raw front end on this
  capture and match the original exactly (10599/10613 frames, 49358 packets).
- `MotionFilterOracle` compares per-update filter behavior (see runtime.md). **Verified:**
  8241 optical updates (3505 on a secondary base) and 4444 inertial steps, 0 divergent
  (>1e-6 relative), max relative state divergence 8.8e-13, covariance 1.2e-14, 0/8241
  return-flag mismatches.
  Bootstrap and alternate-base insertion are not covered (each check starts from the
  port's state), so they remain ported-but-unverified.

Next requested input: none required yet. The motion capture already exercises
occlusion, fast motion, and single-base visibility. A capture with SteamVR running
(not yet supported by lhcapture) is still needed to compare against the stock
driver's *pose output* rather than its internal functions.
