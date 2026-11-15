#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace term::agent {

  using Json = nlohmann::json;

  // One mode for the whole session (TASK-terminal step 9 state model). A turn that waits for a permission
  // answer is Working with a non-empty pending list.
  enum class Phase : std::uint8_t { Starting, Idle, Working, Cancelling, Failed };

  enum class ActionStatus : std::uint8_t { Pending, Running, Done, Failed, Rejected, Cancelled };

  struct Turn {
    std::string text;
  };
  struct Prose {
    std::string text;
    bool streaming = true;
  };
  struct Note {
    std::string text;
  };
  struct DiffHunk {
    std::string path;
    std::optional<std::string> oldText; // empty: a new file
    std::string newText;
  };
  struct Action {
    std::string id;
    std::string kind; // ACP tool kind: read, edit, execute, search, ...
    std::string title;
    std::string command; // execute: rawInput.command
    std::vector<std::string> paths;
    std::vector<DiffHunk> diffs;
    std::string output; // tail of the tool output
    ActionStatus status = ActionStatus::Running;
  };
  struct Option {
    std::string id;
    std::string name;
    std::string kind; // allow_once, allow_always, reject_once, reject_always
  };
  struct Request {
    Json rpcId;
    std::string toolCallId;
    std::vector<Option> options;
    std::optional<std::string> answer; // option id, or "cancelled"
  };
  enum class StopKind : std::uint8_t { MaxTokens, MaxTurns, Refusal, Cancelled, Failed };
  struct Stop {
    StopKind kind;
    std::string message;
  };
  using Entry = std::variant<Turn, Prose, Note, Action, Request, Stop>;

  struct PlanEntry {
    std::string content;
    std::string status; // pending, in_progress, completed
  };

  // What the sidebar row says about the session.
  enum class RowStatus : std::uint8_t { Starting, Ready, Working, NeedsPermission, Done, Stopped, Failed };

  // The agent session model: ACP input and user actions in, transcript and outgoing JSON-RPC out. No IO;
  // the client drains outbox() into the agent's stdin.
  class Session {
  public:
    explicit Session(std::string cwd);

    // Agent side.
    void started(std::string sessionId);
    void update(const Json& params);           // session/update notification params
    void permission(Json rpcId, const Json& params); // session/request_permission request
    void promptDone(const std::string& stopReason);
    void promptFailed(const std::string& message);
    void exited(int status, std::string stderrTail);
    void restarting(); // Failed -> Starting: the client starts a new agent and loads the session
    // A JSON-RPC response; true when it answered a request this model sent (session/prompt).
    bool response(const Json& message);

    // User side. send() returns false when the session cannot take a prompt now (Starting, Failed).
    bool send(std::string text);
    void cancel();
    // Answers the oldest open request with its n-th option (0-based); false when there is none.
    bool answer(std::size_t option);

    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] RowStatus rowStatus() const noexcept;
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return m_entries; }
    [[nodiscard]] const std::vector<PlanEntry>& plan() const noexcept { return m_plan; }
    [[nodiscard]] const std::vector<std::string>& commands() const noexcept { return m_commands; }
    [[nodiscard]] const std::vector<std::size_t>& pending() const noexcept { return m_pending; }
    [[nodiscard]] const std::vector<std::string>& queued() const noexcept { return m_queued; }
    [[nodiscard]] const std::string& mode() const noexcept { return m_mode; }
    [[nodiscard]] std::uint64_t contextUsed() const noexcept { return m_used; }
    [[nodiscard]] std::uint64_t contextSize() const noexcept { return m_size; }
    [[nodiscard]] const std::string& cwd() const noexcept { return m_cwd; }
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }

    // Outgoing JSON-RPC messages since the last call, oldest first.
    std::vector<Json> takeOutbox();

  private:
    bool transition(Phase next);
    void startPrompt(std::string text);
    void endTurn(std::optional<Stop> stop);
    void closeRequests(const char* answer);
    void respond(Request& request, const std::string& optionId);
    Action* findAction(const std::string& id);
    void applyTool(Action& action, const Json& fields);

    std::string m_cwd;
    std::string m_sessionId;
    std::string m_title;
    Phase m_phase = Phase::Starting;
    std::optional<StopKind> m_lastStop;
    std::vector<Entry> m_entries;
    std::vector<std::size_t> m_pending; // indices of unanswered Request entries, oldest first
    std::vector<std::string> m_queued;
    std::vector<PlanEntry> m_plan;
    std::vector<std::string> m_commands;
    std::string m_mode;
    std::uint64_t m_used = 0;
    std::uint64_t m_size = 0;
    std::int64_t m_nextId = 1;
    std::optional<std::int64_t> m_promptId;
    std::vector<Json> m_outbox;
  };

} // namespace term::agent
