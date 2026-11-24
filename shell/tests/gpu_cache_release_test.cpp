// The one trim entry (RenderContext::unusedCacheRelease) frees an AsyncTextureCache's unused entries and keeps the
// entries in use. Failed decodes stand in for textures: they are resident and unused the same way, without GL.

#include "render/core/async_texture_cache.h"
#include "ui/signal.h"

#include <chrono>
#include <poll.h>
#include <print>
#include <vector>

namespace {
  int gFailures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "gpu_cache_release_test: {}", message);
      ++gFailures;
    }
  }

  bool pumpUntilFailed(AsyncTextureCache& cache, const char* a, const char* b) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(cache.failed(a) && cache.failed(b))) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::vector<pollfd> fds;
      const std::size_t start = cache.addPollFds(fds);
      (void)::poll(fds.data(), fds.size(), 100);
      cache.dispatch(fds, start);
    }
    return true;
  }
} // namespace

int main() {
  constexpr const char* kUnused = "/nonexistent/gpu-cache-release/unused.png";
  constexpr const char* kInUse = "/nonexistent/gpu-cache-release/in-use.png";
  Signal<> release;
  AsyncTextureCache cache;
  cache.releaseUnusedOn(release);

  (void)cache.acquire(kUnused);
  (void)cache.acquire(kInUse);
  expect(pumpUntilFailed(cache, kUnused, kInUse), "decodes did not finish in 5 s");
  cache.release(kUnused);
  expect(cache.failed(kUnused), "a released entry stays resident until a trim");

  release.emit();
  expect(!cache.failed(kUnused), "the release signal did not free the unused entry");
  expect(cache.failed(kInUse), "the release signal freed an entry in use");

  cache.release(kInUse);
  return gFailures == 0 ? 0 : 1;
}
