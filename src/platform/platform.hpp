// Platform services shared by transport, runtime and the OpenVR wrapper.
#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace lh::platform {
inline std::string Env(const char* key) {
  const char* value = std::getenv(key);
  return value ? value : "";
}
inline unsigned long ProcessId() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}
inline std::string Home() {
#ifdef _WIN32
  if (auto home = Env("USERPROFILE"); !home.empty()) return home;
#endif
  if (auto home = Env("HOME"); !home.empty()) return home;
  return ".";
}
inline std::string ConfigDirectory() {
#ifdef _WIN32
  if (auto dir = Env("APPDATA"); !dir.empty()) return dir + "/lh";
  return Home() + "/AppData/Roaming/lh";
#else
  auto dir = Env("XDG_CONFIG_HOME");
  return (dir.empty() ? Home() + "/.config" : dir) + "/lh";
#endif
}
inline std::string SteamRoot() {
  if (auto root = Env("LH_STEAM_ROOT"); !root.empty()) return root;
#ifdef _WIN32
  // Steam's per-user key knows nonstandard install locations too.
  wchar_t value[32768]{};
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                   RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS && *value)
    return std::filesystem::path(value).string();
  auto dir = Env("ProgramFiles(x86)");
  return (dir.empty() ? "C:/Program Files (x86)" : dir) + "/Steam";
#else
  const auto modern = Home() + "/.local/share/Steam";
  if (std::filesystem::exists(modern)) return modern;
  return Home() + "/.steam/steam";
#endif
}
inline std::string SteamConfigDirectory() {
  if (auto dir = Env("LH_STEAM_CONFIG_DIR"); !dir.empty()) return dir;
  return SteamRoot() + "/config/lighthouse";
}
inline std::string SteamVRDirectory() {
  if (auto dir = Env("STEAMVR_PATH"); !dir.empty()) return dir;
#ifdef _WIN32
  // Installed module path also handles SteamVR in another Steam library.
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(&SteamVRDirectory), &module)) {
    wchar_t buffer[32768];
    const DWORD n = GetModuleFileNameW(module, buffer, DWORD(std::size(buffer)));
    if (n && n < std::size(buffer)) {
      auto dir = std::filesystem::path(buffer).parent_path();
      for (int i = 0; i < 4; ++i) dir = dir.parent_path();
      if (std::filesystem::exists(dir / "drivers/indexcontroller/resources")) return dir.string();
    }
  }
#endif
  return SteamRoot() + "/steamapps/common/SteamVR";
}
// std::filesystem::rename doesn't replace an existing destination on Windows.
// Never delete the old valid file before installing the new one.
inline void ReplaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::system_error(int(GetLastError()), std::system_category(), "replacing " + destination.string());
#else
  std::filesystem::rename(source, destination);
#endif
}
// A short sleep that lasts about as long as asked. Windows rounds sleep_for up to its
// timer tick (15.6 ms unless some process raised the resolution), which held the driver
// loop and USB reads to ~64 passes per second; a high-resolution waitable timer doesn't.
inline void SleepFor(std::chrono::microseconds duration) {
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(duration.count()) * 10;  // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(duration);
}
inline bool VrserverRunning() {
#ifdef _WIN32
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) do {
    if (_wcsicmp(entry.szExeFile, L"vrserver.exe") == 0) { found = true; break; }
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return found;
#else
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
    if (!entry.is_directory(error)) continue;
    // Name from the executable symlink avoids parsing /proc text with extra includes.
    const auto exe = std::filesystem::read_symlink(entry.path() / "exe", error);
    if (!error && exe.filename() == "vrserver") return true;
    error.clear();
  }
  return false;
#endif
}
}
