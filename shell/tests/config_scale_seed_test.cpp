// The shell scale keys are the only user keys; the compositor's appearance.ui_scale and corner_scale mirror them. A
// shell key that no config file sets takes the compositor's value once, clamped to the shell range.
#include "config/config_service.h"
#include "config/config_types.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_scale_seed: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  bool near(float a, float b) { return std::abs(a - b) < 1.0e-4F; }

  void batch(ConfigService& config, std::string uiScale, std::string cornerScale) {
    const std::map<std::string, std::string, std::less<>> values{
        {"appearance.ui_scale", std::move(uiScale)}, {"appearance.corner_scale", std::move(cornerScale)}};
    config.seedScalesFromCompositor([&values](std::string_view key) {
      const auto it = values.find(key);
      return it != values.end() ? it->second : std::string();
    });
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-scale-seed-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const std::filesystem::path configFile = root / "config" / "andromeda" / "shell" / "config.toml";
  const std::filesystem::path overridesFile = root / "state" / "andromeda" / "shell" / "overrides.toml";
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  ::unsetenv("ANDROMEDA_STATE_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");

  writeFile(configFile, "[dock]\nenabled = false\n");
  {
    ConfigService config;
    batch(config, "1.25", "0.5");
    expect(near(config.config().accessibility.uiScale, 1.25F), "an unset ui_scale did not take the compositor value");
    expect(near(config.config().shell.cornerRadiusScale, 0.5F), "an unset corner scale did not take the compositor value");
    batch(config, "1.4", "1.5");
    expect(near(config.config().accessibility.uiScale, 1.25F), "a later batch (the shell's own mirror) seeded again");
  }
  {
    ConfigService restarted;
    batch(restarted, "1.4", "1.5");
    expect(near(restarted.config().accessibility.uiScale, 1.25F), "a seeded key was replaced at the next start");
    expect(near(restarted.config().shell.cornerRadiusScale, 0.5F), "a seeded corner scale was replaced at the next start");
  }

  std::filesystem::remove(overridesFile);
  writeFile(configFile, "[accessibility]\nui_scale = 1.1\n");
  {
    ConfigService config;
    batch(config, "1.4", "3");
    expect(near(config.config().accessibility.uiScale, 1.1F), "a ui_scale the config file sets was replaced");
    expect(near(config.config().shell.cornerRadiusScale, 2.0F), "the compositor corner scale was not clamped to the shell range");
  }

  std::filesystem::remove(overridesFile);
  writeFile(configFile, "[dock]\nenabled = false\n");
  {
    ConfigService config;
    batch(config, "9", "1");
    expect(near(config.config().accessibility.uiScale, 1.5F), "the compositor ui scale was not clamped to the shell range");
    expect(!readFile(overridesFile).contains("corner_radius_scale"), "a value equal to the shell default was written");
  }

  std::filesystem::remove(overridesFile);
  {
    ConfigService config;
    batch(config, "abc", "");
    expect(near(config.config().accessibility.uiScale, 1.0F), "an unparsable compositor value changed the shell key");
    expect(!readFile(overridesFile).contains("scale"), "an unparsable compositor value was written");
  }

  std::filesystem::remove_all(root);
  if (g_failures != 0) {
    std::println(stderr, "config_scale_seed: {} failure(s)", g_failures);
    return 1;
  }
  std::println("config_scale_seed: ok");
  return 0;
}
