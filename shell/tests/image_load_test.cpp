// Model test for Image::load: random load, rewrite and pump steps over files that never decode.
// Invariants: a load returns Pending unless this exact file content already failed (then Failed at once);
// a rewritten file is a new request; every pending load ends with exactly one ready callback and Failed;
// without a cache, load decides at once.
#include "render/backend/render_backend.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "tests/test_check.h"
#include "ui/controls/image.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  class StubRenderer : public Renderer {
  public:
    explicit StubRenderer(AsyncTextureCache* cache) : m_cache(cache) {}
    TextMetrics measureText(std::string_view, float fontSize, const TextFont&, float, int, TextAlign, TextEllipsize, bool)
        override {
      return TextMetrics{.bottom = fontSize};
    }
    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float fontSize) override { return TextMetrics{.bottom = fontSize}; }
    TextureManager& textureManager() override { return *m_textures; }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
    AsyncTextureCache* asyncTextures() noexcept override { return m_cache; }

  private:
    AsyncTextureCache* m_cache;
    std::unique_ptr<TextureManager> m_textures = createDefaultTextureManager();
  };

  constexpr std::size_t kFiles = 3;

  void writeGarbage(const std::filesystem::path& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
  }

  void runSequence(std::uint32_t seed, const std::filesystem::path& dir) {
    std::mt19937 rng(seed);
    AsyncTextureCache cache;
    StubRenderer renderer(&cache);
    std::array<std::string, kFiles> paths;
    std::array<std::size_t, kFiles> version{};
    std::set<std::pair<std::size_t, std::size_t>> failed; // (file, version) whose decode has ended
    std::array<std::unique_ptr<Image>, kFiles> images;
    std::array<int, kFiles> callbacks{};
    std::array<bool, kFiles> pending{};
    std::array<std::size_t, kFiles> pendingVersion{};
    std::size_t nextSize = 1;
    for (std::size_t i = 0; i < kFiles; ++i) {
      paths[i] = (dir / ("f" + std::to_string(i) + ".png")).string();
      writeGarbage(paths[i], nextSize++);
      images[i] = std::make_unique<Image>();
      images[i]->setAsyncReadyCallback([&callbacks, i] { ++callbacks[i]; });
    }
    for (int step = 0; step < 40; ++step) {
      const std::size_t i = rng() % kFiles;
      switch (rng() % 3) {
      case 0: {
        const ImageLoad got = images[i]->load(renderer, paths[i], {.targetSize = 16});
        const bool knownFailed = failed.contains({i, version[i]});
        TEST_CHECK(got == (knownFailed ? ImageLoad::Failed : ImageLoad::Pending));
        TEST_CHECK(images[i]->loadState() == got);
        pending[i] = !knownFailed;
        pendingVersion[i] = version[i];
        break;
      }
      case 1:
        writeGarbage(paths[i], nextSize++);
        ++version[i];
        break;
      default: {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const auto allDone = [&] {
          for (const auto& image : images) {
            if (image->loadState() == ImageLoad::Pending) {
              return false;
            }
          }
          return true;
        };
        while (!allDone()) {
          TEST_CHECK(std::chrono::steady_clock::now() < deadline);
          std::vector<pollfd> fds;
          const std::size_t start = cache.addPollFds(fds);
          (void)::poll(fds.data(), fds.size(), 50);
          cache.dispatch(fds, start);
        }
        for (std::size_t k = 0; k < kFiles; ++k) {
          TEST_CHECK(callbacks[k] == (pending[k] ? 1 : 0));
          callbacks[k] = 0;
          if (pending[k]) {
            TEST_CHECK(images[k]->loadState() == ImageLoad::Failed);
            failed.insert({k, pendingVersion[k]});
          }
          pending[k] = false;
        }
        break;
      }
      }
    }
    for (auto& image : images) {
      image->clear(renderer);
    }
  }

} // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("image_load_test." + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    runSequence(seed, dir);
  }
  StubRenderer noCache(nullptr);
  Image image;
  TEST_CHECK(image.load(noCache, (dir / "f0.png").string()) == ImageLoad::Failed);
  std::filesystem::remove_all(dir);
  return 0;
}
