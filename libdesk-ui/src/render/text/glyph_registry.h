#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Glyph names resolve in this order:
// 1. Explicit codepoint literals such as U+F123 or 0xF123.
// 2. Hand-curated Noctalia aliases from glyph_registry.cpp.
// 3. Native Tabler icon names (compiled in from libdesk-ui/data/tabler.json).
namespace GlyphRegistry {

  struct TablerGlyph {
    std::string_view name;
    char32_t codepoint = 0;
    std::string_view category;
  };

  [[nodiscard]] bool contains(std::string_view name);
  [[nodiscard]] char32_t lookup(std::string_view name);

  // The Tabler catalog, sorted by name; the views point to static storage.
  [[nodiscard]] std::size_t tablerGlyphCount();
  [[nodiscard]] TablerGlyph tablerGlyphAt(std::size_t index);
  [[nodiscard]] std::optional<TablerGlyph> findTablerGlyph(std::string_view name);
  [[nodiscard]] std::optional<std::string_view> categoryFor(std::string_view name);
  // Hand-curated Noctalia alias -> native Tabler icon name map.
  [[nodiscard]] const std::unordered_map<std::string, std::string_view>& aliases();

} // namespace GlyphRegistry
