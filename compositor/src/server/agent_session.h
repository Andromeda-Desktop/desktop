#pragma once

#include <cstdint>
#include <optional>

namespace andromeda {

  // `andromeda --agent-session <id>`: a headless instance an AI agent drives, beside the user's session
  // (.agent/TASK-agent-seat.md). Its socket is wayland-agent-<id>; it shares no state file with the user's session.
  [[nodiscard]] std::optional<uint32_t> agentSession();
  void setAgentSession(uint32_t id);

} // namespace andromeda
