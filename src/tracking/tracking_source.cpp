#include <cstdio>
#include <cstdlib>
#include "tracking_source.hpp"

#include <cstdlib>
#include <string>

namespace lh {

TrackingSource::TrackingSource(int sensorType, const tdm::TdmConfig& config, bool inferSensorType)
    : inferSensorType_(inferSensorType), tdm_(config, sensorType), multi_(sensorType, [] {
        const char* v = std::getenv("LH_MULTISTATION");
        if (!v) return tdm::MultiDisambiguator::Mode::Auto;
        const std::string s = v;
        return s == "off" || s == "0" ? tdm::MultiDisambiguator::Mode::Off
             : s == "on" || s == "1"  ? tdm::MultiDisambiguator::Mode::On
                                      : tdm::MultiDisambiguator::Mode::Auto;
      }()) {}

void TrackingSource::OnImu(const ImuSample& sample) {
  std::lock_guard lock(mutex_);
  // SwSync_OnImu (0x3e2d90) snapshots these before Source_PushImu.
  const double previousImuTime = latestImuTime_;
  QueuedImu queued{sample.time, {}, {}, sample.saturated};
  for (int i = 0; i < 3; ++i) {
    queued.accel[size_t(i)] = float(sample.accel[size_t(i)]);
    queued.gyro[size_t(i)] = float(sample.gyro[size_t(i)]);
  }
  if (imu_.size() >= 1000) imu_.pop_front();
  imu_.push_back(queued);
  latestImuTime_ = sample.time;

  // Equality means a pulse arrived after the previous IMU sample. Do not
  // advance in that case. Keep the strict comparisons and zero sentinel.
  if (lastPulseImuTime_ != previousImuTime &&
      (nextAdvanceCheck_ == 0 || sample.time > nextAdvanceCheck_)) {
    if (lastPulseImuTime_ + 0.02 < sample.time) {
      tdm_.Advance(sample.time - 0.02);
      multi_.Advance(sample.time - 0.02);
    }
    // Once enough sync widths are seen, re-derive the classifier offset from them.
    else if (inferSensorType_ && tdm_.SensorType() == 0 && !sample.saturated) tdm_.RetrySensorType();
    nextAdvanceCheck_ = sample.time + 0.05;
  }
}

void TrackingSource::OnLightPulse(const LightPulse& pulse) {
  std::lock_guard lock(mutex_);
  lastPulseImuTime_ = latestImuTime_;
  const int type = tdm_.SensorType();
  tdm_.OnPulse(pulse.sensor, pulse.time, pulse.width);
  if (tdm_.SensorType() != type) multi_.SetSensorType(tdm_.SensorType());
  multi_.OnPulse(pulse.sensor, pulse.time, pulse.width);
  if (multi_.Active()) {  // the stock path's frames would be mis-attributed; drop them
    while (tdm_.PopFrame(nullptr)) {}
  }
}

bool TrackingSource::PopImu(ImuSample* out) {
  std::lock_guard lock(mutex_);
  if (imu_.empty()) return false;
  const auto& sample = imu_.front();
  out->time = sample.time;
  out->saturated = sample.saturated;
  for (int i = 0; i < 3; ++i) {
    out->accel[size_t(i)] = sample.accel[size_t(i)];
    out->gyro[size_t(i)] = sample.gyro[size_t(i)];
  }
  imu_.pop_front();
  return true;
}

bool TrackingSource::PeekImuTime(DeviceTime* out) const {
  std::lock_guard lock(mutex_);
  if (imu_.empty()) return false;
  *out = imu_.front().time;
  return true;
}

bool TrackingSource::PopFrame(OpticalFrame* out) {
  std::lock_guard lock(mutex_);
  return multi_.Active() ? multi_.PopFrame(out) : tdm_.PopFrame(out);
}

bool TrackingSource::PopBaseConfig(BaseConfig* config, BaseDynamicState* dynamic) {
  std::lock_guard lock(mutex_);
  if (multi_.Active()) {
    while (tdm_.PopBaseConfig(nullptr, nullptr)) {}
    return multi_.PopBaseConfig(config, dynamic);
  }
  return tdm_.PopBaseConfig(config, dynamic);
}

bool TrackingSource::PopEvent(tdm::TdmEvent* out) {
  std::lock_guard lock(mutex_);
  return tdm_.PopEvent(out);
}

bool TrackingSource::PopLog(std::string* out) {
  std::lock_guard lock(mutex_);
  if (multi_.PopLog(out)) { *out = "[multi] " + *out; return true; }
  if (multi_.Active()) {  // the stock path re-acquiring in a multi room ("tdm sync acquired", "max of 2"): noise
    while (tdm_.PopLog(nullptr)) {}
    return false;
  }
  return tdm_.PopLog(out);
}

void TrackingSource::SetKnownIdentities(std::vector<tdm::TdmDisambiguator::Identity> identities) {
  std::lock_guard lock(mutex_);
  multi_.SetKnownIdentities(identities);
  tdm_.SetKnownIdentities(std::move(identities));
}

bool TrackingSource::PopIdentity(tdm::TdmDisambiguator::Identity* out) {
  std::lock_guard lock(mutex_);
  return multi_.PopIdentity(out) || tdm_.PopIdentity(out);
}

}  // namespace lh
