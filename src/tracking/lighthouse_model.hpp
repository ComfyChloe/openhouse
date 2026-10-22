// Lighthouse 1.0 measurement model.
//
// Port of vortex::CLighthouseModel::Functors<...>::CSensorPositionAngular, model
// type 1 (double evaluate: axis 0 = 0x376380, axis 1 = 0x375a20; Jet versions
// 0x39ab80 / 0x3983b0). Templated on the scalar so the same code yields values
// (double) and Jacobians (ceres::Jet). Operation order follows the binary.
//
// Frames: the object state is expressed in the frame of the base station that
// produced the measurement (see ai/driver/measurement_models.md).
#pragma once

#include <cmath>

namespace lh::model {

// Per-axis base station calibration as stored by the driver (7 doubles per axis,
// axis 1 starts at index 7). Only the first 5 are used by the LH1 model.
enum CalIndex { kTilt = 0, kPhase = 1, kCurve = 2, kGibPhase = 3, kGibMag = 4, kOgeePhase = 5, kOgeeMag = 6 };
constexpr int kCalPerAxis = 7;

// Object state layout used by the model (CLighthouseDirect::CStateTraits).
// Index into the 49-double state: pos 0..2, vel 3..5, acc 6..8, quat (w,x,y,z) 9..12,
// angular velocity 13..15.
enum StateIndex { kPos = 0, kVel = 3, kAcc = 6, kQuat = 9, kOmega = 13 };

// Extrapolates position and orientation by dt (0x376380 prologue).
//   p' = (0.5 a) dt² + (v dt + p)
//   q' = q ⊗ [cos(|ω dt|/2), sin(|ω dt|/2) ω dt / |ω dt|]
template <typename T>
void ExtrapolatePose(const T* s, double dt, T pos[3], T quat[4]) {
  using std::sin;
  using std::cos;
  using std::sqrt;
  if (dt == 0.0) {
    for (int i = 0; i < 3; i++) pos[i] = s[kPos + i];
    for (int i = 0; i < 4; i++) quat[i] = s[kQuat + i];
    return;
  }
  const T qw = s[9], qx = s[10], qy = s[11], qz = s[12];
  T wx = s[13] * dt, wy = s[14] * dt, wz = s[15] * dt;
  T n2 = wx * wx + wy * wy + wz * wz;
  T c = T(1.0), k = T(0.5);
  if (n2 > T(0.0)) {
    T n = sqrt(n2);
    c = cos(n * 0.5);
    k = sin(n * 0.5) / n;
  }
  wz = k * wz;
  wy = wy * k;
  wx = wx * k;
  quat[0] = (qw * c - (qx * wx + wy * qy)) - wz * qz;
  quat[1] = (qy * wz + qw * wx + qx * c) - qz * wy;
  quat[2] = (qw * wy + c * qy + qz * wx) - wz * qx;
  quat[3] = (qw * wz + qx * wy + qz * c) - qy * wx;
  const double dt2 = dt * dt;
  for (int i = 0; i < 3; i++) pos[i] = (s[kAcc + i] * 0.5) * dt2 + (s[kVel + i] * dt + s[kPos + i]);
}

// Rotates p by unit quaternion q = (w,x,y,z) (0x356e80).
template <typename T, typename P>
void QuatRotate(const T q[4], const P p[3], T out[3]) {
  const T w = q[0], x = q[1], y = q[2], z = q[3];
  const T nx2 = -(x * x);
  T d = ((x * y - w * z) * p[1] - (y * y + z * z) * p[0]) + (w * y + x * z) * p[2];
  out[0] = d + d + p[0];
  d = (y * z - w * x) * p[2] + (nx2 - z * z) * p[1] + (w * z + x * y) * p[0];
  out[1] = d + d + p[1];
  T e = p[1] * (w * x + y * z) + (x * z - w * y) * p[0] + (nx2 - y * y) * p[2];
  out[2] = e + e + p[2];
}

// LH1 sweep angle of point (x,y,z) in base frame for one axis (model type 1).
// cal points at the 7 doubles of that axis.
template <typename T>
T Lh1Angle(int axis, const T& x, const T& y, const T& z, const double* cal) {
  using std::asin;
  using std::atan2;
  using std::cos;
  using std::sqrt;
  using std::tan;
  const T nz = -z;
  if (axis == 0) {
    T a = atan2(nz, x) - cal[kPhase];
    a = a - asin((tan(T(cal[kTilt])) * y) / sqrt(z * z + x * x));
    a = a - cos(a + cal[kGibPhase]) * cal[kGibMag];
    const T d = atan2(y, nz);
    return (cal[kCurve] * d) * d + a;
  }
  T a = atan2(nz, -y) - cal[kPhase];
  a = a - asin((tan(T(cal[kTilt])) * x) / sqrt(z * z + y * y));
  a = a - cos(a + cal[kGibPhase]) * cal[kGibMag];
  const T d = atan2(x, nz);
  return (cal[kCurve] * d) * d + a;
}

// Full CSensorPositionAngular model type 1: state -> predicted angle.
// sensor: sensor position in object frame; cal14: both axes' calibration.
template <typename T>
T PredictLh1Angle(const T* state, double dt, const double sensor[3], int axis, const double* cal14) {
  T pos[3], q[4], r[3];
  ExtrapolatePose(state, dt, pos, q);
  QuatRotate(q, sensor, r);
  const T x = r[0] + pos[0], y = r[1] + pos[1], z = r[2] + pos[2];
  return Lh1Angle(axis, x, y, z, cal14 + axis * kCalPerAxis);
}

}  // namespace lh::model
