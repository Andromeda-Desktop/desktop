// Model test for ThemeController (theme-engine STORES.md I1, I5): random selects, theme file edits and overrides
// edits, each watch notification delivered late or never, against an oracle of the same disk.
#include "theme/theme_controller.h"

#include <array>
#include <format>
#include <map>
#include <print>
#include <random>
#include <string>
#include <vector>

using namespace noctalia::theme;

namespace {

  bool g_ok = true;
  std::string g_trace;

  void expect(bool condition, const std::string& message) {
    if (!condition && g_ok) {
      std::println(stderr, "theme_controller_model_test: FAIL: {}\ntrace:{}", message, g_trace);
      g_ok = false;
    }
  }

  constexpr const char* kVocabulary = R"(
version = 1
roles = ["accent"]
[key."corner.window"]
type = "length"
range = [0, 64]
tokens = ["component.window.radius"]
[key."corner.card"]
type = "length"
range = [0, 64]
tokens = ["component.window.card_radius"]
)";

  enum class FileState { Absent, Good1, Good2, Broken };
  constexpr std::array kStates = {FileState::Absent, FileState::Good1, FileState::Good2, FileState::Broken};
  // "bb" extends "aa"; "cc" extends "zz", which is never installed.
  const std::map<std::string, std::string> kParents = {{"andromeda", ""}, {"aa", "andromeda"}, {"bb", "aa"}, {"cc", "zz"}};
  const std::array<std::string, 6> kSelectable = {"", "andromeda", "aa", "bb", "cc", "zz"};

  struct World {
    std::map<std::string, FileState> themes;
    FileState overrides = FileState::Absent;

    std::string themeText(const std::string& id) const {
      const FileState state = themes.at(id);
      if (state == FileState::Broken) {
        return "[theme]\nformat = 9\n";
      }
      const std::string& parent = kParents.at(id);
      const int value = static_cast<int>(id.size()) * 3 + id.front() % 7 + (state == FileState::Good2 ? 20 : 0);
      return std::format("[theme]\nformat = 1\nid = \"{}\"\nname = \"{}\"\n{}[values]\n\"corner.window\" = {}\n", id,
                         id, parent.empty() ? "" : std::format("extends = \"{}\"\n", parent), value);
    }

    std::expected<ThemePackage, ThemeIssue> load(std::string_view id, const ThemeVocabulary& vocabulary) const {
      const auto it = themes.find(std::string(id));
      if (it == themes.end() || it->second == FileState::Absent) {
        return std::unexpected(ThemeIssue{.key = "theme", .message = "not installed"});
      }
      return parseThemePackage(themeText(it->first), id, vocabulary);
    }

    std::expected<std::optional<ThemePackage>, ThemeIssue> loadOverrides(const ThemeVocabulary& vocabulary) const {
      switch (overrides) {
      case FileState::Absent:
        return std::optional<ThemePackage>{};
      case FileState::Broken:
        return std::unexpected(parseThemeOverrides("[values\n", vocabulary).error());
      default:
        return std::optional(*parseThemeOverrides(
            std::format("[values]\n\"corner.card\" = {}\n", overrides == FileState::Good1 ? 5 : 7), vocabulary));
      }
    }
  };

  bool same(const ResolvedTheme& a, const ResolvedTheme& b) {
    return a.chain == b.chain && a.values == b.values && a.origin == b.origin;
  }

  void runSequence(std::mt19937& rng, const ThemeVocabulary& vocabulary) {
    World disk;
    for (const auto& [id, parent] : kParents) {
      disk.themes[id] = FileState::Good1;
    }
    const BuiltInNumber builtIn = [](std::string_view) { return std::nullopt; };

    ResolvedTheme applied;
    std::uint64_t applies = 0;
    std::uint64_t lastGeneration = 0;
    ThemeController controller(
        {.loadTheme = [&](std::string_view id) { return disk.load(id, vocabulary); },
         .loadOverrides = [&] { return disk.loadOverrides(vocabulary); }},
        vocabulary, builtIn, [&](const ResolvedTheme& table, std::uint64_t generation) {
          expect(generation == lastGeneration + 1, "each apply is the next generation");
          expect(applies == 0 || !same(table, applied), "an apply always changes the table");
          lastGeneration = generation;
          applied = table;
          ++applies;
        });

    // Oracle: the overrides the controller last read, and the table that must show.
    std::optional<ThemePackage> goodOverrides;
    ResolvedTheme expected;
    const auto oracleRefresh = [&] {
      if (controller.selected().empty()) {
        expected = {};
        return;
      }
      const ThemeLookup lookup = [&](std::string_view id) { return disk.load(id, vocabulary); };
      if (auto t = resolveTheme(controller.selected(), lookup, goodOverrides ? &*goodOverrides : nullptr, vocabulary,
                                builtIn)) {
        expected = std::move(*t);
      }
    };

    bool themeEventPending = false;
    bool overridesEventPending = false;
    std::uniform_int_distribution<int> pick(0, 99);
    for (int step = 0; step < 40 && g_ok; ++step) {
      const int roll = pick(rng);
      if (roll < 20) {
        const std::string& id = kSelectable[static_cast<std::size_t>(pick(rng)) % kSelectable.size()];
        g_trace += std::format(" select({})", id);
        controller.select(id);
        oracleRefresh();
      } else if (roll < 50) {
        auto it = disk.themes.begin();
        std::advance(it, pick(rng) % static_cast<int>(disk.themes.size()));
        it->second = kStates[static_cast<std::size_t>(pick(rng)) % kStates.size()];
        themeEventPending = true;
        g_trace += std::format(" edit({}={})", it->first, static_cast<int>(it->second));
      } else if (roll < 65) {
        disk.overrides = kStates[static_cast<std::size_t>(pick(rng)) % kStates.size()];
        overridesEventPending = true;
        g_trace += std::format(" overrides={}", static_cast<int>(disk.overrides));
      } else if (roll < 85 && themeEventPending) {
        g_trace += " themeWatch";
        themeEventPending = false;
        controller.themeFilesChanged();
        oracleRefresh();
      } else if (overridesEventPending) {
        g_trace += " overridesWatch";
        overridesEventPending = false;
        controller.overridesChanged();
        if (auto loaded = disk.loadOverrides(vocabulary)) {
          goodOverrides = std::move(*loaded);
          oracleRefresh();
        }
      }
      expect(controller.generation() == applies, "the generation counts applies");
      expect(same(applied, expected), std::format("shown {} != expected {}", applied.chain.empty() ? "builtin" : applied.chain[0],
                                                  expected.chain.empty() ? "builtin" : expected.chain[0]));
      expect(controller.shownId() == (applied.chain.empty() ? "" : applied.chain[0]), "shownId names the applied table");
      const bool selectedLoads = !controller.selected().empty() && disk.load(controller.selected(), vocabulary);
      if (!themeEventPending && !overridesEventPending && !controller.selected().empty()) {
        expect(!selectedLoads || controller.shownId() == controller.selected(),
               "settled: the selected theme shows when it loads (else the last good table stays)");
        expect(selectedLoads == controller.errors().empty() || disk.overrides == FileState::Broken,
               "settled: an error is reported exactly when the selected theme does not load");
      }
      if (controller.selected().empty()) {
        expect(applied.chain.empty(), "no selection shows the built-in tokens");
      }
    }
  }

} // namespace

int main() {
  const auto vocabulary = *parseThemeVocabulary(kVocabulary);
  std::mt19937 rng(20261128);
  for (int sequence = 0; sequence < 3000 && g_ok; ++sequence) {
    g_trace.clear();
    runSequence(rng, vocabulary);
  }
  if (!g_ok) {
    return 1;
  }
  std::println("theme_controller_model_test: ok");
  return 0;
}
