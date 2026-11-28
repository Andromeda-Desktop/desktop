// theme.desktop_theme through the shell's path: ConfigService loads the id, DesktopTheme writes the token slots, the
// config defaults follow the new tokens, ThemeService takes the theme's scheme pin; overrides apply without a theme.
// The Settings font and dock rows write theme-overrides.toml through ConfigService; the old shell keys and the
// compositor's window corner and blur saturation move there.

#include "config/config_migrations.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/files/directory_watcher.h"
#include "core/files/file_watcher.h"
#include "design_tokens.h"
#include "render/core/color.h"
#include "net/http_client.h"
#include "protocols/async_file_writer.h"
#include "tests/test_check.h"
#include "theme/desktop_theme.h"
#include "theme/theme_service.h"
#include "theme/theme_store.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    return desktopTheme.sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
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
  write(shellOverrides, "[shell]\nfont_family = \"Old\"\n[dock]\nicon_size = \"big\"\n");
  write(overrides, "[values]\n\"font.ui\" = \"Kept\"\n");
  {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
      return desktopTheme.sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
    });
    TEST_CHECK(Tokens::fontFamily == "Kept");
    migrating.moveLegacyThemeKeys();
    TEST_CHECK(Tokens::fontFamily == "Kept");
    TEST_CHECK(!migrating.hasOverride({"shell", "font_family"}));
    // A value of the wrong type is dropped and blocks no other key.
    TEST_CHECK(!migrating.hasOverride({"dock", "icon_size"}));
    TEST_CHECK(!migrating.hasOverride({std::string(ConfigService::kThemeOverridesRoot), "dock.icon"}));
  }
  TEST_CHECK(landed(shellOverrides));
  TEST_CHECK(read(shellOverrides).find("font_family") == std::string::npos);
  TEST_CHECK(read(overrides).find("Kept") != std::string::npos);

  // No theme keys yet: the legacy values move; an empty font ("the system font") becomes "system", a number outside
  // the vocabulary range (dock.icon: 24..80) is clamped.
  write(shellOverrides, "[shell]\nfont_family = \"\"\n[dock]\nicon_size = 100\nradius = 10\n");
  std::filesystem::remove(overrides);
  {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
      return desktopTheme.sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
    });
    migrating.moveLegacyThemeKeys();
    TEST_CHECK(Tokens::fontFamily == "system");
    TEST_CHECK(migrating.hasOverride(fontPath));
    TEST_CHECK(!migrating.hasOverride({"shell", "font_family"}));
    TEST_CHECK(Tokens::componentDockIcon == 80.0F);
    TEST_CHECK(migrating.config().dock.iconSize == 80);
    TEST_CHECK(Tokens::componentDockRadius == 10.0F);
    TEST_CHECK(migrating.config().dock.radiusTopLeft == 10); // the corners follow dock.corner
    TEST_CHECK(!migrating.hasOverride({"dock", "icon_size"}) && !migrating.hasOverride({"dock", "radius"}));
  }
  TEST_CHECK(landed(shellOverrides) && landed(overrides));

  // The compositor's window corner and blur saturation move to theme-overrides.toml on the first batch: a value its
  // settings file holds is cleared there, a config file value stays. Values and ranges come from the shipped data.
  struct CompositorValue {
    std::string text;
    bool customized = true;
  };
  std::map<std::string, CompositorValue> compositor;
  std::vector<std::string> sent;
  const auto value = [&](std::string_view key) {
    const auto it = compositor.find(std::string(key));
    return it == compositor.end() ? std::string() : it->second.text;
  };
  const auto customized = [&](std::string_view key) {
    const auto it = compositor.find(std::string(key));
    return it != compositor.end() && it->second.customized;
  };
  const auto set = [&](const std::string& key, const std::string& text) { sent.push_back(key + "=" + text); };
  const auto builtIn = [](std::string_view token) {
    const auto& slots = Tokens::slots();
    return std::ranges::find(slots, token, &Tokens::Slot::path)->number;
  };
  const auto& vocabulary = noctalia::theme::shippedVocabulary();
  const double cornerMax = vocabulary.keys.find("corner.window")->second.max;
  const double saturationMax = vocabulary.keys.find("glass.saturation_light")->second.max;
  const double darkBuiltIn = builtIn("blur.saturation");
  const double lightBuiltIn = builtIn("blur_light.saturation");
  const std::string themeRoot(ConfigService::kThemeOverridesRoot);
  const auto migrate = [&] {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
      return desktopTheme.sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
    });
    sent.clear();
    migrating.moveLegacyCompositorThemeKeys(value, customized, set, "#c0ffee");
    return migrating.hasOverride({themeRoot, "corner.window"});
  };
  const std::vector<std::string> bothCleared{"appearance.corner_radius=", "appearance.blur.saturation="};

  // A number is clamped to the key's range; the old saturation goes to each scheme whose key is unset.
  const double chosen = (darkBuiltIn + lightBuiltIn) / 2.0;
  write(overrides, std::format("[values]\n\"glass.saturation_light\" = {}\n", saturationMax));
  compositor = {
      {"appearance.corner_radius", {std::format("{}", cornerMax + 8.0)}},
      {"appearance.blur.saturation", {std::format("{}", chosen)}},
  };
  TEST_CHECK(migrate());
  TEST_CHECK(Tokens::componentWindowRadius == static_cast<float>(cornerMax));
  TEST_CHECK(near(Tokens::blurSaturation, static_cast<float>(chosen)));
  TEST_CHECK(near(Tokens::blurLightSaturation, static_cast<float>(saturationMax)));
  TEST_CHECK(sent == bothCleared);
  TEST_CHECK(landed(overrides));

  // A value that equals one scheme's built-in skips only that scheme; a radius an earlier Desktop Layout saved is only
  // cleared.
  std::filesystem::remove(overrides);
  compositor = {
      {"appearance.corner_radius", {std::format("{}", noctalia::config::kWindowCornerShellWritten.front())}},
      {"appearance.blur.saturation", {std::format("{}", lightBuiltIn)}},
  };
  TEST_CHECK(!migrate());
  TEST_CHECK(near(Tokens::blurSaturation, static_cast<float>(lightBuiltIn)));
  TEST_CHECK(read(overrides).find("glass.saturation_light") == std::string::npos);
  TEST_CHECK(sent == bothCleared);
  TEST_CHECK(landed(overrides));

  // A config file value moves too, but only the settings file can be cleared.
  std::filesystem::remove(overrides);
  compositor = {{"appearance.corner_radius", {std::format("{}", cornerMax - 1.0), false}}};
  TEST_CHECK(migrate());
  TEST_CHECK(Tokens::componentWindowRadius == static_cast<float>(cornerMax - 1.0));
  TEST_CHECK(sent.empty());
  TEST_CHECK(landed(overrides));

  // The accent moves to both schemes. The source becomes custom unless it is the wallpaper; the tokens show the moved
  // accent only with the custom source. A value equal to a theme accent or to the published one (#c0ffee here, with
  // high contrast) came from an earlier shell: only cleared.
  const auto tokenRgb = [](std::uint32_t rgba) { return rgba >> 8U; };
  const auto migrateAccent = [&](std::string_view source, const std::string& accent) {
    ConfigService migrating;
    migrating.setTokenSource([&](const Config& c, const std::optional<std::string>& text) {
      return desktopTheme.sync(c.theme.desktopTheme, c.theme.accentSource == AccentSource::Custom, text);
    });
    TEST_CHECK(migrating.setOverride({"theme", "accent_source"}, std::string(source)));
    compositor = {{"appearance.accent", {accent}}};
    sent.clear();
    migrating.moveLegacyCompositorThemeKeys(value, customized, set, "#c0ffee");
    TEST_CHECK(sent == std::vector<std::string>{"appearance.accent="});
    const std::pair after{migrating.config().theme.accentSource, tokenRgb(Tokens::schemeLight.accent)};
    TEST_CHECK(migrating.clearOverrides({{"theme", "accent_source"}}, nullptr));
    return after;
  };
  std::filesystem::remove(overrides);
  TEST_CHECK((migrateAccent("theme", "#2B7D34FF") == std::pair{AccentSource::Custom, 0x2b7d34U}));
  TEST_CHECK(read(overrides).find("[values.dark]\naccent = \"#2b7d34\"") != std::string::npos);
  TEST_CHECK(read(overrides).find("[values.light]\naccent = \"#2b7d34\"") != std::string::npos);
  std::filesystem::remove(overrides);
  const std::uint32_t themeLightAccent = tokenRgb(Tokens::schemeLight.accent);
  TEST_CHECK((migrateAccent("wallpaper", "#2b7d34") == std::pair{AccentSource::Wallpaper, themeLightAccent}));
  TEST_CHECK(read(overrides).find("#2b7d34") != std::string::npos);
  std::filesystem::remove(overrides);
  TEST_CHECK((migrateAccent("theme", formatRgbHex(rgbaHex(Tokens::schemeDark.accent)))
              == std::pair{AccentSource::Theme, themeLightAccent}));
  TEST_CHECK(read(overrides).find("accent") == std::string::npos);
  TEST_CHECK((migrateAccent("theme", "#C0FFEEFF") == std::pair{AccentSource::Theme, themeLightAccent}));
  TEST_CHECK(read(overrides).find("accent") == std::string::npos);
  TEST_CHECK(landed(overrides) && landed(shellOverrides));

  // Only the first batch moves: later ones show the shell's layer values. A refused write keeps the compositor's.
  write(overrides, "[values\n");
  sent.clear();
  compositor = {{"appearance.corner_radius", {std::format("{}", cornerMax - 2.0)}}};
  {
    ConfigService migrating;
    migrating.moveLegacyCompositorThemeKeys(value, customized, set, "#c0ffee");
    TEST_CHECK(sent.empty());
    std::filesystem::remove(overrides);
    migrating.moveLegacyCompositorThemeKeys(value, customized, set, "#c0ffee");
    TEST_CHECK(sent.empty());
    TEST_CHECK(!migrating.hasOverride({themeRoot, "corner.window"}));
  }

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
    TEST_CHECK(dev.sync("deep", false, std::nullopt));
    TEST_CHECK(Tokens::componentDockLensRadius == 24.0F); // 30 from the theme - 6 from the dev file
    TEST_CHECK(Tokens::componentWindowRadius == 3.0F);
    TEST_CHECK(!dev.sync("deep", false, std::nullopt));
    ::unsetenv("ANDROMEDA_DEV_TOKENS");
  }
#endif

  std::filesystem::remove_all(root);
  return 0;
}
