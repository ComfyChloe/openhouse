// Diagnostic: find out which haptic packet shape makes a Watchman device vibrate.
// The stock 0x3cb9a0 payload is undocumented internally; this A/Bs possible
// interpretations against the older libsurvive framing. Watch and note which
// attempt buzzes. Not part of the driver.
#include "usb_transport.hpp"
#include "lh/session.hpp"
#include "platform/platform.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Sink : lh::InputSinkBase {
  std::string target; int imu = 0;
  void OnImu(const lh::DeviceId& id, const lh::ImuSample&) override { if (id == target) ++imu; }
};
std::array<uint8_t, 64> MakePayload(std::initializer_list<uint8_t> bytes) {
  std::array<uint8_t, 64> r{}; r[0] = 0xff;
  size_t i = 1; for (uint8_t b : bytes) if (i < r.size()) r[i++] = b;
  return r;
}
void Put16(std::array<uint8_t, 64>& r, size_t at, unsigned v) { r[at] = uint8_t(v); r[at + 1] = uint8_t(v >> 8); }
}

int main(int argc, char** argv) {
  if (argc < 2) { std::puts("usage: lhidentify SERIAL [--stop-at N] [--delay seconds]\n"
    "Close SteamVR first. Attempts 1..6 = hypothesis for the stock ff 8f 08 payload.\n"
    "Writes each attempt's exact bytes, then a zero pulse, waiting between them.\n"
    "Tell me the number of the attempt that buzzes."); return 2; }
  const std::string serial = argv[1];
  int stopAt = 6; double delay = 1.5;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--stop-at") stopAt = std::atoi(argv[i + 1]);
    else if (std::string(argv[i]) == "--delay") delay = std::atof(argv[i + 1]);
  }
  if (lh::platform::VrserverRunning()) { std::puts("close SteamVR first"); return 1; }
  std::vector<std::array<uint8_t, 64>> reports;
  using R = std::array<uint8_t, 64>;
  // Observing 0x8f means 100 ms; try the plausible field interpretations.
  R a = MakePayload({0x8f, 0x08, 100}); Put16(a, 4, 4000); Put16(a, 6, 6000); Put16(a, 8, 30); a[10] = 0xff; reports.push_back(a);
  R b = MakePayload({0x8f, 0x08, 0});    Put16(b, 4, 400);  Put16(b, 6, 600);  Put16(b, 8, 20); b[10] = 0xff; reports.push_back(b);
  R c = MakePayload({0x8f, 30, 0x08, 100}); Put16(c, 4, 4000); Put16(c, 6, 6000); Put16(c, 8, 1); c[10] = 0xff; reports.push_back(c);
  R d = MakePayload({0x8f, 0x08, 100}); Put16(d, 4, 30); Put16(d, 6, 4000); Put16(d, 8, 6000); d[10] = 1; reports.push_back(d);
  R e = MakePayload({0x8f, 0x07, 0}); Put16(e, 4, 4000); Put16(e, 6, 6000); Put16(e, 8, 30); reports.push_back(e);
  R f = MakePayload({0x8f, 0x08, 100}); Put16(f, 4, 4000); Put16(f, 6, 6000); Put16(f, 8, 30); Put16(f, 10, 0xff); reports.push_back(f);
  Sink sink; sink.target = serial;
  lh::usb::UsbTransportOptions options;
  options.onCommandResult = [](const lh::usb::DeviceCommandResult& r) {
    std::printf("  write %s: %s (%d bytes)", r.serial.c_str(), r.status, r.bytes);
    std::putchar('\n'); std::fflush(stdout);
  };
  lh::usb::UsbTransport transport(&sink, options);
  if (!transport.Start()) { std::puts("no receiver opened"); return 1; }
  const auto wait = [&](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); };
  for (int k = 0; k < 25 && !sink.imu; ++k) wait(.05);
  if (!sink.imu) { std::puts("device not attached / no IMU; check serial and power"); transport.Stop(); return 1; }
  for (int i = 0; i < int(reports.size()) && i < stopAt; ++i) {
    std::printf("attempt %d bytes:", i + 1);
    for (int j = 0; j < 12; ++j) std::printf(" %02x", reports[size_t(i)][size_t(j)]);
    std::putchar('\n'); std::fflush(stdout);
    transport.Haptic(serial, 100, .8, .4);
    wait(delay);
    transport.Haptic(serial, 0, 0, 0);
    wait(.5);
  }
  transport.Stop();
  return 0;
}
