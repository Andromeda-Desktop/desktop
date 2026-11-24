#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace andromeda::index {

  // Bytes of text kept per file.
  inline constexpr std::size_t kMaxText = 256 * 1024;

  enum class DocumentKind : std::uint8_t { None, Text, Html, Pdf, Zip, Photo, Audio };

  // Lowercase extension of a file name, or empty.
  [[nodiscard]] std::string extensionOf(std::string_view name);
  [[nodiscard]] DocumentKind documentKind(std::string_view name);
  // True when the file name has an extension the indexer reads: plain text, Markdown and other text documents,
  // HTML, PDF, OpenDocument, Office Open XML and EPUB; for photos and audio, their metadata (camera, date, tags).
  // Source and config files are not read.
  [[nodiscard]] bool extractable(std::string_view name);

  // Text of a Text document, read in the daemon.
  [[nodiscard]] std::optional<std::string> extractPlain(const std::string& path);
  // Every other kind is parsed by andromeda-index-extract (indexer/extract, Rust, sandboxed).
  [[nodiscard]] inline bool needsHelper(DocumentKind kind) { return kind != DocumentKind::Text && kind != DocumentKind::None; }

} // namespace andromeda::index
