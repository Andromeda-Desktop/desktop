#include "workspace_pager.h"

#include <cmath>
#include <cstdio>
#include <random>

using namespace term;

namespace {

int failures = 0;
unsigned seed = 0;
int step = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s (seed %u step %d)\n", __FILE__, __LINE__, #c, seed, step); ++failures; } } while (0)

bool near(float a, float b) { return std::fabs(a - b) < 0.01F; }

void checkInvariants(const WorkspacePager& p) {
  const float max = static_cast<float>(p.count() - 1) * p.width();
  CHECK(p.page() < p.count());
  // The rubber band never lets the drawn page leave the ends by a full page.
  CHECK(p.shown() > -p.width() - 0.01F && p.shown() < max + p.width() + 0.01F);
  if (p.position() >= 0.0F && p.position() <= max) CHECK(near(p.shown(), p.position()));
  if (const auto* rest = std::get_if<WorkspacePager::Rest>(&p.mode())) {
    CHECK(near(p.position(), static_cast<float>(rest->page) * p.width()));
  }
}

} // namespace

int main() {
  for (seed = 1; seed <= 3000; ++seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
    WorkspacePager p;
    p.setWidth(static_cast<float>(200 + pick(100)));
    p.setPages(static_cast<std::size_t>(1 + pick(8)), static_cast<std::size_t>(pick(8)));
    std::uint32_t time = 1000;
    float fingerStart = 0.0F;
    float fingerSum = 0.0F;
    for (step = 0; step < 60; ++step) {
      time += static_cast<std::uint32_t>(1 + pick(30));
      switch (pick(7)) {
        case 0: case 1: { // finger motion, 1:1
          if (!std::holds_alternative<WorkspacePager::Tracking>(p.mode())) {
            fingerStart = p.position();
            fingerSum = 0.0F;
          }
          const float dx = static_cast<float>(pick(161) - 80);
          p.track(dx, time);
          fingerSum += dx;
          CHECK(near(p.position(), fingerStart + fingerSum));
          break;
        }
        case 2: { // release lands on exactly one page
          const bool tracking = std::holds_alternative<WorkspacePager::Tracking>(p.mode());
          const std::size_t target = p.release();
          CHECK(target < p.count());
          if (tracking) CHECK(std::holds_alternative<WorkspacePager::Settling>(p.mode()) && p.page() == target);
          break;
        }
        case 3: p.goTo(static_cast<std::size_t>(pick(10))); break;
        case 4: { // the spring moves toward the target
          const float target = static_cast<float>(p.page()) * p.width();
          p.animate(p.position() + (target - p.position()) * 0.5F);
          break;
        }
        case 5: {
          const bool settling = std::holds_alternative<WorkspacePager::Settling>(p.mode());
          const std::size_t target = p.page();
          p.settled();
          if (settling) CHECK(std::holds_alternative<WorkspacePager::Rest>(p.mode()) && p.page() == target);
          break;
        }
        case 6: p.setPages(static_cast<std::size_t>(1 + pick(8)), static_cast<std::size_t>(pick(8))); break;
      }
      checkInvariants(p);
    }
  }
  // At pane width (the finger tracks the pane cards): a moderate swipe (~2000 px/s) advances one page, a slow push
  // of 88 px stays, and a still release stays.
  seed = 0;
  WorkspacePager p;
  p.setWidth(900.0F);
  p.setPages(4, 1);
  for (std::uint32_t t = 0; t <= 80; t += 8) p.track(16.0F, t);
  CHECK(p.release() == 2);
  p.settled();
  for (std::uint32_t t = 1000; t <= 1080; t += 8) p.track(8.0F, t);
  CHECK(p.release() == 2);
  p.settled();
  p.track(30.0F, 2000);
  p.track(0.0F, 2200);
  CHECK(p.release() == 2);
  if (failures == 0) std::puts("workspace_pager: all passed");
  return failures == 0 ? 0 : 1;
}
