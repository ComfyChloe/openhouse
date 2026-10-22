// Hardware diagnostic: bypass OpenVR and tracking, test receiver vibration writes.
#include "usb_transport.hpp"
#include "platform/platform.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Stats { size_t n = 0; double sum[3]{}, squared = 0; };
struct Sink : lh::InputSinkBase {
  std::string target;
  std::mutex mutex;
  std::atomic<int> phase{0};
  std::vector<Stats> stats;
  size_t written = 0, failures = 0;
  void OnImu(const lh::DeviceId& id, const lh::ImuSample& s) override {
    if (id != target) return;
    std::lock_guard lock(mutex);
    auto& v = stats.at(size_t(phase.load()));
    ++v.n;
    for (int i = 0; i < 3; ++i) { v.sum[i] += s.accel[size_t(i)]; v.squared += s.accel[size_t(i)] * s.accel[size_t(i)]; }
  }
  void Result(const lh::usb::DeviceCommandResult& r) {
    if (r.serial != target) return;
    std::lock_guard lock(mutex);
    written += std::string(r.status) == "written";
    failures += std::string(r.status) != "written";
    nlohmann::json j = {{"serial", r.serial}, {"status", r.status}, {"hid_bytes", r.bytes},
                       {"on_us", r.report[4] | r.report[5] << 8}, {"off_us", r.report[6] | r.report[7] << 8},
                       {"repeats", r.report[8] | r.report[9] << 8}, {"phase", phase.load()}};
    std::puts(j.dump().c_str()); std::fflush(stdout);
  }
};
void Wait(double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }
}
int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help") {
      std::puts("usage: lhhaptics SERIAL [--frequency HZ] [--amplitude 0..1] [--duration 0..2] [--pulses 1..10] [--capture OUT.lhraw]\n"
                "Close SteamVR/Monado/lhviz first. Sends short vibration bursts with quiet intervals.\n"
                "Logs actual HID write results and per-phase acceleration variation. A write is NOT a motor acknowledgement.");
      return argc < 2 ? 2 : 0;
    }
    Sink sink; sink.target = argv[1];
    double frequency = 100, amplitude = .8, duration = .5;
    int pulses = 3;
    lh::usb::UsbTransportOptions options;
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (i + 1 >= argc) throw std::runtime_error("missing option value");
      const std::string value = argv[++i];
      if (a == "--frequency") frequency = std::stod(value);
      else if (a == "--amplitude") amplitude = std::stod(value);
      else if (a == "--duration") duration = std::stod(value);
      else if (a == "--pulses") pulses = std::stoi(value);
      else if (a == "--capture") options.capturePath = value;
      else throw std::runtime_error("unknown option: " + a);
    }
    if (!std::isfinite(frequency) || frequency < 0 || frequency > 3333 ||
        !std::isfinite(amplitude) || amplitude <= 0 || amplitude > 1 ||
        !std::isfinite(duration) || duration < 0 || duration > 2 || pulses < 1 || pulses > 10)
      throw std::runtime_error("invalid bounds");
    if (lh::platform::VrserverRunning()) throw std::runtime_error("close SteamVR first");
    if (!options.capturePath.empty() && std::filesystem::exists(options.capturePath)) throw std::runtime_error("capture already exists");
    sink.stats.resize(size_t(2 * pulses + 1));
    options.onCommandResult = [&](const auto& r) { sink.Result(r); };
    lh::usb::UsbTransport transport(&sink, options);
    if (!transport.Start()) throw std::runtime_error("no receivers could be opened");
    // Wait for config/protocol setup and one second of the actual target's IMU.
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < deadline) {
      { std::lock_guard lock(sink.mutex); if (sink.stats[0].n >= 200) break; }
      Wait(.02);
    }
    { std::lock_guard lock(sink.mutex); if (!sink.stats[0].n) throw std::runtime_error("target has no IMU data; check power/serial/receiver"); }
    for (int i = 0; i < pulses; ++i) {
      sink.phase = 2 * i + 1;
      transport.Haptic(sink.target, frequency, amplitude, duration);
      Wait(std::max(duration, .05) + .05);
      sink.phase = 2 * i + 2;
      Wait(.7);
    }
    transport.Stop();
    for (size_t i = 0; i < sink.stats.size(); ++i) {
      const auto& s = sink.stats[i];
      double variance = s.n ? s.squared / s.n : 0;
      for (double mean : s.sum) if (s.n) variance -= (mean / s.n) * (mean / s.n);
      const nlohmann::json j = {{"phase", i}, {"kind", i % 2 ? "pulse" : "quiet"}, {"imu_samples", s.n},
                               {"accel_rms_about_mean_mps2", std::sqrt(std::max(0., variance))}};
      std::puts(j.dump().c_str());
    }
    return sink.failures == 0 && sink.written == size_t(pulses) ? 0 : 1;
  } catch (const std::exception& e) { std::fprintf(stderr, "lhhaptics: %s\n", e.what()); return 1; }
}
