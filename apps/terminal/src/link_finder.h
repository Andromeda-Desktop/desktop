#pragma once

#include <optional>
#include <span>
#include <string>

namespace term {

  struct LinkSpan {
    int start = 0; // columns [start, end)
    int end = 0;
    std::string uri;
  };

  // The link under column col of a row: an http(s)/file URL, or a path to an existing file or directory with an
  // optional :line[:col] suffix (absolute, ~/, or relative to cwd). cells holds one codepoint per column, 0 for the
  // spacer after a wide character.
  [[nodiscard]] std::optional<LinkSpan> findLink(std::span<const char32_t> cells, int col, const std::string& cwd);

} // namespace term
