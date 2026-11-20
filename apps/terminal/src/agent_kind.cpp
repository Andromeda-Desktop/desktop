#include "agent_kind.h"

#include "agent_host.h"
#include "core/sys/sys.h"

#include <array>
#include <cstdlib>
#include <unistd.h>

namespace term::agent {

  namespace {

    constexpr std::array<KindInfo, 4> kKinds{{
        {Kind::Claude, "claude", "Claude"},
        {Kind::Copilot, "copilot", "Copilot"},
        {Kind::Gemini, "gemini", "Gemini"},
        {Kind::Codex, "codex", "Codex"},
    }};

  } // namespace

  std::span<const KindInfo> kinds() { return kKinds; }

  const KindInfo& info(Kind kind) { return kKinds[static_cast<std::size_t>(kind)]; }

  std::vector<std::string> command(Kind kind) {
    switch (kind) {
    case Kind::Claude: return {Host::program()}; // andromeda-agentd wraps the claude CLI
    case Kind::Copilot: return {"copilot", "--acp"};
    case Kind::Gemini: return {"gemini", "--experimental-acp"};
    case Kind::Codex: return {"codex-acp"}; // Zed's ACP adapter for the codex CLI
    }
    return {};
  }

  std::string resolve(Kind kind) {
    const std::string program = command(kind).front();
    if (program.find('/') != std::string::npos) {
      return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    }
    const std::string path = sys::userPath();
    for (std::string_view rest = path; !rest.empty();) {
      const std::size_t colon = rest.find(':');
      const std::string dir(rest.substr(0, colon));
      rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
      if (!dir.empty() && ::access((dir + "/" + program).c_str(), X_OK) == 0) {
        return dir + "/" + program;
      }
    }
    return {};
  }

  bool installed(Kind kind) { return !resolve(kind).empty(); }

} // namespace term::agent
