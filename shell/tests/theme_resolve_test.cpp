#include "theme/theme_resolve.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <format>
#include <map>
#include <print>
#include <string>
#include <variant>

using namespace noctalia::theme;

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "theme_resolve_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  constexpr const char* kVocabulary = R"(
version = 1
roles = ["accent", "card"]

[key."corner.window"]
type = "length"
range = [0, 32]
tokens = ["component.window.radius"]

[key."corner.card"]
type = "length"
range = [0, 32]
tokens = ["component.window.card_radius", "component.list.card_radius"]

[key."corner.menu"]
type = "length"
range = [0, 32]
tokens = ["component.menu.radius"]

[key."corner.popover"]
type = "length"
range = [0, 32]
tokens = ["component.popover.radius"]
aliases = ["corner.bubble"]

[key."font.ui"]
type = "font"
tokens = ["font.family"]

[key."spring.open"]
type = "spring"
tokens = ["spring.open"]
)";

  std::string theme(std::string_view id, std::string_view extends, std::string_view body) {
    const std::string parent = extends.empty() ? "" : std::format("extends = \"{}\"\n", extends);
    return std::format("[theme]\nformat = 1\nid = \"{}\"\nname = \"{}\"\n{}{}\n", id, id, parent, body);
  }

  struct Fixture {
    ThemeVocabulary vocabulary = *parseThemeVocabulary(kVocabulary);
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, double, std::less<>> builtIn = {{"corner.window", 12.0}, {"corner.menu", 9.0}};

    std::expected<ResolvedTheme, ThemeIssue> resolve(std::string_view id, const ThemePackage* overrides = nullptr) {
      const ThemeLookup lookup = [this](std::string_view name) -> std::expected<ThemePackage, ThemeIssue> {
        const auto it = files.find(name);
        if (it == files.end()) {
          return std::unexpected(ThemeIssue{.key = "theme", .message = "not installed"});
        }
        return parseThemePackage(it->second, name, vocabulary);
      };
      const BuiltInNumber numbers = [this](std::string_view key) -> std::optional<double> {
        const auto it = builtIn.find(key);
        return it == builtIn.end() ? std::nullopt : std::optional(it->second);
      };
      return resolveTheme(id, lookup, overrides, vocabulary, numbers);
    }

    ResolvedTheme must(std::string_view id, const ThemePackage* overrides = nullptr) {
      auto t = resolve(id, overrides);
      if (!t) {
        std::println(stderr, "theme_resolve_test: FAIL: '{}' does not resolve: {} {} (line {})", id, t.error().key,
                     t.error().message, t.error().line);
        std::exit(1);
      }
      return std::move(*t);
    }
  };

  std::optional<double> number(const ResolvedTheme& t, const std::string& key) {
    const auto it = t.values.find(key);
    const auto* value = it == t.values.end() ? nullptr : std::get_if<double>(&it->second);
    return value ? std::optional(*value) : std::nullopt;
  }

  bool hasIssue(const ResolvedTheme& t, std::string_view source, std::string_view key) {
    return std::ranges::any_of(t.issues, [&](const ThemeIssue& i) { return i.source == source && i.key == key; });
  }

  void referencesResolveOverTheMergedTable() {
    Fixture f;
    f.files["andromeda"] = theme("andromeda", "", "[values]\n\"corner.window\" = 10\n");
    f.files["mid"] = theme("mid", "andromeda", "[values]\n\"corner.card\" = \"=corner.window - 4\"\n");
    f.files["leaf"] = theme("leaf", "mid", "[values]\n\"corner.window\" = 20\n");
    const auto t = f.must("leaf");
    expect(t.chain == std::vector<std::string>{"leaf", "mid", "andromeda"}, "chain is leaf first");
    expect(number(t, "corner.window") == 20.0, "the leaf wins over its ancestor");
    expect(number(t, "corner.card") == 16.0, "an ancestor's reference reads the leaf's value");
    expect(t.origin.at("corner.card") == "mid", "origin names the layer that set the key");

    const auto overrides = parseThemePackage(theme("user", "", "[values]\n\"corner.window\" = 30\n"), "user", f.vocabulary);
    const auto o = f.must("leaf", &*overrides);
    expect(number(o, "corner.window") == 30.0 && number(o, "corner.card") == 26.0, "overrides sit above the theme");
    expect(o.origin.at("corner.window") == "overrides", "the overrides layer is named");
  }

  void badReferenceFallsToTheLayerBelow() {
    Fixture f;
    f.files["andromeda"] = theme("andromeda", "", "[values]\n\"corner.card\" = 10\n\"corner.menu\" = 8\n");
    f.files["leaf"] = theme("leaf", "andromeda",
                            "[values]\n\"corner.card\" = \"=font.ui\"\n\"corner.menu\" = \"=corner.window * 4\"\n");
    const auto t = f.must("leaf");
    expect(number(t, "corner.card") == 10.0, "a reference to a non-number key falls to the ancestor's value");
    expect(hasIssue(t, "leaf", "corner.card"), "the fallback is reported against the leaf");
    const auto line = std::ranges::find(t.issues, std::string("corner.card"), &ThemeIssue::key)->line;
    expect(line == 7, std::format("the report has the reference's line (got {})", line));
    expect(number(t, "corner.menu") == 8.0, "a result outside the range (48) falls to the ancestor's value");
  }

  void unsetTargetUsesBuiltInAndCyclesDrop() {
    Fixture f;
    f.files["andromeda"] = theme("andromeda", "", "");
    f.files["leaf"] = theme("leaf", "andromeda",
                            "[values]\n\"corner.card\" = \"=corner.window + 2\"\n\"corner.menu\" = \"=corner.popover\"\n"
                            "\"corner.popover\" = \"=corner.bubble\"\n");
    const auto t = f.must("leaf");
    expect(number(t, "corner.card") == 14.0, "a reference to an unset key reads the built-in value");
    expect(!t.values.contains("corner.menu") && !t.values.contains("corner.popover"),
           "a reference cycle (through an alias) leaves both keys to the built-in value");
    expect(hasIssue(t, "leaf", "corner.popover"), "the cycle is reported");
  }

  void engineEntriesReplaceWhole() {
    Fixture f;
    f.files["andromeda"] = theme("andromeda", "",
                                 "[backdrop]\ndesktop = { scene = \"galaxy\", seed = 7 }\nlock = { scene = \"desktop\" }\n");
    f.files["leaf"] = theme("leaf", "andromeda", "[backdrop]\ndesktop = { scene = \"image\", path = \"assets/a.png\" }\n");
    const auto t = f.must("leaf");
    expect(!t.values.contains("backdrop.desktop.seed"), "an entry the leaf sets drops the ancestor's other fields");
    expect(t.origin.at("backdrop.desktop.path") == "leaf", "an asset path keeps the folder that set it");
    expect(t.values.contains("backdrop.lock.scene"), "an entry the leaf does not set is inherited");
  }

  void chainLimits() {
    Fixture f;
    expect(!f.resolve("missing"), "a theme that cannot load fails, so the caller keeps its last good table");

    f.files["aa"] = theme("aa", "bb", "");
    f.files["bb"] = theme("bb", "aa", "[values]\n\"corner.window\" = 5\n");
    const auto cycle = f.must("aa");
    expect(cycle.chain == std::vector<std::string>{"aa", "bb"}, "an extends cycle ends the chain");
    expect(number(cycle, "corner.window") == 5.0 && hasIssue(cycle, "bb", "theme.extends"), "the cycle is reported");

    f.files["cc"] = theme("cc", "gone", "");
    const auto missing = f.must("cc");
    expect(missing.chain.size() == 1 && hasIssue(missing, "cc", "theme.extends"),
           "a missing ancestor ends the chain with an issue");

    for (int i = 0; i < 7; ++i) {
      const std::string id = std::format("t{}", i);
      f.files[id] = theme(id, i < 6 ? std::format("t{}", i + 1) : "", "");
    }
    const auto deep = f.must("t0");
    expect(deep.chain.size() == static_cast<std::size_t>(kMaxExtendsDepth) + 1, "at most 4 ancestors");
  }

  void slotValuesFanOut() {
    Fixture f;
    f.files["andromeda"] = theme("andromeda", "",
                                 "[values]\n\"corner.card\" = 18\n\"spring.open\" = { response = 0.26, bounce = 0.1 }\n"
                                 "[values.dark]\naccent = \"#f2c879\"\n");
    const auto slots = slotValues(f.must("andromeda"), f.vocabulary);
    const auto get = [&](const std::string& path) -> const desk::tokens::SlotValue* {
      const auto it = slots.find(path);
      return it == slots.end() ? nullptr : &it->second;
    };
    expect(get("component.window.card_radius") && std::get<double>(*get("component.window.card_radius")) == 18.0
               && get("component.list.card_radius"),
           "a key sets each of its tokens");
    const auto* stiffness = get("spring.open.stiffness");
    expect(stiffness && std::abs(std::get<double>(*stiffness) - 584.0) < 0.5, "spring stiffness = (2 pi / response)^2");
    expect(get("spring.open.damping") && std::abs(std::get<double>(*get("spring.open.damping")) - 0.9) < 1e-9,
           "spring damping = 1 - bounce");
    expect(get("scheme.dark.accent") && std::get<std::string>(*get("scheme.dark.accent")) == "#F2C879FF",
           "a role sets its scheme slot as #rrggbbaa");
    expect(!get("corner.card"), "vocabulary names never reach the slot table");
  }

} // namespace

int main() {
  referencesResolveOverTheMergedTable();
  badReferenceFallsToTheLayerBelow();
  unsetTargetUsesBuiltInAndCyclesDrop();
  engineEntriesReplaceWhole();
  chainLimits();
  slotValuesFanOut();
  if (!g_ok) {
    return 1;
  }
  std::println("theme_resolve_test: ok");
  return 0;
}
