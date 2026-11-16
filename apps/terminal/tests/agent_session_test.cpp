// Model test for term::agent::Session: random agent and user event sequences; the step 9 invariants hold
// after every event (.agent/TASK-terminal.md, "Step 9 plan").
#include "agent_session.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

using namespace term::agent;

namespace {

  int g_failures = 0;

  void check(bool ok, const char* what, unsigned seed, int step) {
    if (!ok) {
      std::fprintf(stderr, "FAIL seed %u step %d: %s\n", seed, step, what);
      ++g_failures;
    }
  }

  struct Harness {
    Session session{"/p"};
    std::mt19937 rng;
    std::int64_t promptInFlight = -1; // id of the session/prompt the agent has not answered
    std::int64_t nextRpc = 100;
    std::set<std::int64_t> openRpc;     // permission requests the agent sent and got no response to
    std::map<std::int64_t, int> responses;
    int userAnswers = 0;
    int selectedResponses = 0;
    bool alive = true;

    int pick(int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); }

    void drain(unsigned seed, int step) {
      for (const auto& message : session.takeOutbox()) {
        const auto method = message.value("method", std::string{});
        if (method == "session/prompt") {
          check(promptInFlight < 0, "a second prompt while one is in flight", seed, step);
          promptInFlight = message["id"].get<std::int64_t>();
        } else if (method.empty() && message.contains("result")) {
          const auto id = message["id"].get<std::int64_t>();
          check(openRpc.erase(id) == 1, "a response to no open permission request", seed, step);
          check(++responses[id] == 1, "two responses to one permission request", seed, step);
          if (message["result"]["outcome"].value("outcome", "") == "selected") {
            ++selectedResponses;
          }
        }
      }
    }

    void agentEvent(unsigned seed, int step) {
      const std::string tool = "t" + std::to_string(pick(4));
      switch (pick(10)) {
      case 0:
        session.update({{"update", {{"sessionUpdate", "agent_message_chunk"}, {"content", {{"type", "text"}, {"text", "x"}}}}}});
        break;
      case 1:
        session.update({{"update", {{"sessionUpdate", "tool_call"}, {"toolCallId", tool}, {"kind", "execute"}, {"status", "in_progress"}}}});
        break;
      case 2: {
        static const char* statuses[] = {"completed", "failed", "in_progress", "pending"};
        session.update({{"update", {{"sessionUpdate", "tool_call_update"}, {"toolCallId", tool}, {"status", statuses[pick(4)]}}}});
        break;
      }
      case 3:
      case 4: {
        const auto rpc = nextRpc++;
        openRpc.insert(rpc);
        session.permission(rpc, {{"toolCall", {{"toolCallId", tool}, {"kind", "execute"}}},
                                 {"options", Json::array({{{"optionId", "allow_once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
                                                          {{"optionId", "reject_once"}, {"name", "Reject"}, {"kind", "reject_once"}}})}});
        break;
      }
      case 5:
      case 6:
        if (promptInFlight >= 0) {
          static const char* reasons[] = {"end_turn", "refusal", "max_tokens", "cancelled"};
          const bool error = pick(5) == 0;
          Json message = {{"jsonrpc", "2.0"}, {"id", promptInFlight}};
          const bool claudeExit = error && pick(2) == 0;
          if (error) {
            message["error"] = {{"code", -32603}, {"message", "boom"}};
            if (claudeExit) {
              message["error"]["data"] = {{"exitCode", 1}, {"stderr", "boom"}};
            }
          } else {
            message["result"] = {{"stopReason", reasons[pick(4)]}};
          }
          promptInFlight = -1;
          session.response(message);
          if (claudeExit) {
            check(session.phase() == Phase::Failed && session.pending().empty(),
                  "a claude exit fails the session and closes its requests", seed, step);
            alive = false;
            openRpc.clear();
          }
        }
        break;
      case 7:
        if (pick(6) == 0) {
          session.exited(1, "stderr");
          alive = false;
          promptInFlight = -1;
          openRpc.clear();
        }
        break;
      case 8:
        session.update({{"update", {{"sessionUpdate", "plan"}, {"entries", Json::array({{{"content", "a"}, {"status", "pending"}}})}}}});
        break;
      case 9:
        session.setCompacting(pick(2) == 0);
        break;
      }
    }

    void userEvent(unsigned seed, int step) {
      switch (pick(5)) {
      case 0:
      case 1: {
        const auto phase = session.phase();
        const bool sent = session.send("do it");
        check(sent, "a reply is always taken (run, queued, or it restarts the agent)", seed, step);
        if (phase == Phase::Failed) {
          check(session.phase() == Phase::Starting, "a reply after a failure restarts the agent", seed, step);
          alive = true;
        }
        break;
      }
      case 2:
        session.cancel();
        break;
      case 3: {
        const bool hadPending = !session.pending().empty();
        const bool answered = session.answer(static_cast<std::size_t>(pick(3)));
        check(!answered || hadPending, "an answer without an open request", seed, step);
        if (answered) {
          ++userAnswers;
        }
        break;
      }
      case 4:
        if (session.phase() == Phase::Starting) {
          session.started("s1");
        } else if (session.phase() == Phase::Failed) {
          session.restarting();
          alive = true;
        }
        break;
      }
    }

    void invariants(unsigned seed, int step) {
      std::size_t unanswered = 0;
      std::set<std::string> actionIds;
      for (std::size_t i = 0; i < session.entries().size(); ++i) {
        const auto& entry = session.entries()[i];
        if (const auto* request = std::get_if<Request>(&entry); request != nullptr && !request->answer) {
          ++unanswered;
          check(std::ranges::find(session.pending(), i) != session.pending().end(), "an open request is not in the pending list", seed, step);
        }
        if (const auto* action = std::get_if<Action>(&entry)) {
          check(actionIds.insert(action->id).second, "two actions with one id", seed, step);
          const bool final = action->status != ActionStatus::Pending && action->status != ActionStatus::Running;
          check(!final || action->ended >= action->started, "a finished action has no end time", seed, step);
        }
      }
      check(unanswered == session.pending().size(), "pending list and open requests differ", seed, step);
      check(session.phase() == Phase::Working || session.pending().empty(), "an open request outside a running turn", seed, step);
      check((session.rowStatus() == RowStatus::NeedsPermission) == (session.phase() == Phase::Working && !session.pending().empty()),
            "the row says needs permission iff a request is open", seed, step);
      check(selectedResponses <= userAnswers, "a request approved or rejected without a user answer", seed, step);
      if (alive) {
        // Every request the agent sent is open in the model or got exactly one response.
        check(openRpc.size() == session.pending().size(), "a request lost its response", seed, step);
      }
      check(session.phase() != Phase::Idle || promptInFlight < 0, "idle while a prompt is in flight", seed, step);
      check(!session.compacting() || session.phase() == Phase::Working || session.phase() == Phase::Cancelling,
            "compacting outside a running turn", seed, step);
    }
  };

} // namespace

int main() {
  for (unsigned seed = 1; seed <= 3000; ++seed) {
    Harness h;
    h.rng.seed(seed);
    for (int step = 0; step < 80; ++step) {
      if (h.pick(2) == 0 && h.alive) {
        h.agentEvent(seed, step);
      } else {
        h.userEvent(seed, step);
      }
      h.drain(seed, step);
      h.invariants(seed, step);
      if (g_failures > 20) {
        return EXIT_FAILURE;
      }
    }
  }
  std::printf("%s\n", g_failures == 0 ? "ok" : "failed");
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
