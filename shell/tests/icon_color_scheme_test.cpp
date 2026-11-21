#include "render/core/icon_color_scheme.h"
#include "render/core/image_file_loader.h"

#include <cstdint>
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
      std::println(stderr, "icon_color_scheme_test: {}", message);
    }
    return condition;
  }

  // A KDE color-scheme icon: a square filled with currentColor, blue unless a stylesheet restyles the class.
  constexpr std::string_view kIcon =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">)"
      R"(<style id="current-color-scheme">.ColorScheme-Text { color: #0000ff; }</style>)"
      R"(<rect class="ColorScheme-Text" width="16" height="16" fill="currentColor"/></svg>)";

  // RGB of the center pixel.
  std::uint32_t centerRgb(const std::string& path) {
    const auto loaded = loadImageFile(path, 16, false);
    if (!loaded || loaded->width <= 0 || loaded->height <= 0) {
      return 0xFFFFFFFF;
    }
    const std::size_t i = (static_cast<std::size_t>(loaded->height / 2) * loaded->width + loaded->width / 2) * 4;
    return (std::uint32_t{loaded->rgba[i]} << 16) | (std::uint32_t{loaded->rgba[i + 1]} << 8) | loaded->rgba[i + 2];
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("icon_color_scheme_test_" + std::to_string(::getpid()));
  const fs::path themed = root / "theme" / "apps";
  const fs::path other = root / "other";
  fs::create_directories(themed);
  fs::create_directories(other);
  std::ofstream(themed / "icon.svg") << kIcon;
  std::ofstream(other / "icon.svg") << kIcon;

  bool ok = true;
  ok &= expect(centerRgb(themed / "icon.svg") == 0x0000FF, "without a scheme the icon keeps its own colour");

  icon_color_scheme::set({(root / "theme").string()}, ".ColorScheme-Text { color: #ff0000 !important; }");
  ok &= expect(centerRgb(themed / "icon.svg") == 0xFF0000, "an icon under a following theme takes the scheme");
  ok &= expect(centerRgb(other / "icon.svg") == 0x0000FF, "an icon outside the theme keeps its own colour");

  icon_color_scheme::set({}, "");
  ok &= expect(centerRgb(themed / "icon.svg") == 0x0000FF, "no roots turns the scheme off");

  fs::remove_all(root);
  return ok ? 0 : 1;
}
