#include "settings.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
// Only this file includes toml++: header-only, parse errors come back as values.
#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>
#pragma GCC diagnostic pop

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace term {

  namespace {

    template <typename E, std::size_t N> using Names = std::array<std::pair<E, std::string_view>, N>;

    constexpr Names<Theme, 4> kBuiltinThemes{{{Theme::System, "system"},
                                       {Theme::Light, "light"},
                                       {Theme::Dark, "dark"},
                                       {Theme::Amoled, "amoled"}}};
    constexpr Names<CursorShape, 3> kCursors{
        {{CursorShape::Block, "block"}, {CursorShape::Bar, "bar"}, {CursorShape::Underline, "underline"}}};
    constexpr Names<StartDir, 2> kStartDirs{{{StartDir::Home, "home"}, {StartDir::Current, "current"}}};
    constexpr Names<Bell, 3> kBells{{{Bell::Flash, "flash"}, {Bell::Sound, "sound"}, {Bell::Off, "off"}}};
    constexpr Names<Scrollback, 2> kScrollbacks{{{Scrollback::Limited, "limited"}, {Scrollback::Unlimited, "unlimited"}}};
    constexpr Names<WindowMaterial, 2> kMaterials{
        {{WindowMaterial::Solid, "solid"}, {WindowMaterial::Glass, "glass"}}};

    template <typename E, std::size_t N> std::string_view nameOf(const Names<E, N>& names, E value) {
      for (const auto& [e, name] : names) {
        if (e == value) {
          return name;
        }
      }
      return names[0].second;
    }

    struct Reader {
      const toml::table& root;
      std::optional<SettingsError> error;

      const toml::node* find(std::string_view table, std::string_view key) {
        const toml::node* t = root.get(table);
        if (t == nullptr) {
          return nullptr;
        }
        if (!t->is_table()) {
          fail(*t, "[" + std::string(table) + "] must be a table");
          return nullptr;
        }
        return t->as_table()->get(key);
      }

      void fail(const toml::node& node, std::string message) {
        if (!error) {
          error = SettingsError{std::move(message), static_cast<int>(node.source().begin.line)};
        }
      }

      static std::string where(std::string_view table, std::string_view key) {
        return std::string(table) + "." + std::string(key);
      }

      void boolean(std::string_view table, std::string_view key, bool& out) {
        if (const toml::node* n = find(table, key)) {
          if (const auto v = n->value_exact<bool>()) {
            out = *v;
          } else {
            fail(*n, where(table, key) + " must be true or false");
          }
        }
      }

      void integer(std::string_view table, std::string_view key, int min, int max, int& out) {
        if (const toml::node* n = find(table, key)) {
          const auto v = n->value_exact<std::int64_t>();
          if (v && *v >= min && *v <= max) {
            out = static_cast<int>(*v);
          } else {
            fail(*n, where(table, key) + " must be a whole number from " + std::to_string(min) + " to "
                         + std::to_string(max));
          }
        }
      }

      void string(std::string_view table, std::string_view key, std::string& out) {
        if (const toml::node* n = find(table, key)) {
          if (const auto v = n->value_exact<std::string>()) {
            out = *v;
          } else {
            fail(*n, where(table, key) + " must be a string");
          }
        }
      }

      void themeId(std::string_view table, std::string_view key, std::string& out) {
        if (const toml::node* n = find(table, key)) {
          if (const auto v = n->value_exact<std::string>(); v && isThemeId(*v)) {
            out = *v;
          } else {
            fail(*n, where(table, key) + " must be \"system\", \"light\", \"dark\", \"amoled\" or a theme folder name");
          }
        }
      }

      template <typename E, std::size_t N>
      void choice(std::string_view table, std::string_view key, const Names<E, N>& names, E& out) {
        if (const toml::node* n = find(table, key)) {
          if (const auto v = n->value_exact<std::string>()) {
            for (const auto& [e, name] : names) {
              if (name == *v) {
                out = e;
                return;
              }
            }
          }
          std::string list;
          for (const auto& [e, name] : names) {
            list += (list.empty() ? "\"" : ", \"") + std::string(name) + "\"";
          }
          fail(*n, where(table, key) + " must be one of " + list);
        }
      }
    };

    void keybindings(Reader& r, Keybindings& out) {
      const toml::node* t = r.root.get("keybindings");
      if (t == nullptr) {
        return;
      }
      if (!t->is_table()) {
        r.fail(*t, "[keybindings] must be a table");
        return;
      }
      for (const auto& [key, node] : *t->as_table()) {
        const auto action = actionNamed(key.str());
        if (!action) {
          r.fail(node, "keybindings." + std::string(key.str()) + " is not an action");
          return;
        }
        const std::string where = "keybindings." + std::string(key.str());
        std::vector<const toml::node*> specs;
        if (const toml::array* list = node.as_array()) {
          for (const toml::node& item : *list) {
            specs.push_back(&item);
          }
        } else {
          specs.push_back(&node);
        }
        std::vector<Chord> chords;
        for (const toml::node* spec : specs) {
          const auto text = spec->value_exact<std::string>();
          if (text && *text == "none" && specs.size() == 1) {
            break;
          }
          const auto chord = text ? parseChord(*text) : std::nullopt;
          if (!chord) {
            r.fail(*spec, where + " must be a key like \"ctrl+shift+t\" (ctrl, shift, alt), a list of keys, or \"none\"");
            return;
          }
          chords.push_back(*chord);
        }
        out.bind(*action, std::move(chords));
      }
    }

    toml::table& tableIn(toml::table& root, std::string_view name) {
      toml::node* t = root.get(name);
      if (t == nullptr || !t->is_table()) {
        root.insert_or_assign(name, toml::table{});
        t = root.get(name);
      }
      return *t->as_table();
    }

  } // namespace

  std::optional<Theme> builtinTheme(std::string_view id) {
    for (const auto& [theme, name] : kBuiltinThemes) {
      if (name == id) {
        return theme;
      }
    }
    return std::nullopt;
  }

  bool isThemeId(std::string_view id) {
    auto lowerOrDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return !id.empty() && id.size() <= 64 && lowerOrDigit(id[0])
           && std::ranges::all_of(id, [&](char c) { return lowerOrDigit(c) || c == '-' || c == '_'; });
  }

  std::variant<ThemePackage, SettingsError> parseThemePackage(std::string_view text) {
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
      const toml::parse_error& e = parsed.error();
      return SettingsError{std::string(e.description()), static_cast<int>(e.source().begin.line)};
    }
    const toml::table& root = parsed.table();
    auto failAt = [](const toml::node& n, std::string message) {
      return SettingsError{std::move(message), static_cast<int>(n.source().begin.line)};
    };
    // Unknown keys are errors, so a typo in a hand-made or generated theme shows up at once.
    for (const auto& [key, node] : root) {
      if (key != "name" && key != "font" && key != "light" && key != "dark") {
        return failAt(node, "unknown key \"" + std::string(key.str()) + "\"");
      }
    }
    ThemePackage theme;
    const toml::node* nameNode = root.get("name");
    if (nameNode == nullptr) {
      return SettingsError{"name is required", 0};
    }
    const auto name = nameNode->value_exact<std::string>();
    if (!name || name->empty()) {
      return failAt(*nameNode, "name must be a non-empty string");
    }
    theme.name = *name;
    if (const toml::node* font = root.get("font")) {
      const auto v = font->value_exact<std::string>();
      if (!v) {
        return failAt(*font, "font must be a string");
      }
      theme.font = *v;
    }
    for (const auto& [schemeName, anchors] : {std::pair{"light", &theme.light}, std::pair{"dark", &theme.dark}}) {
      const toml::node* scheme = root.get(schemeName);
      if (scheme == nullptr || !scheme->is_table()) {
        return scheme == nullptr ? SettingsError{"[" + std::string(schemeName) + "] with surface, text and accent is required", 0}
                                 : failAt(*scheme, "[" + std::string(schemeName) + "] must be a table");
      }
      const toml::table& t = *scheme->as_table();
      for (const auto& [key, node] : t) {
        if (key != "surface" && key != "text" && key != "accent" && key != "palette") {
          return failAt(node, "unknown key \"" + std::string(schemeName) + "." + std::string(key.str()) + "\"");
        }
      }
      const auto hexColor = [](const toml::node& n) -> std::optional<std::uint32_t> {
        const auto v = n.value_exact<std::string>();
        const bool hex = v && v->size() == 7 && (*v)[0] == '#'
                         && std::ranges::all_of(v->substr(1), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        return hex ? std::optional(static_cast<std::uint32_t>(std::stoul(v->substr(1), nullptr, 16))) : std::nullopt;
      };
      for (const auto& [key, out] : {std::pair{"surface", &anchors->surface}, std::pair{"text", &anchors->text},
                                     std::pair{"accent", &anchors->accent}}) {
        const std::string where = std::string(schemeName) + "." + key;
        const toml::node* n = t.get(key);
        if (n == nullptr) {
          return failAt(*scheme, where + " is required");
        }
        const auto colour = hexColor(*n);
        if (!colour) {
          return failAt(*n, where + " must be a colour like \"#1e1e2e\"");
        }
        *out = *colour;
      }
      if (const toml::node* n = t.get("palette")) {
        const std::string where = std::string(schemeName) + ".palette";
        const toml::array* list = n->as_array();
        if (list == nullptr || list->size() != 16) {
          return failAt(*n, where + " must list 16 colours (ANSI 0-15)");
        }
        std::array<std::uint32_t, 16> palette{};
        for (std::size_t i = 0; i < palette.size(); ++i) {
          const auto colour = hexColor(*list->get(i));
          if (!colour) {
            return failAt(*list->get(i), where + " entries must be colours like \"#1e1e2e\"");
          }
          palette[i] = *colour;
        }
        anchors->palette = palette;
      }
    }
    return theme;
  }

  std::variant<Settings, SettingsError> parseSettings(std::string_view text) {
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
      const toml::parse_error& e = parsed.error();
      return SettingsError{std::string(e.description()), static_cast<int>(e.source().begin.line)};
    }
    Settings s;
    Reader r{parsed.table(), std::nullopt};
    r.themeId("appearance", "theme", s.theme);
    r.choice("appearance", "cursor", kCursors, s.cursor);
    r.boolean("appearance", "cursor_blink", s.cursorBlink);
    r.boolean("appearance", "bold_is_bright", s.boldIsBright);
    r.integer("appearance", "padding", SettingsLimits::kPaddingMin, SettingsLimits::kPaddingMax, s.padding);
    r.choice("appearance", "window", kMaterials, s.material);
    if (r.find("appearance", "glass_opacity") != nullptr) {
      int opacity = 0;
      r.integer("appearance", "glass_opacity", SettingsLimits::kGlassOpacityMin, SettingsLimits::kGlassOpacityMax, opacity);
      s.glassOpacity = opacity;
    }
    r.boolean("appearance", "focus_ring", s.focusRing);
    r.string("text", "font", s.font);
    r.integer("text", "size", SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax, s.fontSize);
    r.choice("terminal", "scrollback", kScrollbacks, s.scrollback);
    r.integer("terminal", "scrollback_mb", SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax,
              s.scrollbackMb);
    r.boolean("terminal", "copy_on_select", s.copyOnSelect);
    r.string("terminal", "word_separators", s.wordSeparators);
    r.integer("terminal", "scroll_rows", SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax,
              s.scrollRows);
    r.choice("terminal", "bell", kBells, s.bell);
    r.string("sessions", "shell", s.shell);
    r.choice("sessions", "start_in", kStartDirs, s.startDir);
    r.boolean("sessions", "restore", s.restoreSessions);
    r.boolean("notifications", "long_command", s.notifyLong);
    r.integer("notifications", "long_command_seconds", SettingsLimits::kNotifyMin, SettingsLimits::kNotifyMax,
              s.notifySeconds);
    r.boolean("notifications", "unfocused_only", s.notifyUnfocusedOnly);
    keybindings(r, s.keys);
    if (r.error) {
      return *r.error;
    }
    return s;
  }

  std::string writeSettings(const Settings& s, std::string_view previous) {
    toml::table root;
    if (toml::parse_result parsed = toml::parse(previous)) {
      root = std::move(parsed).table();
    }
    auto& appearance = tableIn(root, "appearance");
    appearance.insert_or_assign("theme", s.theme);
    appearance.insert_or_assign("cursor", std::string(nameOf(kCursors, s.cursor)));
    appearance.insert_or_assign("cursor_blink", s.cursorBlink);
    appearance.insert_or_assign("bold_is_bright", s.boldIsBright);
    appearance.insert_or_assign("padding", s.padding);
    appearance.insert_or_assign("window", std::string(nameOf(kMaterials, s.material)));
    if (s.glassOpacity) {
      appearance.insert_or_assign("glass_opacity", *s.glassOpacity);
    } else {
      appearance.erase("glass_opacity");
    }
    appearance.insert_or_assign("focus_ring", s.focusRing);
    auto& text = tableIn(root, "text");
    text.insert_or_assign("font", s.font);
    text.insert_or_assign("size", s.fontSize);
    auto& terminal = tableIn(root, "terminal");
    terminal.insert_or_assign("scrollback", std::string(nameOf(kScrollbacks, s.scrollback)));
    terminal.insert_or_assign("scrollback_mb", s.scrollbackMb);
    terminal.insert_or_assign("copy_on_select", s.copyOnSelect);
    terminal.insert_or_assign("word_separators", s.wordSeparators);
    terminal.insert_or_assign("scroll_rows", s.scrollRows);
    terminal.insert_or_assign("bell", std::string(nameOf(kBells, s.bell)));
    auto& sessions = tableIn(root, "sessions");
    sessions.insert_or_assign("shell", s.shell);
    sessions.insert_or_assign("start_in", std::string(nameOf(kStartDirs, s.startDir)));
    sessions.insert_or_assign("restore", s.restoreSessions);
    auto& notifications = tableIn(root, "notifications");
    notifications.insert_or_assign("long_command", s.notifyLong);
    notifications.insert_or_assign("long_command_seconds", s.notifySeconds);
    notifications.insert_or_assign("unfocused_only", s.notifyUnfocusedOnly);
    std::ostringstream out;
    out << "# Andromeda Terminal preferences. Preferences (Ctrl+,) edits this file; hand edits apply at once.\n"
        << root << '\n';
    return out.str();
  }

} // namespace term
