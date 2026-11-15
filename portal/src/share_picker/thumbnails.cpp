#include "share_picker/thumbnails.h"

#include "loop/loop.h"
#include "share_picker/live_state.h"
#include "wayland/capture_transform.h"
#include "wayland/shm_format.h"
#include "wayland/wayland.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>

namespace xdpu {
  namespace {
    constexpr int kFrameIntervalMs = 500;
    constexpr double kMaxThumbnailSide = 512.0;

    using Clock = std::chrono::steady_clock;

    // Downsamples straight from SHM and undoes the buffer transform in the same pass: no full-size copy.
    Thumbnail downsample(const void* mapping, uint32_t width, uint32_t height, uint32_t format, uint32_t transform) {
      const double scale = std::min(1.0, kMaxThumbnailSide / std::max(width, height));
      const int sw = std::max(1, static_cast<int>(width * scale));
      const int sh = std::max(1, static_cast<int>(height * scale));
      const bool swapAxes = (transform & 1U) != 0;
      Thumbnail thumbnail{.width = swapAxes ? sh : sw, .height = swapAxes ? sw : sh};
      const auto stride = static_cast<std::size_t>(thumbnail.width) * 3;
      thumbnail.rgb.resize(stride * static_cast<std::size_t>(thumbnail.height));
      const RgbOffsets channel = *rgbOffsets(format);
      const auto* pixels = static_cast<const uint32_t*>(mapping);
      for (int y = 0; y < thumbnail.height; ++y) {
        auto* row = thumbnail.rgb.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < thumbnail.width; ++x) {
          const auto [u, v] = displayToBuffer(x, y, sw, sh, transform);
          const uint32_t pixel =
              pixels[(static_cast<std::size_t>(v) * height / sh) * width + static_cast<std::size_t>(u) * width / sw];
          auto* rgb = row + static_cast<std::size_t>(x) * 3;
          rgb[0] = (pixel >> (8 * channel.r)) & 0xff;
          rgb[1] = (pixel >> (8 * channel.g)) & 0xff;
          rgb[2] = (pixel >> (8 * channel.b)) & 0xff;
        }
      }
      return thumbnail;
    }

    // One shown source; its mode lives in m_state and changes only in apply() (share_picker/live_state.h).
    class Live {
    public:
      Live(Loop& loop, WaylandContext& wayland, std::size_t index, const ThumbnailSource& source,
           std::function<void(Thumbnail)> publish)
          : m_loop(loop), m_wayland(wayland), m_index(index), m_publish(std::move(publish)) {
        auto constraints = [this](const CaptureConstraints& info) { configure(info); };
        m_session = source.monitor ? wayland.createOutputCapture(source.identifier, CaptureCursorMode::Hidden, constraints)
                                   : wayland.createToplevelCapture(source.identifier, CaptureCursorMode::Hidden, constraints);
        if (!m_session) {
          apply(LiveEvent::SessionStopped);
          return;
        }
        m_session->stoppedCb = [this] { apply(LiveEvent::SessionStopped); };
      }

      ~Live() {
        if (m_state == LiveState::Waiting) {
          m_loop.removeTimer(m_timer);
        }
        m_frame.reset();
        m_session.reset();
        releaseBuffer();
      }

      Live(const Live&) = delete;
      Live& operator=(const Live&) = delete;

    private:
      // A frame callback may call apply() again from inside startCapture: the state is stored first.
      void apply(LiveEvent event) {
        const std::optional<LiveStep> step = liveTransition(m_state, event);
        if (!step) {
          std::fprintf(stderr, "share-picker: thumbnail event %d ignored in state %d\n", static_cast<int>(event),
                       static_cast<int>(m_state));
          return;
        }
        if (step->cancelTimer) {
          m_loop.removeTimer(m_timer);
        }
        if (step->dropFrame) {
          m_frame.reset();
        }
        m_state = step->next;
        if (step->startTimer) {
          m_timer = m_loop.addTimer(m_delayMs, [this] { apply(LiveEvent::TimerFired); });
        }
        if (step->startCapture) {
          capture();
        }
      }

      void configure(const CaptureConstraints& info) {
        if (m_state == LiveState::Stopped) {
          return;
        }
        // The buffer is replaced below: no frame may still write into the old one.
        m_frame.reset();
        apply(allocate(info) ? LiveEvent::Configured : LiveEvent::ConfigureFailed);
      }

      bool allocate(const CaptureConstraints& info) {
        const uint32_t format = WaylandContext::preferredShmFormat(info.shmFormats);
        if (!rgbOffsets(format) || info.bufferWidth == 0 || info.bufferHeight == 0 || info.bufferWidth > 16384
            || info.bufferHeight > 16384) {
          return false;
        }
        releaseBuffer();
        const uint32_t stride = info.bufferWidth * 4;
        m_size = static_cast<std::size_t>(stride) * info.bufferHeight;
        m_fd = memfd_create("andromeda-share-thumbnail", MFD_CLOEXEC);
        if (m_fd < 0 || ftruncate(m_fd, static_cast<off_t>(m_size)) < 0) {
          return false;
        }
        m_mapping = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (m_mapping == MAP_FAILED) {
          return false;
        }
        m_buffer = m_wayland.createShmBuffer(info.bufferWidth, info.bufferHeight, format, stride, m_fd, m_size);
        if (m_buffer == nullptr) {
          return false;
        }
        m_width = info.bufferWidth;
        m_height = info.bufferHeight;
        m_format = format;
        return true;
      }

      void capture() {
        m_started = Clock::now();
        m_frame = m_wayland.captureFrame(
            *m_session, m_buffer, /*damageBuffer=*/true,
            [this](CaptureBuffer& buffer, uint64_t, uint32_t) {
              Thumbnail thumbnail = downsample(m_mapping, m_width, m_height, m_format, buffer.transform);
              thumbnail.index = m_index;
              m_publish(std::move(thumbnail));
              const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m_started);
              m_delayMs = std::max(1, kFrameIntervalMs - static_cast<int>(elapsed.count()));
              apply(LiveEvent::FrameReady);
            },
            [this](CaptureFailureReason reason) {
              switch (reason) {
              case CaptureFailureReason::Retry:
                m_delayMs = kFrameIntervalMs;
                apply(LiveEvent::Retry);
                break;
              case CaptureFailureReason::ConstraintsChanged:
                apply(LiveEvent::ConstraintsChanged);
                break;
              case CaptureFailureReason::Stopped:
                apply(LiveEvent::SessionStopped);
                break;
              }
            });
        m_wayland.flush();
      }

      void releaseBuffer() {
        if (m_buffer != nullptr) {
          wl_buffer_destroy(m_buffer);
          m_buffer = nullptr;
        }
        if (m_mapping != MAP_FAILED) {
          munmap(m_mapping, m_size);
          m_mapping = MAP_FAILED;
        }
        if (m_fd >= 0) {
          close(m_fd);
          m_fd = -1;
        }
      }

      Loop& m_loop;
      WaylandContext& m_wayland;
      std::size_t m_index;
      std::function<void(Thumbnail)> m_publish;
      LiveState m_state = LiveState::Configuring;
      std::unique_ptr<WaylandContext::CaptureSession> m_session;
      std::unique_ptr<WaylandContext::CaptureFrame> m_frame;
      wl_buffer* m_buffer = nullptr;
      void* m_mapping = MAP_FAILED;
      std::size_t m_size = 0;
      int m_fd = -1;
      uint32_t m_width = 0;
      uint32_t m_height = 0;
      uint32_t m_format = 0;
      int m_timer = 0;   // valid only in Waiting
      int m_delayMs = kFrameIntervalMs;
      Clock::time_point m_started;
    };
  } // namespace

  struct Thumbnails::Impl {
    const std::vector<ThumbnailSource> sources;
    const ReadyCallback onReady;
    const int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    std::mutex mutex;
    std::set<std::size_t> wanted;
    std::vector<Thumbnail> results;
    std::atomic<bool> stopping = false;
    std::thread worker;

    Impl(std::vector<ThumbnailSource> sources, ReadyCallback onReady)
        : sources(std::move(sources)), onReady(std::move(onReady)), worker([this] { run(); }) {}

    ~Impl() {
      stopping = true;
      signal();
      worker.join();
      if (wake >= 0) {
        close(wake);
      }
    }

    void signal() const {
      const uint64_t one = 1;
      if (wake >= 0) {
        (void)!write(wake, &one, sizeof(one));
      }
    }

    void publish(Thumbnail thumbnail) {
      {
        std::scoped_lock lock(mutex);
        std::erase_if(results, [&](const Thumbnail& old) { return old.index == thumbnail.index; });
        results.push_back(std::move(thumbnail));
      }
      onReady();
    }

    void run() {
      if (wake < 0) {
        return;
      }
      try {
        Loop loop;
        // The Wayland connection waits for the first shown source.
        std::unique_ptr<WaylandContext> wayland;
        std::map<std::size_t, std::unique_ptr<Live>> live;
        auto reconcile = [&] {
          uint64_t count = 0;
          (void)!read(wake, &count, sizeof(count));
          if (stopping) {
            loop.quit();
            return;
          }
          std::set<std::size_t> next;
          {
            std::scoped_lock lock(mutex);
            next = wanted;
          }
          std::erase_if(live, [&](const auto& entry) { return !next.contains(entry.first); });
          if (!next.empty() && !wayland) {
            wayland = std::make_unique<WaylandContext>(loop);
          }
          if (!wayland || !wayland->connected()) {
            return;
          }
          for (const std::size_t index : next) {
            if (!live.contains(index)) {
              live.emplace(index, std::make_unique<Live>(loop, *wayland, index, sources[index],
                                                         [this](Thumbnail thumbnail) { publish(std::move(thumbnail)); }));
            }
          }
          wayland->flush();
        };
        loop.addFd(wake, EPOLLIN, [&](uint32_t) { reconcile(); });
        loop.run();
        live.clear();
      } catch (const std::exception& error) {
        std::fprintf(stderr, "andromeda-share-picker: thumbnails unavailable: %s\n", error.what());
      }
    }
  };

  Thumbnails::Thumbnails(std::vector<ThumbnailSource> sources, ReadyCallback onReady)
      : m_impl(std::make_unique<Impl>(std::move(sources), std::move(onReady))) {}
  Thumbnails::~Thumbnails() = default;

  void Thumbnails::show(std::vector<std::size_t> indices) {
    {
      std::scoped_lock lock(m_impl->mutex);
      m_impl->wanted.clear();
      for (const std::size_t index : indices) {
        if (index < m_impl->sources.size()) {
          m_impl->wanted.insert(index);
        }
      }
    }
    m_impl->signal();
  }

  std::vector<Thumbnail> Thumbnails::take() {
    std::scoped_lock lock(m_impl->mutex);
    std::vector<Thumbnail> results;
    results.swap(m_impl->results);
    return results;
  }

} // namespace xdpu
