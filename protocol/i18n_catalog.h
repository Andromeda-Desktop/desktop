#pragma once

// Translation catalogs shared by libdesk-ui (i18n::Service) and the compositor: JSON files whose string leaves become
// dotted keys, and "{name}" interpolation.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace i18n {

  // Transparent hash/eq so Catalog::find() can take string_view without allocating a temporary std::string.
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    std::size_t operator()(const char* s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  struct StringEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  };

  using Catalog = std::unordered_map<std::string, std::string, StringHash, StringEq>;

  enum class CatalogRead { Ok, Missing, Invalid };

  // Merges the string leaves of the JSON object in `path` into `out` as dotted keys, each after "<prefix>." when a
  // prefix is given. On Invalid, `error` (when given) says why.
  CatalogRead readCatalogFile(const std::filesystem::path& path, Catalog& out, std::string_view prefix = {},
                              std::string* error = nullptr);

  // A module's catalog directory: `sourceDir` when it holds en.json (an uninstalled build), else `installedDir`.
  [[nodiscard]] std::filesystem::path catalogDir(const std::filesystem::path& sourceDir,
                                                 const std::filesystem::path& installedDir);

  namespace detail {
    using Pair = std::pair<std::string_view, std::string>;
    // Replaces each "{name}" with its value; names without a value stay as "{name}" so mismatches show.
    std::string interpolate(std::string_view tmpl, std::span<const Pair> args);
  } // namespace detail

} // namespace i18n
