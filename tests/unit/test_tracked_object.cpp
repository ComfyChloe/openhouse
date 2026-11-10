// Tests for the learned IMU calibration fallback.
//
// A device's own config is authoritative for its accelerometer scale and bias. Some
// devices carry none - a Tundra Tracker's config has no acc_scale, acc_bias or
// gyro_bias at all - and SteamVR has learned values for those serials. The fallback
// fills that gap and only that gap.
#include <gtest/gtest.h>

#include <string>

#include "tracking/tracked_object.hpp"

namespace {

// A minimal device config: the optical model is required by the parser, the IMU
// calibration block is what these tests are about.
std::string ConfigWithImu(const std::string& imuBlock) {
  return R"({
    "device_serial_number": "LHR-TEST0001",
    "device_class": "generic_tracker",
    "model_number": "Test Tracker",
    "lighthouse_config": {
      "modelPoints": [[0,0,0],[0.01,0,0]],
      "modelNormals": [[0,0,1],[0,0,1]]
    },
    "imu": {)" + imuBlock + R"(}
  })";
}

std::string LearnedDb() {
  return R"({
    "known_objects": [
      { "serialNumber": "LHR-TEST0001",
        "imu": { "acc_scale": [0.999, 0.998, 1.001], "acc_bias": [0.01, -0.02, 0.03],
                 "gyro_bias": [0.001, -0.002, 0.003] } },
      { "serialNumber": "LHR-OTHER0001",
        "imu": { "acc_scale": [0.5, 0.5, 0.5], "acc_bias": [0.5, 0.5, 0.5],
                 "gyro_bias": [0.5, 0.5, 0.5] } }
    ]
  })";
}

TEST(LearnedImuCalibration, AConfigWithCalibrationIsMarkedAsHavingOne) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(
      ConfigWithImu(R"("acc_scale": [1,1,1], "acc_bias": [0,0,0], "gyro_bias": [0,0,0])"), &model, &error))
      << error;
  EXPECT_TRUE(model.hasImuCalibration);
}

TEST(LearnedImuCalibration, AConfigWithoutCalibrationIsMarkedAsHavingNone) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(ConfigWithImu(R"("position": [0,0,0])"), &model, &error)) << error;
  EXPECT_FALSE(model.hasImuCalibration);
  // The defaults are indistinguishable from real values, which is why the presence
  // flag exists at all.
  EXPECT_EQ(model.accScale[0], 1.0);
  EXPECT_EQ(model.accBias[0], 0.0);
}

TEST(LearnedImuCalibration, FillsTheGapWhenTheDeviceHasNone) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(ConfigWithImu(R"("position": [0,0,0])"), &model, &error)) << error;
  ASSERT_TRUE(lh::ApplyLearnedImuCalibration(&model, LearnedDb(), "LHR-TEST0001", &error)) << error;
  EXPECT_TRUE(model.hasImuCalibration);
  EXPECT_DOUBLE_EQ(model.accScale[0], 0.999);
  EXPECT_DOUBLE_EQ(model.accScale[2], 1.001);
  EXPECT_DOUBLE_EQ(model.accBias[1], -0.02);
  EXPECT_DOUBLE_EQ(model.gyroBias[2], 0.003);
}

TEST(LearnedImuCalibration, TheDeviceOwnsValuesWin) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(
      ConfigWithImu(R"("acc_scale": [0.9,0.9,0.9], "acc_bias": [0.1,0.1,0.1], "gyro_bias": [0,0,0])"), &model, &error))
      << error;
  EXPECT_FALSE(lh::ApplyLearnedImuCalibration(&model, LearnedDb(), "LHR-TEST0001", &error));
  EXPECT_DOUBLE_EQ(model.accScale[0], 0.9);
  EXPECT_DOUBLE_EQ(model.accBias[0], 0.1);
}

TEST(LearnedImuCalibration, AnotherSerialIsNotBorrowed) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(ConfigWithImu(R"("position": [0,0,0])"), &model, &error)) << error;
  EXPECT_FALSE(lh::ApplyLearnedImuCalibration(&model, LearnedDb(), "LHR-NOSUCH0001", &error));
  EXPECT_FALSE(model.hasImuCalibration);
  EXPECT_DOUBLE_EQ(model.accScale[0], 1.0);
}

TEST(LearnedImuCalibration, AMalformedDatabaseIsRefusedRatherThanApplied) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(ConfigWithImu(R"("position": [0,0,0])"), &model, &error)) << error;
  EXPECT_FALSE(lh::ApplyLearnedImuCalibration(&model, "{not json", "LHR-TEST0001", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(model.hasImuCalibration);
}

TEST(LearnedImuCalibration, AnImpossibleScaleIsRefused) {
  lh::TrackedObjectModel model;
  std::string error;
  ASSERT_TRUE(lh::TrackedObjectModel::FromJson(ConfigWithImu(R"("position": [0,0,0])"), &model, &error)) << error;
  const std::string bad = R"({"known_objects":[{"serialNumber":"LHR-TEST0001",
      "imu":{"acc_scale":[0,-1,1],"acc_bias":[0,0,0],"gyro_bias":[0,0,0]}}]})";
  EXPECT_FALSE(lh::ApplyLearnedImuCalibration(&model, bad, "LHR-TEST0001", &error));
  EXPECT_FALSE(model.hasImuCalibration);
}

}  // namespace
