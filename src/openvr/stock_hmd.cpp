#include "stock_hmd.hpp"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include <hidapi.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lh::openvr {

using namespace vr;

// Wraps the stock headset so the host's Activate tells us its object id.
class StockHmd::DeviceProxy : public ITrackedDeviceServerDriver {
 public:
  DeviceProxy(StockHmd* owner, std::string serial, ITrackedDeviceServerDriver* inner)
      : owner_(owner), serial_(std::move(serial)), inner_(inner) {}
  EVRInitError Activate(uint32_t id) override {
    const EVRInitError r = inner_->Activate(id);
    if (r == VRInitError_None) {
      alive_ = true;
      std::lock_guard lock(owner_->mutex_);
      owner_->activated_.push_back({serial_, id, &alive_});
      // SteamVR sends "leave standby" when it comes up and the stock driver powers the
      // headset's panels then; other hosts (Monado) never do, and the panels stay dark.
      // Harmless under SteamVR (it is out of standby already).
      if (!std::getenv("LH_NO_STOCK_WAKE")) {
        VREvent_t wake{};
        wake.eventType = VREvent_LeaveStandbyMode;
        wake.trackedDeviceIndex = id;
        owner_->events_.push_back(wake);
        wake.eventType = VREvent_TrackedDeviceUserInteractionStarted;
        owner_->events_.push_back(wake);
      }
    }
    return r;
  }
  void Deactivate() override {
    alive_ = false;  // ours stops publishing its pose first
    inner_->Deactivate();
  }
  void EnterStandby() override { inner_->EnterStandby(); }
  void* GetComponent(const char* name) override { return inner_->GetComponent(name); }
  void DebugRequest(const char* request, char* response, uint32_t size) override {
    inner_->DebugRequest(request, response, size);
  }
  DriverPose_t GetPose() override { return inner_->GetPose(); }

 private:
  StockHmd* owner_;
  std::string serial_;
  ITrackedDeviceServerDriver* inner_;
  std::atomic<bool> alive_{false};
};

class StockHmd::Host : public IVRServerDriverHost {
 public:
  explicit Host(StockHmd* owner) : owner_(owner) {}
  bool TrackedDeviceAdded(const char* serial, ETrackedDeviceClass cls, ITrackedDeviceServerDriver* driver) override {
    if (cls != TrackedDeviceClass_HMD) return true;  // ours already; the stock one stays inactive
    auto* proxy = new DeviceProxy(owner_, serial ? serial : "", driver);
    {
      std::lock_guard lock(owner_->mutex_);
      owner_->devices_.push_back(proxy);
    }
    if (owner_->log_) owner_->log_(std::string("stock driver: head-mounted display ") + (serial ? serial : "?"));
    return owner_->realHost_->TrackedDeviceAdded(serial, cls, proxy);
  }
  void TrackedDevicePoseUpdated(uint32_t id, const DriverPose_t& pose, uint32_t size) override {
    {
      std::lock_guard lock(owner_->mutex_);
      if (owner_->owned_.count(id)) return;
    }
    owner_->realHost_->TrackedDevicePoseUpdated(id, pose, size);
  }
  void VsyncEvent(double offset) override { owner_->realHost_->VsyncEvent(offset); }
  void VendorSpecificEvent(uint32_t id, EVREventType type, const VREvent_Data_t& data, double offset) override {
    // Status alerts about the stock driver's own tracking, which isn't used (ours drives
    // the pose): 10002 "Too Many Base Stations" (its TDM tracker in a room of more than
    // two bases, e.g. lhbs multi-base firmware). Other types pass; each is logged once.
    if (int(type) == 10002) return;
    {
      std::lock_guard lock(owner_->mutex_);
      if (owner_->vendorEvents_.insert(int(type)).second && owner_->log_)
        owner_->log_("stock driver: vendor event " + std::to_string(int(type)) + " for device " + std::to_string(id));
    }
    owner_->realHost_->VendorSpecificEvent(id, type, data, offset);
  }
  bool IsExiting() override { return owner_->realHost_->IsExiting(); }
  bool PollNextEvent(VREvent_t* event, uint32_t size) override {
    std::lock_guard lock(owner_->mutex_);
    if (owner_->events_.empty() || size < sizeof(VREvent_t)) return false;
    *event = owner_->events_.front();
    owner_->events_.pop_front();
    return true;
  }
  void GetRawTrackedDevicePoses(float predicted, TrackedDevicePose_t* poses, uint32_t count) override {
    owner_->realHost_->GetRawTrackedDevicePoses(predicted, poses, count);
  }
  void RequestRestart(const char* reason, const char* exe, const char* args, const char* dir) override {
    owner_->realHost_->RequestRestart(reason, exe, args, dir);
  }
  uint32_t GetFrameTimings(Compositor_FrameTiming* timing, uint32_t frames) override {
    return owner_->realHost_->GetFrameTimings(timing, frames);
  }
  void SetDisplayEyeToHead(uint32_t id, const HmdMatrix34_t& left, const HmdMatrix34_t& right) override {
    owner_->realHost_->SetDisplayEyeToHead(id, left, right);
  }
  void SetDisplayProjectionRaw(uint32_t id, const HmdRect2_t& left, const HmdRect2_t& right) override {
    owner_->realHost_->SetDisplayProjectionRaw(id, left, right);
  }
  void SetRecommendedRenderTargetSize(uint32_t id, uint32_t width, uint32_t height) override {
    owner_->realHost_->SetRecommendedRenderTargetSize(id, width, height);
  }

 private:
  StockHmd* owner_;
};

// Settings the host doesn't have come from the stock driver's resources/settings/
// default.vrsettings (SteamVR merges that file itself; other hosts, Monado, don't: without
// it the stock driver has no "usedisambiguation" and never brings its headset up).
class StockHmd::Settings : public IVRSettings {
 public:
  Settings(IVRSettings* real, nlohmann::json defaults) : real_(real), defaults_(std::move(defaults)) {}
  const char* GetSettingsErrorNameFromEnum(EVRSettingsError e) override { return real_->GetSettingsErrorNameFromEnum(e); }
  void SetBool(const char* s, const char* k, bool v, EVRSettingsError* e) override { real_->SetBool(s, k, v, e); }
  void SetInt32(const char* s, const char* k, int32_t v, EVRSettingsError* e) override { real_->SetInt32(s, k, v, e); }
  void SetFloat(const char* s, const char* k, float v, EVRSettingsError* e) override { real_->SetFloat(s, k, v, e); }
  void SetString(const char* s, const char* k, const char* v, EVRSettingsError* e) override { real_->SetString(s, k, v, e); }
  bool GetBool(const char* s, const char* k, EVRSettingsError* e) override {
    EVRSettingsError err = VRSettingsError_None;
    const bool v = real_->GetBool(s, k, &err);
    if (err == VRSettingsError_None) return Ok(e), v;
    const auto* d = Default(s, k);
    if (!d || !(d->is_boolean() || d->is_number())) return Fail(e, err), v;
    return Ok(e), d->is_boolean() ? d->get<bool>() : d->get<double>() != 0;
  }
  int32_t GetInt32(const char* s, const char* k, EVRSettingsError* e) override {
    EVRSettingsError err = VRSettingsError_None;
    const int32_t v = real_->GetInt32(s, k, &err);
    if (err == VRSettingsError_None) return Ok(e), v;
    const auto* d = Default(s, k);
    if (!d || !(d->is_boolean() || d->is_number())) return Fail(e, err), v;
    return Ok(e), d->is_boolean() ? int32_t(d->get<bool>()) : int32_t(d->get<double>());
  }
  float GetFloat(const char* s, const char* k, EVRSettingsError* e) override {
    EVRSettingsError err = VRSettingsError_None;
    const float v = real_->GetFloat(s, k, &err);
    if (err == VRSettingsError_None) return Ok(e), v;
    const auto* d = Default(s, k);
    if (!d || !d->is_number()) return Fail(e, err), v;
    return Ok(e), float(d->get<double>());
  }
  void GetString(const char* s, const char* k, char* v, uint32_t n, EVRSettingsError* e) override {
    EVRSettingsError err = VRSettingsError_None;
    real_->GetString(s, k, v, n, &err);
    if (err == VRSettingsError_None) return Ok(e);
    const auto* d = Default(s, k);
    if (!d || !d->is_string() || !v || n == 0) return Fail(e, err);
    const std::string str = d->get<std::string>();
    std::snprintf(v, n, "%s", str.c_str());
    Ok(e);
  }
  void RemoveSection(const char* s, EVRSettingsError* e) override { real_->RemoveSection(s, e); }
  void RemoveKeyInSection(const char* s, const char* k, EVRSettingsError* e) override { real_->RemoveKeyInSection(s, k, e); }

 private:
  static void Ok(EVRSettingsError* e) { if (e) *e = VRSettingsError_None; }
  static void Fail(EVRSettingsError* e, EVRSettingsError err) { if (e) *e = err; }
  const nlohmann::json* Default(const char* s, const char* k) const {
    if (!s || !k || !defaults_.is_object()) return nullptr;
    const auto sec = defaults_.find(s);
    if (sec == defaults_.end() || !sec->is_object()) return nullptr;
    const auto it = sec->find(k);
    return it == sec->end() ? nullptr : &*it;
  }
  IVRSettings* real_;
  nlohmann::json defaults_;
};

// The stock driver's log, minus repeats: its own tracker runs for the headset too (unused,
// ours drives the pose) and in rooms it can't handle (more than 2 bases on TDM, multi-base
// lhbs firmware) repeats the same lines several times a second. A line repeated within
// a minute is dropped.
class StockHmd::LogFilter : public IVRDriverLog {
 public:
  explicit LogFilter(IVRDriverLog* real) : real_(real) {}
  void Log(const char* text) override {
    if (!text || !real_) return;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(mutex_);
      auto& last = seen_[text];
      const bool repeat = last.time_since_epoch().count() != 0 && now - last < std::chrono::minutes(1);
      if (!repeat) last = now;
      if (seen_.size() > 4096) seen_.clear();
      if (repeat) return;
    }
    real_->Log(text);
  }

 private:
  IVRDriverLog* real_;
  std::mutex mutex_;
  std::map<std::string, std::chrono::steady_clock::time_point> seen_;
};

class StockHmd::Context : public IVRDriverContext {
 public:
  Context(IVRDriverContext* real, Host* host, Settings* settings, StockHmd::LogFilter* log)
      : real_(real), host_(host), settings_(settings), log_(log) {}
  void* GetGenericInterface(const char* version, EVRInitError* error) override {
    if (version && std::string(version) == IVRServerDriverHost_Version) {
      if (error) *error = VRInitError_None;
      return host_;
    }
    if (log_ && version && std::string(version) == IVRDriverLog_Version) {
      if (error) *error = VRInitError_None;
      return log_;
    }
    if (settings_ && version && std::string(version) == IVRSettings_Version) {
      if (error) *error = VRInitError_None;
      return settings_;
    }
    return real_->GetGenericInterface(version, error);
  }
  DriverHandle_t GetDriverHandle() override { return real_->GetDriverHandle(); }

 private:
  IVRDriverContext* real_;
  Host* host_;
  Settings* settings_;
  StockHmd::LogFilter* log_;
};

std::string StockHmd::DefaultPath() {
  if (const char* p = std::getenv("LH_STOCK_DRIVER"); p && *p) return p;
#ifdef _WIN32
  return {};  // TODO(windows): driver_lighthouse.dll.stock next to ours
#else
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&StockHmd::DefaultPath), &info) || !info.dli_fname) return {};
  const auto path = std::filesystem::path(info.dli_fname).parent_path() / "driver_lighthouse.so.stock";
  std::error_code ec;
  return std::filesystem::exists(path, ec) ? path.string() : std::string();
#endif
}

StockHmd::Presence StockHmd::HeadsetPresent(std::string* which, std::vector<std::string>* maybe) {
  // LH_STOCK_HMD=1: load the stock driver even if no USB device looks like a headset.
  if (const char* force = std::getenv("LH_STOCK_HMD"); force && *force == '1') {
    if (which) *which = "(forced by LH_STOCK_HMD)";
    return Presence::Yes;
  }
  hid_device_info* list = hid_enumerate(0x28de, 0);
  Presence result = Presence::No;
  for (auto* d = list; d; d = d->next) {
    const std::wstring product = d->product_string ? d->product_string : L"";
    Presence p = Presence::No;
    if (d->product_id == 0x2000 && !std::getenv("LH_STOCK_PROBE_ALL")) p = Presence::Yes;
    else if (d->product_id == 0x2300 && (product.find(L"Controller") == std::wstring::npos || std::getenv("LH_STOCK_PROBE_ALL")))
      p = Presence::Maybe;  // LH_STOCK_PROBE_ALL: controllers too (tests the config check)
    if (p == Presence::No) continue;
    char buf[160];
    std::snprintf(buf, sizeof buf, "28de:%04x %ls", d->product_id, product.c_str());
    if (p > result && which) *which = buf;
    result = std::max(result, p);
    if (p == Presence::Maybe && maybe && d->serial_number) {
      char serial[64];
      std::snprintf(serial, sizeof serial, "%ls", d->serial_number);
      if (std::find(maybe->begin(), maybe->end(), serial) == maybe->end()) maybe->push_back(serial);
    }
  }
  hid_free_enumeration(list);
  return result;
}

bool StockHmd::Load(const std::string& path, IVRDriverContext* realContext, LogFn log) {
  log_ = std::move(log);
#ifdef _WIN32
  (void)path; (void)realContext;
  if (log_) log_("stock driver: not supported on Windows yet");
  return false;
#else
  if (path.empty()) return false;
  // RTLD_LOCAL: its symbols (HmdDriverFactory, its OpenVR helpers) stay its own.
  library_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_) {
    if (log_) log_(std::string("stock driver: cannot load ") + path + ": " + dlerror());
    return false;
  }
  using Factory = void* (*)(const char*, int*);
  auto factory = reinterpret_cast<Factory>(dlsym(library_, "HmdDriverFactory"));
  int code = 0;
  auto* provider = factory ? static_cast<IServerTrackedDeviceProvider*>(factory(IServerTrackedDeviceProvider_Version, &code))
                           : nullptr;
  if (!provider) {
    if (log_) log_("stock driver: no " + std::string(IServerTrackedDeviceProvider_Version) + " in " + path);
    dlclose(library_);
    library_ = nullptr;
    return false;
  }
  real_ = realContext;
  realHost_ = static_cast<IVRServerDriverHost*>(real_->GetGenericInterface(IVRServerDriverHost_Version, nullptr));
  host_ = new Host(this);
  if (auto* realSettings = static_cast<IVRSettings*>(real_->GetGenericInterface(IVRSettings_Version, nullptr))) {
    // <driver>/bin/linux64/<library> -> <driver>/resources/settings/default.vrsettings
    const auto file = std::filesystem::path(path).parent_path().parent_path().parent_path() / "resources" /
                      "settings" / "default.vrsettings";
    nlohmann::json defaults;
    if (std::ifstream in(file); in) defaults = nlohmann::json::parse(in, nullptr, false, true);
    if (defaults.is_discarded() || !defaults.is_object()) {
      if (log_) log_("stock driver: no settings defaults at " + file.string());
      defaults = nlohmann::json::object();
    }
    settings_ = new Settings(realSettings, std::move(defaults));
  }
  if (auto* realLog = static_cast<IVRDriverLog*>(real_->GetGenericInterface(IVRDriverLog_Version, nullptr)))
    logProxy_ = new LogFilter(realLog);
  context_ = new Context(real_, host_, settings_, logProxy_);
  const EVRInitError r = provider->Init(context_);
  if (r != VRInitError_None) {
    if (log_) log_("stock driver: Init failed (" + std::to_string(int(r)) + ")");
    return false;  // library stays loaded: its provider may hold threads
  }
  provider_ = provider;
  if (log_) log_("stock driver loaded for head-mounted displays: " + path);
  return true;
#endif
}

void StockHmd::Cleanup() {
  if (provider_) provider_->Cleanup();
  provider_ = nullptr;
  // The library, proxies and context stay alive: the host may still hold the headset's
  // proxy until it shuts down, and unloading code that may still run is worse than a leak.
}

void StockHmd::PushEvent(const VREvent_t& event) {
  if (!provider_) return;
  // Haptics are ours (its other devices are never activated, but don't risk a double pulse).
  if (event.eventType == VREvent_Input_HapticVibration) return;
  std::lock_guard lock(mutex_);
  if (events_.size() >= 256) events_.pop_front();
  events_.push_back(event);
}

void StockHmd::RunFrame() {
  if (provider_) provider_->RunFrame();
}

std::optional<StockHmd::Activated> StockHmd::TakeActivated() {
  std::lock_guard lock(mutex_);
  if (activated_.empty()) return std::nullopt;
  auto a = activated_.front();
  activated_.pop_front();
  return a;
}

void StockHmd::OwnPose(uint32_t objectId) {
  std::lock_guard lock(mutex_);
  owned_.insert(objectId);
}

}  // namespace lh::openvr
