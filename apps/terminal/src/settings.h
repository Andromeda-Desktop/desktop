#pragma once

#include "keybindings.h"

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace term {

  enum class Theme : std::uint8_t { System, Light, Dark, Amoled };
  // The built-in theme an id names in terminal.toml ("system", "light", "dark", "amoled").
  [[nodiscard]] std::optional<Theme> builtinTheme(std::string_view id);
  // A theme id is a built-in name or a package folder name: [a-z0-9][a-z0-9_-]*, at most 64 bytes.
  [[nodiscard]] bool isThemeId(std::string_view id);
  enum class CursorShape : std::uint8_t { Block, Bar, Underline };
  enum class StartDir : std::uint8_t { Home, Current };
  enum class Bell : std::uint8_t { Flash, Sound, Off };
  enum class WindowMaterial : std::uint8_t { Solid, Glass };

  // User preferences, from $XDG_CONFIG_HOME/andromeda/terminal.toml. The defaults are the built-in behaviour.
  struct Settings {
    std::string theme = "system"; // a built-in name or a theme package id
    CursorShape cursor = CursorShape::Bar; // until the program sets its own (DECSCUSR)
    bool cursorBlink = false;                // until the program sets its own (DECSCUSR, mode 12)
    WindowMaterial material = WindowMaterial::Solid;
    bool focusRing = false; // accent ring on the focused split pane
    std::string font;                        // empty: the type token's mono family
    int fontSize = 13;                       // px at ui_scale 1
    bool boldIsBright = false; // bold text in palette colours 0-7 takes the bright 8-15
    int padding = 0;           // px at ui_scale 1, added to the built-in space on every side of the grid
    int scrollbackMb = 10;
    bool copyOnSelect = false;
    int scrollRows = 3; // per wheel notch
    Bell bell = Bell::Flash;
    std::string shell;  // empty: the login shell
    StartDir startDir = StartDir::Home;
    bool notifyLong = true;
    int notifySeconds = 10;
    bool notifyUnfocusedOnly = true;
    // [keybindings]: action = "ctrl+shift+t", a list of chords, or "none"; Preferences never writes it.
    Keybindings keys;

    bool operator==(const Settings&) const = default;
  };

  struct SettingsLimits {
    static constexpr int kFontSizeMin = 8, kFontSizeMax = 32;
    static constexpr int kScrollbackMin = 1, kScrollbackMax = 1000;
    static constexpr int kPaddingMin = 0, kPaddingMax = 64;
    static constexpr int kScrollRowsMin = 1, kScrollRowsMax = 20;
    static constexpr int kNotifyMin = 1, kNotifyMax = 3600;
  };

  struct SettingsError {
    std::string message;
    int line = 0; // 1-based; 0 when the error has no position
    bool operator==(const SettingsError&) const = default;
  };

  // 0xRRGGBB colours a theme author picks for one scheme.
  struct ThemeAnchorSet {
    std::uint32_t surface = 0;
    std::uint32_t text = 0;
    std::uint32_t accent = 0;
    std::optional<std::array<std::uint32_t, 16>> palette; // ANSI 0-15; derived from the anchors when absent
    bool operator==(const ThemeAnchorSet&) const = default;
  };

  // <XDG_DATA_HOME>/andromeda/themes/<id>/theme.toml. Light and dark twins are both required.
  struct ThemePackage {
    std::string name;
    ThemeAnchorSet light;
    ThemeAnchorSet dark;
    std::string font;  // empty: the user's font
    struct Shader {
      std::string source; // scene.glsl beside theme.toml
      bool operator==(const Shader&) const = default;
    };
    struct Picture {
      std::string path;       // background.<png|jpg|jpeg|webp|jxl|avif> beside theme.toml
      std::int64_t stamp = 0; // file mtime, so an edited picture reloads
      bool operator==(const Picture&) const = default;
    };
    std::variant<std::monostate, Shader, Picture> backdrop; // drawn behind the whole window
    bool operator==(const ThemePackage&) const = default;
  };

  [[nodiscard]] std::variant<ThemePackage, SettingsError> parseThemePackage(std::string_view text);

  [[nodiscard]] std::variant<Settings, SettingsError> parseSettings(std::string_view text);
  // The TOML for `settings`. Keys and tables of `previous` this version does not know stay.
  [[nodiscard]] std::string writeSettings(const Settings& settings, std::string_view previous);

} // namespace term
