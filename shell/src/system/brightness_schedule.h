#pragma once

#include "system/day_night_schedule.h"

#include <chrono>

namespace brightness_schedule {

  inline constexpr std::chrono::minutes kFade{30};

  // The level at `at`: for kFade after each boundary it moves from the previous phase level to the new one.
  [[nodiscard]] float level(const day_night_schedule::Evaluation& at, float day, float night);

  // The wait before the next write: about one 1 % step during a fade (2-60 s), else the next boundary (at most 1 h).
  [[nodiscard]] std::chrono::milliseconds nextDelay(const day_night_schedule::Evaluation& at, float day, float night);

} // namespace brightness_schedule
