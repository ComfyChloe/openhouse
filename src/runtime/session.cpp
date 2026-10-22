// lh::Session implementation: transport -> per-device trackers + shared room map.
#include "lh/session.hpp"
#include "platform/platform.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <thread>

#include "device_role.hpp"
#include "device_tracker.hpp"
#include "index_input.hpp"
#include "lh/input_sink.hpp"
#include "room_map.hpp"
#include "room_solver.hpp"
#include "tracking/tracked_object.hpp"

#ifdef LH_RUNTIME_USB
#include "raw_capture.hpp"
#include "usb_transport.hpp"
#endif

namespace lh {

const char* ToString(DeviceRole role) {
  switch (role) {
    case DeviceRole::Hmd: return "hmd";
    case DeviceRole::Controller: return "controller";
    case DeviceRole::Tracker: return "tracker";
    case DeviceRole::BaseStation: return "base_station";
  }
  return "?";
}

const char* ToString(TrackingState state) {
  switch (state) {
    case TrackingState::Disconnected: return "disconnected";
    case TrackingState::Uninitialized: return "uninitialized";
    case TrackingState::Acquiring: return "acquiring";
    case TrackingState::Tracking: return "tracking";
    case TrackingState::Coasting: return "coasting";
    case TrackingState::Lost: return "lost";
  }
  return "?";
}

namespace runtime {
namespace {
using nlohmann::json;

double SteadyNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string BaseName(uint32_t serial) {
  char text[16];
  std::snprintf(text, sizeof text, "LHB-%08X", serial);
  return text;
}

std::string DefaultRoomMapPath() { return platform::ConfigDirectory() + "/roommap.json"; }
std::string DefaultSettingsPath() { return platform::ConfigDirectory() + "/settings.json"; }

std::string FirstExisting(const std::vector<std::string>& paths) {
  for (const auto& p : paths)
    if (std::filesystem::exists(p)) return p;
  return {};
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

Pose ToPose(const Eigen::Isometry3d& t) {
  Pose p;
  const Eigen::Quaterniond q(t.rotation());
  p.position = {t.translation().x(), t.translation().y(), t.translation().z()};
  p.orientation = {q.w(), q.x(), q.y(), q.z()};
  return p;
}

// Rotation with +X = plus_x, +Z = plus_z (same construction as the IMU block).
bool AxesRotation(const json& block, Eigen::Isometry3d* out) {
  if (!block.contains("plus_x") || !block.contains("plus_z")) return false;
  const auto px = block.at("plus_x").get<std::array<double, 3>>();
  const auto pz = block.at("plus_z").get<std::array<double, 3>>();
  Eigen::Vector3d x(px[0], px[1], px[2]), z(pz[0], pz[1], pz[2]);
  if (x.norm() < 1e-6 || z.norm() < 1e-6 || x.cross(z).norm() < 1e-6) return false;
  x.normalize(); z.normalize();
  Eigen::Matrix3d r;
  r.col(0) = x; r.col(1) = z.cross(x).normalized(); r.col(2) = x.cross(r.col(1)).normalized();
  *out = Eigen::Isometry3d::Identity();
  out->linear() = r;
  if (block.contains("position")) {
    const auto p = block.at("position").get<std::array<double, 3>>();
    out->translation() = Eigen::Vector3d(p[0], p[1], p[2]);
  }
  return true;
}

bool PoseArray(const json& a, Eigen::Isometry3d* out) {  // [qx qy qz qw x y z]
  if (!a.is_array() || a.size() != 7) return false;
  *out = Eigen::Isometry3d::Identity();
  out->linear() = Eigen::Quaterniond(a[3].get<double>(), a[0].get<double>(), a[1].get<double>(), a[2].get<double>())
                      .normalized().toRotationMatrix();
  out->translation() = Eigen::Vector3d(a[4].get<double>(), a[5].get<double>(), a[6].get<double>());
  return true;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
}  // namespace

DeviceRole RoleFromConfig(const std::string& configJson) {
  const json j = json::parse(configJson);
  const std::string cls = Lower(j.value("device_class", ""));
  if (cls == "hmd") return DeviceRole::Hmd;
  if (cls == "controller") {
    const std::string hint = Lower(j.value("model_number", "") + " " + j.value("render_model", "") + " " +
                                   j.value("input_profile", ""));
    if (hint.find("knuckles") != std::string::npos || hint.find("index") != std::string::npos ||
        hint.find("knu_") != std::string::npos)
      return DeviceRole::Controller;
  }
  return DeviceRole::Tracker;
}

class SessionImpl final : public Session, public InputSinkBase {
 public:
  explicit SessionImpl(SessionOptions options) : options_(std::move(options)) {}
  ~SessionImpl() override { Stop(); }

  bool Start(std::string* error) override;
  void Stop() override;
  bool Running() const override { return running_; }
  bool Finished() const override { return finished_; }
  SessionSnapshot Snapshot() const override;
  std::vector<DeviceUpdate> Updates() const override;
  double HostNow() const override {
    std::lock_guard lock(mutex_);
    if (options_.replayPath.empty() && running_) now_ = std::max(now_, SteadyNow());
    return now_;
  }
  std::vector<SessionEvent> PollEvents() override;
  void ResetRoom() override;
  void SetDeviceEnabled(const std::string& serial, bool enabled) override;
  bool Haptic(const std::string& serial, double frequency, double amplitude, double duration) override;
  bool PowerOff(const std::string& serial) override;
  bool SaveRoom(std::string* error) override;

  // IInputSink (transport thread)
  void OnDeviceAttached(const DeviceId&, const std::string& configJson) override;
  void OnDeviceDetached(const DeviceId&) override;
  void OnClockOffset(const DeviceId&, double offset) override;
  void OnImu(const DeviceId&, const ImuSample&) override;
  void OnLightPulse(const DeviceId&, const LightPulse&) override;
  void OnLh2BeamWord(const DeviceId&, const Lh2BeamWord&) override;
  void OnLh2SensorWord(const DeviceId&, const Lh2SensorWord&) override;
  void OnInput(const DeviceId&, const InputReport&) override;
  void OnBattery(const DeviceId&, const BatteryReport&) override;

 private:
  struct Device {
    DeviceInfo info;
    std::unique_ptr<DeviceTracker> tracker;
    std::optional<IndexInputDecoder> input;
    Eigen::Isometry3d imuFromHead = Eigen::Isometry3d::Identity();
    double clockOffset = 0;
    bool haveClock = false;
    bool attached = false;
    double lastData = -1e300;  // host time
    TrackingState lastState = TrackingState::Disconnected;
    // Light mode (live only): 0 = LH1 pulses, 1 = LH2 words; -1 not set yet.
    int lightMode = -1;
    double modeSince = 0, lastGoodLight = -1e300;  // host time
  };
  struct Base {
    BaseConfig config;
    BaseDynamicState dynamic;
    bool seen = false;
  };

  void Emit(SessionEventType type, const std::string& serial, std::string text);
  void Tick() { if (options_.replayPath.empty()) now_ = std::max(now_, SteadyNow()); }
  void Touch(Device& d, double deviceTime);
  void Process(const DeviceId& id, Device& d);
  void UpdateLightMode(const DeviceId& id, Device& d);
  void PushIdentities();
  void TakeKeyframes(Device& d);
  bool SolveDue() const;
  RoomSolveInput SolveInput();
  void ApplySolve(const RoomSolveOutput& out);
  void DebugSplit(const RoomSolveOutput& all);
  void SolverMain();
  void LoadSettings();
  void SaveSettings();
  bool Enabled(const DeviceId& id) const { return !disabled_.count(id); }
  void CollectLogs(Device* d);
  void MaybeSave();
  void ReplayMain();
  DevicePose MakePose(const Device& d) const;
  double HostTime(const Device& d, double deviceTime) const { return deviceTime + d.clockOffset; }

  SessionOptions options_;
  mutable std::mutex mutex_;
  std::map<DeviceId, Device> devices_;
  std::map<uint32_t, Base> bases_;
  std::map<uint32_t, tdm::TdmDisambiguator::Identity> identities_;
  RoomMap room_;
  std::string roomPath_, settingsPath_;
  bool explicitControllers_ = false;
  std::string leftController_, rightController_;
  std::set<std::string> disabled_;
  bool persist_ = false;
  std::deque<SessionEvent> events_;
  mutable double now_ = 0;  // latest host time seen (live: steady clock)
  double lastSave_ = 0;
  std::atomic<bool> running_{false}, finished_{false}, stop_{false};
  std::atomic<bool> warming_{false};  // replay: before the window
  std::thread replay_;
  // Room solver.
  KeyframeStore keyframes_;
  std::map<std::string, std::shared_ptr<const TrackedObjectModel>> models_;
  bool newKeyframes_ = false;
  int addedKeyframes_ = 0;      // since the last SolveInput: the newest entries of the store
  bool syncSolve_ = false;
  double lastSolve_ = -1e300;   // session time of the last solve
  double solveInterval_ = 1.0;  // grows while solves stop moving the bases (see ApplySolve)
  std::map<uint32_t, Eigen::Isometry3d> lastSolved_;  // previous solve's bases
  double lastAdvance_ = -1e300;  // room bases gliding to solved poses
  std::condition_variable solverWake_;
  std::thread solver_;
#ifdef LH_RUNTIME_USB
  std::unique_ptr<usb::UsbTransport> usb_;
#endif
};

bool SessionImpl::Start(std::string* error) {
  if (running_) return true;
  const bool replay = !options_.replayPath.empty();
  std::string err;
  // Room map: replays use an in-memory map unless a path is given explicitly.
  roomPath_ = options_.roomMapPath.empty() && !replay ? DefaultRoomMapPath() : options_.roomMapPath;
  persist_ = options_.saveRoomMap && !roomPath_.empty();
  if (!roomPath_.empty() && !room_.Load(roomPath_, &err)) Emit(SessionEventType::Log, "", err);
  room_.SetFrozen(std::getenv("LH_EVALUATE_ROOM") != nullptr);
  settingsPath_ = options_.settingsPath.empty() && !replay ? DefaultSettingsPath() : options_.settingsPath;
  LoadSettings();
  for (const auto& serial : options_.disabledDevices) disabled_.insert(serial);
  std::string steamDb = options_.steamDatabasePath;
  if (steamDb.empty()) {
    std::vector<std::string> candidates;
    if (replay) candidates.push_back((std::filesystem::path(options_.replayPath).parent_path() / "lighthousedb.json").string());
    else {
      candidates.push_back(platform::SteamConfigDirectory() + "/lighthousedb.json");
#ifndef _WIN32
      candidates.push_back(platform::Home() + "/.steam/steam/config/lighthouse/lighthousedb.json");
      candidates.push_back(platform::Home() + "/.local/share/Steam/config/lighthouse/lighthousedb.json");
#endif
    }
    steamDb = FirstExisting(candidates);
  }
  if (!steamDb.empty() && steamDb != "-") {
    if (!room_.ImportSteamDatabase(ReadFile(steamDb), &err)) Emit(SessionEventType::Log, "", err);
  }
  {
    std::lock_guard lock(mutex_);
    for (const auto& [serial, c] : room_.KnownConfigs()) {
      bases_[serial] = {c.first, c.second, false};
      const auto frame = room_.OotxFrames().find(serial);
      if (frame != room_.OotxFrames().end()) identities_[serial] = {c.first, c.second, frame->second};
    }
    for (auto& text : room_.TakeLogs()) Emit(SessionEventType::Log, "", text);
  }
  stop_ = false;
  finished_ = false;
  syncSolve_ = replay && !options_.replayRealtime;
  if (!syncSolve_) solver_ = std::thread([this] { SolverMain(); });
  if (replay) {
#ifdef LH_RUNTIME_USB
    if (!std::filesystem::exists(options_.replayPath)) {
      if (error) *error = "capture not found: " + options_.replayPath;
      return false;
    }
    running_ = true;
    replay_ = std::thread([this] { ReplayMain(); });
#else
    if (error) *error = "built without the USB/capture layer";
    return false;
#endif
  } else {
#ifdef LH_RUNTIME_USB
    usb::UsbTransportOptions o;
    o.steamLighthouseConfigDir = options_.steamConfigDir;
    o.capturePath = options_.capturePath;
    o.onCommandResult = [this](const usb::DeviceCommandResult& result) {
      if (std::string(result.status) == "written" && !std::getenv("LH_VERBOSE")) return;
      std::lock_guard lock(mutex_);
      std::ostringstream text;
      text << (result.powerOff ? "power-off " : "haptic ") << result.serial << ": " << result.status
           << " (HID bytes=" << result.bytes;
      if (!result.powerOff) text << ", on=" << (result.report[4] | result.report[5] << 8)
                                << ", off=" << (result.report[6] | result.report[7] << 8)
                                << ", repeats=" << (result.report[8] | result.report[9] << 8);
      text << ")";
      Emit(SessionEventType::Log, result.serial, text.str());
    };
    usb_ = std::make_unique<usb::UsbTransport>(this, o);
    if (!usb_->Start()) {
      usb_.reset();
      if (error) *error = "no Lighthouse USB devices could be opened (is SteamVR running?)";
      return false;
    }
    running_ = true;
#else
    if (error) *error = "built without the USB transport";
    return false;
#endif
  }
  if (error) error->clear();
  return true;
}

bool SessionImpl::Haptic(const std::string& serial, double frequency, double amplitude, double duration) {
#ifdef LH_RUNTIME_USB
  std::lock_guard lock(mutex_);
  const auto d = devices_.find(serial);
  if (!running_ || !usb_ || !Enabled(serial) || d == devices_.end() || !d->second.attached ||
      d->second.info.role != DeviceRole::Controller || !std::isfinite(frequency) ||
      !std::isfinite(amplitude) || !std::isfinite(duration)) return false;
  usb_->Haptic(serial, frequency, amplitude, duration);
  return true;
#else
  return false;
#endif
}

bool SessionImpl::PowerOff(const std::string& serial) {
#ifdef LH_RUNTIME_USB
  std::lock_guard lock(mutex_);
  const auto d = devices_.find(serial);
  if (!running_ || !usb_ || !Enabled(serial) || d == devices_.end() || !d->second.attached ||
      (d->second.info.role != DeviceRole::Controller && d->second.info.role != DeviceRole::Tracker)) return false;
  usb_->PowerOff(serial);
  return true;
#else
  return false;
#endif
}

void SessionImpl::Stop() {
  stop_ = true;
#ifdef LH_RUNTIME_USB
  std::unique_ptr<usb::UsbTransport> transport;
  { std::lock_guard lock(mutex_); transport = std::move(usb_); }
  if (transport) transport->Stop();  // callbacks take mutex_: never hold it here
#endif
  if (replay_.joinable()) replay_.join();
  solverWake_.notify_all();
  if (solver_.joinable()) solver_.join();
  if (running_ && persist_) {
    std::string err;
    std::lock_guard lock(mutex_);
    if (room_.Dirty() && room_.Save(roomPath_, &err)) room_.ClearDirty();
  }
  running_ = false;
}

void SessionImpl::ReplayMain() {
#ifdef LH_RUNTIME_USB
  usb::ReportDecoder decoder(this);
  usb::RawCaptureReader reader;
  if (!reader.Open(options_.replayPath)) {
    std::lock_guard lock(mutex_);
    Emit(SessionEventType::Log, "", "cannot open capture " + options_.replayPath);
    finished_ = true;
    return;
  }
  double firstHost = 0, wallStart = 0, captureStart = -1;
  bool first = true;
  std::set<uint16_t> skipped;  // endpoints of devices outside replayOnly
  std::FILE* trace = options_.replayTracePath.empty() ? nullptr : std::fopen(options_.replayTracePath.c_str(), "w");
  double nextTrace = 0;
  const auto& only = options_.replayOnly;
  reader.ReadAll([&](const usb::EndpointInfo& e) {
                   if (!only.empty() && std::find(only.begin(), only.end(), e.serial) == only.end()) { skipped.insert(e.ep); return; }
                   decoder.AddEndpoint(e);
                 },
                 [&](const usb::RawReport& r) {
                   if (stop_) return;
                   static int dbg = 0; if (std::getenv("LH_DEBUG_REPLAY") && dbg++ < 8) fprintf(stderr, "report %.6f ep %u\n", r.hostTime, unsigned(r.ep));
                   if (captureStart < 0 && r.hostTime > 0) captureStart = r.hostTime;  // header-time reports carry 0
                   const double at = captureStart < 0 ? 0 : r.hostTime - captureStart;
                   if (skipped.count(r.ep)) return;
                   // Before the window: decoded (attach, protocol negotiation, clock), but
                   // no IMU or light reaches the trackers.
                   warming_ = at < options_.replayFrom;
                   if (options_.replayTo > 0 && at > options_.replayTo) { stop_ = true; return; }
                   if (options_.replayRealtime && !warming_) {  // the warm-up runs at full speed
                     if (first) { firstHost = r.hostTime; wallStart = SteadyNow(); first = false; }
                     const double wait = (r.hostTime - firstHost) - (SteadyNow() - wallStart);
                     if (wait > 0.0005) platform::SleepFor(std::chrono::microseconds(int64_t(wait * 1e6)));
                   }
                   {
                     std::lock_guard lock(mutex_);
                     now_ = std::max(now_, r.hostTime);
                   }
                   decoder.OnReport(r);
                   if (trace && !warming_ && r.hostTime >= nextTrace) {
                     nextTrace = std::max(nextTrace + 1.0 / options_.replayTraceRate, r.hostTime);
                     for (const auto& u : Updates()) {
                       if (u.role == DeviceRole::Hmd) continue;
                       const auto& p = u.pose;
                       std::fprintf(trace,
                                    "{\"t\":%.6f,\"serial\":\"%s\",\"valid\":%d,\"state\":\"%s\",\"result\":%d,"
                                    "\"position\":[%.6f,%.6f,%.6f],\"orientation\":[%.7f,%.7f,%.7f,%.7f],\"velocity\":[%.5f,%.5f,%.5f],\"ft\":%.6f,\"fp\":[%.6f,%.6f,%.6f],\"pt\":%.6f}\n",
                                    r.hostTime, u.serial.c_str(), int(p.valid), ToString(p.state), int(p.result),
                                    p.worldFromImu.position[0], p.worldFromImu.position[1], p.worldFromImu.position[2],
                                    p.worldFromImu.orientation[0], p.worldFromImu.orientation[1], p.worldFromImu.orientation[2],
                                    p.worldFromImu.orientation[3], p.velocity[0], p.velocity[1], p.velocity[2],
                                    p.filterTime, p.filterPosition[0], p.filterPosition[1], p.filterPosition[2], p.hostTime);
                     }
                   }
                 });
  if (trace) std::fclose(trace);
  finished_ = true;
#endif
}

void SessionImpl::Emit(SessionEventType type, const std::string& serial, std::string text) {
  if (events_.size() > 4096) events_.pop_front();
  events_.push_back({now_, type, serial, std::move(text)});
}

void SessionImpl::OnDeviceAttached(const DeviceId& id, const std::string& configJson) {
  std::lock_guard lock(mutex_);
  Tick();
  auto& d = devices_[id];
  const bool wasAttached = d.attached;
  const std::string config = configJson.empty() ? d.info.configJson : configJson;
  d.attached = true;
  d.info.serial = id;
  d.lastData = now_;
  if (config.empty() || (wasAttached && d.tracker && d.info.configJson == config)) {
    if (d.tracker) Emit(SessionEventType::DeviceConnected, id, id + " connected");
    return;
  }
  try {
    // A radio reconnect is a new timestamp/protocol epoch, even when the device
    // JSON is identical; never take the duplicate-attach shortcut above for it.
    Emit(SessionEventType::Log, id, id + (wasAttached ? ": reattached with a new radio epoch" : ": attached"));
    const json j = json::parse(config);
    TrackedObjectModel model;
    std::string err;
    if (!TrackedObjectModel::FromJson(config, &model, &err)) throw std::runtime_error(err);
    // A radio reconnect is a new timestamp/protocol epoch, not a duplicate attach.
    d.haveClock = false;
    d.clockOffset = 0;
    d.info.batteryPercent.reset();
    d.info.charging = false;
    d.input.reset();
    d.info.configJson = config;
    d.info.role = RoleFromConfig(config);
    d.info.deviceClass = j.value("device_class", "");
    d.info.model = j.value("model_number", "");
    d.info.manufacturer = j.value("manufacturer", "");
    d.info.renderModel = j.value("render_model", "");
    d.info.controllerRole = j.value("tracked_controller_role", "");
    if (explicitControllers_ && d.info.role != DeviceRole::Hmd) {
      d.info.controllerRole.clear();
      d.info.role = DeviceRole::Tracker;
      if (id == leftController_ || id == rightController_) {
        d.info.role = DeviceRole::Controller;
        d.info.controllerRole = id == leftController_ ? "left_hand" : "right_hand";
      }
    }
    d.info.sensors = model.InImuFrame().points;
    // imuFromHead = (objectFromImu)^-1 * objectFromHead.
    Eigen::Isometry3d objectFromImu = Eigen::Isometry3d::Identity();
    const auto& r = model.objectFromImuRotation;
    objectFromImu.linear() = Eigen::Quaterniond(r[0], r[1], r[2], r[3]).normalized().toRotationMatrix();
    objectFromImu.translation() = Eigen::Vector3d(model.imuPosition.data());
    Eigen::Isometry3d objectFromHead = Eigen::Isometry3d::Identity();
    if (j.contains("head")) AxesRotation(j.at("head"), &objectFromHead);
    else if (j.contains("trackref_from_head")) PoseArray(j.at("trackref_from_head"), &objectFromHead);
    d.imuFromHead = objectFromImu.inverse() * objectFromHead;
    model.serial = id;
    d.tracker = std::make_unique<DeviceTracker>(id, model, &room_);
    models_[id] = d.tracker->ImuModel();
    for (const auto& [serial, b] : bases_) d.tracker->OnBaseConfig(b.config, b.dynamic);
    PushIdentities();
    if (d.info.role == DeviceRole::Controller) d.input.emplace();
    else d.input.reset();
    Emit(SessionEventType::DeviceActivated, id,
         id + " activated as " + ToString(d.info.role) + (d.info.model.empty() ? "" : " (" + d.info.model + ")") +
             (Enabled(id) ? "" : " [disabled]"));
  } catch (const std::exception& e) {
    Emit(SessionEventType::Log, id, id + ": unusable device configuration: " + e.what());
  }
}

void SessionImpl::OnDeviceDetached(const DeviceId& id) {
  std::lock_guard lock(mutex_);
  Tick();
  auto found = devices_.find(id);
  if (found == devices_.end()) return;
  found->second.attached = false;
  Emit(SessionEventType::DeviceDisconnected, id, id + " disconnected");
}

void SessionImpl::OnClockOffset(const DeviceId& id, double offset) {
  if (!std::isfinite(offset)) return;
  std::lock_guard lock(mutex_);
  auto& d = devices_[id];
  d.clockOffset = offset;
  d.haveClock = true;
}

void SessionImpl::Touch(Device& d, double deviceTime) {
  const double host = d.haveClock ? HostTime(d, deviceTime) : now_;
  if (host > d.lastData) d.lastData = std::min(host, now_ + 1.0);
}

void SessionImpl::OnImu(const DeviceId& id, const ImuSample& sample) {
  if (warming_) return;
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end() || !found->second.tracker) return;
  Touch(found->second, sample.time);
  found->second.tracker->OnImu(sample);
  Process(id, found->second);
  UpdateLightMode(id, found->second);
}

// A device reports LH1 or LH2 light, whichever mode the host set (LH1 after power-on). In
// LH2 mode it reports nothing from LH1 bases; in LH1 mode, LH2 bases give stray pulses but
// no sync flash. Usable light: an LH1 sync-width pulse, or an LH2 beam word.
//  * Start from the room's known bases (SteamVR's database): LH2 if all are 2.0 bases.
//  * Once bases of one kind only have been seen, keep every device in that mode (a device
//    that sees nothing is occluded; re-send the mode now and then in case it was lost).
//  * Otherwise (nothing seen yet, or a mixed room), a device without usable light for
//    kDark seconds tries the other mode.
void SessionImpl::UpdateLightMode(const DeviceId& id, Device& d) {
  constexpr double kDark = 2.5, kResend = 5.0;
  if (!usb_ || !d.attached) return;
  bool knownLh1 = false, knownLh2 = false, seenLh1 = false, seenLh2 = false;
  for (const auto& [serial, b] : bases_) {
    const bool lh2 = b.config.model != 1;
    (lh2 ? knownLh2 : knownLh1) = true;
    if (b.seen) (lh2 ? seenLh2 : seenLh1) = true;
  }
  auto set = [&](int mode, const char* why) {
    usb_->SetOpticalSubProtocol(id, mode);
    if (mode != d.lightMode) Emit(SessionEventType::Log, id, id + ": light mode " + (mode ? "LH2" : "LH1") + " (" + why + ")");
    d.lightMode = mode;
    d.modeSince = now_;
  };
  if (d.lightMode < 0) {
    set(knownLh2 && !knownLh1 ? 1 : 0, knownLh2 && !knownLh1 ? "room has 2.0 bases" : "default");
    return;
  }
  if (now_ - d.lastGoodLight < kDark || now_ - d.modeSince < kDark) return;
  if (seenLh1 != seenLh2) {
    const int want = seenLh2 ? 1 : 0;
    if (want != d.lightMode) set(want, "room's bases");
    else if (now_ - d.modeSince >= kResend) set(want, "re-send");
    return;
  }
  set(1 - d.lightMode, "no usable light");
}

void SessionImpl::OnLightPulse(const DeviceId& id, const LightPulse& pulse) {
  if (warming_) return;
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end() || !found->second.tracker) return;
  Touch(found->second, pulse.time);
  if (pulse.width >= 56e-6 && pulse.width <= 140e-6) found->second.lastGoodLight = now_;  // a sync flash
  found->second.tracker->OnPulse(pulse);
}

void SessionImpl::OnLh2BeamWord(const DeviceId& id, const Lh2BeamWord& word) {
  if (warming_) return;
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end() || !found->second.tracker) return;
  Touch(found->second, word.time);
  found->second.lastGoodLight = now_;
  found->second.tracker->OnLh2BeamWord(word);
}

void SessionImpl::OnLh2SensorWord(const DeviceId& id, const Lh2SensorWord& word) {
  if (warming_) return;
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end() || !found->second.tracker) return;
  Touch(found->second, word.time);
  found->second.tracker->OnLh2SensorWord(word);
}

void SessionImpl::OnInput(const DeviceId& id, const InputReport& report) {
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end() || !found->second.input) return;  // inputs of non-Index devices are ignored
  auto& d = found->second;
  d.input->Apply(report, d.haveClock ? HostTime(d, report.time) : now_);
}

void SessionImpl::OnBattery(const DeviceId& id, const BatteryReport& b) {
  std::lock_guard lock(mutex_);
  Tick();
  if (!Enabled(id)) return;
  auto found = devices_.find(id);
  if (found == devices_.end()) return;
  const int percent = b.raw & 0x7f;
  const bool charging = b.raw & 0x80;
  if (percent == 0 && !charging) return;
  found->second.info.batteryPercent = percent;
  found->second.info.charging = charging;
}

void SessionImpl::Process(const DeviceId& id, Device& d) {
  if (now_ - lastAdvance_ >= 0.01) { lastAdvance_ = now_; room_.Advance(now_); }
  std::vector<std::pair<BaseConfig, BaseDynamicState>> decoded;
  d.tracker->Process(&decoded);
  if (std::getenv("LH_DEBUG_TDM")) {
    const auto& st = d.tracker->SourceStats();
    fprintf(stderr, "TDM %s st=%d total=%d frames=%llu attempts=%d boot=%d\n", id.c_str(), d.tracker->SensorType(), st.total,
            (unsigned long long)d.tracker->FramesHandled(), d.tracker->BootstrapAttempts(), int(d.tracker->Bootstrapped()));
  }
  tdm::TdmDisambiguator::Identity identity;
  bool newIdentity = false;
  while (d.tracker->Source().PopIdentity(&identity)) {
    room_.SetOotxFrame(identity.config.serial, identity.frame);
    auto& stored = identities_[identity.config.serial];
    newIdentity |= stored.frame != identity.frame;
    stored = identity;
  }
  if (newIdentity) PushIdentities();
  for (const auto& [config, dynamic] : decoded) {
    auto& base = bases_[config.serial];
    const bool first = !base.seen;
    const bool newChannel = dynamic.sobChannel >= 0 && base.dynamic.sobChannel != dynamic.sobChannel;
    base.config = config;
    base.dynamic = dynamic;
    if (newChannel) PushIdentities();  // the other devices identify this LH2 base at once
    base.seen = true;
    room_.OnBaseSeen(config, dynamic);
    for (auto& [otherId, other] : devices_)
      if (other.tracker && otherId != id) other.tracker->OnBaseConfig(config, dynamic);
    if (first) {
      char text[96];
      std::snprintf(text, sizeof text, "%s seen by %s (mode %d)", BaseName(config.serial).c_str(), id.c_str(), dynamic.mode);
      Emit(SessionEventType::BaseStationSeen, BaseName(config.serial), text);
    }
  }
  TakeKeyframes(d);
  CollectLogs(&d);
  const auto out = d.tracker->Output();
  if (out.state != d.lastState) {
    Emit(SessionEventType::TrackingStateChanged, id,
         id + ": " + ToString(d.lastState) + " -> " + ToString(out.state));
    d.lastState = out.state;
  }
  MaybeSave();
}

void SessionImpl::TakeKeyframes(Device& d) {
  for (uint32_t base : room_.TakeRelearned()) keyframes_.DropBase(base);
  for (auto& k : d.tracker->TakeKeyframes()) {
    std::optional<Eigen::Isometry3d> pose;
    if (const RoomBase* primary = room_.Base(k->primary)) pose = primary->worldFromBase * k->primaryFromDevice;
    if (static const bool truth = std::getenv("LH_DEBUG_TRUTH") != nullptr; truth && pose) {
      // Where each base's light alone puts the device, against the filter's pose then.
      const auto model = models_.find(k->device);
      for (const auto& [base, samples] : k->samples) {
        const RoomBase* rb = room_.Base(base);
        const auto b = bases_.find(base);
        if (!rb || model == models_.end() || b == bases_.end() || b->second.config.cal.size() != 14) continue;
        std::array<double, 14> cal;
        std::copy(b->second.config.cal.begin(), b->second.config.cal.end(), cal.begin());
        const auto solved = SolveFromBase(*k, base, *model->second, cal, b->second.config.model);
        if (!solved) continue;
        const Eigen::Isometry3d light = rb->worldFromBase * *solved;
        const Eigen::Vector3d d = light.translation() - pose->translation();
        std::fprintf(stderr, "TRUTH %s %.3f %08X primary %08X n %zu speed %.2f d %.1f %.1f %.1f mm |%.1f| rot %.2f deg\n",
                     k->device.c_str(), now_, base, k->primary, samples.size(), k->speed, d.x() * 1e3, d.y() * 1e3,
                     d.z() * 1e3, d.norm() * 1e3,
                     Eigen::AngleAxisd(light.rotation().transpose() * pose->rotation()).angle() * 57.2958);
      }
    }
    if (keyframes_.Add(std::move(k), pose)) { newKeyframes_ = true; ++addedKeyframes_; }
  }
  if (!newKeyframes_ || !SolveDue()) return;
  if (syncSolve_) {
    lastSolve_ = now_;
    newKeyframes_ = false;
    ApplySolve(SolveRoom(SolveInput()));
  } else {
    solverWake_.notify_one();
  }
}

bool SessionImpl::SolveDue() const {
  if (!room_.Active() || !room_.Origin()) return false;
  // Locating a seen base quickly matters most; refinement can be slower.
  bool missing = false;
  for (const auto& [serial, b] : bases_) missing |= b.seen && !room_.Base(serial);
  return now_ - lastSolve_ >= (missing ? 0.1 : solveInterval_);
}

RoomSolveInput SessionImpl::SolveInput() {
  RoomSolveInput in;
  in.keyframes = keyframes_.All();
  in.fresh = std::min(addedKeyframes_, int(in.keyframes.size()));
  for (const auto& b : room_.Bases()) in.known[b.serial] = b.worldFromBase;
  // A knocked origin is relearned against another placed base, held fixed meanwhile.
  in.origin = room_.OriginRelearning() && room_.OriginGauge() ? room_.OriginGauge() : room_.Origin();
  for (const auto& [serial, b] : bases_)
    if (b.config.cal.size() == 14) {
      std::array<double, 14> cal;
      std::copy(b.config.cal.begin(), b.config.cal.end(), cal.begin());
      in.calibration[serial] = cal;
      in.baseModels[serial] = b.config.model;
    }
  in.models = models_;
  in.refineBases = !std::getenv("LH_EVALUATE_ROOM");  // diagnostics: score a fixed room
  addedKeyframes_ = 0;
  return in;
}

void SessionImpl::ApplySolve(const RoomSolveOutput& out) {
  if (std::getenv("LH_DEBUG_SOLVER")) Emit(SessionEventType::Log, "", "room solve: " + out.summary);
  if (std::getenv("LH_DEBUG_ROOM_SPLIT") && out.ok) DebugSplit(out);
  if (!out.ok || std::getenv("LH_EVALUATE_ROOM")) return;
  const uint32_t origin = room_.Origin();
  // A settled room: re-solving every second only burns CPU (up to 400 keyframes, two
  // passes). Back off while the solved bases agree with the room, up to 4 s (it also bounds how late a knocked base shows up); any real
  // change (or a base still being located, see SolveDue) brings it back to 1 s.
  // Compared with the previous solve, not the room map: the map moves toward solves
  // gradually, so it always trails them by a little.
  bool settled = !lastSolved_.empty();
  double worstMove = 0, worstTurn = 0;
  for (const auto& [serial, b] : out.bases) {
    if (serial == origin || b.keyframes <= 0) continue;
    const auto previous = lastSolved_.find(serial);
    if (previous == lastSolved_.end()) { settled = false; continue; }
    worstMove = std::max(worstMove, (b.worldFromBase.translation() - previous->second.translation()).norm());
    worstTurn = std::max(worstTurn, Eigen::AngleAxisd(previous->second.rotation().transpose() * b.worldFromBase.rotation()).angle());
  }
  settled = settled && worstMove < 0.003 && worstTurn < 0.15 * M_PI / 180;
  lastSolved_.clear();
  for (const auto& [serial, b] : out.bases) lastSolved_[serial] = b.worldFromBase;
  solveInterval_ = settled ? std::min(2 * solveInterval_, 4.0) : 1.0;
  if (std::getenv("LH_DEBUG_SOLVER")) {
    char text[128];
    std::snprintf(text, sizeof text, "room solve: moved %.1f mm %.3f deg since last; next in %.0f s", worstMove * 1e3,
                  worstTurn * 57.2958, solveInterval_);
    Emit(SessionEventType::Log, "", text);
  }
  if (room_.OriginRelearning()) {
    const auto o = out.bases.find(origin);
    if (o != out.bases.end() && o->second.keyframes > 0)
      room_.ApplyOriginSolved(o->second.worldFromBase, o->second.positionVariance, o->second.keyframes);
  } else {
    // Every other base far off at once (three or more bases): it's the origin that moved.
    // With two, a knocked origin and a knocked other look alike; only the
    // accelerometers tell them apart.
    int others = 0, farOff = 0;  // (not `far`: a macro in the Windows headers)
    for (const auto& [serial, b] : out.bases) {
      if (serial == origin || b.keyframes <= 0 || !room_.Base(serial)) continue;
      ++others;
      farOff += room_.FarOff(serial, b.worldFromBase, b.positionVariance, b.rotationVariance);
    }
    if (others >= 2 && farOff == others) {
      room_.Bump(origin, "every other base appears moved by it");
      CollectLogs(nullptr);
      return;
    }
  }
  const uint32_t gauge = room_.OriginRelearning() ? room_.OriginGauge() : 0;  // held fixed by the solve
  for (const auto& [serial, b] : out.bases)
    if (serial != origin && serial != gauge && b.keyframes > 0)
      room_.ApplySolved(serial, b.worldFromBase, b.positionVariance, b.rotationVariance, b.keyframes, b.rms, b.fresh);
  CollectLogs(nullptr);
}

// Diagnostics: the room from subsets of the same keyframes (each device alone, slow and
// fast ones). Where they disagree beyond their spread is a bias, not noise.
void SessionImpl::DebugSplit(const RoomSolveOutput& all) {
  const int added = addedKeyframes_;
  const RoomSolveInput in = SolveInput();
  addedKeyframes_ = added;
  auto line = [&](const std::string& what, const std::vector<std::shared_ptr<const Keyframe>>& kfs) {
    if (kfs.size() < 10) return;
    RoomSolveInput sub = in;
    sub.keyframes = kfs;
    const RoomSolveOutput o = SolveRoom(sub);
    std::string text = what + " (" + std::to_string(kfs.size()) + " kf):";
    for (const auto& [serial, b] : o.bases) {
      if (serial == in.origin || !all.bases.count(serial)) continue;
      const Eigen::Vector3d d = b.worldFromBase.translation() - all.bases.at(serial).worldFromBase.translation();
      const double r = Eigen::AngleAxisd(all.bases.at(serial).worldFromBase.rotation().transpose() *
                                         b.worldFromBase.rotation()).angle();
      char t[160];
      std::snprintf(t, sizeof t, " %08X d=[%+.1f %+.1f %+.1f] mm %.2f deg rms %.0f urad", serial, d.x() * 1e3,
                    d.y() * 1e3, d.z() * 1e3, r * 57.2958, b.rms * 1e6);
      text += t;
    }
    std::fprintf(stderr, "split %s\n", text.c_str());
  };
  std::map<std::string, std::vector<std::shared_ptr<const Keyframe>>> byDevice;
  std::vector<std::shared_ptr<const Keyframe>> slow, fast, early, late;
  for (const auto& k : in.keyframes) {
    byDevice[k->device].push_back(k);
    (k->speed < 0.2 && k->rate < 0.5 ? slow : fast).push_back(k);
  }
  for (size_t i = 0; i < in.keyframes.size(); ++i) (i < in.keyframes.size() / 2 ? early : late).push_back(in.keyframes[i]);
  std::fprintf(stderr, "split --- t=%.1f all %zu kf\n", now_, in.keyframes.size());
  for (const auto& [dev, kfs] : byDevice) line(dev, kfs);
  line("slow", slow);
  line("fast", fast);
  line("older half", early);
  line("newer half", late);
}

void SessionImpl::SolverMain() {
  std::unique_lock lock(mutex_);
  while (!stop_) {
    solverWake_.wait_for(lock, std::chrono::milliseconds(100));
    if (stop_) break;
    Tick();
    if (!newKeyframes_ || !SolveDue()) continue;
    newKeyframes_ = false;
    lastSolve_ = now_;
    const RoomSolveInput input = SolveInput();
    lock.unlock();
    const RoomSolveOutput out = SolveRoom(input);
    lock.lock();
    ApplySolve(out);
  }
}

void SessionImpl::PushIdentities() {
  std::vector<tdm::TdmDisambiguator::Identity> all;
  for (const auto& [serial, id] : identities_) all.push_back(id);
  std::map<int, std::pair<BaseConfig, BaseDynamicState>> channels;  // LH2: one channel per base, every device
  for (const auto& [serial, b] : bases_)
    if (b.dynamic.sobChannel >= 0 && b.config.valid && b.config.cal.size() == 14)
      channels[b.dynamic.sobChannel] = {b.config, b.dynamic};
  for (auto& [id, d] : devices_)
    if (d.tracker) {
      d.tracker->Source().SetKnownIdentities(all);
      d.tracker->Source().SetKnownSobChannels(channels);
    }
}

void SessionImpl::CollectLogs(Device* d) {
  if (d)
    for (auto& text : d->tracker->TakeLogs()) Emit(SessionEventType::Log, d->info.serial, std::move(text));
  const uint64_t before = room_.Revision();
  for (auto& text : room_.TakeLogs()) {
    const bool located = text.rfind("Located base", 0) == 0 || text.rfind("Setting ", 0) == 0;
    Emit(located ? SessionEventType::BaseStationLocated : SessionEventType::RoomChanged, "", std::move(text));
  }
  (void)before;
}

void SessionImpl::MaybeSave() {
  if (!persist_ || !room_.Dirty()) return;
  const double wall = SteadyNow();
  if (wall - lastSave_ < 10.0) return;
  lastSave_ = wall;
  std::string err;
  if (room_.Save(roomPath_, &err)) room_.ClearDirty();
  else Emit(SessionEventType::Log, "", err);
}

DevicePose SessionImpl::MakePose(const Device& d) const {
  DevicePose pose;
  pose.imuFromHead = ToPose(d.imuFromHead);
  if (!d.tracker) return pose;
  const auto out = d.tracker->Output();
  pose.state = out.state;
  pose.result = out.result;
  pose.valid = out.valid;
  pose.hostTime = d.haveClock ? HostTime(d, out.deviceTime) : now_;
  pose.worldFromImu = ToPose(out.worldFromImu);
  pose.worldFromHead = ToPose(out.worldFromImu * d.imuFromHead);
  pose.velocity = {out.velocity.x(), out.velocity.y(), out.velocity.z()};
  pose.angularVelocity = {out.angularVelocity.x(), out.angularVelocity.y(), out.angularVelocity.z()};
  pose.primaryBase = out.primary;
  pose.bases = out.bases;
  pose.updates = out.updates;
  pose.filterPosition = {out.filterPosition.x(), out.filterPosition.y(), out.filterPosition.z()};
  pose.filterTime = d.haveClock ? HostTime(d, out.filterTime) : now_;
  return pose;
}

SessionSnapshot SessionImpl::Snapshot() const {
  std::lock_guard lock(mutex_);
  SessionSnapshot snap;
  if (options_.replayPath.empty() && running_) now_ = std::max(now_, SteadyNow());
  snap.hostTime = now_;
  snap.roomRevision = room_.Revision();
  if (const Room* room = room_.Active()) snap.roomId = room->id;
  snap.originEpoch = room_.OriginEpoch();
  for (const auto& [id, d] : devices_) {
    if (!d.tracker) continue;  // receivers / devices without a usable tracking config
    DeviceState s;
    s.info = d.info;
    s.info.connected = d.attached && now_ - d.lastData < 1.0;
    s.info.enabled = Enabled(id);
    s.pose = MakePose(d);
    if (!s.info.connected || !s.info.enabled) {
      s.pose.state = TrackingState::Disconnected;
      s.pose.valid = false;
      s.pose.result = TrackingResult::Uninitialized;
    }
    if (d.input && s.info.enabled) s.input = d.input->State();
    snap.devices.push_back(std::move(s));
  }
  std::map<uint32_t, BaseStationState> bases;
  for (const auto& [serial, b] : bases_) {
    auto& s = bases[serial];
    s.id = serial;
    s.serial = BaseName(serial);
    s.seen = b.seen;
    s.mode = b.dynamic.mode;
    // lhbs multi channels are its slot (1-4 on the display), stock ones A, b, c.
    if (b.dynamic.sobChannel >= 0) s.channel = std::to_string(b.dynamic.sobChannel + 1);  // LH2: 1-16
    else if (b.dynamic.multiSlot >= 0) s.channel = std::to_string(b.dynamic.multiSlot + 1);
    else if (b.dynamic.mode >= 1 && b.dynamic.mode <= 3) s.channel = std::string(1, "Abc"[b.dynamic.mode - 1]);
    s.faults = b.dynamic.faults;
  }
  for (const auto& rb : room_.Bases()) {
    auto& s = bases[rb.serial];
    s.id = rb.serial;
    s.serial = BaseName(rb.serial);
    s.known = true;
    s.worldFromBase = ToPose(rb.worldFromBase);
    s.positionVariance = rb.positionVariance;
    s.rotationVariance = rb.rotationVariance;
    s.origin = rb.origin;
  }
  for (auto& [serial, s] : bases)
    if (s.seen || s.known) snap.bases.push_back(s);
  return snap;
}

std::vector<DeviceUpdate> SessionImpl::Updates() const {
  std::lock_guard lock(mutex_);
  if (options_.replayPath.empty() && running_) now_ = std::max(now_, SteadyNow());
  std::vector<DeviceUpdate> out;
  out.reserve(devices_.size());
  for (const auto& [id, d] : devices_) {
    if (!d.tracker) continue;
    DeviceUpdate u;
    u.serial = id;
    u.role = d.info.role;
    u.connected = d.attached && now_ - d.lastData < 1.0;
    u.enabled = Enabled(id);
    u.batteryPercent = d.info.batteryPercent;
    u.charging = d.info.charging;
    u.pose = MakePose(d);
    if (!u.connected || !u.enabled) {
      u.pose.state = TrackingState::Disconnected;
      u.pose.valid = false;
      u.pose.result = TrackingResult::Uninitialized;
    }
    if (d.input && u.enabled) u.input = d.input->State();
    out.push_back(std::move(u));
  }
  return out;
}

std::vector<SessionEvent> SessionImpl::PollEvents() {
  std::lock_guard lock(mutex_);
  std::vector<SessionEvent> out(events_.begin(), events_.end());
  events_.clear();
  return out;
}

void SessionImpl::LoadSettings() {
  if (settingsPath_.empty()) return;
  std::ifstream in(settingsPath_);
  if (!in) return;
  try {
    const json j = json::parse(in);
    if (j.contains("controllers")) {
      const auto& controllers = j.at("controllers");
      if (!controllers.is_object()) throw std::runtime_error("controllers must be an object");
      const auto left = controllers.value("left", std::string{});
      const auto right = controllers.value("right", std::string{});
      if (!left.empty() && left == right) throw std::runtime_error("left and right controller serials must differ");
      leftController_ = left;
      rightController_ = right;
      explicitControllers_ = true;
    }
    for (const auto& serial : j.value("disabledDevices", std::vector<std::string>{})) disabled_.insert(serial);
  } catch (const std::exception& e) {
    Emit(SessionEventType::Log, "", "settings " + settingsPath_ + ": " + e.what());
  }
}

void SessionImpl::SaveSettings() {
  if (settingsPath_.empty()) return;
  try {
    json j = json::object();
    {
      std::ifstream in(settingsPath_);
      if (in) j = json::parse(in);
    }
    j["disabledDevices"] = std::vector<std::string>(disabled_.begin(), disabled_.end());
    const std::filesystem::path target(settingsPath_);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    const std::string temp = settingsPath_ + ".tmp";
    {
      std::ofstream out(temp);
      out << j.dump(2) << "\n";
    }
    platform::ReplaceFile(temp, target);
  } catch (const std::exception& e) {
    Emit(SessionEventType::Log, "", "saving settings " + settingsPath_ + ": " + e.what());
  }
}

void SessionImpl::SetDeviceEnabled(const std::string& serial, bool enabled) {
  std::lock_guard lock(mutex_);
  if (Enabled(serial) == enabled) return;
  if (enabled) disabled_.erase(serial);
  else disabled_.insert(serial);
  auto found = devices_.find(serial);
  if (found != devices_.end() && found->second.tracker) {
    // Start clean when re-enabled; stale filter state is worse than a fresh bootstrap.
    auto& d = found->second;
    TrackedObjectModel model;
    if (TrackedObjectModel::FromJson(d.info.configJson, &model)) {
      model.serial = serial;
      d.tracker = std::make_unique<DeviceTracker>(serial, model, &room_);
      for (const auto& [base, b] : bases_) d.tracker->OnBaseConfig(b.config, b.dynamic);
      PushIdentities();
    }
    d.lastState = TrackingState::Disconnected;
  }
  Emit(SessionEventType::DeviceEnabledChanged, serial, serial + (enabled ? " enabled" : " disabled"));
  SaveSettings();
}

void SessionImpl::ResetRoom() {
  std::lock_guard lock(mutex_);
  room_.ResetActive();
  keyframes_.Clear();
  CollectLogs(nullptr);
}

bool SessionImpl::SaveRoom(std::string* error) {
  std::lock_guard lock(mutex_);
  if (roomPath_.empty()) {
    if (error) *error = "no room map path (replay without --room)";
    return false;
  }
  if (!room_.Save(roomPath_, error)) return false;
  room_.ClearDirty();
  return true;
}

}  // namespace runtime

std::unique_ptr<Session> Session::Create(const SessionOptions& options) {
  return std::make_unique<runtime::SessionImpl>(options);
}

}  // namespace lh
