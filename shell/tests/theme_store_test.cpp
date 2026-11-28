#include "theme/theme_store.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace noctalia::theme;
namespace fs = std::filesystem;

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "theme_store_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  constexpr const char* kVocabulary = R"(
version = 1
roles = ["accent"]

[key."corner.window"]
type = "length"
range = [0, 32]
tokens = ["component.window.radius"]
)";

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  void writeTheme(const fs::path& root, std::string_view id, std::string_view name) {
    write(root / id / "theme.toml",
          std::format("[theme]\nformat = 1\nid = \"{}\"\nname = \"{}\"\n[values]\n\"corner.window\" = 10\n", id, name));
  }

  struct TempDir {
    fs::path path;
    TempDir() {
      std::string pattern = (fs::temp_directory_path() / "theme_store_test.XXXXXX").string();
      path = ::mkdtemp(pattern.data());
    }
    ~TempDir() { fs::remove_all(path); }
  };

  void userRootWins() {
    const TempDir dir;
    const auto vocabulary = *parseThemeVocabulary(kVocabulary);
    const ThemeRoots roots{.user = dir.path / "user", .system = dir.path / "system"};
    writeTheme(roots.system, "space", "System");
    writeTheme(roots.system, "only-system", "Only");
    writeTheme(roots.user, "space", "User");
    const auto space = loadTheme(roots, "space", vocabulary);
    expect(space && space->manifest.name == "User", "the user root shadows the system root");
    const auto only = loadTheme(roots, "only-system", vocabulary);
    expect(only && only->manifest.name == "Only", "a theme only in the system root loads");

    write(roots.user / "space" / "theme.toml", "[theme]\nformat = 9\n");
    expect(!loadTheme(roots, "space", vocabulary), "a broken user copy fails instead of loading the system theme");
    expect(!loadTheme(roots, "absent", vocabulary), "an id in no root fails");
  }

  void folderLimits() {
    const TempDir dir;
    const auto vocabulary = *parseThemeVocabulary(kVocabulary);
    const ThemeRoots roots{.user = dir.path / "user", .system = {}};
    writeTheme(roots.user, "linked", "Linked");
    write(roots.user / "linked" / "assets" / "a.png", "png");
    fs::create_symlink("assets/a.png", roots.user / "linked" / "inside.png");
    expect(loadTheme(roots, "linked", vocabulary).has_value(), "a symlink inside the folder is allowed");

    write(dir.path / "secret", "x");
    fs::create_symlink(dir.path / "secret", roots.user / "linked" / "assets" / "out.png");
    const auto escaped = loadTheme(roots, "linked", vocabulary);
    expect(!escaped && escaped.error().message.contains("outside"), "a symlink that leaves the folder fails the load");
    fs::remove(roots.user / "linked" / "assets" / "out.png");
    fs::create_directory_symlink(dir.path, roots.user / "linked" / "up");
    expect(!loadTheme(roots, "linked", vocabulary), "a directory symlink that leaves the folder fails the load");
    fs::remove(roots.user / "linked" / "up");

    expect(::mkfifo((roots.user / "linked" / "pipe").c_str(), 0600) == 0, "mkfifo");
    const auto fifo = loadTheme(roots, "linked", vocabulary);
    expect(!fifo && fifo.error().message.contains("not a regular file"), "a fifo fails the load");
    fs::remove(roots.user / "linked" / "pipe");

    expect(!checkThemeFolder(roots.user / "linked", {.maxBytes = 1024, .maxFiles = 3}), "more files than the limit fail");
    write(roots.user / "linked" / "assets" / "big.bin", std::string(2048, 'x'));
    expect(!checkThemeFolder(roots.user / "linked", {.maxBytes = 1024, .maxFiles = 100}), "more bytes than the limit fail");
    expect(checkThemeFolder(roots.user / "linked", {.maxBytes = 4096, .maxFiles = 100}).has_value(),
           "a folder within the limits passes");
  }

  void overridesFile() {
    const TempDir dir;
    const auto vocabulary = *parseThemeVocabulary(kVocabulary);
    const fs::path path = dir.path / "theme-overrides.toml";
    const auto none = loadThemeOverrides(path, vocabulary);
    expect(none && !none->has_value(), "no file means no overrides");

    write(path, "[values]\n\"corner.window\" = 6\n[values.light]\naccent = \"#112233\"\n[surface.bar]\nedge = \"top\"\n");
    const auto loaded = loadThemeOverrides(path, vocabulary);
    expect(loaded && loaded->has_value(), "an overrides file loads");
    const ThemePackage& o = **loaded;
    expect(o.values.contains("corner.window") && o.values.contains("scheme.light.accent"),
           "overrides set values and roles of either scheme");
    expect(std::ranges::any_of(o.issues, [](const ThemeIssue& i) { return i.key == "surface"; }),
           "a table other than [values] is reported and ignored");

    write(path, "[values\n");
    expect(!loadThemeOverrides(path, vocabulary), "a syntax error fails, so the caller keeps its last good overrides");
  }

} // namespace

int main() {
  userRootWins();
  folderLimits();
  overridesFile();
  if (!g_ok) {
    return 1;
  }
  std::println("theme_store_test: ok");
  return 0;
}
