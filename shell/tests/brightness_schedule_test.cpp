#include "system/brightness_schedule.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <print>
#include <random>

using namespace std::chrono;

int main() {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> unit(0.0F, 1.0F);
  std::uniform_int_distribution<int> minutes(0, 12 * 60);
  int failures = 0;
  auto expect = [&failures](bool condition, const char* message, int seed) {
    if (!condition && failures++ < 10) {
      std::println(stderr, "brightness_schedule_test: {} (case {})", message, seed);
    }
  };

  for (int i = 0; i < 20000; ++i) {
    const float day = unit(rng);
    const float night = unit(rng);
    const day_night_schedule::Evaluation at{
        .night = (rng() & 1U) != 0, .untilBoundary = milliseconds(minutes(rng) * 60000LL),
        .sinceBoundary = milliseconds(minutes(rng) * 60000LL + rng() % 60000)
    };
    const float value = brightness_schedule::level(at, day, night);
    const float phase = at.night ? night : day;
    const float previous = at.night ? day : night;
    expect(value >= std::min(day, night) - 1e-6F && value <= std::max(day, night) + 1e-6F, "level lies between day and night", i);
    if (at.sinceBoundary >= brightness_schedule::kFade) {
      expect(value == phase, "after the fade the level is the phase level", i);
    }
    expect(std::abs(brightness_schedule::level({.night = at.night, .sinceBoundary = 0ms}, day, night) - previous) < 1e-6F,
           "a fade starts at the previous phase level", i);

    const auto delay = brightness_schedule::nextDelay(at, day, night);
    expect(delay >= 1s && delay <= 1h, "the next write is 1 s to 1 h away", i);
    if (at.sinceBoundary < brightness_schedule::kFade) {
      expect(delay >= 2s && delay <= 60s, "a fade steps every 2-60 s", i);
      const auto later = day_night_schedule::Evaluation{.night = at.night, .sinceBoundary = at.sinceBoundary + delay};
      expect(std::abs(brightness_schedule::level(later, day, night) - value) <= 0.0101F, "one fade step moves at most 1 %", i);
    }
  }
  return failures == 0 ? 0 : 1;
}
