#include "render/text/terminal_glyph_atlas.h"
#include "render/text/terminal_sprites.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <new>
#include <optional>

struct TerminalGlyphAtlas::Library {
  FT_Library ft = nullptr;
  ~Library() {
    if (ft != nullptr) {
      FT_Done_FreeType(ft);
    }
  }
};

struct TerminalGlyphAtlas::FontFace {
  FT_Face face = nullptr;
  bool color = false; // bitmap colour font (CBDT/sbix): rendered as BGRA, scaled to the cell
  bool embolden = false;
  bool oblique = false;
  ~FontFace() {
    if (face != nullptr) {
      FT_Done_Face(face);
    }
  }
};

namespace {

  constexpr int kInitialHeight = 256;
  constexpr int kPadding = 1;
  constexpr std::size_t kBytesPerPixel = 4;

  struct FontMatch {
    std::string file;
    int index = 0;
    bool embolden = false; // fontconfig asks for synthetic bold: the family has no bold file
  };

  // needs != 0 asks for a face that covers that codepoint (fallback).
  // color asks for a colour font (emoji); otherwise colour fonts are excluded.
  std::optional<FontMatch> matchFont(const std::string& family, bool bold, bool italic, char32_t needs, bool color) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (pattern == nullptr) {
      return std::nullopt;
    }
    FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_SEMIBOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddBool(pattern, FC_COLOR, color ? FcTrue : FcFalse);
    if (needs != 0) {
      FcCharSet* charset = FcCharSetCreate();
      FcCharSetAddChar(charset, needs);
      FcPatternAddCharSet(pattern, FC_CHARSET, charset);
      FcCharSetDestroy(charset);
    }
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == nullptr) {
      return std::nullopt;
    }
    std::optional<FontMatch> out;
    FcChar8* file = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
      out = FontMatch{.file = reinterpret_cast<const char*>(file)};
      FcPatternGetInteger(match, FC_INDEX, 0, &out->index);
      FcBool embolden = FcFalse;
      FcPatternGetBool(match, FC_EMBOLDEN, 0, &embolden);
      out->embolden = embolden == FcTrue;
    }
    FcPatternDestroy(match);
    return out;
  }

  int fromFixed(FT_Pos value) { return static_cast<int>((value + 32) >> 6); }

} // namespace

TerminalGlyphAtlas::TerminalGlyphAtlas() : m_library(std::make_unique<Library>()) {
  FT_Init_FreeType(&m_library->ft);
}

TerminalGlyphAtlas::~TerminalGlyphAtlas() = default;

bool TerminalGlyphAtlas::load(const std::string& family, float pixelSize, float lineHeight) {
  if (m_library->ft == nullptr) {
    return false;
  }
  m_family = family;
  m_pixelSize = pixelSize;
  m_faces.clear();
  std::fill(std::begin(m_primary), std::end(m_primary), nullptr);
  reset();

  FontFace* regular = faceFor(Face::Regular, 0);
  if (regular == nullptr) {
    return false;
  }
  FT_Face face = regular->face;
  m_cellWidth = 1;
  if (FT_Load_Char(face, 'M', FT_LOAD_TARGET_LIGHT) == 0) {
    m_cellWidth = std::max(1, fromFixed(face->glyph->advance.x));
  }
  const int ascender = fromFixed(face->size->metrics.ascender);
  const int descender = fromFixed(face->size->metrics.descender);
  m_cellHeight = std::max(static_cast<int>(std::lround(lineHeight)), ascender - descender);
  m_baseline = (m_cellHeight - (ascender - descender)) / 2 + ascender;
  m_glyphTop = m_baseline - ascender;
  m_glyphBottom = m_baseline - descender;
  const FT_Fixed yScale = face->size->metrics.y_scale;
  m_strokeThickness = std::max(1, fromFixed(FT_MulFix(face->underline_thickness, yScale)));
  m_underlinePosition =
      std::min(m_cellHeight - m_strokeThickness, m_baseline + std::max(1, fromFixed(-FT_MulFix(face->underline_position, yScale))));
  return true;
}

TerminalGlyphAtlas::FontFace* TerminalGlyphAtlas::openFace(const std::string& file, int index, bool embolden, bool oblique) {
  const std::string key = std::format("{}#{}#{}{}", file, index, embolden ? 'b' : '-', oblique ? 'i' : '-');
  if (auto it = m_faces.find(key); it != m_faces.end()) {
    return it->second.get();
  }
  auto font = std::make_unique<FontFace>();
  const bool opened = FT_New_Face(m_library->ft, file.c_str(), index, &font->face) == 0;
  bool sized = false;
  if (opened && FT_IS_SCALABLE(font->face)) {
    sized = FT_Set_Pixel_Sizes(font->face, 0, static_cast<FT_UInt>(std::lround(m_pixelSize))) == 0;
  } else if (opened && FT_HAS_COLOR(font->face) && font->face->num_fixed_sizes > 0) {
    // A bitmap emoji font: the smallest strike at least the cell height, else the largest; glyph() scales it down.
    int best = 0;
    for (int i = 1; i < font->face->num_fixed_sizes; ++i) {
      const int h = font->face->available_sizes[i].height;
      const int bestH = font->face->available_sizes[best].height;
      if ((bestH < m_cellHeight && h > bestH) || (h >= m_cellHeight && h < bestH)) {
        best = i;
      }
    }
    sized = FT_Select_Size(font->face, best) == 0;
    font->color = sized;
  }
  if (!sized) {
    font.reset();
  } else {
    font->embolden = embolden;
    // fontconfig reports a synthetic slant for a roman file; the face's own flag tells the truth.
    font->oblique = oblique && (font->face->style_flags & FT_STYLE_FLAG_ITALIC) == 0;
  }
  FontFace* raw = font.get();
  m_faces.emplace(key, std::move(font));
  return raw;
}

TerminalGlyphAtlas::FontFace* TerminalGlyphAtlas::faceFor(Face face, char32_t needs, bool color) {
  const bool bold = face == Face::Bold || face == Face::BoldItalic;
  const bool italic = face == Face::Italic || face == Face::BoldItalic;
  if (needs == 0 && m_primary[static_cast<int>(face)] != nullptr) {
    return m_primary[static_cast<int>(face)];
  }
  const auto match = matchFont(m_family, bold, italic, needs, color);
  if (!match) {
    return nullptr;
  }
  // A family without the style gets FreeType's synthetic bold or slant.
  FontFace* font = openFace(match->file, match->index, bold && match->embolden, italic);
  if (needs == 0) {
    m_primary[static_cast<int>(face)] = font;
  }
  return font;
}

const TerminalGlyphAtlas::Glyph* TerminalGlyphAtlas::glyph(char32_t codepoint, Face face, float scale) {
  const auto units = static_cast<std::uint16_t>(std::clamp(std::lround(scale * kScaleUnit), 1L, 16L * kScaleUnit));
  const Key key{codepoint, face, units};
  scale = static_cast<float>(units) / kScaleUnit;
  const auto scaled = [scale](int px) { return static_cast<int>(std::lround(static_cast<float>(px) * scale)); };
  const int cellWidth = scaled(m_cellWidth);
  const int cellHeight = scaled(m_cellHeight);
  const int baseline = scaled(m_baseline);
  if (auto it = m_glyphs.find(key); it != m_glyphs.end()) {
    return it->second.width == 0 ? nullptr : &it->second;
  }
  if (m_missing.contains(key)) {
    return nullptr;
  }
  if (std::vector<std::uint8_t> alpha;
      terminal_sprites::draw(codepoint, cellWidth, cellHeight, std::max(1, scaled(m_strokeThickness)), alpha)) {
    return sprite(key, alpha, cellWidth, cellHeight, baseline);
  }

  FontFace* font = faceFor(face, 0);
  FT_UInt index = font != nullptr ? FT_Get_Char_Index(font->face, codepoint) : 0;
  // Pictographs from U+1F000 default to emoji presentation: a colour font first. Other symbols prefer a text face.
  const bool emoji = codepoint >= 0x1F000;
  for (const bool color : {emoji, !emoji}) {
    if (index != 0) {
      break;
    }
    font = faceFor(face, codepoint, color);
    index = font != nullptr ? FT_Get_Char_Index(font->face, codepoint) : 0;
  }
  const FT_Int32 loadFlags = font != nullptr && font->color ? FT_LOAD_COLOR : FT_LOAD_TARGET_LIGHT;
  // Faces stay at the grid size; a scaled glyph sizes its face for this one load.
  const bool resize = font != nullptr && units != kScaleUnit && FT_IS_SCALABLE(font->face);
  if (resize) {
    FT_Set_Pixel_Sizes(font->face, 0, static_cast<FT_UInt>(std::lround(m_pixelSize * scale)));
  }
  const bool loaded = index != 0 && FT_Load_Glyph(font->face, index, loadFlags) == 0;
  if (resize) {
    FT_Set_Pixel_Sizes(font->face, 0, static_cast<FT_UInt>(std::lround(m_pixelSize)));
  }
  if (!loaded) {
    m_missing.insert(key);
    return nullptr;
  }
  FT_GlyphSlot slot = font->face->glyph;
  if (font->embolden && !font->color) {
    FT_GlyphSlot_Embolden(slot);
  }
  if (font->oblique && !font->color) {
    FT_GlyphSlot_Oblique(slot);
  }
  if ((slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT) != 0)
      || (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && slot->bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)) {
    m_missing.insert(key);
    return nullptr;
  }

  Glyph g{};
  const auto& bitmap = slot->bitmap;
  const bool bgra = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
  const int srcW = static_cast<int>(bitmap.width);
  const int srcH = static_cast<int>(bitmap.rows);
  // A colour glyph shrinks to fit two cells; box filtering keeps it smooth.
  const float shrink = bgra && srcW > 0 && srcH > 0
                           ? std::min({1.0F, 2.0F * static_cast<float>(cellWidth) / static_cast<float>(srcW),
                                       static_cast<float>(cellHeight) / static_cast<float>(srcH)})
                           : 1.0F;
  const int w = std::max(bgra ? 1 : 0, static_cast<int>(std::lround(static_cast<float>(srcW) * shrink)));
  const int h = std::max(bgra ? 1 : 0, static_cast<int>(std::lround(static_cast<float>(srcH) * shrink)));
  if (srcW > 0 && srcH > 0) {
    int x = 0;
    int y = 0;
    if (!place(w + kPadding, h + kPadding, x, y)) {
      reset();
      if (!place(w + kPadding, h + kPadding, x, y)) {
        m_missing.insert(key);
        return nullptr;
      }
    }
    for (int row = 0; row < h; ++row) {
      std::uint8_t* dst =
          m_pixels.get() + (static_cast<std::size_t>(y + row) * kWidth + static_cast<std::size_t>(x)) * kBytesPerPixel;
      for (int col = 0; col < w; ++col, dst += kBytesPerPixel) {
        if (!bgra) {
          const std::uint8_t a = bitmap.buffer[static_cast<std::ptrdiff_t>(row) * bitmap.pitch + col];
          dst[0] = dst[1] = dst[2] = dst[3] = a;
          continue;
        }
        // Average the source pixels this destination pixel covers (premultiplied BGRA in, RGBA out).
        const int sx0 = col * srcW / w;
        const int sx1 = std::max(sx0 + 1, (col + 1) * srcW / w);
        const int sy0 = row * srcH / h;
        const int sy1 = std::max(sy0 + 1, (row + 1) * srcH / h);
        std::uint32_t sum[4] = {};
        for (int sy = sy0; sy < sy1; ++sy) {
          const std::uint8_t* src = bitmap.buffer + static_cast<std::ptrdiff_t>(sy) * bitmap.pitch + sx0 * 4;
          for (int sx = sx0; sx < sx1; ++sx, src += 4) {
            sum[0] += src[2];
            sum[1] += src[1];
            sum[2] += src[0];
            sum[3] += src[3];
          }
        }
        const auto n = static_cast<std::uint32_t>((sx1 - sx0) * (sy1 - sy0));
        for (int c = 0; c < 4; ++c) {
          dst[c] = static_cast<std::uint8_t>(sum[c] / n);
        }
      }
    }
    m_dirtyTop = std::min(m_dirtyTop, y);
    m_dirtyBottom = std::max(m_dirtyBottom, y + h);
    g = Glyph{
        .x = static_cast<std::uint16_t>(x),
        .y = static_cast<std::uint16_t>(y),
        .width = static_cast<std::uint16_t>(w),
        .height = static_cast<std::uint16_t>(h),
        .left = static_cast<std::int16_t>(bgra ? (2 * cellWidth - w) / 2 : slot->bitmap_left),
        .top = static_cast<std::int16_t>(bgra ? baseline - (cellHeight - h) / 2 : slot->bitmap_top),
        .color = bgra,
    };
  }
  auto [it, inserted] = m_glyphs.emplace(key, g);
  return it->second.width == 0 ? nullptr : &it->second;
}

// A sprite fills its cell exactly: origin at the cell's top left.
const TerminalGlyphAtlas::Glyph* TerminalGlyphAtlas::sprite(const Key& key, const std::vector<std::uint8_t>& alpha, int width,
                                                           int height, int baseline) {
  int x = 0;
  int y = 0;
  if (!place(width + kPadding, height + kPadding, x, y)) {
    reset();
    if (!place(width + kPadding, height + kPadding, x, y)) {
      m_missing.insert(key);
      return nullptr;
    }
  }
  for (int row = 0; row < height; ++row) {
    std::uint8_t* dst =
        m_pixels.get() + (static_cast<std::size_t>(y + row) * kWidth + static_cast<std::size_t>(x)) * kBytesPerPixel;
    for (int col = 0; col < width; ++col, dst += kBytesPerPixel) {
      dst[0] = dst[1] = dst[2] = dst[3] = alpha[static_cast<std::size_t>(row) * width + col];
    }
  }
  m_dirtyTop = std::min(m_dirtyTop, y);
  m_dirtyBottom = std::max(m_dirtyBottom, y + height);
  const Glyph g{.x = static_cast<std::uint16_t>(x),
                .y = static_cast<std::uint16_t>(y),
                .width = static_cast<std::uint16_t>(width),
                .height = static_cast<std::uint16_t>(height),
                .left = 0,
                .top = static_cast<std::int16_t>(baseline),
                .color = false};
  return &m_glyphs.insert_or_assign(key, g).first->second;
}

std::unique_ptr<std::uint8_t, TerminalGlyphAtlas::FreeDeleter> TerminalGlyphAtlas::allocatePixels(int height) {
  auto* p = static_cast<std::uint8_t*>(
      std::calloc(static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(height), kBytesPerPixel));
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return std::unique_ptr<std::uint8_t, FreeDeleter>(p);
}

bool TerminalGlyphAtlas::place(int width, int height, int& x, int& y) {
  if (width > kWidth) {
    return false;
  }
  if (m_shelfX + width > kWidth) {
    m_shelfY += m_shelfHeight;
    m_shelfX = 0;
    m_shelfHeight = 0;
  }
  if (m_shelfY + height > m_height) {
    int grown = m_height;
    while (grown < m_shelfY + height) {
      grown *= 2;
    }
    if (grown > kMaxHeight) {
      return false;
    }
    auto pixels = allocatePixels(grown);
    std::memcpy(pixels.get(), m_pixels.get(), static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(m_height) * kBytesPerPixel);
    m_pixels = std::move(pixels);
    m_height = grown;
    m_dirtyTop = 0;
    m_dirtyBottom = m_height;
  }
  x = m_shelfX;
  y = m_shelfY;
  m_shelfX += width;
  m_shelfHeight = std::max(m_shelfHeight, height);
  return true;
}

void TerminalGlyphAtlas::reset() {
  m_glyphs.clear();
  m_missing.clear();
  m_height = kInitialHeight;
  m_pixels = allocatePixels(kInitialHeight);
  m_shelfX = 0;
  m_shelfY = 0;
  m_shelfHeight = 0;
  m_dirtyTop = 0;
  m_dirtyBottom = m_height;
  ++m_generation;
}

void TerminalGlyphAtlas::clearDirty() noexcept {
  m_dirtyTop = m_height;
  m_dirtyBottom = 0;
}
