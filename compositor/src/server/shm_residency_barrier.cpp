#include "server/shm_residency_barrier.h"

#include "core/log.h"
#include "wlr.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace andromeda {

  namespace {

    constexpr Logger kLog("shm-residency");

    // Past this the commit applies anyway: the upload then faults the rest in, as without the barrier.
    constexpr auto kPageInLimit = std::chrono::milliseconds(1000);

    struct SurfaceWatch;

    // One held commit. The worker reads only `start`, `length` and writes `timedOut`; the buffer lock keeps the
    // mapping alive while it runs.
    struct Hold {
      SurfaceWatch* watch = nullptr; // null once the surface is gone
      wlr_buffer* buffer = nullptr;
      uintptr_t start = 0;
      size_t length = 0;
      uint32_t seq = 0;
      bool timedOut = false;
    };

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      ShmResidencyBarrier::Impl* barrier = nullptr;
      std::vector<Hold*> holds;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    const uintptr_t kPage = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));

    // mincore never faults: it reports what is in RAM (shmem pages in swap or the swap cache included).
    bool resident(uintptr_t start, size_t length, std::vector<unsigned char>& pages) {
      pages.resize((length + kPage - 1) / kPage);
      if (mincore(reinterpret_cast<void*>(start), length, pages.data()) != 0) {
        return true; // not a mapping mincore understands: nothing to page in
      }
      for (const unsigned char page : pages) {
        if ((page & 1) == 0) {
          return false;
        }
      }
      return true;
    }

    // Starts swap-in of every missing run and waits until all pages are in RAM. Reads no client memory, so a client
    // that truncates its file cannot raise SIGBUS here.
    bool pageIn(uintptr_t start, size_t length) {
      std::vector<unsigned char> pages;
      const auto deadline = std::chrono::steady_clock::now() + kPageInLimit;
      while (!resident(start, length, pages)) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        for (size_t i = 0; i < pages.size();) {
          if ((pages[i] & 1) != 0) {
            ++i;
            continue;
          }
          size_t end = i;
          while (end < pages.size() && (pages[end] & 1) == 0) {
            ++end;
          }
          madvise(reinterpret_cast<void*>(start + i * kPage), (end - i) * kPage, MADV_WILLNEED);
          i = end;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return true;
    }

  } // namespace

  struct ShmResidencyBarrier::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Hold*> queued; // guarded by mutex
    std::vector<Hold*> done;  // guarded by mutex
    bool stopping = false;    // guarded by mutex
    int eventFd = -1;
    wl_event_source* doneSource = nullptr;
    wl_listener newSurface{};
    std::thread worker;

    void run() {
      std::unique_lock lock(mutex);
      while (true) {
        wake.wait(lock, [this] { return stopping || !queued.empty(); });
        if (stopping) {
          return;
        }
        Hold* hold = queued.front();
        queued.pop_front();
        lock.unlock();
        const bool paged = pageIn(hold->start, hold->length);
        lock.lock();
        hold->timedOut = !paged;
        done.push_back(hold);
        const uint64_t one = 1;
        (void)!write(eventFd, &one, sizeof(one));
      }
    }

    static void release(Hold* hold) {
      if (hold->timedOut) {
        kLog.warn("a shm buffer was still not in RAM after {} ms; its commit applies anyway", kPageInLimit.count());
      }
      wlr_buffer_unlock(hold->buffer);
      if (SurfaceWatch* watch = hold->watch) {
        std::erase(watch->holds, hold);
        // May apply the cached state, and with it every later one no other lock holds back.
        wlr_surface_unlock_cached(watch->surface, hold->seq);
      }
      delete hold;
    }

    static int onDone(int fd, uint32_t /*mask*/, void* data) {
      auto* self = static_cast<Impl*>(data);
      uint64_t count = 0;
      (void)!read(fd, &count, sizeof(count));
      std::vector<Hold*> finished;
      {
        const std::scoped_lock lock(self->mutex);
        finished.swap(self->done);
      }
      for (Hold* hold : finished) {
        release(hold);
      }
      return 0;
    }

    void hold(SurfaceWatch* watch, wlr_buffer* buffer) {
      void* data = nullptr;
      uint32_t format = 0;
      size_t stride = 0;
      if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        return;
      }
      wlr_buffer_end_data_ptr_access(buffer);
      const auto first = reinterpret_cast<uintptr_t>(data);
      const uintptr_t start = first & ~(kPage - 1);
      const size_t length = first + stride * static_cast<size_t>(buffer->height) - start;
      std::vector<unsigned char> pages;
      if (length == 0 || resident(start, length, pages)) {
        return;
      }
      auto* held = new Hold{.watch = watch, .buffer = wlr_buffer_lock(buffer), .start = start, .length = length};
      held->seq = wlr_surface_lock_pending(watch->surface);
      watch->holds.push_back(held);
      {
        const std::scoped_lock lock(mutex);
        queued.push_back(held);
      }
      wake.notify_one();
    }

    static void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      wlr_surface* surface = watch->surface;
      if ((surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0 || surface->pending.buffer == nullptr) {
        return;
      }
      watch->barrier->hold(watch, surface->pending.buffer);
    }

    static void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      // The worker may still page the buffer in: the hold stays queued, owned by the barrier, and only drops its lock.
      for (Hold* hold : watch->holds) {
        hold->watch = nullptr;
        wlr_surface_unlock_cached(watch->surface, hold->seq);
      }
      wl_list_remove(&watch->clientCommit.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

    static void onNewSurface(wl_listener* listener, void* data) {
      Impl* self;
      self = wl_container_of(listener, self, newSurface);
      auto* watch = new SurfaceWatch();
      watch->surface = static_cast<wlr_surface*>(data);
      watch->barrier = self;
      watch->clientCommit.notify = onClientCommit;
      wl_signal_add(&watch->surface->events.client_commit, &watch->clientCommit);
      watch->destroy.notify = onSurfaceDestroy;
      wl_signal_add(&watch->surface->events.destroy, &watch->destroy);
    }
  };

  ShmResidencyBarrier::ShmResidencyBarrier(wl_display* display, wlr_compositor* compositor) : m_impl(std::make_unique<Impl>()) {
    m_impl->eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    wl_event_loop* loop = wl_display_get_event_loop(display);
    m_impl->doneSource = wl_event_loop_add_fd(loop, m_impl->eventFd, WL_EVENT_READABLE, Impl::onDone, m_impl.get());
    m_impl->worker = std::thread([impl = m_impl.get()] { impl->run(); });
    m_impl->newSurface.notify = Impl::onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_impl->newSurface);
  }

  ShmResidencyBarrier::~ShmResidencyBarrier() {
    wl_list_remove(&m_impl->newSurface.link);
    {
      const std::scoped_lock lock(m_impl->mutex);
      m_impl->stopping = true;
    }
    m_impl->wake.notify_one();
    m_impl->worker.join();
    std::vector<Hold*> left(m_impl->queued.begin(), m_impl->queued.end());
    left.insert(left.end(), m_impl->done.begin(), m_impl->done.end());
    for (Hold* hold : left) {
      Impl::release(hold);
    }
    wl_event_source_remove(m_impl->doneSource);
    close(m_impl->eventFd);
  }

} // namespace andromeda
