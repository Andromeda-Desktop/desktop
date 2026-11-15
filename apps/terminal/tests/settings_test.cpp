#include "settings.h"

#include <cstdio>
#include <random>
#include <string>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

Settings randomSettings(std::mt19937& rng) {
  auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
  const char* fonts[] = {"", "Fira Code", "JetBrains Mono", "Noto Sans Mono CJK \"JP\"", "x\\y\n"};
  Settings s;
  s.theme = static_cast<Theme>(pick(0, 3));
  s.cursor = static_cast<CursorShape>(pick(0, 2));
  s.font = fonts[pick(0, 4)];
  s.fontSize = pick(SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax);
  s.scrollbackMb = pick(SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax);
  s.copyOnSelect = pick(0, 1) != 0;
  s.scrollRows = pick(SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax);
  s.shell = pick(0, 1) != 0 ? "" : "/usr/bin/fish";
  s.startDir = static_cast<StartDir>(pick(0, 1));
  s.notifyLong = pick(0, 1) != 0;
  s.notifySeconds = pick(SettingsLimits::kNotifyMin, SettingsLimits::kNotifyMax);
  s.notifyUnfocusedOnly = pick(0, 1) != 0;
  return s;
}

// Values that must be rejected, each with the line it sits on.
const char* kBad[] = {
    "[appearance]\ntheme = \"neon\"\n",
    "[text]\nsize = 200\n",
    "[text]\nsize = \"13\"\n",
    "[terminal]\ncopy_on_select = 1\n",
    "[notifications]\nlong_command_seconds = 0\n",
    "appearance = 3\n",
    "[text\n",
};

} // namespace

int main() {
  std::random_device rd;
  seed = rd();
  std::mt19937 rng(seed);

  // Invariant: write then parse gives the same settings, and keys this version does not know survive.
  std::string file = "[future]\nkey = \"kept\"\n[text]\nligatures = true\n";
  for (step = 0; step < 300; ++step) {
    const Settings s = randomSettings(rng);
    file = writeSettings(s, file);
    const auto parsed = parseSettings(file);
    CHECK(std::holds_alternative<Settings>(parsed));
    if (const auto* p = std::get_if<Settings>(&parsed)) {
      CHECK(*p == s);
    }
    CHECK(file.find("[future]") != std::string::npos && file.find("kept") != std::string::npos);
    CHECK(file.find("ligatures = true") != std::string::npos);
    if (failures != 0) {
      break;
    }
  }

  // Invariant: an empty file is the defaults.
  CHECK(std::get<Settings>(parseSettings("")) == Settings{});

  // Invariant: a bad value is an error with its line, never silently replaced.
  step = -1;
  for (const char* bad : kBad) {
    const auto parsed = parseSettings(bad);
    const auto* e = std::get_if<SettingsError>(&parsed);
    CHECK(e != nullptr);
    if (e != nullptr) {
      CHECK(!e->message.empty());
      CHECK(e->line >= 1);
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::puts("settings: ok");
  return 0;
}
