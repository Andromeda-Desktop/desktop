#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace term::agent {

  // The coding agents a session can run. Each speaks ACP on stdio, directly or through an adapter.
  enum class Kind : std::uint8_t { Claude, Copilot, Gemini, Codex };

  struct KindInfo {
    Kind kind;
    std::string_view id;   // stable: stored with restored sessions
    std::string_view name; // shown in the menu, the row and the view
  };

  [[nodiscard]] std::span<const KindInfo> kinds();
  [[nodiscard]] const KindInfo& info(Kind kind);
  // The ACP server command line. argv[0] is an absolute path or a name looked up on PATH.
  [[nodiscard]] std::vector<std::string> command(Kind kind);
  // argv[0] of command(kind) resolves to an executable file.
  [[nodiscard]] bool installed(Kind kind);

} // namespace term::agent
