#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A glyph atlas for a monospace cell grid: FreeType rasterizes each (codepoint, face) once into one premultiplied
// RGBA bitmap. Coverage glyphs are stored as white (a, a, a, a) and take the text colour; colour glyphs (emoji) keep
// their own and are scaled to fit two cells. Glyphs missing from the font come from a fontconfig fallback face.
class TerminalGlyphAtlas {
public:
  enum class Face : std::uint8_t { Regular, Bold, Italic, BoldItalic };

  // Device pixels. Bearings put the bitmap relative to the cell's left edge and baseline.
  struct Glyph {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t left = 0;
    std::int16_t top = 0;
    bool color = false; // draw with a white vertex colour, not the text colour
  };

  TerminalGlyphAtlas();
  ~TerminalGlyphAtlas();
  TerminalGlyphAtlas(const TerminalGlyphAtlas&) = delete;
  TerminalGlyphAtlas& operator=(const TerminalGlyphAtlas&) = delete;

  // family: a fontconfig family list ("Fira Code,monospace"). Sizes are device pixels. False when no face loads.
  bool load(const std::string& family, float pixelSize, float lineHeight);

  // Rasterizes on first use. Null for a glyph with no ink (space) or no font that has it.
  const Glyph* glyph(char32_t codepoint, Face face);

  [[nodiscard]] int cellWidth() const noexcept { return m_cellWidth; }
  [[nodiscard]] int cellHeight() const noexcept { return m_cellHeight; }
  [[nodiscard]] int baseline() const noexcept { return m_baseline; }
  // Font ascender and descender from the cell top, in pixels.
  [[nodiscard]] int glyphTop() const noexcept { return m_glyphTop; }
  [[nodiscard]] int glyphBottom() const noexcept { return m_glyphBottom; }
  [[nodiscard]] int underlinePosition() const noexcept { return m_underlinePosition; }
  [[nodiscard]] int strokeThickness() const noexcept { return m_strokeThickness; }

  // RGBA, 4 bytes per pixel, width() pixels per row, height() rows.
  [[nodiscard]] const std::uint8_t* pixels() const noexcept { return m_pixels.get(); }
  [[nodiscard]] int width() const noexcept { return kWidth; }
  [[nodiscard]] int height() const noexcept { return m_height; }
  // Rows [dirtyTop, dirtyBottom) changed since clearDirty(); empty when dirtyTop >= dirtyBottom.
  [[nodiscard]] int dirtyTop() const noexcept { return m_dirtyTop; }
  [[nodiscard]] int dirtyBottom() const noexcept { return m_dirtyBottom; }
  void clearDirty() noexcept;
  // Changes when the atlas is cleared: every Glyph position taken before is invalid.
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

private:
  struct FontFace;
  struct FreeDeleter {
    void operator()(std::uint8_t* p) const noexcept { std::free(p); }
  };
  struct Key {
    char32_t codepoint;
    Face face;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept { return (std::size_t(k.codepoint) << 2) | std::size_t(k.face); }
  };

  static constexpr int kWidth = 1024;
  static constexpr int kMaxHeight = 4096;

  FontFace* faceFor(Face face, char32_t needs, bool color = false);
  FontFace* openFace(const std::string& file, int index, bool embolden, bool oblique);
  bool place(int width, int height, int& x, int& y);
  const Glyph* sprite(const Key& key, const std::vector<std::uint8_t>& alpha);
  static std::unique_ptr<std::uint8_t, FreeDeleter> allocatePixels(int height);
  void reset();

  std::string m_family;
  float m_pixelSize = 0.0F;
  struct Library;
  std::unique_ptr<Library> m_library;
  std::unordered_map<std::string, std::unique_ptr<FontFace>> m_faces; // by "file#index"
  FontFace* m_primary[4] = {};
  std::unordered_map<Key, Glyph, KeyHash> m_glyphs;
  std::unordered_set<Key, KeyHash> m_missing;

  int m_cellWidth = 0;
  int m_cellHeight = 0;
  int m_baseline = 0;
  int m_glyphTop = 0;
  int m_glyphBottom = 0;
  int m_underlinePosition = 0;
  int m_strokeThickness = 1;

  // calloc: rows no glyph has reached stay untouched pages, so they take no RAM.
  std::unique_ptr<std::uint8_t, FreeDeleter> m_pixels;
  int m_height = 0;
  int m_shelfX = 0;
  int m_shelfY = 0;
  int m_shelfHeight = 0;
  int m_dirtyTop = 0;
  int m_dirtyBottom = 0;
  std::uint64_t m_generation = 0;
};
