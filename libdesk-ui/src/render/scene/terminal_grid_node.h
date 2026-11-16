#pragma once

#include "render/core/mat3.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "render/text/terminal_glyph_atlas.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class TextureManager;

struct TerminalCell {
  enum Flag : std::uint16_t {
    Background = 1 << 0, // bg is drawn; without it the grid background shows
    Bold = 1 << 1,
    Italic = 1 << 2,
    Underline = 1 << 3,
    Strikethrough = 1 << 4,
    Overline = 1 << 5,
    Wide = 1 << 6, // the glyph and background span this cell and the next
    DecorationColor = 1 << 7, // underline in decoration, not fg
  };
  enum class UnderlineStyle : std::uint8_t { Single, Double, Curly, Dotted, Dashed };
  char32_t codepoint = 0;      // 0 = no glyph
  std::uint32_t fg = 0;        // 0xRRGGBB
  std::uint32_t bg = 0;        // 0xRRGGBB
  std::uint32_t decoration = 0; // 0xRRGGBB, with DecorationColor
  std::uint16_t flags = 0;
  UnderlineStyle underline = UnderlineStyle::Single; // with Underline
  bool operator==(const TerminalCell&) const = default;
};

enum class TerminalCursorStyle : std::uint8_t { Block, HollowBlock, Bar, Underline };

struct TerminalCursor {
  int column = 0;
  int row = 0;
  TerminalCursorStyle style = TerminalCursorStyle::Block;
  std::uint32_t color = 0;     // 0xRRGGBB
  std::uint32_t textColor = 0; // the glyph under a block cursor
  bool wide = false;
  bool operator==(const TerminalCursor&) const = default;
};

// A monospace cell grid drawn in two batched passes (backgrounds, then glyphs from one atlas). The owner feeds
// changed rows; only those rows rebuild their vertices. Positions are logical px; glyphs stay on device pixels.
class TerminalGridNode : public Node {
public:
  // Children with zIndex <= kBelowBackgroundZ draw between the background fill and the cell backgrounds; those up to
  // kBelowTextZ between the cell backgrounds and the text; the rest above the text.
  static constexpr int kBelowBackgroundZ = -2;
  static constexpr int kBelowTextZ = -1;
  TerminalGridNode();
  ~TerminalGridNode() override;
  TerminalGridNode(const TerminalGridNode&) = delete;
  TerminalGridNode& operator=(const TerminalGridNode&) = delete;

  // size and lineHeight in logical px; scale = device px per logical px. False when no font loads.
  bool setFont(const std::string& family, float size, float lineHeight, float scale);
  [[nodiscard]] float cellWidth() const noexcept;
  [[nodiscard]] float cellHeight() const noexcept;

  void setGridSize(int columns, int rows);
  [[nodiscard]] int columns() const noexcept { return m_columns; }
  [[nodiscard]] int rows() const noexcept { return static_cast<int>(m_cells.size()); }
  // Offset of cell (0, 0) from the node origin, logical px.
  void setGridOrigin(float x, float y);
  void setBackground(std::uint32_t rgb);
  void setRow(int row, std::span<const TerminalCell> cells);
  void setCursor(std::optional<TerminalCursor> cursor);

  // Uploads new glyphs to the atlas texture. Call with the render context current, before the frame renders.
  void commit(TextureManager& textures);

  [[nodiscard]] std::uint32_t background() const noexcept { return m_background; }
  // The background fill's alpha: below 1 the window's glass shows through the default-background cells.
  void setBackgroundAlpha(float alpha);
  [[nodiscard]] float backgroundAlpha() const noexcept { return m_backgroundAlpha; }
  // Over a see-through background: the background colour at this alpha behind each run of text (runs join across
  // gaps of fewer than kBackingGap blank cells), so the text keeps its contrast while empty cells stay clear. 0 = off.
  void setTextBacking(float alpha);
  [[nodiscard]] float textBacking() const noexcept { return m_textBacking; }
  static constexpr int kBackingGap = 4;
  [[nodiscard]] TextureId atlasTexture() const noexcept { return m_texture.id; }
  [[nodiscard]] int atlasWidth() const noexcept { return m_atlas.width(); }
  [[nodiscard]] int atlasHeight() const noexcept { return m_texture.height; }
  // The grid origin snapped to device px; cell vertices exclude it, so a scroll step rebuilds no rows.
  [[nodiscard]] Vec2 cellOffset() const noexcept;
  [[nodiscard]] std::span<const std::vector<float>> rectLists() const noexcept { return m_rects; }
  [[nodiscard]] std::span<const std::vector<float>> glyphLists() const noexcept { return m_glyphs; }
  [[nodiscard]] std::span<const std::vector<float>> cursorRectLists() const noexcept { return {&m_cursorRects, 1}; }
  [[nodiscard]] std::span<const std::vector<float>> cursorGlyphLists() const noexcept { return {&m_cursorGlyphs, 1}; }

private:
  void doInvalidateGpuResources(Renderer& renderer) override;
  void buildRow(int row);
  template <typename Rect>
  void underline(const TerminalCell& cell, int x, int right, int top, std::uint32_t rgb, Rect& rect) const;
  void markRowDirty(int row);
  void markRowDirty(int row, float left, float right); // node-local logical x range
  void buildCursor();
  void rebuildAll();
  void appendGlyph(std::vector<float>& out, char32_t codepoint, std::uint16_t flags, int x, int y, std::uint32_t rgb);
  [[nodiscard]] float logical(int devicePx) const noexcept;

  TerminalGlyphAtlas m_atlas;
  float m_scale = 1.0F;
  int m_columns = 0;
  float m_originX = 0.0F;
  float m_originY = 0.0F;
  std::uint32_t m_background = 0;
  float m_backgroundAlpha = 1.0F;
  float m_textBacking = 0.0F;
  std::vector<std::vector<TerminalCell>> m_cells;
  std::optional<TerminalCursor> m_cursor;
  std::uint64_t m_builtGeneration = 0;

  // One list per row.
  std::vector<std::vector<float>> m_rects;
  std::vector<std::vector<float>> m_glyphs;
  std::vector<float> m_cursorRects;
  std::vector<float> m_cursorGlyphs;

  TextureManager* m_textureManager = nullptr;
  TextureHandle m_texture;
};
