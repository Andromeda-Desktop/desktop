#include "render/scene/terminal_grid_node.h"

#include "render/core/texture_manager.h"

#include <algorithm>
#include <cmath>

namespace {

  struct Rgba {
    float r, g, b, a;
  };

  Rgba premultiplied(std::uint32_t rgb, float alpha = 1.0F) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0F * alpha, static_cast<float>((rgb >> 8) & 0xFF) / 255.0F * alpha,
            static_cast<float>(rgb & 0xFF) / 255.0F * alpha, alpha};
  }

  void pushRect(std::vector<float>& out, float x0, float y0, float x1, float y1, Rgba c) {
    const float corners[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (const auto& p : corners) {
      out.insert(out.end(), {p[0], p[1], c.r, c.g, c.b, c.a});
    }
  }

  TerminalGlyphAtlas::Face faceFor(std::uint16_t flags) {
    const bool bold = (flags & TerminalCell::Bold) != 0;
    const bool italic = (flags & TerminalCell::Italic) != 0;
    if (bold && italic) {
      return TerminalGlyphAtlas::Face::BoldItalic;
    }
    if (bold) {
      return TerminalGlyphAtlas::Face::Bold;
    }
    return italic ? TerminalGlyphAtlas::Face::Italic : TerminalGlyphAtlas::Face::Regular;
  }

} // namespace

TerminalGridNode::TerminalGridNode() : Node(NodeType::TerminalGrid) {}

TerminalGridNode::~TerminalGridNode() {
  if (m_textureManager != nullptr) {
    m_textureManager->unload(m_texture);
  }
}

bool TerminalGridNode::setFont(const std::string& family, float size, float lineHeight, float scale) {
  m_scale = std::max(scale, 0.25F);
  if (!m_atlas.load(family, size * m_scale, lineHeight * m_scale)) {
    return false;
  }
  rebuildAll();
  markPaintDirty();
  return true;
}

float TerminalGridNode::cellWidth() const noexcept { return logical(m_atlas.cellWidth()); }

float TerminalGridNode::cellHeight() const noexcept { return logical(m_atlas.cellHeight()); }

float TerminalGridNode::logical(int devicePx) const noexcept { return static_cast<float>(devicePx) / m_scale; }

void TerminalGridNode::setGridSize(int columns, int rows) {
  m_columns = std::max(columns, 0);
  rows = std::max(rows, 0);
  m_cells.assign(static_cast<std::size_t>(rows), std::vector<TerminalCell>(static_cast<std::size_t>(m_columns)));
  m_rects.assign(static_cast<std::size_t>(rows), {});
  m_glyphs.assign(static_cast<std::size_t>(rows), {});
  buildCursor();
  markPaintDirty();
}

void TerminalGridNode::setGridOrigin(float x, float y) {
  if (x == m_originX && y == m_originY) {
    return;
  }
  m_originX = x;
  m_originY = y;
  rebuildAll();
  markPaintDirty();
}

void TerminalGridNode::setBackground(std::uint32_t rgb) {
  if (rgb == m_background) {
    return;
  }
  m_background = rgb;
  markPaintDirty();
}

void TerminalGridNode::setRow(int row, std::span<const TerminalCell> cells) {
  if (row < 0 || row >= rows()) {
    return;
  }
  auto& stored = m_cells[static_cast<std::size_t>(row)];
  const std::size_t n = std::min(cells.size(), stored.size());
  std::copy_n(cells.begin(), n, stored.begin());
  std::fill(stored.begin() + static_cast<std::ptrdiff_t>(n), stored.end(), TerminalCell{});
  buildRow(row);
  if (m_cursor && m_cursor->row == row) {
    buildCursor();
  }
  markPaintDirty();
}

void TerminalGridNode::setCursor(std::optional<TerminalCursor> cursor) {
  if (cursor == m_cursor) {
    return;
  }
  m_cursor = cursor;
  buildCursor();
  markPaintDirty();
}

void TerminalGridNode::commit(TextureManager& textures) {
  // A full atlas clears itself while rows build; the rows built before that point at stale glyphs.
  for (int pass = 0; pass < 2 && m_atlas.generation() != m_builtGeneration; ++pass) {
    rebuildAll();
  }
  if (m_textureManager != nullptr && m_textureManager != &textures) {
    m_textureManager->unload(m_texture);
  }
  m_textureManager = &textures;
  const auto& pixels = m_atlas.pixels();
  if (!m_texture.valid() || m_texture.height != m_atlas.height()) {
    textures.replace(m_texture, pixels.data(), m_atlas.width(), m_atlas.height(), TextureDataFormat::Alpha,
                     TextureFilter::Nearest);
    markPaintDirty();
  } else if (m_atlas.dirtyTop() < m_atlas.dirtyBottom()) {
    const int top = m_atlas.dirtyTop();
    textures.updateSubImage(m_texture, pixels.data() + static_cast<std::size_t>(top) * static_cast<std::size_t>(m_atlas.width()),
                            0, top, m_atlas.width(), m_atlas.dirtyBottom() - top, TextureDataFormat::Alpha);
    markPaintDirty();
  }
  m_atlas.clearDirty();
}

void TerminalGridNode::doInvalidateGpuResources(Renderer& renderer) {
  (void)renderer;
  if (m_textureManager != nullptr && m_texture.valid()) {
    m_textureManager->unload(m_texture);
  }
  m_texture = {};
  if (m_textureManager != nullptr) {
    commit(*m_textureManager);
  }
  markPaintDirty();
}

void TerminalGridNode::rebuildAll() {
  m_builtGeneration = m_atlas.generation();
  for (int row = 0; row < rows(); ++row) {
    buildRow(row);
  }
  buildCursor();
}

void TerminalGridNode::appendGlyph(std::vector<float>& out, char32_t codepoint, std::uint16_t flags, int x, int y,
                                   std::uint32_t rgb) {
  const TerminalGlyphAtlas::Glyph* g = m_atlas.glyph(codepoint, faceFor(flags));
  if (g == nullptr) {
    return;
  }
  const int ox = static_cast<int>(std::lround(m_originX * m_scale));
  const int oy = static_cast<int>(std::lround(m_originY * m_scale));
  const float x0 = logical(ox + x + g->left);
  const float y0 = logical(oy + y + m_atlas.baseline() - g->top);
  const float x1 = x0 + logical(g->width);
  const float y1 = y0 + logical(g->height);
  const float u0 = g->x;
  const float v0 = g->y;
  const float u1 = u0 + g->width;
  const float v1 = v0 + g->height;
  const Rgba c = premultiplied(rgb);
  const float corners[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                               {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
  for (const auto& p : corners) {
    out.insert(out.end(), {p[0], p[1], p[2], p[3], c.r, c.g, c.b, c.a});
  }
}

void TerminalGridNode::buildRow(int row) {
  auto& rects = m_rects[static_cast<std::size_t>(row)];
  auto& glyphs = m_glyphs[static_cast<std::size_t>(row)];
  rects.clear();
  glyphs.clear();
  const int cw = m_atlas.cellWidth();
  const int ch = m_atlas.cellHeight();
  if (cw <= 0 || ch <= 0) {
    return;
  }
  const int ox = static_cast<int>(std::lround(m_originX * m_scale));
  const int oy = static_cast<int>(std::lround(m_originY * m_scale));
  const int top = row * ch;
  const int thickness = m_atlas.strokeThickness();
  const auto& cells = m_cells[static_cast<std::size_t>(row)];
  auto rect = [&](int x0, int y0, int x1, int y1, std::uint32_t rgb) {
    pushRect(rects, logical(ox + x0), logical(oy + y0), logical(ox + x1), logical(oy + y1), premultiplied(rgb));
  };
  for (int col = 0; col < static_cast<int>(cells.size()); ++col) {
    const TerminalCell& cell = cells[static_cast<std::size_t>(col)];
    const int x = col * cw;
    const int right = x + ((cell.flags & TerminalCell::Wide) != 0 ? 2 : 1) * cw;
    if ((cell.flags & TerminalCell::Background) != 0) {
      rect(x, top, right, top + ch, cell.bg);
    }
    if (cell.codepoint > U' ') {
      appendGlyph(glyphs, cell.codepoint, cell.flags, x, top, cell.fg);
    }
    if ((cell.flags & TerminalCell::Underline) != 0) {
      const int y = top + m_atlas.underlinePosition();
      rect(x, y, right, y + thickness, cell.fg);
    }
    if ((cell.flags & TerminalCell::Strikethrough) != 0) {
      const int y = top + m_atlas.baseline() - ch / 4;
      rect(x, y, right, y + thickness, cell.fg);
    }
    if ((cell.flags & TerminalCell::Overline) != 0) {
      rect(x, top, right, top + thickness, cell.fg);
    }
  }
}

void TerminalGridNode::buildCursor() {
  m_cursorRects.clear();
  m_cursorGlyphs.clear();
  const int cw = m_atlas.cellWidth();
  const int ch = m_atlas.cellHeight();
  if (!m_cursor || cw <= 0 || m_cursor->row < 0 || m_cursor->row >= rows() || m_cursor->column < 0
      || m_cursor->column >= m_columns) {
    return;
  }
  const TerminalCursor& c = *m_cursor;
  const int ox = static_cast<int>(std::lround(m_originX * m_scale));
  const int oy = static_cast<int>(std::lround(m_originY * m_scale));
  const int x0 = c.column * cw;
  const int y0 = c.row * ch;
  const int x1 = x0 + (c.wide ? 2 : 1) * cw;
  const int y1 = y0 + ch;
  const int stroke = std::max(1, static_cast<int>(std::lround(1.5F * m_scale)));
  auto rect = [&](int a, int b, int r, int d) {
    pushRect(m_cursorRects, logical(ox + a), logical(oy + b), logical(ox + r), logical(oy + d), premultiplied(c.color));
  };
  switch (c.style) {
  case TerminalCursorStyle::Block: {
    rect(x0, y0, x1, y1);
    const TerminalCell& cell = m_cells[static_cast<std::size_t>(c.row)][static_cast<std::size_t>(c.column)];
    if (cell.codepoint > U' ') {
      appendGlyph(m_cursorGlyphs, cell.codepoint, cell.flags, x0, y0, c.textColor);
    }
    break;
  }
  case TerminalCursorStyle::HollowBlock:
    rect(x0, y0, x1, y0 + stroke);
    rect(x0, y1 - stroke, x1, y1);
    rect(x0, y0 + stroke, x0 + stroke, y1 - stroke);
    rect(x1 - stroke, y0 + stroke, x1, y1 - stroke);
    break;
  case TerminalCursorStyle::Bar:
    rect(x0, y0, x0 + stroke, y1);
    break;
  case TerminalCursorStyle::Underline:
    rect(x0, y1 - stroke, x1, y1);
    break;
  }
}
