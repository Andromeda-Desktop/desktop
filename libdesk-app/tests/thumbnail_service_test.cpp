#include "render/core/texture_manager.h"
#include "render/core/thumbnail_service.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <print>
#include <string>
#include <vector>

namespace {
  int g_failures = 0;

  void expect(bool ok, const std::string& what) {
    if (!ok) {
      std::println(stderr, "thumbnail_service_test: {}", what);
      ++g_failures;
    }
  }

  // Counts uploads; there is no GPU in the test.
  class FakeTextures final : public TextureManager {
  public:
    int uploads = 0;
    TextureHandle loadFromRgba(const std::uint8_t*, int width, int height, bool) override {
      ++uploads;
      return {.id = TextureId(static_cast<std::uint32_t>(uploads)), .width = width, .height = height};
    }
    TextureHandle loadFromRaw(const std::uint8_t*, std::size_t, int, int, int, PixmapFormat, bool) override { return {}; }
    TextureHandle loadFromPixels(const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return {};
    }
    TextureHandle createEmpty(int, int, TextureDataFormat, TextureFilter) override { return {}; }
    bool replace(TextureHandle&, const std::uint8_t*, int, int, TextureDataFormat, TextureFilter, bool) override {
      return false;
    }
    bool updateSubImage(TextureHandle&, const std::uint8_t*, int, int, int, int, TextureDataFormat) override {
      return false;
    }
    TextureHandle importDmabuf(const DmabufImage&) override { return {}; }
    int submittedWorkFence() override { return -1; }
    void unload(TextureHandle& handle) override { handle = {}; }
    void cleanup() override {}
    void abandonGpuResources() noexcept override {}
    void probeExtensions() override {}
  };

  // Holds "running" until opened and records the order of the work.
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    bool entered = false;
    std::vector<std::string> calls;
  };
} // namespace

int main() {
  Gate gate;
  FakeTextures textures; // outlives the service, which unloads its textures on destruction
  ThumbnailService service(
      [&gate](const std::string& path, int) {
        std::unique_lock lock(gate.mutex);
        gate.calls.push_back(path);
        if (path == "running") {
          gate.entered = true;
          gate.cv.notify_all();
          gate.cv.wait_for(lock, std::chrono::seconds(3), [&] { return gate.open; });
        }
        return std::optional<thumbnail_pipeline::Pixels>(thumbnail_pipeline::Pixels{.rgba = {1, 2, 3, 255}, .width = 1, .height = 1});
      },
      1
  );

  (void)service.acquire("running");
  {
    std::unique_lock lock(gate.mutex);
    expect(gate.cv.wait_for(lock, std::chrono::seconds(3), [&] { return gate.entered; }), "the worker takes the first request");
  }
  (void)service.acquire("queued");
  (void)service.acquire("p1", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Prefetch);
  (void)service.acquire("p2", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Prefetch);
  (void)service.acquire("p2", ThumbnailService::kDefaultTargetPx, ThumbnailService::Priority::Visible);
  service.release("queued");
  service.release("running");
  bool p1Ready = false;
  auto ready = service.subscribeReady("p1", [&](const std::string&, TextureHandle) { p1Ready = true; });
  (void)service.acquire("after");
  {
    std::scoped_lock lock(gate.mutex);
    gate.open = true;
  }
  gate.cv.notify_all();

  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!p1Ready && std::chrono::steady_clock::now() < until) {
    std::vector<pollfd> fds;
    const std::size_t start = service.addPollFds(fds);
    ::poll(fds.data(), fds.size(), 100);
    service.dispatch(fds, start);
    (void)service.uploadPending(textures);
  }
  expect(p1Ready, "the last request arrives");

  std::vector<std::string> calls;
  {
    std::scoped_lock lock(gate.mutex);
    calls = gate.calls;
  }
  expect(calls == std::vector<std::string>{"running", "p2", "after", "p1"},
         "a released queued request never runs, a Visible acquire lifts a Prefetch one");
  expect(textures.uploads == 3, "the released running request is not uploaded: " + std::to_string(textures.uploads));
  expect(service.peek("running").id.value() == 0, "the released running request has no texture");

  // A reload while the decode runs (the file changed under it, as a gallery rotate does) ends with the new pixels.
  Gate held;
  int version = 1; // the width the next decode reads; guarded by held.mutex
  FakeTextures reloadTextures;
  ThumbnailService reloading(
      [&held, &version](const std::string&, int) {
        std::unique_lock lock(held.mutex);
        const int width = version;
        held.entered = true;
        held.cv.notify_all();
        held.cv.wait_for(lock, std::chrono::seconds(3), [&] { return held.open; });
        return std::optional<thumbnail_pipeline::Pixels>(
            thumbnail_pipeline::Pixels{.rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * 4, 255),
                                       .width = width, .height = 1});
      },
      1
  );
  (void)reloading.acquire("photo");
  {
    std::unique_lock lock(held.mutex);
    expect(held.cv.wait_for(lock, std::chrono::seconds(3), [&] { return held.entered; }), "the worker takes the photo");
    version = 2;
  }
  reloading.reload("photo");
  {
    std::scoped_lock lock(held.mutex);
    held.open = true;
  }
  held.cv.notify_all();
  const auto reloadUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (reloading.peek("photo").width != 2 && std::chrono::steady_clock::now() < reloadUntil) {
    std::vector<pollfd> fds;
    const std::size_t start = reloading.addPollFds(fds);
    ::poll(fds.data(), fds.size(), 100);
    reloading.dispatch(fds, start);
    (void)reloading.uploadPending(reloadTextures);
  }
  expect(reloading.peek("photo").width == 2,
         "a reload during the decode shows the changed file: width " + std::to_string(reloading.peek("photo").width));
  return g_failures == 0 ? 0 : 1;
}
