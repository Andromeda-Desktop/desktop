#include "server/shm_residency_barrier.h"

#include "core/log.h"
#include "core/main_loop_worker.h"
#include "wlr.h"

#include <chrono>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace andromeda {

  namespace {

    constexpr Logger kLog("shm-residency");

    // Past this the commit applies anyway: the upload then faults the rest in, as without the barrier.
    constexpr auto kPageInLimit = std::chrono::milliseconds(1000);

    struct SurfaceWatch;

    // One held commit. The worker job gets only `start` and `length`; the buffer lock keeps the mapping alive while
    // it runs.
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
    std::vector<Hold*> inFlight; // released by the worker result, or by the destructor when that result is dropped
    wl_listener newSurface{};
    std::unique_ptr<MainLoopWorker> worker;

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
      inFlight.push_back(held);
      worker->post<bool>([start, length] { return pageIn(start, length); }, [this, held](bool paged) {
        held->timedOut = !paged;
        std::erase(inFlight, held);
        release(held);
      });
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
    m_impl->worker = std::make_unique<MainLoopWorker>(wl_display_get_event_loop(display), "andro-residency");
    m_impl->newSurface.notify = Impl::onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_impl->newSurface);
  }

  ShmResidencyBarrier::~ShmResidencyBarrier() {
    wl_list_remove(&m_impl->newSurface.link);
    m_impl->worker.reset();
    for (Hold* hold : m_impl->inFlight) {
      Impl::release(hold);
    }
  }

} // namespace andromeda
