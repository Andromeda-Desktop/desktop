#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xdpu {

  struct ThumbnailSource {
    bool monitor = false;
    std::string identifier; // output name or toplevel identifier
  };

  struct Thumbnail {
    std::size_t index = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; // tightly packed, stride width * 3
  };

  // Live thumbnails at up to 2 fps on a private Wayland connection, one capture session per shown source.
  // A source sends a new frame only after its content changes, so static sources cost nothing.
  class Thumbnails {
  public:
    // Runs on the worker thread after a thumbnail is queued: post take() to the UI thread.
    using ReadyCallback = std::function<void()>;

    Thumbnails(std::vector<ThumbnailSource> sources, ReadyCallback onReady);
    ~Thumbnails();
    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    // The visible source indices; sessions of all other sources close.
    void show(std::vector<std::size_t> indices);
    std::vector<Thumbnail> take();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace xdpu
