#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace term {

  enum class Theme : std::uint8_t { System, Light, Dark, Amoled };
  enum class CursorShape : std::uint8_t { Block, Bar, Underline };
  enum class StartDir : std::uint8_t { Home, Current };
  enum class Bell : std::uint8_t { Flash, Sound, Off };
  enum class WindowMaterial : std::uint8_t { Solid, Glass };

  // User preferences, from $XDG_CONFIG_HOME/andromeda/terminal.toml. The defaults are the built-in behaviour.
  struct Settings {
    Theme theme = Theme::System;
    CursorShape cursor = CursorShape::Block; // until the program sets its own (DECSCUSR)
    bool cursorBlink = true;                 // until the program sets its own (DECSCUSR, mode 12)
    WindowMaterial material = WindowMaterial::Solid;
    std::string font;                        // empty: the type token's mono family
    int fontSize = 13;                       // px at ui_scale 1
    int scrollbackMb = 10;
    bool copyOnSelect = false;
    int scrollRows = 3; // per wheel notch
    Bell bell = Bell::Flash;
    std::string shell;  // empty: the login shell
    StartDir startDir = StartDir::Home;
    bool notifyLong = true;
    int notifySeconds = 10;
    bool notifyUnfocusedOnly = true;

    bool operator==(const Settings&) const = default;
  };

  struct SettingsLimits {
    static constexpr int kFontSizeMin = 8, kFontSizeMax = 32;
    static constexpr int kScrollbackMin = 1, kScrollbackMax = 1000;
    static constexpr int kScrollRowsMin = 1, kScrollRowsMax = 20;
    static constexpr int kNotifyMin = 1, kNotifyMax = 3600;
  };

  struct SettingsError {
    std::string message;
    int line = 0; // 1-based; 0 when the error has no position
    bool operator==(const SettingsError&) const = default;
  };

  // A missing key keeps its default; a wrong type or an out-of-range value is an error.
  [[nodiscard]] std::variant<Settings, SettingsError> parseSettings(std::string_view text);
  // The TOML for `settings`. Keys and tables of `previous` this version does not know stay.
  [[nodiscard]] std::string writeSettings(const Settings& settings, std::string_view previous);

} // namespace term
