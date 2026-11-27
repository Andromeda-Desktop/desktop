#include "theme/theme_package.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <variant>

using namespace noctalia::theme;

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "theme_package_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  template <typename T> const T* get(const ThemePackage& p, const std::string& key) {
    const auto it = p.values.find(key);
    return it == p.values.end() ? nullptr : std::get_if<T>(&it->second);
  }

  bool hasIssue(const ThemePackage& p, const std::string& key, int line) {
    return std::ranges::any_of(p.issues, [&](const ThemeIssue& i) { return i.key == key && i.line == line; });
  }

  constexpr const char* kVocabulary = R"(
version = 1
roles = ["accent", "card"]

[key."corner.window"]
type = "length"
range = [0, 32]
tokens = ["component.window.radius"]
aliases = ["window.corner"]

[key."font.ui"]
type = "font"
tokens = ["font.family"]

[key."glass.blur"]
type = "enum"
values = ["low", "medium"]
tokens = ["blur.tier"]

[key."spring.open"]
type = "spring"
tokens = ["spring.open"]
)";

  ThemeVocabulary vocabulary() {
    auto parsed = parseThemeVocabulary(kVocabulary);
    expect(parsed.has_value(), "the test vocabulary parses");
    return parsed.value_or(ThemeVocabulary{});
  }

  constexpr const char* kValid = R"(
[theme]
format = 1
id = "space-hud"
name = "Space HUD"
version = "1.0.0"

[contracts]
bar = 1

[values]
"corner.window" = 14
"font.ui" = "Inter Variable"
"glass.blur" = "low"
"spring.open" = { response = 0.26, bounce = 0.10 }
"window.corner" = 20

[values.dark]
accent = "#f2c879"
card = "#1c2030"

[values.light]
accent = "#5147e5"

[motion]
window_open = { kind = "popin", spring = "spring.open" }

[backdrop]
desktop = { scene = "galaxy", seed = 7, live = false }
)";

  void validPackage(const ThemeVocabulary& v) {
    const auto result = parseThemePackage(kValid, "space-hud", v);
    expect(result.has_value(), "a valid package parses");
    if (!result) {
      return;
    }
    const ThemePackage& p = *result;
    expect(p.manifest.extends == "andromeda", "extends defaults to andromeda");
    expect(p.manifest.schemes.size() == 2, "no schemes list means both schemes");
    expect(p.manifest.contracts.at("bar") == 1, "contract version read");
    expect(get<std::string>(p, "font.ui") && *get<std::string>(p, "font.ui") == "Inter Variable", "font read");
    expect(get<std::string>(p, "glass.blur") && *get<std::string>(p, "glass.blur") == "low", "enum read");
    const auto* spring = get<ThemeSpring>(p, "spring.open");
    expect(spring && spring->response == 0.26 && spring->bounce == 0.10, "spring read");
    // The old and the current name of one key are both set: the current name wins.
    expect(get<double>(p, "corner.window") && *get<double>(p, "corner.window") == 14.0, "the current name wins");
    expect(!p.values.contains("window.corner"), "an alias is never stored under its old name");
    expect(hasIssue(p, "window.corner", 16), "the report lists the ignored old name");
    const auto* dark = get<Color>(p, "scheme.dark.accent");
    const auto* light = get<Color>(p, "scheme.light.accent");
    expect(dark && light && !(*dark == *light), "dark and light accent are separate keys");
    expect(get<std::string>(p, "motion.window_open.kind") != nullptr, "engine tables flatten");
    expect(get<bool>(p, "backdrop.desktop.live") && !*get<bool>(p, "backdrop.desktop.live"), "bool kept");
    expect(p.issues.size() == 1, "only the alias is reported");
  }

  void badValuesFallBackPerKey(const ThemeVocabulary& v) {
    const auto result = parseThemePackage(R"([theme]
format = 1
id = "broken"
name = "Broken"
schemes = ["dark"]
[values]
"radius.r12" = 12
"corner.window" = 99
"glass.blur" = "ultra"
"spring.open" = { response = 0.26 }
"font.ui" = "Inter"
[values.dark]
accent = "#f2c8"
card = "#1c2030"
term_red = "#ff0000"
[values.light]
accent = "#5147e5"
[tables]
x = 1
)",
                                          "broken", v);
    expect(result.has_value(), "bad values do not reject the theme");
    if (!result) {
      return;
    }
    const ThemePackage& p = *result;
    expect(!p.values.contains("radius.r12") && hasIssue(p, "radius.r12", 7), "an internal token never reaches the table");
    expect(!p.values.contains("corner.window") && hasIssue(p, "corner.window", 8), "an out-of-range length inherits");
    expect(!p.values.contains("glass.blur") && hasIssue(p, "glass.blur", 9), "an unknown enum name inherits");
    expect(!p.values.contains("spring.open") && hasIssue(p, "spring.open", 10), "a half spring inherits");
    expect(get<std::string>(p, "font.ui") != nullptr, "a good key beside bad ones stays");
    expect(!p.values.contains("scheme.dark.accent") && hasIssue(p, "scheme.dark.accent", 13), "a bad colour inherits");
    expect(get<Color>(p, "scheme.dark.card") != nullptr, "the good colour beside it stays");
    expect(!p.values.contains("scheme.dark.term_red") && hasIssue(p, "scheme.dark.term_red", 15),
           "a colour outside the roles never reaches the table");
    expect(!p.values.contains("scheme.light.accent") && hasIssue(p, "values.light", 16),
           "values for a scheme the theme does not list are ignored");
    expect(hasIssue(p, "tables", 18), "an unknown table is reported");
  }

  void fatalManifest(const ThemeVocabulary& v) {
    const auto newer = parseThemePackage("[theme]\nformat = 2\nid = \"a-b\"\nname = \"x\"\n", "a-b", v);
    expect(!newer && newer.error().key == "theme.format" && newer.error().line == 2, "a newer format is refused");
    const auto folder = parseThemePackage("[theme]\nformat = 1\nid = \"a-b\"\nname = \"x\"\n", "a-c", v);
    expect(!folder && folder.error().key == "theme.id", "an id that differs from its folder is refused");
    const auto self = parseThemePackage("[theme]\nformat = 1\nid = \"a-b\"\nname = \"x\"\nextends = \"a-b\"\n", "a-b", v);
    expect(!self && self.error().key == "theme.extends", "a theme extending itself is refused");
    const auto none = parseThemePackage("name = \"x\"\n", "a-b", v);
    expect(!none && none.error().key == "theme", "a file without [theme] is refused");
    const auto syntax = parseThemePackage("[theme]\nformat = 1\nid = \n", "a-b", v);
    expect(!syntax && syntax.error().line == 3, "a syntax error reports its line");
    const auto base = parseThemePackage("[theme]\nformat = 1\nid = \"andromeda\"\nname = \"Andromeda\"\n", "andromeda", v);
    expect(base && base->manifest.extends.empty(), "the base theme extends nothing");
  }

  void badVocabulary() {
    const auto twice = parseThemeVocabulary(R"(version = 1
[key."a.b"]
type = "font"
tokens = ["x"]
aliases = ["old"]
[key."a.c"]
type = "font"
tokens = ["y"]
aliases = ["old"]
)");
    expect(!twice, "one alias for two keys is refused");
    const auto shadow = parseThemeVocabulary(R"(version = 1
[key."a.b"]
type = "font"
tokens = ["x"]
aliases = ["a.c"]
[key."a.c"]
type = "font"
tokens = ["y"]
)");
    expect(!shadow, "an alias equal to a current key is refused");
    const auto noRange = parseThemeVocabulary("version = 1\n[key.\"a\"]\ntype = \"length\"\ntokens = [\"x\"]\n");
    expect(!noRange, "a length without a range is refused");
  }

  // The shipped vocabulary must load, or every theme loses every value.
  void shippedVocabulary(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const auto parsed = parseThemeVocabulary(text.str());
    expect(in.good() && parsed.has_value(),
           std::format("{} loads: {}", path, parsed ? std::string("ok") : parsed.error().message));
  }

} // namespace

int main(int argc, char** argv) {
  const ThemeVocabulary v = vocabulary();
  validPackage(v);
  badValuesFallBackPerKey(v);
  fatalManifest(v);
  badVocabulary();
  if (argc > 1) {
    shippedVocabulary(argv[1]);
  }
  if (g_ok) {
    std::println("theme_package_test: ok");
  }
  return g_ok ? 0 : 1;
}
