// term::agent::Host against the real andromeda-agentd with a fake `claude` on PATH: a session opens, a prompt
// runs, a permission request waits for the user, the answer reaches claude, the turn ends; then the agent
// dies and the session fails. Usage: agent_host_test <andromeda-agentd>
#include "agent_host.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <unistd.h>
#include <string>

#include "core/deferred_call.h"

using namespace term::agent;
namespace fs = std::filesystem;

namespace {

  bool pumpUntil(const std::function<bool()>& done, std::chrono::seconds limit = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      pollfd pfd{.fd = DeferredCall::wakeFd(), .events = POLLIN, .revents = 0};
      ::poll(&pfd, 1, 100);
      DeferredCall::drainWakeFd();
      for (auto& fn : DeferredCall::takePending()) {
        fn();
      }
    }
    return true;
  }

  int fail(const char* what) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    return EXIT_FAILURE;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return fail("usage: agent_host_test <andromeda-agentd>");
  }
  const auto root = fs::temp_directory_path() / ("agent-host-test-" + std::to_string(::getpid()));
  fs::create_directories(root / "bin");
  fs::create_directories(root / "project");
  {
    std::ofstream claude(root / "bin" / "claude");
    claude << R"(#!/bin/sh
IFS= read -r initial || exit 10
printf '%s\n' '{"type":"system","subtype":"init","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","slash_commands":["compact"]}'
printf '%s\n' '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"tool-1","name":"Bash","input":{"command":"just package"}}]}}'
printf '%s\n' '{"type":"control_request","request_id":"p1","request":{"subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"tool-1","input":{"command":"just package"}}}'
IFS= read -r answer || exit 12
case "$answer" in *'"behavior":"allow"'*) ;; *) exit 13;; esac
printf '%s\n' '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"tool-1","content":"built"}]}}'
printf '%s\n' '{"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Done."}]}}'
printf '%s\n' '{"type":"result","subtype":"success","session_id":"79ed6f89-b693-4192-8169-4554404b43a2","result":"Done."}'
)";
  }
  fs::permissions(root / "bin" / "claude", fs::perms::owner_all);
  const std::string path = (root / "bin").string() + ":" + std::getenv("PATH");
  ::setenv("PATH", path.c_str(), 1);
  ::setenv("CLAUDE_ACP_STORE_DIR", (root / "store").c_str(), 1);
  ::setenv("ANDROMEDA_AGENTD", argv[1], 1);

  int changes = 0;
  int result = EXIT_SUCCESS;
  {
    Host host(Kind::Claude, [&](std::uint64_t) { ++changes; });
    Session& s = host.open(7, (root / "project").string());
    if (!pumpUntil([&] { return s.phase() == Phase::Idle; })) {
      result = fail("the session did not start");
    } else if (!s.send("package it") || (host.flush(7), false)) {
    } else if (!pumpUntil([&] { return !s.pending().empty(); })) {
      result = fail("no permission request reached the model");
    } else if (s.rowStatus() != RowStatus::NeedsPermission) {
      result = fail("the row does not say needs permission");
    } else if (!s.answer(0) || (host.flush(7), false)) {
    } else if (!pumpUntil([&] { return s.phase() == Phase::Idle; })) {
      result = fail("the turn did not end after the answer");
    } else {
      const Action* action = nullptr;
      const Prose* prose = nullptr;
      for (const auto& e : s.entries()) {
        action = std::get_if<Action>(&e) != nullptr ? std::get_if<Action>(&e) : action;
        prose = std::get_if<Prose>(&e) != nullptr ? std::get_if<Prose>(&e) : prose;
      }
      if (action == nullptr || action->command != "just package" || action->status != ActionStatus::Done ||
          action->output != "built") {
        result = fail("the Run action is not done with its command and output");
      } else if (prose == nullptr || prose->text != "Done.") {
        result = fail("the reply text is missing");
      } else if (s.rowStatus() != RowStatus::Done || s.commands().empty()) {
        result = fail("row status or slash commands wrong after the turn");
      } else {
        ::setenv("ANDROMEDA_AGENTD", "/nonexistent/andromeda-agentd", 1);
        // Kill the agent: the session must fail and say so.
        std::system(("pkill -TERM -P " + std::to_string(::getpid())).c_str());
        if (!pumpUntil([&] { return s.phase() == Phase::Failed; })) {
          result = fail("the session did not fail when the agent died");
        } else if (s.rowStatus() != RowStatus::Failed) {
          result = fail("the row does not say failed");
        }
      }
    }
  }
  if (result == EXIT_SUCCESS) {
    // An agent that answers initialize and then stops reading: the next request must fail the session, not
    // raise SIGPIPE in the terminal.
    const auto deaf = root / "bin" / "deaf-agentd";
    {
      std::ofstream script(deaf);
      script << R"(#!/bin/sh
IFS= read -r line
id=$(printf '%s' "$line" | sed 's/.*"id":\([0-9]*\).*/\1/')
exec 0<&-
printf '{"jsonrpc":"2.0","id":%s,"result":{}}\n' "$id"
sleep 1
)";
    }
    fs::permissions(deaf, fs::perms::owner_all);
    ::setenv("ANDROMEDA_AGENTD", deaf.c_str(), 1);
    Host host(Kind::Claude, [](std::uint64_t) {});
    Session& s = host.open(8, (root / "project").string());
    if (!pumpUntil([&] { return s.phase() == Phase::Failed; })) {
      result = fail("the session did not fail when the agent stopped reading");
    }
  }
  fs::remove_all(root);
  if (result == EXIT_SUCCESS) {
    std::printf("ok (%d changes)\n", changes);
  }
  return result;
}
