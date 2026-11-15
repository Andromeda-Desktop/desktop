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

#include <array>
#include <optional>
#include <sstream>
#include <utility>

namespace term {

  namespace {

    template <typename E, std::size_t N> using Names = std::array<std::pair<E, std::string_view>, N>;

    constexpr Names<Theme, 4> kThemes{{{Theme::System, "system"},
                                       {Theme::Light, "light"},
                                       {Theme::Dark, "dark"},
                                       {Theme::Amoled, "amoled"}}};
    constexpr Names<CursorShape, 3> kCursors{
        {{CursorShape::Block, "block"}, {CursorShape::Bar, "bar"}, {CursorShape::Underline, "underline"}}};
    constexpr Names<StartDir, 2> kStartDirs{{{StartDir::Home, "home"}, {StartDir::Current, "current"}}};

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

    toml::table& tableIn(toml::table& root, std::string_view name) {
      toml::node* t = root.get(name);
      if (t == nullptr || !t->is_table()) {
        root.insert_or_assign(name, toml::table{});
        t = root.get(name);
      }
      return *t->as_table();
    }

  } // namespace

  std::variant<Settings, SettingsError> parseSettings(std::string_view text) {
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
      const toml::parse_error& e = parsed.error();
      return SettingsError{std::string(e.description()), static_cast<int>(e.source().begin.line)};
    }
    Settings s;
    Reader r{parsed.table(), std::nullopt};
    r.choice("appearance", "theme", kThemes, s.theme);
    r.choice("appearance", "cursor", kCursors, s.cursor);
    r.string("text", "font", s.font);
    r.integer("text", "size", SettingsLimits::kFontSizeMin, SettingsLimits::kFontSizeMax, s.fontSize);
    r.integer("terminal", "scrollback_mb", SettingsLimits::kScrollbackMin, SettingsLimits::kScrollbackMax,
              s.scrollbackMb);
    r.boolean("terminal", "copy_on_select", s.copyOnSelect);
    r.integer("terminal", "scroll_rows", SettingsLimits::kScrollRowsMin, SettingsLimits::kScrollRowsMax,
              s.scrollRows);
    r.string("sessions", "shell", s.shell);
    r.choice("sessions", "start_in", kStartDirs, s.startDir);
    r.boolean("notifications", "long_command", s.notifyLong);
    r.integer("notifications", "long_command_seconds", SettingsLimits::kNotifyMin, SettingsLimits::kNotifyMax,
              s.notifySeconds);
    r.boolean("notifications", "unfocused_only", s.notifyUnfocusedOnly);
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
    appearance.insert_or_assign("theme", std::string(nameOf(kThemes, s.theme)));
    appearance.insert_or_assign("cursor", std::string(nameOf(kCursors, s.cursor)));
    auto& text = tableIn(root, "text");
    text.insert_or_assign("font", s.font);
    text.insert_or_assign("size", s.fontSize);
    auto& terminal = tableIn(root, "terminal");
    terminal.insert_or_assign("scrollback_mb", s.scrollbackMb);
    terminal.insert_or_assign("copy_on_select", s.copyOnSelect);
    terminal.insert_or_assign("scroll_rows", s.scrollRows);
    auto& sessions = tableIn(root, "sessions");
    sessions.insert_or_assign("shell", s.shell);
    sessions.insert_or_assign("start_in", std::string(nameOf(kStartDirs, s.startDir)));
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
