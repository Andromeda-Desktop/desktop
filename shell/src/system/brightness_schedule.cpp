#include "system/brightness_schedule.h"

#include <algorithm>
#include <cmath>

namespace brightness_schedule {

  namespace {

    using std::chrono::milliseconds;

    constexpr milliseconds kFadeMs = kFade;

  } // namespace

  float level(const day_night_schedule::Evaluation& at, float day, float night) {
    const float from = at.night ? day : night;
    const float to = at.night ? night : day;
    if (at.sinceBoundary >= kFadeMs) {
      return to;
    }
    const float progress =
        static_cast<float>(std::max<milliseconds::rep>(at.sinceBoundary.count(), 0)) / static_cast<float>(kFadeMs.count());
    return from + (to - from) * progress;
  }

  milliseconds nextDelay(const day_night_schedule::Evaluation& at, float day, float night) {
    if (at.sinceBoundary < kFadeMs) {
      const float steps = std::max(std::abs(day - night) * 100.0F, 1.0F);
      const auto step = milliseconds(static_cast<milliseconds::rep>(static_cast<float>(kFadeMs.count()) / steps));
      return std::clamp(step, milliseconds(std::chrono::seconds(2)), milliseconds(std::chrono::seconds(60)));
    }
    return std::clamp(at.untilBoundary, milliseconds(std::chrono::seconds(1)), milliseconds(std::chrono::hours(1)));
  }

} // namespace brightness_schedule
