#include "theme_cli.h"

#include "settings_store.h"
#include "theme_store.h"
#include "ui/contrast.h"
#include "ui/palette.h"
#include "ui/theme_palette.h"

#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace term {

  namespace {
    constexpr std::string_view kUsage = R"(usage: andromeda-terminal theme <command>
  new <id>        make <themes>/<id>/theme.toml from the desktop's colours
  validate <id>   check a package; prints file:line: message on an error
  preview <id>    print the derived light and dark palettes with their contrast ratios
  apply <id>      validate, then set it as the terminal theme (open windows follow)
<id> is a folder name under the themes folder, or a path to a package folder.
)";

    std::filesystem::path folderOf(std::string_view id) {
      return id.find('/') != std::string_view::npos ? std::filesystem::path(id) : themesDir() / id;
    }

    std::uint32_t rgb(const Color& c) {
      auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
      return (byte(c.r) << 16U) | (byte(c.g) << 8U) | byte(c.b);
    }

    Color color(std::uint32_t v) {
      return Color{static_cast<float>((v >> 16U) & 0xffU) / 255.0F, static_cast<float>((v >> 8U) & 0xffU) / 255.0F,
                   static_cast<float>(v & 0xffU) / 255.0F, 1.0F};
    }

    // The package, or nullopt after printing why not.
    std::optional<ThemePackage> load(std::string_view id) {
      const std::filesystem::path folder = folderOf(id);
      std::error_code ec;
      if (!std::filesystem::is_directory(folder, ec)) {
        std::fprintf(stderr, "%s: no such theme folder\n", folder.c_str());
        return std::nullopt;
      }
      ThemeEntry entry = readThemeFolder(folder);
      if (const auto* invalid = std::get_if<ThemeEntry::Invalid>(&entry.state)) {
        const std::string file = (folder / "theme.toml").string();
        if (invalid->error.line > 0) {
          std::fprintf(stderr, "%s:%d: %s\n", file.c_str(), invalid->error.line, invalid->error.message.c_str());
        } else {
          std::fprintf(stderr, "%s: %s\n", folder.c_str(), invalid->error.message.c_str());
        }
        return std::nullopt;
      }
      return std::get<ThemeEntry::Valid>(std::move(entry.state)).package;
    }

    int create(std::string_view id) {
      if (!isThemeId(id) || builtinTheme(id)) {
        std::fprintf(stderr, "%.*s: use a-z, 0-9, - and _, and not a built-in theme name\n", static_cast<int>(id.size()),
                     id.data());
        return 2;
      }
      const std::filesystem::path folder = themesDir() / id;
      std::error_code ec;
      if (std::filesystem::exists(folder, ec)) {
        std::fprintf(stderr, "%s: exists\n", folder.c_str());
        return 1;
      }
      std::filesystem::create_directories(folder, ec);
      std::ofstream out(folder / "theme.toml");
      out << std::format("name = \"{}\"\n", id);
      for (const bool light : {true, false}) {
        const Palette p = tokenPalette(light);
        out << std::format("\n[{}]\nsurface = \"#{:06x}\"\ntext = \"#{:06x}\"\naccent = \"#{:06x}\"\n", light ? "light" : "dark",
                           rgb(p.solidRegular), rgb(p.label1), rgb(p.accent));
      }
      out.close();
      if (!out) {
        std::fprintf(stderr, "%s: cannot write\n", folder.c_str());
        return 1;
      }
      std::printf("%s\n", (folder / "theme.toml").c_str());
      return 0;
    }

    int preview(const ThemePackage& theme) {
      for (const auto& [name, anchors] : {std::pair{"light", theme.light}, std::pair{"dark", theme.dark}}) {
        const ThemePalette p = deriveThemePalette({color(anchors.surface), color(anchors.text), color(anchors.accent)});
        std::printf("[%s] view #%06x\n", name, rgb(p.view));
        auto line = [&](std::string_view role, const Color& c) {
          std::printf("  %-8.*s #%06x %5.2f:1\n", static_cast<int>(role.size()), role.data(), rgb(c), contrastRatio(c, p.view));
        };
        line("label1", p.label1);
        line("label2", p.label2);
        line("accent", p.accent);
        for (std::size_t i = 0; i < p.ansi.size(); ++i) {
          line(std::format("ansi{}", i), p.ansi[i]);
        }
      }
      const char* backdrop = std::holds_alternative<ThemePackage::Shader>(theme.backdrop)    ? "scene.glsl"
                             : std::holds_alternative<ThemePackage::Picture>(theme.backdrop) ? "picture"
                                                                                             : "none";
      std::printf("backdrop %s\n", backdrop);
      return 0;
    }

    int apply(std::string_view id) {
      if (id.find('/') != std::string_view::npos) {
        std::fprintf(stderr, "apply takes a theme id: install the folder under %s first\n", themesDir().c_str());
        return 2;
      }
      if (!load(id)) {
        return 1;
      }
      SettingsStore store;
      if (const auto* invalid = std::get_if<SettingsStore::Invalid>(&store.fileState())) {
        std::fprintf(stderr, "%s:%d: %s (fix it first; it was not changed)\n", store.path().c_str(), invalid->error.line,
                     invalid->error.message.c_str());
        return 1;
      }
      Settings settings = store.settings();
      settings.theme = std::string(id);
      store.update(settings);
      return 0;
    }
  } // namespace

  std::string themeAgentPrompt() {
    std::error_code ec;
    const std::string self = std::filesystem::read_symlink("/proc/self/exe", ec).string();
    const std::string cli = ec ? std::string("andromeda-terminal") : self;
    return std::format(R"(Make an Andromeda Terminal theme with me. Ask me in one short question what mood or colours I want, unless I already said it.

Use the terminal's theme CLI ({0} theme ...):
- `{0} theme new <id>` makes {1}/<id>/theme.toml; <id> uses only a-z, 0-9, - and _.
- Edit theme.toml. It has exactly these keys; any other key is an error:
    name = "Display name"
    font = "Mono font family"   # optional; must be installed (fc-list :spacing=mono family)
    [light] and [dark], each with surface, text and accent = "#rrggbb" (both tables are required)
    palette = ["#rrggbb", ...]  # optional in [light] or [dark]: exactly 16 ANSI colours, 0-15
  Pick the anchors for mood: without a palette the terminal derives 16 ANSI colours at 7:1 from them, hues turned
  toward the accent. A palette entry below 4.5:1 (except 0) is lifted by lightness only. The accent colours the cursor.
- Optional backdrop beside theme.toml, one of: scene.glsl (GLSL ES 1.0 that defines vec4 scene(vec2 uv); inputs scene_time, scene_size, scene_dark, scene_surface, scene_text, scene_accent, scene_busy, scene_success, scene_failure, scene_bell; at most 64 KiB) or background.png/jpg/webp (at most 64 MiB).
- `{0} theme validate <id>` must pass; it prints file:line: message on an error.
- `{0} theme preview <id>` prints the derived palettes and contrast ratios.
- `{0} theme apply <id>` uses it; the window changes at once. Then ask me if I want changes.)",
                       cli, themesDir().string());
  }

  std::optional<int> runThemeCli(int argc, char* argv[]) {
    if (argc < 2 || std::string_view(argv[1]) != "theme") {
      return std::nullopt;
    }
    const std::string_view verb = argc > 2 ? argv[2] : "";
    if (argc != 4) {
      std::fputs(kUsage.data(), verb == "help" || verb == "--help" ? stdout : stderr);
      return verb == "help" || verb == "--help" ? 0 : 2;
    }
    const std::string_view id = argv[3];
    if (verb == "new") {
      return create(id);
    }
    if (verb == "validate") {
      return load(id) ? 0 : 1;
    }
    if (verb == "preview") {
      const auto theme = load(id);
      return theme ? preview(*theme) : 1;
    }
    if (verb == "apply") {
      return apply(id);
    }
    std::fputs(kUsage.data(), stderr);
    return 2;
  }

} // namespace term
