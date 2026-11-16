#pragma once

#include <optional>

namespace term {

  // `andromeda-terminal theme new|validate|preview|apply ...`, run before any window or bus: the exit code, or nullopt
  // when argv is not a theme command.
  [[nodiscard]] std::optional<int> runThemeCli(int argc, char* argv[]);

} // namespace term
