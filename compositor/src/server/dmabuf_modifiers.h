#pragma once

struct wlr_renderer;

namespace andromeda {
  // Removes from the renderer's dma-buf texture formats every modifier whose allocations break the linux-dmabuf
  // offset + stride * height bound, so no feedback (default or per surface) offers it to clients.
  void dropUnsafeDmabufModifiers(wlr_renderer* renderer);
} // namespace andromeda
