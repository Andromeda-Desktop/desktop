#pragma once

#include "render/core/texture_manager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace linkapp {

  // Decodes a Link1 OpenMirror stream on a worker thread: units of a big-endian u32 length, u64 time (us) and a
  // flags byte, then H.264 (VA-API when the GPU has it, else software), scaled to the target size: on the GPU into a
  // BGRA dmabuf when VA-API can, else to RGBA bytes.
  // Callbacks run on the UI thread; a frame is posted only when the UI took the last one, so a slow UI drops frames
  // instead of queueing them.
  class MirrorDecoder {
  public:
    struct Frame {
      std::vector<std::uint8_t> rgba; // empty for a dmabuf frame
      int width = 0;
      int height = 0;
      TextureManager::DmabufImage dmabuf; // fd -1 for an RGBA frame
      std::shared_ptr<void> hold;         // keeps the dmabuf's VA surface out of the pool until dropped
    };

    // Owns `fd`. `ended` gets an empty string when the phone stopped, else the reason.
    MirrorDecoder(int fd, std::function<void(Frame)> frame, std::function<void(std::string)> ended);
    ~MirrorDecoder();
    MirrorDecoder(const MirrorDecoder&) = delete;
    MirrorDecoder& operator=(const MirrorDecoder&) = delete;

    // The size frames are scaled to, in pixels; 0 keeps the video size.
    void setTarget(int width, int height);
    // The UI could not import a dmabuf frame: send RGBA frames from now on.
    void refuseDmabuf() { m_rgbaOnly = true; }
    // Decoded frames in the last full second.
    [[nodiscard]] int fps() const { return m_fps.load(); }

  private:
    void run();

    int m_fd;
    std::function<void(Frame)> m_frame;
    std::function<void(std::string)> m_ended;
    std::atomic<int> m_targetW{0};
    std::atomic<int> m_targetH{0};
    std::atomic<int> m_fps{0};
    std::atomic<bool> m_posted{false}; // a frame waits for the UI thread
    std::atomic<bool> m_rgbaOnly{false};
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::thread m_thread;
  };

} // namespace linkapp
