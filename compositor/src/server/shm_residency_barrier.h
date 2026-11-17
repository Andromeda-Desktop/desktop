#pragma once

#include <memory>

struct wl_display;
struct wlr_compositor;

namespace andromeda {

  // The upload of a shm buffer reads the client's memory on the main thread. When the client is swapped out (hidden
  // for a while under memory pressure), each page then faults in from swap and the whole desktop waits. This holds
  // such a commit while a worker thread pages the buffer in, so only that surface waits; resident buffers (the normal
  // case) cost one mincore call.
  class ShmResidencyBarrier {
  public:
    ShmResidencyBarrier(wl_display* display, wlr_compositor* compositor);
    ~ShmResidencyBarrier();

    ShmResidencyBarrier(const ShmResidencyBarrier&) = delete;
    ShmResidencyBarrier& operator=(const ShmResidencyBarrier&) = delete;

    struct Impl;

  private:
    std::unique_ptr<Impl> m_impl;
  };

} // namespace andromeda
