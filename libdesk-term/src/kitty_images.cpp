#include "kitty_images.h"

#include "pty_session.h"
#include "render/core/image_decoder.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/node.h"
#include "render/scene/terminal_grid_node.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <tuple>

namespace term {

  namespace {

    bool decodePng(void* /*userdata*/, const GhosttyAllocator* allocator, const std::uint8_t* data, std::size_t len,
                   GhosttySysImage* out) {
      auto image = decodePngImage(data, len);
      if (!image) {
        return false;
      }
      std::uint8_t* pixels = ghostty_alloc(allocator, image->pixels.size());
      if (pixels == nullptr) {
        return false;
      }
      std::memcpy(pixels, image->pixels.data(), image->pixels.size());
      *out = {.width = static_cast<std::uint32_t>(image->width),
              .height = static_cast<std::uint32_t>(image->height),
              .data = pixels,
              .data_len = image->pixels.size()};
      return true;
    }

    // Kitty pixels in any stored format, as RGBA8.
    std::vector<std::uint8_t> toRgba(const std::uint8_t* src, std::size_t len, std::uint32_t width,
                                     std::uint32_t height, GhosttyKittyImageFormat format) {
      const std::size_t count = static_cast<std::size_t>(width) * height;
      const std::size_t bpp = format == GHOSTTY_KITTY_IMAGE_FORMAT_RGBA        ? 4
                              : format == GHOSTTY_KITTY_IMAGE_FORMAT_RGB       ? 3
                              : format == GHOSTTY_KITTY_IMAGE_FORMAT_GRAY_ALPHA ? 2
                                                                               : 1;
      if (len < count * bpp) {
        return {};
      }
      std::vector<std::uint8_t> rgba(count * 4);
      for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = src + i * bpp;
        std::uint8_t* d = rgba.data() + i * 4;
        if (bpp >= 3) {
          d[0] = p[0];
          d[1] = p[1];
          d[2] = p[2];
          d[3] = bpp == 4 ? p[3] : 255;
        } else {
          d[0] = d[1] = d[2] = p[0];
          d[3] = bpp == 2 ? p[1] : 255;
        }
      }
      return rgba;
    }

    template <typename T> T imageData(GhosttyRenderStateImageIterator it, GhosttyRenderStateImageData data) {
      T value{};
      ghostty_render_state_image_get(it, data, &value);
      return value;
    }

  } // namespace

  void KittyImages::installPngDecoder() {
    ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, reinterpret_cast<const void*>(&decodePng));
  }

  KittyImages::KittyImages(TerminalGridNode& grid) {
    static_assert(GHOSTTY_IMAGE_LAYER_BELOW_BACKGROUND == 0 && GHOSTTY_IMAGE_LAYER_BELOW_TEXT == 1
                  && GHOSTTY_IMAGE_LAYER_ABOVE_TEXT == 2);
    constexpr std::array<int, 3> kZ{TerminalGridNode::kBelowBackgroundZ, TerminalGridNode::kBelowTextZ, 0};
    for (std::size_t i = 0; i < m_layers.size(); ++i) {
      auto layer = std::make_unique<Node>();
      layer->setZIndex(kZ[i]);
      m_layers[i] = layer.get();
      grid.addChild(std::move(layer));
    }
  }

  void KittyImages::setOrigin(float x, float y) {
    for (Node* layer : m_layers) {
      layer->setPosition(x, y);
    }
  }

  KittyImages::~KittyImages() {
    freeAll();
    ghostty_render_state_image_iterator_free(m_iterator);
  }

  void KittyImages::freeAll() {
    if (m_textures != nullptr) {
      for (auto& [id, texture] : m_cache) {
        m_textures->unload(texture.handle);
      }
    }
    m_cache.clear();
  }

  bool KittyImages::sync(GhosttyRenderState state, PtySession& session,
                         const std::function<TextureManager*()>& texturesFn, float cellWidth, float cellHeight,
                         float scale) {
    struct Raw {
      std::uint32_t image;
      std::uint64_t generation;
      std::int32_t layer;
      std::int32_t z;
      float x, y, width, height;
      std::uint32_t sx, sy, sw, sh;
    };
    std::vector<Raw> raw;
    if ((m_iterator != nullptr || ghostty_render_state_image_iterator_new(nullptr, &m_iterator) == GHOSTTY_SUCCESS)
        && ghostty_render_state_get(state, GHOSTTY_RENDER_STATE_DATA_IMAGE_ITERATOR, &m_iterator) == GHOSTTY_SUCCESS) {
      using D = GhosttyRenderStateImageData;
      while (ghostty_render_state_image_iterator_next(m_iterator)) {
        if (imageData<GhosttyImageProtocol>(m_iterator, D::GHOSTTY_RENDER_STATE_IMAGE_DATA_PROTOCOL)
            != GHOSTTY_IMAGE_PROTOCOL_KITTY) {
          continue;
        }
        const auto px = [&](D d) { return static_cast<float>(imageData<std::uint32_t>(m_iterator, d)) / scale; };
        raw.push_back({
            .image = imageData<std::uint32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_ID),
            .generation = imageData<std::uint64_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_IMAGE_GENERATION),
            .layer = static_cast<std::int32_t>(
                imageData<GhosttyImageLayer>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_LAYER)),
            .z = imageData<std::int32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_Z),
            .x = static_cast<float>(imageData<std::int32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_COL))
                     * cellWidth
                 + px(GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_X),
            .y = static_cast<float>(imageData<std::int32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_VIEWPORT_ROW))
                     * cellHeight
                 + px(GHOSTTY_RENDER_STATE_IMAGE_DATA_CELL_OFFSET_Y),
            .width = px(GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_WIDTH),
            .height = px(GHOSTTY_RENDER_STATE_IMAGE_DATA_PIXEL_HEIGHT),
            .sx = imageData<std::uint32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_X),
            .sy = imageData<std::uint32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_Y),
            .sw = imageData<std::uint32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_WIDTH),
            .sh = imageData<std::uint32_t>(m_iterator, GHOSTTY_RENDER_STATE_IMAGE_DATA_SOURCE_HEIGHT),
        });
      }
    }
    // With no image and no texture the steps below touch no texture: they skip the GL context.
    TextureManager* textures = nullptr;
    if (!raw.empty() || !m_cache.empty()) {
      textures = texturesFn ? texturesFn() : nullptr;
      if (textures == nullptr) {
        return raw.empty();
      }
      m_textures = textures;
    }

    // A texture lives while a visible placement shows its image; one that scrolls back in uploads again.
    std::erase_if(m_cache, [&](auto& entry) {
      if (std::ranges::any_of(raw, [&](const Raw& r) { return r.image == entry.first; })) {
        return false;
      }
      textures->unload(entry.second.handle);
      return true;
    });
    for (const Raw& r : raw) {
      Texture& texture = m_cache[r.image];
      if (texture.generation == r.generation && texture.handle.valid()) {
        continue;
      }
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      const std::vector<std::uint8_t> rgba = session.withTerminal([&](GhosttyTerminal t) {
        GhosttyKittyGraphics graphics = nullptr;
        if (t == nullptr || ghostty_terminal_get(t, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics) != GHOSTTY_SUCCESS) {
          return std::vector<std::uint8_t>{};
        }
        GhosttyKittyGraphicsImage image = ghostty_kitty_graphics_image(graphics, r.image);
        GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
        GhosttyString pixels{};
        if (image == nullptr || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_WIDTH, &width) != GHOSTTY_SUCCESS
            || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_HEIGHT, &height) != GHOSTTY_SUCCESS
            || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_FORMAT, &format) != GHOSTTY_SUCCESS
            || ghostty_kitty_graphics_image_get(image, GHOSTTY_KITTY_GRAPHICS_IMAGE_DATA_PIXELS, &pixels) != GHOSTTY_SUCCESS) {
          return std::vector<std::uint8_t>{};
        }
        return toRgba(pixels.ptr, pixels.len, width, height, format);
      });
      if (rgba.empty()) {
        continue; // pixels still arriving, or the image left the store since the render state was captured
      }
      // Nearest at 1:1 (icat and most programs draw at the source size), so pixels stay exact.
      const bool exact = std::lround(r.width * scale) == static_cast<long>(r.sw)
                         && std::lround(r.height * scale) == static_cast<long>(r.sh);
      if (textures->replace(texture.handle, rgba.data(), static_cast<int>(width), static_cast<int>(height),
                            TextureDataFormat::Rgba, exact ? TextureFilter::Nearest : TextureFilter::Linear)) {
        texture.generation = r.generation;
      }
    }

    std::ranges::stable_sort(raw, {}, [](const Raw& r) { return std::tuple(r.layer, r.z); });
    std::vector<Placement> placements;
    for (const Raw& r : raw) {
      const auto it = m_cache.find(r.image);
      if (it == m_cache.end() || !it->second.handle.valid() || r.sw == 0 || r.sh == 0) {
        continue;
      }
      const TextureHandle& h = it->second.handle;
      const auto w = static_cast<float>(h.width);
      const auto hh = static_cast<float>(h.height);
      placements.push_back({.texture = h.id,
                            .width = h.width,
                            .height = h.height,
                            .rect = {r.x, r.y, r.width, r.height},
                            .uv = {static_cast<float>(r.sx) / w, static_cast<float>(r.sy) / hh,
                                   static_cast<float>(r.sw) / w, static_cast<float>(r.sh) / hh},
                            .layer = static_cast<std::size_t>(r.layer)});
    }
    if (placements == m_drawn) {
      return true;
    }
    for (Node* layer : m_layers) {
      while (!layer->children().empty()) {
        layer->removeChild(layer->children().back().get());
      }
    }
    for (const Placement& p : placements) {
      auto node = std::make_unique<ImageNode>();
      node->setTextureId(p.texture);
      node->setTextureSize(p.width, p.height);
      node->setUvRect(p.uv);
      node->setFitMode(ImageFitMode::Stretch);
      node->setPosition(p.rect[0], p.rect[1]);
      node->setFrameSize(p.rect[2], p.rect[3]);
      m_layers[p.layer]->addChild(std::move(node));
    }
    m_drawn = std::move(placements);
    return true;
  }

} // namespace term
