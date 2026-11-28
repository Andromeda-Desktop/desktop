// theme.desktop_theme through the shell's path: ConfigService loads the id, DesktopTheme writes the token slots, the
// config defaults follow the new tokens, ThemeService takes the theme's scheme pin; overrides apply without a theme.
// The Settings font row writes theme-overrides.toml through ConfigService; [shell] font_family moves there once.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "design_tokens.h"
#include "net/http_client.h"
#include "protocols/async_file_writer.h"
#include "tests/test_check.h"
#include "theme/desktop_theme.h"
#include "theme/theme_service.h"
#include "ui/palette.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << text;
  }

  bool near(float a, float b) { return std::fabs(a - b) < 0.002F; }

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  }

  // True once no queued write of `path` is left (the async writer landed it), within 5 s.
  bool landed(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (andromeda::pendingFileText(path)) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-desktop-theme-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  ::setenv("ANDROMEDA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("ANDROMEDA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("ANDROMEDA_DATA_HOME", (root / "data").c_str(), 1);
  ::unsetenv("ANDROMEDA_DEV_TOKENS");
  std::filesystem::create_directories(root / "config" / "andromeda" / "shell");
  const auto themes = root / "data" / "andromeda" / "desktop-themes";
  const auto shellConfig = root / "config" / "andromeda" / "shell" / "config.toml";

  const float builtInIcon = Tokens::componentDockIcon;
  const float builtInLens = Tokens::componentDockLensRadius;

  write(themes / "deep" / "theme.toml", "[theme]\nformat = 1\nid = \"deep\"\nname = \"Deep\"\nschemes = [\"dark\"]\n"
                                        "[values]\n\"dock.icon\" = 64\n\"dock.corner\" = 30\n"
                                        "[values.dark]\naccent = \"#f2c879\"\n");
  write(root / "config" / "andromeda" / "theme-overrides.toml", "[values]\n\"corner.window\" = 9\n");
  write(shellConfig, "[theme]\nmode = \"light\"\ndesktop_theme = \"deep\"\n");

  DirectoryWatcher dirs;
  FileWatcher files(dirs);
  noctalia::theme::DesktopTheme desktopTheme(files);
  TEST_CHECK(Tokens::componentWindowRadius == 9.0F); // overrides apply over the built-in tokens, before a theme

  ConfigService config;
  TEST_CHECK(config.config().dock.iconSize == static_cast<int>(builtInIcon));
  config.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
    return desktopTheme.sync(c.theme.desktopTheme, text);
  });

  TEST_CHECK(Tokens::componentDockIcon == 64.0F);
  TEST_CHECK(config.config().dock.iconSize == 64); // config defaults read the theme's tokens
  TEST_CHECK(Tokens::componentWindowRadius == 9.0F); // overrides stay above the theme
  TEST_CHECK(Tokens::componentDockLensRadius == 30.0F - Tokens::componentDockLensInset); // derived token follows
  const ::Color accent = tokenPalette(false).accent;
  TEST_CHECK(near(accent.r, 0xf2 / 255.0F) && near(accent.g, 0xc8 / 255.0F) && near(accent.b, 0x79 / 255.0F));

  HttpClient http;
  noctalia::theme::ThemeService theme(config, http, desktopTheme);
  theme.apply();
  for (auto& call : DeferredCall::takePending()) {
    call();
  }
  TEST_CHECK(theme.resolvedMode() == "dark"); // a one-scheme theme pins the desktop over mode = "light"
  TEST_CHECK(theme.resolvedShellMode() == "dark");

  // Back to the built-in tokens through a config reload.
  write(shellConfig, "[theme]\nmode = \"light\"\n");
  config.forceReload();
  TEST_CHECK(Tokens::componentDockIcon == builtInIcon);
  TEST_CHECK(config.config().dock.iconSize == static_cast<int>(builtInIcon));
  TEST_CHECK(Tokens::componentDockLensRadius == builtInLens);
  TEST_CHECK(Tokens::componentWindowRadius == 9.0F);
  theme.onConfigReload();
  TEST_CHECK(theme.resolvedMode() == "light");

  // The font row writes theme-overrides.toml; the token follows in the same reload, before the file lands.
  const auto overrides = root / "config" / "andromeda" / "theme-overrides.toml";
  const auto shellOverrides = root / "state" / "andromeda" / "shell" / "overrides.toml";
  const std::string builtInFont(Tokens::fontFamily);
  const std::vector<std::string> fontPath{std::string(ConfigService::kThemeOverridesRoot), "font.ui"};
  TEST_CHECK(config.setOverride(fontPath, std::string("Noto Sans")));
  TEST_CHECK(Tokens::fontFamily == "Noto Sans");
  TEST_CHECK(config.hasOverride(fontPath));
  TEST_CHECK(Tokens::componentWindowRadius == 9.0F); // the write keeps the file's other keys
  TEST_CHECK(!config.setOverride(fontPath, std::string())); // the theme loader ignores an empty family
  TEST_CHECK(Tokens::fontFamily == "Noto Sans");
  TEST_CHECK(landed(overrides));
  TEST_CHECK(config.clearOverride(fontPath));
  TEST_CHECK(Tokens::fontFamily == builtInFont);
  TEST_CHECK(!config.hasOverride(fontPath));
  TEST_CHECK(landed(overrides));

  // A file that does not parse refuses the write and stays as the user left it.
  write(overrides, "[values\n");
  TEST_CHECK(!config.setOverride(fontPath, std::string("Noto Sans")));
  TEST_CHECK(read(overrides) == "[values\n");

  // [shell] font_family moves once; a font.ui that theme-overrides.toml already holds wins.
  write(shellOverrides, "[shell]\nfont_family = \"Old\"\n");
  write(overrides, "[values]\n\"font.ui\" = \"Kept\"\n");
  {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
    return desktopTheme.sync(c.theme.desktopTheme, text);
  });
    TEST_CHECK(Tokens::fontFamily == "Kept");
    migrating.moveLegacyFontFamily();
    TEST_CHECK(Tokens::fontFamily == "Kept");
    TEST_CHECK(!migrating.hasOverride({"shell", "font_family"}));
  }
  TEST_CHECK(landed(shellOverrides));
  TEST_CHECK(read(shellOverrides).find("font_family") == std::string::npos);
  TEST_CHECK(read(overrides).find("Kept") != std::string::npos);

  // No font.ui yet: the legacy value moves; an empty one ("the system font") becomes "system".
  write(shellOverrides, "[shell]\nfont_family = \"\"\n");
  std::filesystem::remove(overrides);
  {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
    return desktopTheme.sync(c.theme.desktopTheme, text);
  });
    migrating.moveLegacyFontFamily();
    TEST_CHECK(Tokens::fontFamily == "system");
    TEST_CHECK(migrating.hasOverride(fontPath));
    TEST_CHECK(!migrating.hasOverride({"shell", "font_family"}));
  }
  TEST_CHECK(landed(shellOverrides) && landed(overrides));

#ifdef TOKENS_RUNTIME
  // Dev builds: the dev token file is the base layer; the theme's derived tokens evaluate over it.
  {
    write(root / "dev.toml", "[component.dock]\nradius = 20.0\nlens_inset = 6.0\nlens_radius = \"= component.dock.radius "
                             "- component.dock.lens_inset\"\n[component.window]\nradius = 3.0\n");
    ::setenv("ANDROMEDA_DEV_TOKENS", (root / "dev.toml").c_str(), 1);
    std::filesystem::remove(root / "config" / "andromeda" / "theme-overrides.toml");
    noctalia::theme::DesktopTheme dev(files);
    TEST_CHECK(Tokens::componentDockLensRadius == 14.0F);
    TEST_CHECK(Tokens::componentWindowRadius == 3.0F);
    TEST_CHECK(dev.sync("deep", std::nullopt));
    TEST_CHECK(Tokens::componentDockLensRadius == 24.0F); // 30 from the theme - 6 from the dev file
    TEST_CHECK(Tokens::componentWindowRadius == 3.0F);
    TEST_CHECK(!dev.sync("deep", std::nullopt));
    ::unsetenv("ANDROMEDA_DEV_TOKENS");
  }
#endif

  std::filesystem::remove_all(root);
  return 0;
}
