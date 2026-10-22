// Index ("Knuckles") controller input decoding.
//
// The wireless transport frames input into buttons / trigger / 2 axes / an 8-byte
// extended record. Their meaning (from libsurvive driver_vive.c, RFKnuckles and
// handle_input, cross-checked on a recorded Knuckles stream):
//   buttons bit 0 trigger click, 1 trackpad touch, 2 thumbstick click, 3 system,
//           bit 4 A, bit 5 B
//   trigger byte / 255
//   axes    int16 x, y / 32768: trackpad while the trackpad is touched, else thumbstick
//   extended record 0xA1: touch flags (0x01 trigger, 0x08 system, 0x10 A, 0x20 B,
//           0x40 thumbstick), finger proximity middle, ring, pinky, index,
//           grip force, trackpad force (each byte / 255)
// Packets carry only the parts that changed, so the decoder keeps state.
#pragma once

#include "lh/session.hpp"
#include "lh/types.hpp"

namespace lh::runtime {

class IndexInputDecoder {
 public:
  // Returns true if the visible state changed.
  bool Apply(const InputReport& report, double hostTime);
  const ControllerInput& State() const { return state_; }

 private:
  ControllerInput state_;
};

}  // namespace lh::runtime
