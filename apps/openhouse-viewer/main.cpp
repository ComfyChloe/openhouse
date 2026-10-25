// openhouse-viewer: a live view of the running openhouse driver (SteamVR or Monado).
//
// Connects to the driver's telemetry on 127.0.0.1 (lh/telemetry.hpp) and shows what a
// user needs: is everything tracked, is the room ready, which base stations see what,
// and the driver's log by level. View > Advanced adds positions, per-base light and
// placement details.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl2.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "lh/telemetry.hpp"
#include "log_view.hpp"
#include "telemetry_client.hpp"
#include "views.hpp"

namespace {
using namespace openhouse::viewer;
using json = nlohmann::json;

std::filesystem::path SettingsPath() {
#ifdef _WIN32
  const char* base = std::getenv("APPDATA");
  return std::filesystem::path(base ? base : ".") / "lh" / "viewer.json";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return std::filesystem::path(xdg) / "lh" / "viewer.json";
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".config" / "lh" / "viewer.json";
#endif
}

struct Settings {
  Detail detail = Detail::Basic;
  void Load(LogFilter& f) {
    std::ifstream in(SettingsPath());
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    detail = j.value("advanced", false) ? Detail::Advanced : Detail::Basic;
    const json l = j.value("logs", json::object());
    f.info = l.value("info", true);
    f.warn = l.value("warn", true);
    f.error = l.value("error", true);
    f.debug = l.value("debug", false);
    f.hideMulti = l.value("hideMulti", true);
    f.hideLh2 = l.value("hideLh2", true);
    for (const auto& h : l.value("hidden", json::array())) if (h.is_string()) f.hidden.insert(h.get<std::string>());
  }
  void Save(const LogFilter& f) const {
    const json j = {{"advanced", detail == Detail::Advanced},
                    {"logs", {{"info", f.info}, {"warn", f.warn}, {"error", f.error}, {"debug", f.debug},
                              {"hideMulti", f.hideMulti}, {"hideLh2", f.hideLh2}, {"hidden", f.hidden}}}};
    std::error_code ec;
    std::filesystem::create_directories(SettingsPath().parent_path(), ec);
    std::ofstream(SettingsPath()) << j.dump(2) << "\n";
  }
};

void Section(const char* title) {
  ImGui::Spacing();
  ImGui::SeparatorText(title);
}
}  // namespace

int main() {
  if (!glfwInit()) {
    std::fprintf(stderr, "openhouse-viewer: no display\n");
    return 1;
  }
  GLFWwindow* window = glfwCreateWindow(1280, 820, "openhouse viewer", nullptr, nullptr);
  if (!window) return 1;
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;  // our own settings file; the layout is fixed
  ImGui::StyleColorsDark();
  ImGui::GetStyle().WindowRounding = 0;
  ImGui::GetStyle().FrameRounding = 3;
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL2_Init();

  TelemetryClient client(lh::telemetry::PortFromEnvironment() > 0 ? lh::telemetry::PortFromEnvironment()
                                                                   : lh::telemetry::kDefaultPort);
  LogView logs;
  Settings settings;
  settings.Load(logs.Filter());
  if (lh::MinimumLogLevelFromEnvironment() == lh::LogLevel::Debug) logs.Filter().debug = true;  // LH_LOG_LEVEL=DEBUG
  float logShare = 0.38f;  // of the window height

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    client.Poll(glfwGetTime());
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("openhouse", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Basic", nullptr, settings.detail == Detail::Basic)) settings.detail = Detail::Basic;
        if (ImGui::MenuItem("Advanced", nullptr, settings.detail == Detail::Advanced)) settings.detail = Detail::Advanced;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Logs")) {
        ImGui::MenuItem("Show DEBUG messages", nullptr, &logs.Filter().debug);
        ImGui::MenuItem("Hide multi-station chatter", nullptr, &logs.Filter().hideMulti);
        ImGui::MenuItem("Hide 2.0 light chatter", nullptr, &logs.Filter().hideLh2);
        if (ImGui::MenuItem("Show all hidden kinds again", nullptr, false, !logs.Filter().hidden.empty())) logs.Filter().hidden.clear();
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    DrawConnection(client);
    if (client.HaveState()) DrawRoom(client.Current());
    const State& state = client.Current();

    const float top = ImGui::GetContentRegionAvail().y * (1 - logShare);
    ImGui::BeginChild("status", ImVec2(0, top), ImGuiChildFlags_None);
    const float left = ImGui::GetContentRegionAvail().x * 0.6f;
    ImGui::BeginChild("tables", ImVec2(left, 0), ImGuiChildFlags_None);
    Section("Devices");
    if (state.devices.empty()) ImGui::TextDisabled("No tracked devices yet");
    else DrawDevices(state, settings.detail);
    Section("Base stations");
    if (state.bases.empty()) ImGui::TextDisabled("No base stations seen yet");
    else DrawBases(state, settings.detail);
    if (settings.detail == Detail::Advanced) {
      Section("Wireless receivers");
      DrawReceivers(state);
      if (!state.room.id.empty()) {
        Section("Room");
        ImGui::Text("Room %s, version %llu%s", state.room.id.c_str(), state.room.epoch, state.room.settled ? ", settled" : "");
        ImGui::TextDisabled("A base station counts as placed below %.0f mm uncertainty.", state.room.settleSigma * 1000);
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("map", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawMap(state);
    ImGui::EndChild();
    ImGui::EndChild();

    // A draggable splitter between the status and the log.
    ImGui::InvisibleButton("splitter", ImVec2(-1, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) logShare = std::clamp(logShare - ImGui::GetIO().MouseDelta.y / vp->WorkSize.y, 0.15f, 0.85f);
    Section("Log");
    logs.Draw(client);
    ImGui::End();

    ImGui::Render();
    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.07f, 0.08f, 0.1f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }
  settings.Save(logs.Filter());
  ImGui_ImplOpenGL2_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
