/**
 * @file main.cpp
 * @brief Plugin entrypoints, config registration, and logging bootstrap.
 */
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <typeinfo>
#include <vector>

#include "dispatchers.h"
#include "hyprlang.hpp"
#include "layout/canvas/layout.h"
#include "model/stack.h" // for set_width_fractions
#include "overview/render.h"

HANDLE PHANDLE = nullptr;

namespace {
std::string log_file_path() {
  const char *home = std::getenv("HOME");
  const auto base =
      home ? std::filesystem::path(home) : std::filesystem::path("/tmp");
  return (base / ".hyprland/plugins/hyprscroller/hyprscroller.log").string();
}

void init_logging() {
  const auto path = log_file_path();
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path());

  spdlog::drop("hyprscroller");
  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, true);
  auto logger =
      std::make_shared<spdlog::logger>("hyprscroller", std::move(sink));
  spdlog::set_default_logger(std::move(logger));
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [hyprscroller] [%^%l%$] %v");
#ifndef NDEBUG
  spdlog::set_level(spdlog::level::debug);
#else
  spdlog::set_level(spdlog::level::info);
#endif
  spdlog::flush_on(spdlog::level::debug);
  spdlog::info("logging initialized path={}", path);
}

double parse_width_token(const std::string &token) {
  std::string t = token;
  t.erase(0, t.find_first_not_of(" \t"));
  t.erase(t.find_last_not_of(" \t") + 1);
  if (t.empty())
    return -1.0;

  if (t == "onethird")
    return 1.0 / 3.0;
  if (t == "onehalf")
    return 0.5;
  if (t == "twothirds")
    return 2.0 / 3.0;
  if (t == "maximized" || t == "floating")
    return -1.0;

  if (t.find('%') != std::string::npos) {
    double val = std::stod(t.substr(0, t.find('%')));
    return val / 100.0;
  }
  if (t.find('/') != std::string::npos) {
    size_t slash = t.find('/');
    double num = std::stod(t.substr(0, slash));
    double den = std::stod(t.substr(slash + 1));
    if (den != 0.0)
      return num / den;
    return 0.5;
  }
  double val = std::stod(t);
  return std::clamp(val, 0.01, 1.0);
}

// Parse the config value directly using the same method as original code
void load_width_presets() {
  static auto const *column_default_width =
      (Hyprlang::STRING const *)HyprlandAPI::getConfigValue(
          PHANDLE, "plugin:scroller:column_default_width")
          ->getDataStaticPtr();
  const std::string presetStr = *column_default_width;

  std::vector<double> widthFractions;
  std::string str = presetStr;
  str.erase(std::remove_if(str.begin(), str.end(), ::isspace), str.end());
  if (str.empty()) {
    widthFractions = {1.0 / 3.0, 0.5, 2.0 / 3.0};
  } else {
    std::vector<std::string> tokens;
    size_t start = 0, end;
    while ((end = str.find(',', start)) != std::string::npos) {
      tokens.push_back(str.substr(start, end - start));
      start = end + 1;
    }
    tokens.push_back(str.substr(start));

    for (const auto &token : tokens) {
      double frac = parse_width_token(token);
      if (frac > 0.0) {
        widthFractions.push_back(frac);
      }
    }
    if (widthFractions.empty())
      widthFractions = {0.5};
  }
  ScrollerModel::set_width_fractions(widthFractions);
  spdlog::info("Loaded width presets: {} fractions", widthFractions.size());
  for (size_t i = 0; i < widthFractions.size(); ++i)
    spdlog::debug("  [{}] = {}", i, widthFractions[i]);
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
  PHANDLE = handle;
  init_logging();
  spdlog::info("pluginInit handle={}", static_cast<const void *>(handle));

#ifdef COLORS_IPC
  HyprlandAPI::addConfigValue(
      PHANDLE, "plugin:scroller:col.freecolumn_border",
      Hyprlang::CConfigValue(Hyprlang::INT(0xff9e1515)));
#endif

  HyprlandAPI::addConfigValue(PHANDLE, "plugin:scroller:column_default_width",
                              Hyprlang::STRING{"onehalf"});
  HyprlandAPI::addConfigValue(PHANDLE, "plugin:scroller:focus_wrap",
                              Hyprlang::INT{0});

  // Load width presets after config values are registered
  load_width_presets();

  dispatchers::addDispatchers();

  HyprlandAPI::addTiledAlgo(PHANDLE, "scroller", &typeid(CanvasLayout),
                            []() -> UP<Layout::ITiledAlgorithm> {
                              return makeUnique<CanvasLayout>();
                            });

  Overview::initializeRendererHooks(PHANDLE);

  return {"hyprscroller", "scrolling window layout", "dawser", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
  Overview::shutdownRendererHooks(PHANDLE);
  spdlog::info("pluginExit");
}
