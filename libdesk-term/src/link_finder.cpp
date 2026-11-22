#include "link_finder.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace term {

  namespace {

    bool separator(char32_t c) {
      return c == ' ' || c == '\t' || (c != 0 && c < 0x20) || std::u32string_view(U"\"'`<>()[]{}|").contains(c);
    }

    bool trailing(char32_t c) { return std::u32string_view(U".,;:!?").contains(c); }

    bool schemeChar(char32_t c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-'
             || c == '.';
    }

    std::string utf8(std::span<const char32_t> cells) {
      std::string out;
      for (const char32_t c : cells) {
        if (c == 0) {
          continue;
        }
        if (c < 0x80) {
          out += static_cast<char>(c);
        } else if (c < 0x800) {
          out += static_cast<char>(0xC0 | (c >> 6));
          out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
          out += static_cast<char>(0xE0 | (c >> 12));
          out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
          out += static_cast<char>(0xF0 | (c >> 18));
          out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
          out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (c & 0x3F));
        }
      }
      return out;
    }


    // Drops up to two ":<digits>" suffixes (line, column).
    std::string_view withoutPosition(std::string_view s) {
      for (int i = 0; i < 2; ++i) {
        const std::size_t colon = s.rfind(':');
        if (colon == std::string_view::npos || colon + 1 == s.size()
            || s.find_first_not_of("0123456789", colon + 1) != std::string_view::npos) {
          break;
        }
        s = s.substr(0, colon);
      }
      return s;
    }

  } // namespace

  std::string fileUri(const std::string& path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out = "file://";
    for (const char ch : path) {
      const auto c = static_cast<unsigned char>(ch);
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
          || std::string_view("/-._~").contains(ch)) {
        out += ch;
      } else {
        out += '%';
        out += kHex[c >> 4];
        out += kHex[c & 0xF];
      }
    }
    return out;
  }

  std::optional<LinkSpan> findLink(std::span<const char32_t> cells, int col, const std::string& cwd) {
    const int n = static_cast<int>(cells.size());
    while (col > 0 && col < n && cells[static_cast<std::size_t>(col)] == 0) {
      --col;
    }
    if (col < 0 || col >= n || separator(cells[static_cast<std::size_t>(col)])) {
      return std::nullopt;
    }
    const auto at = [&](int i) { return cells[static_cast<std::size_t>(i)]; };
    int start = col;
    while (start > 0 && !separator(at(start - 1))) {
      --start;
    }
    int end = col + 1;
    while (end < n && !separator(at(end))) {
      ++end;
    }
    while (end > start && trailing(at(end - 1))) {
      --end;
    }
    if (col >= end) {
      return std::nullopt;
    }

    for (int k = start; k + 3 <= end; ++k) {
      if (at(k) != ':' || at(k + 1) != '/' || at(k + 2) != '/') {
        continue;
      }
      int p = k;
      while (p > start && schemeChar(at(p - 1))) {
        --p;
      }
      std::string scheme = utf8(cells.subspan(static_cast<std::size_t>(p), static_cast<std::size_t>(k - p)));
      for (char& c : scheme) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if ((scheme != "http" && scheme != "https" && scheme != "file") || col < p || k + 3 == end) {
        return std::nullopt;
      }
      return LinkSpan{.start = p,
                      .end = end,
                      .uri = utf8(cells.subspan(static_cast<std::size_t>(p), static_cast<std::size_t>(end - p)))};
    }

    const std::string token = utf8(cells.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
    const std::string_view text = withoutPosition(token);
    std::filesystem::path path;
    if (text.starts_with('/')) {
      path = text;
    } else if (text.starts_with("~/")) {
      const char* home = std::getenv("HOME");
      if (home == nullptr) {
        return std::nullopt;
      }
      path = std::filesystem::path(home) / text.substr(2);
    } else if (!cwd.empty()) {
      path = std::filesystem::path(cwd) / text;
    } else {
      return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      return std::nullopt;
    }
    return LinkSpan{.start = start, .end = end, .uri = fileUri(path.lexically_normal().string())};
  }

} // namespace term
