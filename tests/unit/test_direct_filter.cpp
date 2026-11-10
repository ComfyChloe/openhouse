// Tests for DirectFilter's correction-rejection diagnostics.
//
// The EKF maths, the limits and their order are covered by the oracle tests; what is
// tested here is the diagnostic layer added on top of RejectCorrection: which limit
// refused a frame, and (for every limit) how far over it the correction was. A log
// that says "rejected" without that cannot tell a threshold problem from a calibration
// problem, which is exactly what it is for.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "tracking/direct_filter.hpp"

namespace {

constexpr double kDeg = 0.017453292519943295;

lh::direct::DirectFilter PreConvergence() {
  lh::direct::DirectFilter f;
  f.updateCount = 0;  // the 10 deg / 0.05 g limits
  return f;
}

lh::direct::Vec Zero() { return lh::direct::Vec::Zero(49); }

// Every refusal must leave exactly one reason and, where the limit is a value, the
// value and the limit it was compared with. A stale reason between calls is the
// failure mode this guards: a log would attribute one frame's rejection to the next.
TEST(RejectDiagnostics, AcceptsAGoodCorrectionAndClearsTheReason) {
  auto f = PreConvergence();
  const auto dx = Zero();
  EXPECT_FALSE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectNone);
  EXPECT_EQ(f.lastRejectSlot, -1);
  EXPECT_DOUBLE_EQ(f.lastRejectOver, 0.0);
  EXPECT_DOUBLE_EQ(f.lastRejectLimit, 0.0);
}

TEST(RejectDiagnostics, NamesANonFiniteCorrection) {
  auto f = PreConvergence();
  auto dx = Zero();
  dx[0] = std::nan("");
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectNaN);
  EXPECT_EQ(f.lastRejectSlot, -1);
}

TEST(RejectDiagnostics, ImuRotationReportsTheAngleAndItsLimit) {
  auto f = PreConvergence();
  auto dx = Zero();
  dx[28] = 12.0 * kDeg;
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectImuRot);
  EXPECT_NEAR(f.lastRejectOver, 12.0 * kDeg, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 10.0 * kDeg, 1e-9);
}

TEST(RejectDiagnostics, ImuRotationUsesTheConvergedLimitAfterConvergence) {
  auto f = PreConvergence();
  f.updateCount = 300;  // past 0x105: the converged limits
  auto dx = Zero();
  dx[28] = 3.0 * kDeg;  // under 10 deg, over the converged 2 deg
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectImuRot);
  EXPECT_NEAR(f.lastRejectOver, 3.0 * kDeg, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 2.0 * kDeg, 1e-9);
}

TEST(RejectDiagnostics, GravityMagnitudeReportsTheOffsetAndItsLimit) {
  auto f = PreConvergence();
  auto dx = Zero();
  dx[18] = 0.6;  // m/s^2 off 9.80665
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectGravMag);
  EXPECT_NEAR(f.lastRejectOver, 0.6, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 0.4903325, 1e-9);
}

TEST(RejectDiagnostics, GravityTiltReportsTheTiltAndItsLimit) {
  auto f = PreConvergence();
  f.updateCount = 300;  // below 0xc9 the tilt test is skipped
  auto dx = Zero();
  dx[16] = 0.1;  // ~5.7 deg of tilt
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectGravTilt);
  EXPECT_NEAR(f.lastRejectOver, 0.1, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 0.06981317007977318, 1e-9);
}

TEST(RejectDiagnostics, AngularRateReportsTheRateAndItsLimit) {
  auto f = PreConvergence();
  f.omegaCorrectionLimit2 = 0.01;
  auto dx = Zero();
  dx[13] = 0.2;
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectOmega);
  EXPECT_NEAR(f.lastRejectOver, 0.2, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 0.1, 1e-9);
}

TEST(RejectDiagnostics, AlternateBasePositionReportsTheSlotAndTheDistance) {
  auto f = PreConvergence();
  f.altBaseActive[0] = true;
  auto dx = Zero();
  dx[lh::direct::kAltBaseIndex[0] + 1] = 0.2;  // 20 cm against a 10 cm limit
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectAltBaseTrans);
  EXPECT_EQ(f.lastRejectSlot, 0);
  EXPECT_NEAR(f.lastRejectOver, 0.2, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 0.1, 1e-9);
}

TEST(RejectDiagnostics, AlternateBaseRotationReportsTheSlotAndTheAngle) {
  auto f = PreConvergence();
  f.altBaseActive[0] = true;
  auto dx = Zero();
  dx[lh::direct::kAltBaseIndex[0] + 3] = 2.0 * kDeg;
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectAltBaseRot);
  EXPECT_EQ(f.lastRejectSlot, 0);
  EXPECT_NEAR(f.lastRejectOver, 2.0 * kDeg, 1e-9);
  EXPECT_NEAR(f.lastRejectLimit, 1.0 * kDeg, 1e-9);
}

// The checks run in a fixed order, and the diagnostic must report the one that actually
// refused the frame, not a later one also over its limit.
TEST(RejectDiagnostics, ReportsTheFirstLimitThatRefusesTheFrame) {
  auto f = PreConvergence();
  f.altBaseActive[0] = true;
  auto dx = Zero();
  dx[28] = 12.0 * kDeg;                         // IMU rotation, checked first
  dx[lh::direct::kAltBaseIndex[0] + 3] = 2.0 * kDeg;  // also over its own limit
  ASSERT_TRUE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectImuRot);
  EXPECT_EQ(f.lastRejectSlot, -1);
}

TEST(RejectDiagnostics, ACleanFrameAfterARefusalClearsTheSlotAndTheValue) {
  auto f = PreConvergence();
  f.altBaseActive[0] = true;
  auto dx = Zero();
  dx[lh::direct::kAltBaseIndex[0] + 3] = 2.0 * kDeg;
  ASSERT_TRUE(f.RejectCorrection(dx));
  dx.setZero();
  EXPECT_FALSE(f.RejectCorrection(dx));
  EXPECT_EQ(f.lastRejectReason, lh::direct::kRejectNone);
  EXPECT_EQ(f.lastRejectSlot, -1);
  EXPECT_DOUBLE_EQ(f.lastRejectOver, 0.0);
  EXPECT_DOUBLE_EQ(f.lastRejectLimit, 0.0);
}

// A refused correction has to be printed in the unit its limit is written in: the
// rotation limits are radians in the filter, an alternate base's position is metres.
// Printing 0.03 "deg" for a radian limit - or a radian limit as "cm" - would make the
// number unreadable, which is the whole point of recording it.
TEST(RejectDiagnostics, RefusalUnitsMatchTheLimitTheyWereComparedWith) {
  using lh::direct::RejectUnitsFor;
  const auto imuRot = RejectUnitsFor(lh::direct::kRejectImuRot);
  EXPECT_DOUBLE_EQ(imuRot.scale, 57.29577951308232);
  EXPECT_STREQ(imuRot.unit, "deg");
  const auto gravTilt = RejectUnitsFor(lh::direct::kRejectGravTilt);
  EXPECT_STREQ(gravTilt.unit, "deg");
  const auto altRot = RejectUnitsFor(lh::direct::kRejectAltBaseRot);
  EXPECT_STREQ(altRot.unit, "deg");
  const auto altTrans = RejectUnitsFor(lh::direct::kRejectAltBaseTrans);
  EXPECT_DOUBLE_EQ(altTrans.scale, 100.0);
  EXPECT_STREQ(altTrans.unit, "cm");
  const auto gravMag = RejectUnitsFor(lh::direct::kRejectGravMag);
  EXPECT_DOUBLE_EQ(gravMag.scale, 1.0);
  EXPECT_STREQ(gravMag.unit, "m/s2");
  const auto omega = RejectUnitsFor(lh::direct::kRejectOmega);
  EXPECT_DOUBLE_EQ(omega.scale, 1.0);
  EXPECT_STREQ(omega.unit, "rad/s");
}

// Three optical position rows plus the final non-optical normalizer row. The
// optical correction is about 35 cm, and all three rows are outliers at the
// configured sigma, so the whole-frame gate is the only thing that can refuse it.
std::vector<lh::direct::Residual> OpticalPositionResidual() {
  std::vector<lh::direct::Residual> res(2);
  res[0].m = 3;
  res[0].eval = [](const double* x, double* out) {
    for (int i = 0; i < 3; ++i) out[i] = x[i];
  };
  res[0].evalJet = [](const lh::direct::Jet* x, lh::direct::Jet* out) {
    for (int i = 0; i < 3; ++i) out[i] = x[i];
  };
  res[1].m = 1;
  res[1].eval = [](const double*, double* out) { out[0] = 0; };
  res[1].evalJet = [](const lh::direct::Jet*, lh::direct::Jet* out) {
    out[0] = lh::direct::Jet(0.0);
  };
  return res;
}

// A zero IMU-style update: gate=false and no optical rows. It must not clear a
// run of refused optical frames; only an applied optical (gated) update may.
std::vector<lh::direct::Residual> ImuResidual() {
  std::vector<lh::direct::Residual> res(1);
  res[0].m = 1;
  res[0].eval = [](const double*, double* out) { out[0] = 0; };
  res[0].evalJet = [](const lh::direct::Jet*, lh::direct::Jet* out) {
    out[0] = lh::direct::Jet(0.0);
  };
  return res;
}

TEST(DirectFilter, FrameGateRunSurvivesImuUpdatesUntilAnOpticalFrameIsApplied) {
  lh::direct::DirectFilter f;
  f.DefaultState();
  std::copy(std::begin(f.x), std::end(f.x), f.xLin);
  f.Ppred = lh::direct::Mat::Identity(f.n, f.n);
  f.gateSigma = 0.01;
  f.updateCount = 200;

  const auto optical = OpticalPositionResidual();
  const auto imu = ImuResidual();
  const lh::direct::Mat opticalR = 1e-9 * lh::direct::Mat::Identity(4, 4);
  lh::direct::Vec opticalZ(4);
  opticalZ << 0.2, 0.2, 0.2, 0.0;
  const lh::direct::Mat imuR = lh::direct::Mat::Identity(1, 1);
  const lh::direct::Vec imuZ = lh::direct::Vec::Zero(1);

  for (int frame = 1; frame <= lh::direct::DirectFilter::kMaxFrameRejectRun; ++frame) {
    EXPECT_FALSE(f.Update(optical, opticalR, opticalZ, true)) << "optical frame " << frame;
    EXPECT_EQ(f.frameRejectRun, frame) << "optical frame " << frame;
    ASSERT_TRUE(f.Update(imu, imuR, imuZ, false)) << "IMU sample after frame " << frame;
    EXPECT_EQ(f.frameRejectRun, frame)
        << "an ungated IMU sample cleared the refused-optical run at frame " << frame;
  }
  EXPECT_EQ(f.rejectedFrames, 6u);

  // The cap is meant to let the light win when the filter itself is stuck wrong.
  // Before the fix, every intervening IMU sample reset the run, so this seventh
  // frame was refused too and the cap could never be reached.
  EXPECT_TRUE(f.Update(optical, opticalR, opticalZ, true));
  EXPECT_EQ(f.frameRejectRun, 0);
  EXPECT_EQ(f.rejectedFrames, 6u);
}

}  // namespace
