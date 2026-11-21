#include "system/system_font.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "system_font_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("system_font_test_" + std::to_string(::getpid()));
  fs::create_directories(root / "gtk-3.0");
  ::setenv("XDG_CONFIG_HOME", root.c_str(), 1);
  ::setenv("DCONF_PROFILE", (root / "no-profile").c_str(), 1);

  bool ok = true;
  ok &= expect(!system_font::readFamily().has_value(), "no settings.ini names no family");

  std::ofstream(root / "gtk-3.0" / "settings.ini") << "[Settings]\ngtk-font-name=\"Noto Sans Bold 11\"\n";
  const auto family = system_font::readFamily();
  ok &= expect(family == "Noto Sans", "gtk-3.0 settings.ini gives the family without style and size");

  ok &= expect(system_font::resolveFamily("", family) == "Noto Sans", "empty config follows the system family");
  ok &= expect(system_font::resolveFamily("  Fira Sans ", family) == "Fira Sans", "a configured family wins");
  ok &= expect(system_font::resolveFamily("", std::nullopt) == "Inter", "no system family falls back to Inter");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
