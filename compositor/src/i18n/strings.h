#pragma once

#include "protocols/i18n_catalog.h"

#include <array>
#include <format>
#include <span>
#include <tuple>
#include <utility>
#include <string>
#include <string_view>

// The compositor's own UI text (overview menus and labels) in the shell's language (config appearance.language),
// with English as the fallback. Catalogs: compositor/translations/<lang>.json.
namespace strings {

  [[nodiscard]] std::string lookup(std::string_view key, std::span<const i18n::detail::Pair> args);

  // tr("compositor.overview.menu.close") or tr("compositor.overview.menu.move-to", "name", name): trailing name, value pairs.
  template <typename... Args> [[nodiscard]] std::string tr(std::string_view key, Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "strings::tr takes name, value pairs");
    std::array<i18n::detail::Pair, sizeof...(Args) / 2> pairs;
    if constexpr (sizeof...(Args) > 0) {
      std::size_t i = 0;
      const auto fill = [&](auto&& name, auto&& value) {
        pairs[i++] = {std::string_view(name), std::format("{}", value)};
      };
      [&]<std::size_t... I>(std::index_sequence<I...>, auto&& tuple) {
        (fill(std::get<2 * I>(tuple), std::get<2 * I + 1>(tuple)), ...);
      }(std::make_index_sequence<sizeof...(Args) / 2>{}, std::forward_as_tuple(args...));
    }
    return lookup(key, pairs);
  }

} // namespace strings
