#include "server/agent_session.h"

namespace andromeda {

  namespace {
    std::optional<uint32_t> g_agentSession;
  } // namespace

  std::optional<uint32_t> agentSession() { return g_agentSession; }

  void setAgentSession(uint32_t id) { g_agentSession = id; }

} // namespace andromeda
