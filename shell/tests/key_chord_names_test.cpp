#include "core/input/key_chord.h"
#include "core/input/key_modifiers.h"
#include "protocols/key_names.h"

#include <xkbcommon/xkbcommon.h>

#include <cctype>
#include <print>
#include <stdexcept>
#include <string>

namespace {

  bool check(bool condition, const std::string& context) {
    if (!condition) {
      std::println(stderr, "key_chord_names_test: {}", context);
    }
    return condition;
  }

  std::string upper(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
  }

} // namespace

int main() {
  bool ok = true;
  for (const auto& [alias, modifier] : key_names::kModifiers) {
    for (const std::string& name : {std::string(alias), upper(alias)}) {
      if (modifier == key_names::Modifier::Super) {
        bool threw = false;
        try {
          (void)parseKeyChordSpec(name + "+a");
        } catch (const std::runtime_error&) {
          threw = true;
        }
        ok = check(threw, name + " (Super) is accepted") && ok;
        continue;
      }
      const std::uint32_t expected = modifier == key_names::Modifier::Shift ? KeyMod::Shift
          : modifier == key_names::Modifier::Ctrl                           ? KeyMod::Ctrl
                                                                            : KeyMod::Alt;
      const auto chord = parseKeyChordSpec(name + "+a");
      ok = check(chord && chord->modifiers == expected, name + " gives the wrong modifier") && ok;
    }
  }
  for (const auto& [alias, keysym] : key_names::kKeyAliases) {
    const xkb_keysym_t expected = xkb_keysym_from_name(std::string(keysym).c_str(), XKB_KEYSYM_NO_FLAGS);
    ok = check(expected != XKB_KEY_NoSymbol, std::string(keysym) + " is no xkb name") && ok;
    for (const std::string& name : {std::string(alias), upper(alias)}) {
      const auto chord = parseKeyChordSpec("ctrl+" + name);
      ok = check(chord && chord->sym == expected, name + " gives the wrong key") && ok;
    }
  }
  return ok ? 0 : 1;
}
