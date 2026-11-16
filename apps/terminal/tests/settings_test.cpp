#include "settings.h"

#include <xkbcommon/xkbcommon-keysyms.h>

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
  const char* themes[] = {"system", "light", "dark", "amoled", "nord", "my_theme-2"};
  s.theme = themes[pick(0, 5)];
  s.cursor = static_cast<CursorShape>(pick(0, 2));
  s.cursorBlink = pick(0, 1) != 0;
  s.material = static_cast<WindowMaterial>(pick(0, 1));
  s.bell = static_cast<Bell>(pick(0, 2));
  s.font = fonts[pick(0, 4)];
  s.fontSize = pick(SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax);
  s.scrollback = static_cast<Scrollback>(pick(0, 1));
  s.scrollbackMb = pick(SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax);
  s.copyOnSelect = pick(0, 1) != 0;
  const char* separators[] = {" ", " -/\\", "\t\u2502()", "\"'"};
  s.wordSeparators = separators[pick(0, 3)];
  s.boldIsBright = pick(0, 1) != 0;
  s.padding = pick(SettingsLimits::kPaddingMin, SettingsLimits::kPaddingMax);
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
    "[appearance]\ntheme = \"Neon Theme\"\n",
    "[text]\nsize = 200\n",
    "[text]\nsize = \"13\"\n",
    "[terminal]\ncopy_on_select = 1\n",
    "[notifications]\nlong_command_seconds = 0\n",
    "appearance = 3\n",
    "[text\n",
    "[keybindings]\nnew_tab = \"ctrl+t\"\n",
    "[keybindings]\ncopy = \"ctrl+shift+nosuchkey\"\n",
    "[keybindings]\ncopy = \"super+c\"\n",
    "[keybindings]\ncopy = [\"ctrl+c\", 3]\n",
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

  // Invariant: every default chord runs its own action (no two defaults collide).
  constexpr std::uint32_t kShift = 1, kCtrl = 2;
  const Keybindings defaults;
  for (const ActionInfo& info : actionTable()) {
    for (const Chord& chord : defaults.chords(info.action)) {
      CHECK(defaults.match(chord.sym, 0, chord.modifiers) == info.action);
    }
  }
  // Keys arrive as the layout gives them: shifted keysym plus the unshifted character.
  CHECK(defaults.match(XKB_KEY_T, 't', kCtrl | kShift) == Action::NewSession);
  CHECK(defaults.match(XKB_KEY_plus, '=', kCtrl | kShift) == Action::FontBigger);
  CHECK(defaults.match(XKB_KEY_ISO_Left_Tab, 0, kCtrl | kShift) == Action::PreviousRecent);
  CHECK(defaults.match(XKB_KEY_t, 't', kCtrl) == std::nullopt);

  // Invariant: a user binding replaces the action's chords and takes its chord from any other action;
  // "none" unbinds; Preferences writes keep the table.
  const std::string user = "[keybindings]\nnew_session = \"ctrl+t\"\nfind = \"Ctrl+Shift+T\"\ncopy = \"none\"\n"
                           "paste = [\"ctrl+shift+v\", \"shift+insert\"]\nclose_session = \"ctrl+shift+n\"\n";
  const auto parsedUser = parseSettings(user);
  CHECK(std::holds_alternative<Settings>(parsedUser));
  if (const auto* p = std::get_if<Settings>(&parsedUser)) {
    CHECK(p->keys.match(XKB_KEY_t, 't', kCtrl) == Action::NewSession);
    CHECK(p->keys.match(XKB_KEY_T, 't', kCtrl | kShift) == Action::Find);
    CHECK(p->keys.match(XKB_KEY_F, 'f', kCtrl | kShift) == std::nullopt);
    CHECK(p->keys.match(XKB_KEY_C, 'c', kCtrl | kShift) == std::nullopt);
    CHECK(p->keys.chords(Action::Copy).empty());
    CHECK(p->keys.match(XKB_KEY_Insert, 0, kShift) == Action::Paste);
    CHECK(p->keys.label(Action::Find) == "Ctrl+Shift+T");
    CHECK(p->keys.match(XKB_KEY_N, 'n', kCtrl | kShift) == Action::CloseSession);
    CHECK(p->keys.chords(Action::NewWindow).empty());
    Settings changed = *p;
    changed.fontSize = 20;
    const auto rewritten = parseSettings(writeSettings(changed, user));
    CHECK(std::holds_alternative<Settings>(rewritten) && std::get<Settings>(rewritten) == changed);
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::puts("settings: ok");
  return 0;
}
