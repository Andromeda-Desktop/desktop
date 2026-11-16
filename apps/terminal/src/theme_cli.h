#pragma once

#include <optional>
#include <string>

namespace term {

  // `andromeda-terminal theme new|validate|preview|apply ...`, run before any window or bus: the exit code, or nullopt
  // when argv is not a theme command.
  [[nodiscard]] std::optional<int> runThemeCli(int argc, char* argv[]);
  // The first message of a "Create with Claude" agent session: how to make, check and apply a theme with this CLI.
  [[nodiscard]] std::string themeAgentPrompt();

} // namespace term
