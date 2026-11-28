#pragma once

#include "ui/token_slots.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <variant>

// The dev token file (design-tokens.toml, dev builds only): the base layer under the desktop theme.
namespace noctalia::theme::dev_tokens {

  using Value = std::variant<double, std::int64_t, bool, std::string>;

  // Leaf values by token path; "= a.b - c.d" expressions stay unevaluated.
  struct File {
    std::map<std::string, Value> raw;
  };

  [[nodiscard]] std::expected<File, std::string> readFile(const std::filesystem::path& path);

  // The file's values with `top` over its leaves; expressions evaluate over the merged table, so a derived token
  // follows a theme value. Release builds return `top`.
  [[nodiscard]] std::expected<desk::tokens::SlotValues, std::string> resolve(const File& file,
                                                                             const desk::tokens::SlotValues& top);

  // ANDROMEDA_DEV_TOKENS: the path to design-tokens.toml, or empty when dev mode is off.
  [[nodiscard]] std::filesystem::path envPath();

} // namespace noctalia::theme::dev_tokens
