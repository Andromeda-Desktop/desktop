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

Vec2 TerminalGridNode::cellOffset() const noexcept {
  return {logical(static_cast<int>(std::lround(m_originX * m_scale))), logical(static_cast<int>(std::lround(m_originY * m_scale)))};
}

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
  markPaintDirty();
}

void TerminalGridNode::setBackground(std::uint32_t rgb) {
  if (rgb == m_background) {
    return;
  }
  m_background = rgb;
  if (m_textBacking > 0.0F) {
    rebuildAll();
  }
  markPaintDirty();
}

void TerminalGridNode::setTextBacking(float alpha) {
  if (alpha == m_textBacking) {
    return;
  }
  m_textBacking = alpha;
  rebuildAll();
  markPaintDirty();
}

void TerminalGridNode::setBackgroundAlpha(float alpha) {
  if (alpha == m_backgroundAlpha) {
    return;
  }
  m_backgroundAlpha = alpha;
  markPaintDirty();
}

void TerminalGridNode::setRow(int row, std::span<const TerminalCell> cells) {
  if (row < 0 || row >= rows()) {
    return;
  }
  auto& stored = m_cells[static_cast<std::size_t>(row)];
  const std::size_t n = std::min(cells.size(), stored.size());
  if (std::equal(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(n), stored.begin())
      && std::all_of(stored.begin() + static_cast<std::ptrdiff_t>(n), stored.end(),
                     [](const TerminalCell& c) { return c == TerminalCell{}; })) {
    return;
  }
  // Rows a sized run reaches below its own, before and after this change.
  const auto reach = [](std::span<const TerminalCell> span) {
    int below = 0;
    for (const TerminalCell& c : span) {
      below = (c.flags & TerminalCell::Sized) != 0 ? std::max(below, c.textScale - 1) : below;
    }
    return below;
  };
  const int below = std::max(reach(stored), reach(cells));
  int first = -1;
  int last = -1;
  for (std::size_t col = 0; col < stored.size(); ++col) {
    if (stored[col] != (col < n ? cells[col] : TerminalCell{})) {
      first = first < 0 ? static_cast<int>(col) : first;
      last = static_cast<int>(col);
    }
  }
  std::copy_n(cells.begin(), n, stored.begin());
  std::fill(stored.begin() + static_cast<std::ptrdiff_t>(n), stored.end(), TerminalCell{});
  buildRow(row);
  if (m_cursor && m_cursor->row == row) {
    buildCursor();
  }
  if (below > 0) {
    // A cell of a sized run moves every glyph after it in the run.
    const float w = cellWidth();
    markRowDirty(row, m_originX - w, m_originX + static_cast<float>(m_columns + 1) * w, below);
  } else if (m_textBacking > 0.0F) {
    markRowDirty(row); // a backing run joins text across blanks, so one cell can change the whole row's backing
  } else {
    // One cell of slack each side: a wide cell's other half and glyph overhang.
    const float w = cellWidth();
    const float left = m_originX + static_cast<float>(first - 1) * w;
    markRowDirty(row, left, left + static_cast<float>(last - first + 3) * w);
  }
}

void TerminalGridNode::markRowDirty(int row) {
  // One cell of slack each side, as for a cell span; the padding outside the cells never changes with a row.
  const float w = cellWidth();
  markRowDirty(row, m_originX - w, m_originX + static_cast<float>(m_columns + 1) * w);
}

void TerminalGridNode::markRowDirty(int row, float left, float right, int below) {
  // Half a cell above and below: glyphs that overhang their cell (italic, tall emoji) leave pixels in the next rows.
  const float h = cellHeight();
  const float top = m_originY + static_cast<float>(row) * h;
  markPaintDirty(DamageRect{std::max(left, 0.0F), top - h * 0.5F, std::min(right, width()),
                            top + h * (1.5F + static_cast<float>(below))});
}

void TerminalGridNode::setCursor(std::optional<TerminalCursor> cursor) {
  if (cursor == m_cursor) {
    return;
  }
  const auto markCursor = [this] {
    if (m_cursor) {
      const float w = cellWidth();
      const float left = m_originX + static_cast<float>(m_cursor->column - 1) * w;
      markRowDirty(m_cursor->row, left, left + static_cast<float>(m_cursor->wide ? 4 : 3) * w);
    }
  };
  markCursor();
  m_cursor = cursor;
  buildCursor();
  markCursor();
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
  const std::uint8_t* pixels = m_atlas.pixels();
  if (!m_texture.valid() || m_texture.height != m_atlas.height()) {
    textures.replace(m_texture, pixels, m_atlas.width(), m_atlas.height(), TextureDataFormat::Rgba,
                     TextureFilter::Nearest);
    markPaintDirty();
  } else if (m_atlas.dirtyTop() < m_atlas.dirtyBottom()) {
    const int top = m_atlas.dirtyTop();
    textures.updateSubImage(m_texture,
                            pixels + static_cast<std::size_t>(top) * static_cast<std::size_t>(m_atlas.width()) * 4,
                            0, top, m_atlas.width(), m_atlas.dirtyBottom() - top, TextureDataFormat::Rgba);
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
                                   std::uint32_t rgb, float scale) {
  const TerminalGlyphAtlas::Glyph* g = m_atlas.glyph(codepoint, faceFor(flags), scale);
  if (g == nullptr) {
    return;
  }
  const int baseline = static_cast<int>(std::lround(static_cast<float>(m_atlas.baseline()) * scale));
  const float x0 = logical(x + g->left);
  const float y0 = logical(y + baseline - g->top);
  const float x1 = x0 + logical(g->width);
  const float y1 = y0 + logical(g->height);
  const float u0 = g->x;
  const float v0 = g->y;
  const float u1 = u0 + g->width;
  const float v1 = v0 + g->height;
  const Rgba c = g->color ? premultiplied(0xFFFFFF) : premultiplied(rgb);
  const float corners[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x0, y1, u0, v1},
                               {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
  for (const auto& p : corners) {
    out.insert(out.end(), {p[0], p[1], p[2], p[3], c.r, c.g, c.b, c.a});
  }
}

// Patterns use absolute x, so they run on across cells.
template <typename Rect>
void TerminalGridNode::underline(const TerminalCell& cell, int x, int right, int top, std::uint32_t rgb, Rect& rect) const {
  const int t = m_atlas.strokeThickness();
  const int ch = m_atlas.cellHeight();
  const int y = top + m_atlas.underlinePosition();
  const int bottom = top + ch;
  switch (cell.underline) {
    case TerminalCell::UnderlineStyle::Single: rect(x, y, right, y + t, rgb); break;
    case TerminalCell::UnderlineStyle::Double: {
      const int second = std::min(y + 2 * t, bottom - t);
      const int first = std::min(y, second - 2 * t);
      rect(x, first, right, first + t, rgb);
      rect(x, second, right, second + t, rgb);
      break;
    }
    case TerminalCell::UnderlineStyle::Dotted: {
      const int period = 2 * t;
      for (int px = x - x % period; px < right; px += period) {
        rect(std::max(px, x), y, std::min(px + t, right), y + t, rgb);
      }
      break;
    }
    case TerminalCell::UnderlineStyle::Dashed: {
      const int period = std::max(4 * t, m_atlas.cellWidth());
      const int dash = period * 2 / 3;
      for (int px = x - x % period; px < right; px += period) {
        rect(std::max(px, x), y, std::min(px + dash, right), y + t, rgb);
      }
      break;
    }
    case TerminalCell::UnderlineStyle::Curly: {
      // One wave per cell width, as tall as the room under the baseline allows.
      const float amplitude = std::max(1.0F, std::min(1.5F * static_cast<float>(t), static_cast<float>(bottom - y - t) / 2.0F));
      const float period = static_cast<float>(std::max(4, m_atlas.cellWidth()));
      const float centre = static_cast<float>(y) + amplitude / 2.0F;
      const auto at = [&](int px) {
        return static_cast<int>(std::lround(centre - amplitude * std::sin(6.2831853F * static_cast<float>(px) / period)));
      };
      for (int px = x; px < right; ++px) {
        const int a = at(px);
        const int b = at(px + 1);
        rect(px, std::max(top, std::min(a, b)), px + 1, std::min(bottom, std::max(a, b) + t), rgb);
      }
      break;
    }
  }
}

// The run's cells hold its text, then blank padding to the block width.
void TerminalGridNode::appendSizedRun(std::vector<float>& out, std::span<const TerminalCell> run, int x, int y) {
  const TerminalCell& first = run.front();
  const int n = first.textFraction >> 4;
  const int d = first.textFraction & 0xF;
  const float fraction = n > 0 && d > n ? static_cast<float>(n) / static_cast<float>(d) : 1.0F;
  const float scale = static_cast<float>(first.textScale) * fraction;
  std::size_t end = run.size();
  while (end > 0 && run[end - 1].codepoint <= U' ' && (end < 2 || (run[end - 2].flags & TerminalCell::Wide) == 0)) {
    --end;
  }
  int natural = 0;
  for (std::size_t i = 0; i < end; i += (run[i].flags & TerminalCell::Wide) != 0 ? 2 : 1) {
    natural += (run[i].flags & TerminalCell::Wide) != 0 ? 2 : 1;
  }
  const float cw = static_cast<float>(m_atlas.cellWidth());
  const float ch = static_cast<float>(m_atlas.cellHeight());
  const auto offset = [](int align, float room) { return align == 1 ? room : align == 2 ? room / 2.0F : 0.0F; };
  float pen = static_cast<float>(x)
              + offset(first.textAlign & 0x3, static_cast<float>(run.size()) * cw - static_cast<float>(natural) * cw * scale);
  const int top = y + static_cast<int>(std::lround(
                          offset(first.textAlign >> 2, static_cast<float>(first.textScale) * ch - ch * scale)));
  for (std::size_t i = 0; i < end;) {
    const TerminalCell& cell = run[i];
    const int cells = (cell.flags & TerminalCell::Wide) != 0 ? 2 : 1;
    if (cell.codepoint > U' ') {
      appendGlyph(out, cell.codepoint, cell.flags, static_cast<int>(std::lround(pen)), top, cell.fg, scale);
    }
    pen += static_cast<float>(cells) * cw * scale;
    i += static_cast<std::size_t>(cells);
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
  const int top = row * ch;
  const int thickness = m_atlas.strokeThickness();
  const auto& cells = m_cells[static_cast<std::size_t>(row)];
  auto rect = [&](int x0, int y0, int x1, int y1, std::uint32_t rgb) {
    pushRect(rects, logical(x0), logical(y0), logical(x1), logical(y1), premultiplied(rgb));
  };
  if (m_textBacking > 0.0F) {
    const int n = static_cast<int>(cells.size());
    int start = -1;
    int end = -1; // one past the last text cell of the open run
    auto flush = [&] {
      if (start >= 0) {
        pushRect(rects, logical(start * cw), logical(top), logical(end * cw), logical(top + ch),
                 premultiplied(m_background, m_textBacking));
      }
    };
    for (int col = 0; col < n; ++col) {
      const TerminalCell& cell = cells[static_cast<std::size_t>(col)];
      if (cell.codepoint <= U' ') {
        continue;
      }
      if (start >= 0 && col - end >= kBackingGap) {
        flush();
        start = -1;
      }
      if (start < 0) {
        start = col;
      }
      end = col + ((cell.flags & TerminalCell::Wide) != 0 ? 2 : 1);
    }
    flush();
  }
  for (int col = 0; col < static_cast<int>(cells.size()); ++col) {
    const TerminalCell& cell = cells[static_cast<std::size_t>(col)];
    const int x = col * cw;
    const int right = x + ((cell.flags & TerminalCell::Wide) != 0 ? 2 : 1) * cw;
    const bool sized = (cell.flags & TerminalCell::Sized) != 0;
    if ((cell.flags & TerminalCell::Background) != 0) {
      rect(x, top, right, top + ch * (sized ? cell.textScale : 1), cell.bg);
    }
    if (sized && (cell.flags & TerminalCell::SizedStart) != 0) {
      std::size_t end = static_cast<std::size_t>(col) + 1;
      while (end < cells.size() && (cells[end].flags & (TerminalCell::Sized | TerminalCell::SizedStart)) == TerminalCell::Sized) {
        ++end;
      }
      appendSizedRun(glyphs, std::span(cells).subspan(static_cast<std::size_t>(col), end - static_cast<std::size_t>(col)),
                     x, top);
    } else if (!sized && cell.codepoint > U' ') {
      appendGlyph(glyphs, cell.codepoint, cell.flags, x, top, cell.fg);
    }
    if ((cell.flags & TerminalCell::Underline) != 0) {
      underline(cell, x, right, top, (cell.flags & TerminalCell::DecorationColor) != 0 ? cell.decoration : cell.fg, rect);
    }
    if ((cell.flags & TerminalCell::Strikethrough) != 0) {
      const int y = top + m_atlas.baseline() - ch / 4;
      rect(x, y, right, y + thickness, cell.fg);
    }
    if ((cell.flags & TerminalCell::Overline) != 0) {
      rect(x, top, right, top + thickness, cell.fg);
    }
    // A wide cell draws both columns; its tail cell would paint over the right half.
    if ((cell.flags & TerminalCell::Wide) != 0) {
      ++col;
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
  const int x0 = c.column * cw;
  const int y0 = c.row * ch;
  const int x1 = x0 + (c.wide ? 2 : 1) * cw;
  const int y1 = y0 + ch;
  const int stroke = std::max(1, static_cast<int>(std::lround(1.5F * m_scale)));
  auto rect = [&](int a, int b, int r, int d) {
    pushRect(m_cursorRects, logical(a), logical(b), logical(r), logical(d), premultiplied(c.color));
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
    rect(x0, y0 + m_atlas.glyphTop(), x0 + stroke, y0 + m_atlas.glyphBottom());
    break;
  case TerminalCursorStyle::Underline:
    rect(x0, y1 - stroke, x1, y1);
    break;
  }
}
