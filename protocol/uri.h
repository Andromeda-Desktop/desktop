#pragma once

// Percent-encoding (RFC 3986), file URIs (RFC 8089) and text/uri-list (RFC 2483): the one codec for every module.
// Pure C++: the compositor, apps and the macOS builds include it.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uri {

  inline constexpr char kUriListMime[] = "text/uri-list";

  namespace detail {

    constexpr int hexValue(char ch) {
      if (ch >= '0' && ch <= '9') {
        return ch - '0';
      }
      if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
      }
      if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
      }
      return -1;
    }

    constexpr bool unreserved(unsigned char ch) {
      return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'
          || ch == '.' || ch == '~';
    }

  } // namespace detail

  // Escapes every byte outside the unreserved set (A-Z a-z 0-9 - _ . ~) and `keep`, with uppercase hex.
  [[nodiscard]] inline std::string encodeComponent(std::string_view text, std::string_view keep = {}) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char raw : text) {
      const auto ch = static_cast<unsigned char>(raw);
      if (detail::unreserved(ch) || keep.find(raw) != std::string_view::npos) {
        out.push_back(raw);
      } else {
        out.push_back('%');
        out.push_back(kHex[ch >> 4U]);
        out.push_back(kHex[ch & 0x0FU]);
      }
    }
    return out;
  }

  // Space: '+' decodes to a space (HTML form values); otherwise '+' stays literal.
  enum class Plus : bool { Literal, Space };

  // An invalid or truncated escape stays literal.
  [[nodiscard]] inline std::string decodeComponent(std::string_view text, Plus plus = Plus::Literal) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size()) {
        const int hi = detail::hexValue(text[i + 1]);
        const int lo = detail::hexValue(text[i + 2]);
        if (hi >= 0 && lo >= 0) {
          out.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      out.push_back(plus == Plus::Space && text[i] == '+' ? ' ' : text[i]);
    }
    return out;
  }

  // nullopt when an escape is invalid or truncated.
  [[nodiscard]] inline std::optional<std::string> decodeComponentStrict(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '%') {
        out.push_back(text[i]);
        continue;
      }
      if (i + 2 >= text.size()) {
        return std::nullopt;
      }
      const int hi = detail::hexValue(text[i + 1]);
      const int lo = detail::hexValue(text[i + 2]);
      if (hi < 0 || lo < 0) {
        return std::nullopt;
      }
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    }
    return out;
  }

  // The file URI of an absolute local path, byte-identical to g_filename_to_uri: thumbnail names hash it.
  [[nodiscard]] inline std::string fileUri(std::string_view path) {
    return "file://" + encodeComponent(path, "!$&'()*+,/:=@");
  }

  // The local path of file:///p or file://localhost/p; nullopt for another scheme, a remote host, a relative path, an
  // invalid escape, or an escaped NUL or '/' (g_filename_from_uri rejects those too).
  [[nodiscard]] inline std::optional<std::string> pathFromFileUri(std::string_view text) {
    constexpr std::string_view kScheme = "file://";
    if (!text.starts_with(kScheme)) {
      return std::nullopt;
    }
    text.remove_prefix(kScheme.size());
    if (text.starts_with("localhost/")) {
      text.remove_prefix(std::string_view("localhost").size());
    }
    if (!text.starts_with('/')) {
      return std::nullopt;
    }
    if (const auto end = text.find_first_of("?#"); end != std::string_view::npos) {
      text = text.substr(0, end);
    }
    auto path = decodeComponentStrict(text);
    if (!path || path->find('\0') != std::string::npos || text.find("%2F") != std::string_view::npos
        || text.find("%2f") != std::string_view::npos) {
      return std::nullopt;
    }
    return path;
  }

  // The URIs of a text/uri-list body: CRLF or LF lines, trailing blanks cut; comments ('#') and blank lines are skipped.
  [[nodiscard]] inline std::vector<std::string> parseUriList(std::string_view body) {
    std::vector<std::string> uris;
    while (!body.empty()) {
      const auto end = body.find('\n');
      std::string_view line = body.substr(0, end);
      body = end == std::string_view::npos ? std::string_view{} : body.substr(end + 1);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
      }
      if (!line.empty() && line.front() != '#') {
        uris.emplace_back(line);
      }
    }
    return uris;
  }

  // A text/uri-list body: each URI ends with CRLF.
  [[nodiscard]] inline std::string joinUriList(const std::vector<std::string>& uris) {
    std::string body;
    for (const auto& item : uris) {
      body += item;
      body += "\r\n";
    }
    return body;
  }

} // namespace uri
