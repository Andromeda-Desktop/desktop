#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace term {

  enum class Action : std::uint8_t {
    NewSession,
    NewAgent,
    NewWindow,
    CloseSession,
    Preferences,
    Find,
    SearchSessions,
    SecureEntry,
    ShowScrollback,
    FontBigger,
    FontSmaller,
    FontReset,
    NextRecent,
    PreviousRecent,
    PreviousSession,
    NextSession,
    Session1,
    Session2,
    Session3,
    Session4,
    Session5,
    Session6,
    Session7,
    Session8,
    Session9,
    FocusLeft,
    FocusRight,
    FocusUp,
    FocusDown,
    MovePaneLeft,
    MovePaneRight,
    MovePaneUp,
    MovePaneDown,
    SplitRight,
    SplitDown,
    SplitLeft,
    SplitUp,
    RotateSplit,
    MoveSessionUp,
    MoveSessionDown,
    Copy,
    Paste,
    CopyLastOutput,
    PreviousPrompt,
    NextPrompt,
    ScrollPageUp,
    ScrollPageDown,
    CommandPalette,
    ToggleSidebar,
    PreviousWorkspace,
    NextWorkspace,
    SelectAll,
    ClearScrollback,
    ResetTerminal,
    ResizeSplitLeft,
    ResizeSplitRight,
    ResizeSplitUp,
    ResizeSplitDown,
    BrowseFiles,
    Count,
  };
  inline constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

  // A key with exact modifiers (KeyMod bits). sym is a lower-case keysym; Shift+Tab is Tab with Shift.
  struct Chord {
    std::uint32_t sym = 0;
    std::uint32_t modifiers = 0;
    bool operator==(const Chord&) const = default;
  };

  struct ActionInfo {
    Action action;
    std::string_view name;  // the key in [keybindings]
    std::string_view label; // Preferences > Keyboard
    std::string_view chord; // default chords, space-separated; empty: none
  };
  [[nodiscard]] std::span<const ActionInfo> actionTable();
  [[nodiscard]] std::optional<Action> actionNamed(std::string_view name);

  // "ctrl+shift+t", "ctrl+comma", "ctrl+,", "shift+page_up": ctrl, shift and alt, then an xkb key name or one character.
  [[nodiscard]] std::optional<Chord> parseChord(std::string_view spec);
  // "Ctrl+Shift+T", "Shift+PgUp"; `separator` joins the parts.
  [[nodiscard]] std::string chordLabel(Chord chord, std::string_view separator = "+");
  // The [keybindings] spelling parseChord reads back: "ctrl+shift+t", "shift+page_up".
  [[nodiscard]] std::string chordSpec(Chord chord);
  // The chord of a key press: `unshifted` is the layout's codepoint for the key without modifiers (0: none).
  [[nodiscard]] Chord chordOf(std::uint32_t sym, std::uint32_t unshifted, std::uint32_t modifiers);

  struct Keybindings {
    Keybindings(); // the defaults
    // Binds `action` to `chords` only; each chord leaves any other action. Empty unbinds.
    void bind(Action action, std::vector<Chord> chords);
    // The action of a key press: `unshifted` is the layout's codepoint for the key without modifiers (0: none).
    [[nodiscard]] std::optional<Action> match(std::uint32_t sym, std::uint32_t unshifted, std::uint32_t modifiers) const;
    // The first chord's label; empty when unbound.
    [[nodiscard]] std::string label(Action action, std::string_view separator = "+") const;
    [[nodiscard]] const std::vector<Chord>& chords(Action action) const { return m_chords[static_cast<std::size_t>(action)]; }
    [[nodiscard]] std::optional<Action> actionOf(Chord chord) const;
    bool operator==(const Keybindings&) const = default;

  private:
    std::array<std::vector<Chord>, kActionCount> m_chords;
  };

} // namespace term
