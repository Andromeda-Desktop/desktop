#pragma once

#include "render/core/texture_handle.h"

#include <array>
#include <cstdint>
#include <functional>
#include <ghostty/vt.h>
#include <unordered_map>
#include <vector>

class Node;
class TerminalGridNode;
class TextureManager;

namespace term {

  class PtySession;

  // Draws the kitty graphics placements of a render state as image nodes on three layer nodes of the grid: below the
  // cell backgrounds, below the text, above the text. The layers sit at the grid origin: placements are in cells from
  // the top-left viewport cell.
  class KittyImages {
  public:
    // Process-wide; call once before a terminal parses output. Without it, PNG transmissions are refused.
    static void installPngDecoder();

    explicit KittyImages(TerminalGridNode& grid);
    void setOrigin(float x, float y);
    ~KittyImages();
    KittyImages(const KittyImages&) = delete;
    KittyImages& operator=(const KittyImages&) = delete;

    // After each render state update; scale is device px per logical px (the cell size the terminal got is in device
    // px). textures is asked only when an image shows or a texture is held. False when placements wait for GL
    // (textures returns null): the caller asks for another frame.
    bool sync(GhosttyRenderState state, PtySession& session, const std::function<TextureManager*()>& textures,
              float cellWidth, float cellHeight, float scale);

  private:
    struct Texture {
      std::uint64_t generation = 0;
      TextureHandle handle;
    };
    struct Placement {
      TextureId texture{};
      int width = 0; // texture px
      int height = 0;
      std::array<float, 4> rect{}; // x, y, width, height in logical px from the layer
      std::array<float, 4> uv{};
      std::size_t layer = 0; // GhosttyImageLayer
      bool operator==(const Placement&) const = default;
    };

    void freeAll();

    std::array<Node*, 3> m_layers{}; // by GhosttyImageLayer
    GhosttyRenderStateImageIterator m_iterator = nullptr;
    TextureManager* m_textures = nullptr;
    std::unordered_map<std::uint32_t, Texture> m_cache; // image id -> texture of that image generation
    std::vector<Placement> m_drawn;
  };

} // namespace term
