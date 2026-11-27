#include "render/galaxy/galaxy_model.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/galaxy_node.h"
#include "render/scene/node.h"
#include "tests/test_check.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 1064;  // the Link Home hero at 1280 x 800
constexpr std::uint32_t kHeight = 416;

// GALAXY_PPM_DIR=<dir> writes each render as a PPM for a visual check.
void dump(const std::vector<std::uint8_t>& rgba, const char* name) {
  const char* dir = std::getenv("GALAXY_PPM_DIR");
  if (dir == nullptr) return;
  const std::string path = std::string(dir) + "/" + name + ".ppm";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  std::print(f, "P6\n{} {}\n255\n", kWidth, kHeight);
  for (std::size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
  std::fclose(f);
}

const std::uint8_t* at(const std::vector<std::uint8_t>& rgba, float x, float y) {
  const auto ix = static_cast<std::uint32_t>(x);
  const auto iy = static_cast<std::uint32_t>(y);
  return &rgba[(iy * kWidth + ix) * 4];
}

int luma(const std::uint8_t* p) { return (p[0] * 3 + p[1] * 6 + p[2]) / 10; }

double ms(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

}  // namespace

int main() {
  GlSharedContext shared;
  shared.initializeSurfaceless();
  RenderContext context;
  context.initialize(shared);
  RenderTarget target;
  target.createOffscreen(context);
  target.setContentScale(1.0F);
  target.setLogicalSize(kWidth, kHeight);
  target.resize(kWidth, kHeight);

  const auto built = std::chrono::steady_clock::now();
  auto points = std::make_shared<const GalaxyPoints>(buildGalaxy(GalaxyShape{}));
  std::println("model: {:.1f} ms, {} light, {} dust points", ms(std::chrono::steady_clock::now() - built),
              points->light.size() / GalaxyPoints::kStride, points->dust.size() / GalaxyPoints::kStride);

  Node root;
  root.setSize(kWidth, kHeight);
  auto owned = std::make_unique<GalaxyNode>();
  GalaxyNode* galaxy = owned.get();
  galaxy->setSize(kWidth, kHeight);
  galaxy->setPoints(points);
  root.addChild(std::move(owned));
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);

  const auto render = [&](const char* name) {
    const auto start = std::chrono::steady_clock::now();
    TEST_CHECK(context.renderScene(target, &root));
    TEST_CHECK(context.readPixels(target, pixels));
    std::println("{}: {:.1f} ms", name, ms(std::chrono::steady_clock::now() - start));
    dump(pixels, name);
  };

  // Dark: a bright warm core on a black sky.
  render("dark");
  const auto core = galaxy->project({0, 0, 0});
  TEST_CHECK(core.has_value());
  const std::uint8_t* c = at(pixels, (*core)[0], (*core)[1]);
  TEST_CHECK(luma(c) > 150);
  TEST_CHECK(c[0] > c[2]);
  TEST_CHECK(luma(at(pixels, 4, kHeight - 4)) < 40);

  // Life turns the cached galaxy about its core: the core stays, an arm point moves to where project() says.
  const std::array<float, 3> arm = {0.6F, 0.0F, 0.0F};
  const auto before = galaxy->project(arm);
  galaxy->setLife({.angle = std::numbers::pi_v<float> / 2, .glow = 1.1F});
  render("dark-turned");
  const auto after = galaxy->project(arm);
  TEST_CHECK(before && after);
  TEST_CHECK(std::abs((*before)[0] - (*after)[0]) + std::abs((*before)[1] - (*after)[1]) > 20.0F);
  TEST_CHECK(luma(at(pixels, (*core)[0], (*core)[1])) > 150);

  // Light: the Aurora sky, the galaxy inked into it.
  galaxy->setLife({});
  galaxy->setLightSky(GalaxyLightSky{});
  render("light");
  TEST_CHECK(luma(at(pixels, 4, kHeight - 4)) > 200);
  TEST_CHECK(luma(at(pixels, (*core)[0], (*core)[1])) < luma(at(pixels, 4, kHeight - 4)));

  // GALAXY_BENCH=<frames>: wall ms of render + glFinish (an upper bound on GPU time) for life (composite of the cached
  // layers) and drag (a new camera each frame: the full point pass).
  if (const char* bench = std::getenv("GALAXY_BENCH")) {
    const int frames = std::max(std::atoi(bench), 1);
    galaxy->setLightSky(std::nullopt);
    const auto run = [&](const char* name, const auto& step) {
      std::vector<double> times;
      for (int i = 0; i < frames; ++i) {
        step(i);
        const auto start = std::chrono::steady_clock::now();
        TEST_CHECK(context.renderScene(target, &root));
        glFinish();
        times.push_back(ms(std::chrono::steady_clock::now() - start));
      }
      std::ranges::sort(times);
      std::println("bench {}: p50 {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms ({} frames)", name, times[times.size() / 2],
                   times[times.size() * 99 / 100], times.back(), frames);
    };
    run("life", [&](int i) { galaxy->setLife({.angle = 0.05F * i / 60.0F, .glow = 1.0F + 0.11F * (i % 240) / 240.0F}); });
    GalaxyCamera camera;
    run("drag", [&](int i) {
      camera.yaw = 0.55F + 0.01F * i;
      galaxy->setCamera(camera);
    });
  }

  target.destroy();
  context.cleanup();
  shared.cleanup();
  return 0;
}
