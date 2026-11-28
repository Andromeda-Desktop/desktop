// theme.desktop_theme through the shell's path: ConfigService loads the id, DesktopTheme writes the token slots, the
// config defaults follow the new tokens, ThemeService takes the theme's scheme pin; overrides apply without a theme.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "design_tokens.h"
#include "net/http_client.h"
#include "tests/test_check.h"
#include "theme/desktop_theme.h"
#include "theme/theme_service.h"
#include "ui/palette.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  void write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << text;
  }

  bool near(float a, float b) { return std::fabs(a - b) < 0.002F; }

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
  config.setTokenSource([&](const Config& c) { return desktopTheme.select(c.theme.desktopTheme); });

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
    TEST_CHECK(dev.select("deep"));
    TEST_CHECK(Tokens::componentDockLensRadius == 24.0F); // 30 from the theme - 6 from the dev file
    TEST_CHECK(Tokens::componentWindowRadius == 3.0F);
    TEST_CHECK(!dev.select("deep"));
    ::unsetenv("ANDROMEDA_DEV_TOKENS");
  }
#endif

  std::filesystem::remove_all(root);
  return 0;
}
