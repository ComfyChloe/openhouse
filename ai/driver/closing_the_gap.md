# Closing the gap to the stock driver

The rewrite already matches the stock driver **exactly** from USB/radio input through
to measured sweep angles (verified: 49,358 wireless packets, 10,599/10,613 TDM frames,
OOTX calibration identical to `lighthousedb.json`). The remaining difference is in the
tracking filter. This file lists, in priority order, how to eliminate it. Each item
includes how to prove it is fixed.

## How divergence is measured

`tests/oracle/test_motion_filter.cpp` replays the recorded motion session through both
the port and the original per-function (`0x36f5b0` prediction, `0x36f740` optical
update), starting every check from the port's own state, and compares the full 49-value
state, the covariance, and the accept/reject return flag after each update.

**Status (2026-10-16): closed for per-update functions.** On
`session-20261016-142931Z-147083`: 8241 optical updates (3505 on a secondary base) and 4444 inertial steps, 0 divergent
(>1e-6 relative), max relative state divergence 8.8e-13, covariance 1.2e-14, 0/8241
return-flag mismatches. The causes are listed in
`ai/driver/runtime.md` ("Divergence causes found and fixed"). Items 1–3 below are
resolved. Items 4–6 (IMU saturation path verification, bootstrap/base-switch policy,
pose-level acceptance against SteamVR output) remain open.

## Fix list (priority order)

### 1. Measurement-noise scratch row placement (highest impact)
The original seeds its fixed-capacity `+0x980` scratch with the optical noise floor and
then writes per-residual noise into rows `[capacity-m .. capacity-1]`, passing `m` (count)
and `capacity` (stride) to `EkfUpdate`. The port builds a compact `m×m` `R`. These are
only equivalent if `EkfUpdate` reads that exact block.
**Do:** confirm which rows `EkfUpdate` uses (`+0x988` row count vs the row offset it
writes), then make the port seed the noise into the same rows, or prove compact-`m`
reads the same values.
**Prove:** the `MotionFilterOracle` divergent count drops toward zero on the first
digits that currently differ (checks ~10, 1742, 2388, ...).

### 2. Alt-base covariance / active-slice mismatch
At optical check 10 the port rejects a secondary-base correction (`altRot 0.0237 rad`
> `0.01745`) that the original accepts. This means the alt-base columns of `H`/`P`
(or `n`) still differ.
**Do:** diff the port's secondary-base insertion (`Bootstrap` slot branch, `0x34b810`
covariance seeding) and the `n` growth against the original; verify the active-base
flag offset (`+0x1d0`, byte `+4`) and the exact 6-state slice indices.
**Prove:** the 14 return-flag mismatches go to zero; `divergentWithMatchingPrediction`
diagnostic shows the update, not the prediction, was at fault.

### 3. Exact `S` solve factorization
`EkfUpdate` (`0x36cae0`) builds `S = H P Hᵀ + R` as a real `m×m` and factorizes it via
Eigen internals (`0x36be50` / `0x36b2e0`). The port uses `S.ldlt().solve(Identity)`.
For the near-singular normalizer row (R last entry 1e-8) the factorization matters at
the 1e-4 level.
**Do:** identify the decomposition used (LDLT vs partial-pivot LU vs LLT) and mirror it.
**Prove:** re-run the 4,000-case `DirectFilterOracle` update test and the motion oracle.

### 4. IMU off-scale / saturation path
The capture triggers "IMU went off scale." The port sets flag 0x400 but the saturation
set of excluded axes and the `R` fallback (precomputed 7×7 at `+0x998` when nothing is
saturated) must match the original exactly.
**Prove:** add saturated-IMU cases to `DirectFilterOracle.OrchestratedImuUpdate` and
check flag + state equality.

### 5. Bootstrap / base-switch policy
Bootstrap tilt/velocity seeding, base selection, and primary switching are simplified.
These are not per-step divergences but change which frames are used at all.
**Do:** port `0x35c4b0`/`0x35bf00` base selection and `0x353380` slot bookkeeping.
**Prove:** extend the oracle to compare the sequence of chosen primary bases and the
bootstrap pose, not only post-bootstrap updates.

### 6. End-to-end pose comparison (final acceptance)
Internal-function agreement does not by itself prove pose agreement. This needs the
stock driver's actual `DriverPose_t` output.
**Do:** add a capture mode that records stock poses while SteamVR drives the same
hardware (tap the HID stream without re-initializing the device), and a comparator
with correct IMU↔head/grip and base↔world transforms plus time alignment.
**Prove:** per-sample pose delta stays within a tight bound on motion, occlusion, and
single-base segments.

## Order of work

1 → 2 → 3 are per-update fixes and are the ones that move the measured number. 4 and 5
are coverage/behavior gaps. 6 is the acceptance gate. Items 1–3 need only the existing
motion capture; item 5 may need a fresh capture that starts from a cold tracker to
exercise bootstrap; item 6 needs a SteamVR-synchronized capture.
