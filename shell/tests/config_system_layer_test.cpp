#include "config/config_merge.h"

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
      std::println(stderr, "config_system_layer_test: {}", message);
    }
    return condition;
  }

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("config_system_layer_test_" + std::to_string(::getpid()));
  const fs::path user = root / "home" / "andromeda" / "shell";
  const fs::path vendor = root / "vendor";
  const fs::path site = root / "site";
  write(user / "config.toml", "[bar]\nposition = \"bottom\"\n");
  write(site / "andromeda" / "shell" / "config.toml", "[bar]\nposition = \"left\"\nheight = 40\n");
  write(vendor / "andromeda" / "shell" / "config.toml", "[bar]\nheight = 30\nlayer = \"top\"\n");
  ::setenv("XDG_CONFIG_DIRS", (site.string() + ":relative/dir:" + vendor.string() + ":" + (root / "home").string()).c_str(), 1);

  bool ok = true;
  const auto dirs = noctalia::config::systemConfigDirs(user.string());
  ok &= expect(dirs.size() == 2, "relative entries and the user dir itself are skipped");

  const auto merged = noctalia::config::mergeConfigWithIncludes(user.string(), dirs).merged;
  ok &= expect(merged["bar"]["position"].value_or(std::string{}) == "bottom", "the user file wins over every system file");
  ok &= expect(merged["bar"]["height"].value_or(0) == 40, "an earlier XDG_CONFIG_DIRS entry wins over a later one");
  ok &= expect(merged["bar"]["layer"].value_or(std::string{}) == "top", "a key only a system file sets is kept");

  const auto userOnly = noctalia::config::mergeConfigWithIncludes(user.string(), {}).merged;
  ok &= expect(!userOnly["bar"]["height"], "without system dirs only the user files merge");

  ::unsetenv("XDG_CONFIG_DIRS");
  const auto fallback = noctalia::config::systemConfigDirs(user.string());
  ok &= expect(fallback.size() == 1 && fallback[0] == fs::path("/etc/xdg/andromeda/shell"), "unset falls back to /etc/xdg");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
