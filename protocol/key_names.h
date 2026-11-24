#pragma once

// The names every key chord grammar shares (shell config, compositor binds, portal triggers, terminal): modifier
// aliases and key aliases to xkb keysym names. Each grammar keeps its own syntax, extra tokens and modifier mask.
// Pure C++: no xkb, so the compositor and the macOS builds include it.

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace key_names {

  enum class Modifier : std::uint8_t { Shift, Ctrl, Alt, Super };

  inline constexpr std::pair<std::string_view, Modifier> kModifiers[] = {
      {"shift", Modifier::Shift}, {"ctrl", Modifier::Ctrl},   {"control", Modifier::Ctrl}, {"ctl", Modifier::Ctrl},
      {"alt", Modifier::Alt},     {"option", Modifier::Alt},  {"super", Modifier::Super},  {"logo", Modifier::Super},
      {"win", Modifier::Super},   {"meta", Modifier::Super},  {"mod4", Modifier::Super},
  };

  // Alias -> xkb keysym name. Names xkb already knows case-insensitively (Left, Tab, space) need no row.
  inline constexpr std::pair<std::string_view, std::string_view> kKeyAliases[] = {
      {"esc", "Escape"},      {"del", "Delete"},      {"enter", "Return"},           {"spacebar", "space"},
      {"pgup", "Prior"},      {"pgdn", "Next"},       {"pageup", "Prior"},           {"pagedown", "Next"},
      {"ins", "Insert"},      {"shift_tab", "ISO_Left_Tab"},
  };

  namespace detail {
    constexpr bool equalsIgnoreCase(std::string_view text, std::string_view lower) {
      if (text.size() != lower.size()) {
        return false;
      }
      for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i] >= 'A' && text[i] <= 'Z' ? static_cast<char>(text[i] - 'A' + 'a') : text[i];
        if (ch != lower[i]) {
          return false;
        }
      }
      return true;
    }
  } // namespace detail

  // Case-insensitive.
  [[nodiscard]] constexpr std::optional<Modifier> modifier(std::string_view name) {
    for (const auto& [alias, mod] : kModifiers) {
      if (detail::equalsIgnoreCase(name, alias)) {
        return mod;
      }
    }
    return std::nullopt;
  }

  // The xkb keysym name for `name` (case-insensitive alias), or `name` itself.
  [[nodiscard]] constexpr std::string_view keysymName(std::string_view name) {
    for (const auto& [alias, keysym] : kKeyAliases) {
      if (detail::equalsIgnoreCase(name, alias)) {
        return keysym;
      }
    }
    return name;
  }

} // namespace key_names
