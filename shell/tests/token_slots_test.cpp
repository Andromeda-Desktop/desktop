#include "design_tokens.h"
#include "ui/token_slots.h"

#include <algorithm>
#include <print>
#include <string>

using desk::tokens::applySlotValues;

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "token_slots_test: FAIL: {}", message);
      g_ok = false;
    }
  }

  double builtIn(std::string_view path) {
    const auto& slots = Tokens::slots();
    return std::ranges::find(slots, path, &Tokens::Slot::path)->number;
  }

  std::uint32_t darkEntry(std::string_view role) {
    return std::ranges::find(Tokens::schemeDarkEntries, role, &std::pair<std::string_view, std::uint32_t>::first)->second;
  }

  // A theme sets a source token: every token derived from it follows, and an empty set reverts all of them.
  void derivedFollowsAndReverts() {
    expect(applySlotValues({{"component.sheet.radius", 20.0}}).has_value(), "a known float applies");
    expect(Tokens::componentSheetRadius == 20.0F, "the source token is written");
    expect(Tokens::componentTileRadius == 20.0F - Tokens::componentSheetPadding, "tile radius = sheet radius - padding");
    expect(Tokens::componentLauncherSearchRadius == 20.0F, "a plain reference follows");
    expect(applySlotValues({}).has_value(), "an empty set applies");
    expect(Tokens::componentSheetRadius == static_cast<float>(builtIn("component.sheet.radius")), "the source reverts");
    expect(Tokens::componentTileRadius == static_cast<float>(builtIn("component.tile.radius")), "the derived token reverts");
  }

  void explicitDerivedWins() {
    expect(applySlotValues({{"component.sheet.radius", 20.0}, {"component.tile.radius", 3.0}}).has_value(), "applies");
    expect(Tokens::componentTileRadius == 3.0F, "a given value beats the expression");
  }

  // A bad set changes nothing, also the keys before the bad one.
  void allOrNothing() {
    expect(applySlotValues({{"component.sheet.radius", 21.0}}).has_value(), "setup applies");
    expect(!applySlotValues({{"component.sheet.radius", 30.0}, {"zz.unknown", 1.0}}), "an unknown token is refused");
    expect(!applySlotValues({{"component.sheet.radius", 30.0}, {"scheme.dark.accent", std::string("#fff")}}),
           "a short colour is refused");
    expect(!applySlotValues({{"component.sheet.radius", 30.0}, {"component.launcher.columns", 2.5}}),
           "a fraction for an int token is refused");
    expect(Tokens::componentSheetRadius == 21.0F, "a refused set leaves every token as it was");
  }

  void colourEntriesFollow() {
    expect(applySlotValues({{"scheme.dark.accent", std::string("#11223344")}}).has_value(), "a colour applies");
    expect(Tokens::schemeDark.accent == 0x11223344U && darkEntry("accent") == 0x11223344U, "the entry table follows");
    expect(applySlotValues({{"font.family", std::string("Test Sans")}}).has_value(), "a string applies");
    expect(Tokens::fontFamily == "Test Sans", "the string slot reads the new text");
    expect(darkEntry("accent") == static_cast<std::uint32_t>(builtIn("scheme.dark.accent")), "the colour reverts");
  }

} // namespace

int main() {
  derivedFollowsAndReverts();
  explicitDerivedWins();
  allOrNothing();
  colourEntriesFollow();
  if (g_ok) {
    std::println("token_slots_test: ok");
  }
  return g_ok ? 0 : 1;
}
