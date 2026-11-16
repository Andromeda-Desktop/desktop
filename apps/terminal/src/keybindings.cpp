#include "keybindings.h"

#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace term {

  namespace {

    // KeyMod bits (libdesk-ui core/input/key_modifiers.h); terminal_window.cpp asserts they match.
    constexpr std::uint32_t kShift = 1U << 0;
    constexpr std::uint32_t kCtrl = 1U << 1;
    constexpr std::uint32_t kAlt = 1U << 2;
    constexpr std::uint32_t kSuper = 1U << 3;

    constexpr std::array<ActionInfo, kActionCount> kActions{{
        {Action::NewSession, "new_session", "New session", "ctrl+shift+t"},
        {Action::NewAgent, "new_agent", "New agent session", "ctrl+shift+a"},
        {Action::NewWindow, "new_window", "New window", "ctrl+shift+n"},
        {Action::CloseSession, "close_session", "Close session", "ctrl+shift+w"},
        {Action::Preferences, "preferences", "Preferences", "ctrl+comma"},
        {Action::Find, "find", "Find", "ctrl+shift+f"},
        {Action::SearchSessions, "search_sessions", "Search sessions", "ctrl+shift+k"},
        {Action::SecureEntry, "secure_keyboard_entry", "Secure keyboard entry", "ctrl+shift+s"},
        {Action::ShowScrollback, "show_scrollback", "Show scrollback", "ctrl+shift+h"},
        {Action::FontBigger, "font_bigger", "Larger text", "ctrl+shift+equal"},
        {Action::FontSmaller, "font_smaller", "Smaller text", "ctrl+shift+minus"},
        {Action::FontReset, "font_reset", "Default text size", "ctrl+shift+0"},
        {Action::NextRecent, "next_recent", "Next recent session", "ctrl+tab"},
        {Action::PreviousRecent, "previous_recent", "Previous recent session", "ctrl+shift+tab"},
        {Action::PreviousSession, "previous_session", "Previous session in the list", "ctrl+page_up"},
        {Action::NextSession, "next_session", "Next session in the list", "ctrl+page_down"},
        {Action::Session1, "session_1", "Session 1", "ctrl+1"},
        {Action::Session2, "session_2", "Session 2", "ctrl+2"},
        {Action::Session3, "session_3", "Session 3", "ctrl+3"},
        {Action::Session4, "session_4", "Session 4", "ctrl+4"},
        {Action::Session5, "session_5", "Session 5", "ctrl+5"},
        {Action::Session6, "session_6", "Session 6", "ctrl+6"},
        {Action::Session7, "session_7", "Session 7", "ctrl+7"},
        {Action::Session8, "session_8", "Session 8", "ctrl+8"},
        {Action::Session9, "session_9", "Session 9", "ctrl+9"},
        {Action::FocusLeft, "focus_pane_left", "Focus the pane on the left", "ctrl+alt+left"},
        {Action::FocusRight, "focus_pane_right", "Focus the pane on the right", "ctrl+alt+right"},
        {Action::FocusUp, "focus_pane_up", "Focus the pane above", "ctrl+alt+up"},
        {Action::FocusDown, "focus_pane_down", "Focus the pane below", "ctrl+alt+down"},
        {Action::Copy, "copy", "Copy", "ctrl+shift+c"},
        {Action::Paste, "paste", "Paste", "ctrl+shift+v"},
        {Action::CopyLastOutput, "copy_last_output", "Copy last command output", "ctrl+shift+x"},
        {Action::PreviousPrompt, "previous_prompt", "Previous prompt", "ctrl+shift+up"},
        {Action::NextPrompt, "next_prompt", "Next prompt", "ctrl+shift+down"},
        {Action::ScrollPageUp, "scroll_page_up", "Scroll up a page", "shift+page_up"},
        {Action::ScrollPageDown, "scroll_page_down", "Scroll down a page", "shift+page_down"},
        {Action::CommandPalette, "command_palette", "Command palette", "ctrl+shift+p"},
    }};
    static_assert(
        [] {
          for (std::size_t i = 0; i < kActions.size(); ++i) {
            if (static_cast<std::size_t>(kActions[i].action) != i) {
              return false;
            }
          }
          return true;
        }(),
        "kActions is in Action order");

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::uint32_t normalSym(std::uint32_t sym) {
      return sym == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : xkb_keysym_to_lower(sym);
    }

  } // namespace

  std::span<const ActionInfo> actionTable() { return kActions; }

  std::optional<Action> actionNamed(std::string_view name) {
    const auto it = std::ranges::find(kActions, name, &ActionInfo::name);
    return it != kActions.end() ? std::optional(it->action) : std::nullopt;
  }

  std::optional<Chord> parseChord(std::string_view spec) {
    Chord chord;
    for (;;) {
      const std::size_t plus = spec.find('+', 1); // "ctrl++" names the plus key
      const std::string part = lower(spec.substr(0, plus));
      if (plus == std::string_view::npos) {
        xkb_keysym_t sym = XKB_KEY_NoSymbol;
        if (part.size() == 1) {
          sym = xkb_utf32_to_keysym(static_cast<std::uint32_t>(static_cast<unsigned char>(part[0])));
        } else if (!part.empty()) {
          sym = xkb_keysym_from_name(part.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        }
        if (sym == XKB_KEY_NoSymbol) {
          return std::nullopt;
        }
        chord.sym = normalSym(sym);
        return chord;
      }
      if (part == "ctrl" || part == "control") {
        chord.modifiers |= kCtrl;
      } else if (part == "shift") {
        chord.modifiers |= kShift;
      } else if (part == "alt") {
        chord.modifiers |= kAlt;
      } else {
        return std::nullopt; // Super belongs to the desktop
      }
      spec.remove_prefix(plus + 1);
    }
  }

  std::string chordLabel(Chord chord, std::string_view separator) {
    std::string out;
    for (const auto& [bit, name] : {std::pair{kCtrl, "Ctrl"}, std::pair{kAlt, "Alt"}, std::pair{kShift, "Shift"}}) {
      if ((chord.modifiers & bit) != 0) {
        out.append(name).append(separator);
      }
    }
    switch (chord.sym) {
      case XKB_KEY_Page_Up: return out + "PgUp";
      case XKB_KEY_Page_Down: return out + "PgDn";
      default: break;
    }
    if (const std::uint32_t cp = xkb_keysym_to_utf32(chord.sym); cp > 0x20 && cp < 0x7f) {
      return out + static_cast<char>(std::toupper(static_cast<int>(cp)));
    }
    std::array<char, 64> name{};
    if (xkb_keysym_get_name(chord.sym, name.data(), name.size()) > 0) {
      out += name.data();
    }
    return out;
  }

  Keybindings::Keybindings() {
    for (const ActionInfo& info : kActions) {
      if (const auto chord = parseChord(info.chord)) {
        m_chords[static_cast<std::size_t>(info.action)].push_back(*chord);
      }
    }
  }

  void Keybindings::bind(Action action, std::vector<Chord> chords) {
    for (std::vector<Chord>& other : m_chords) {
      std::erase_if(other, [&chords](const Chord& c) { return std::ranges::contains(chords, c); });
    }
    m_chords[static_cast<std::size_t>(action)] = std::move(chords);
  }

  std::optional<Action> Keybindings::match(std::uint32_t sym, std::uint32_t unshifted, std::uint32_t modifiers) const {
    // A printable key matches by its unshifted character, so Ctrl+Shift+= is "ctrl+shift+equal", not "plus".
    const Chord pressed{.sym = unshifted >= 0x20 && unshifted != 0x7f ? normalSym(xkb_utf32_to_keysym(unshifted))
                                                                      : normalSym(sym),
                        .modifiers = modifiers & (kCtrl | kShift | kAlt | kSuper)};
    for (std::size_t i = 0; i < m_chords.size(); ++i) {
      if (std::ranges::contains(m_chords[i], pressed)) {
        return static_cast<Action>(i);
      }
    }
    return std::nullopt;
  }

  std::string Keybindings::label(Action action, std::string_view separator) const {
    const std::vector<Chord>& list = chords(action);
    return list.empty() ? std::string{} : chordLabel(list.front(), separator);
  }

} // namespace term
