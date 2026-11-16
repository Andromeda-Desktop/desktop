#include "agent_session.h"

#include <algorithm>
#include <utility>

namespace term::agent {

  namespace {

    std::string str(const Json& value, const char* key) {
      const auto it = value.find(key);
      return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
    }

    // Text of an ACP content block (text only; other blocks are not drawn).
    std::string blockText(const Json& block) {
      return block.is_object() && str(block, "type") == "text" ? str(block, "text") : std::string{};
    }

    ActionStatus toStatus(const std::string& acp, ActionStatus current) {
      if (acp == "pending") {
        return ActionStatus::Pending;
      }
      if (acp == "in_progress") {
        return ActionStatus::Running;
      }
      if (acp == "completed") {
        return ActionStatus::Done;
      }
      if (acp == "failed") {
        // A rejected call reports failed; it stays Rejected.
        return current == ActionStatus::Rejected ? ActionStatus::Rejected : ActionStatus::Failed;
      }
      return current;
    }

  } // namespace

  Session::Session(std::string cwd, std::int64_t firstId) : m_cwd(std::move(cwd)), m_nextId(firstId) {}

  bool Session::transition(Phase next) {
    const auto legal = [&] {
      switch (m_phase) {
      case Phase::Starting:
        return next == Phase::Idle || next == Phase::Failed;
      case Phase::Idle:
        return next == Phase::Working || next == Phase::Failed;
      case Phase::Working:
        return next == Phase::Idle || next == Phase::Cancelling || next == Phase::Failed;
      case Phase::Cancelling:
        return next == Phase::Idle || next == Phase::Failed;
      case Phase::Failed:
        return next == Phase::Starting;
      }
      return false;
    }();
    if (legal) {
      m_phase = next;
    }
    return legal;
  }

  void Session::started(std::string sessionId) {
    if (transition(Phase::Idle)) {
      m_sessionId = std::move(sessionId);
      if (!m_queued.empty()) {
        auto next = std::move(m_queued.front());
        m_queued.erase(m_queued.begin());
        startPrompt(std::move(next));
      }
    }
  }

  void Session::restarting() {
    if (transition(Phase::Starting)) {
      m_promptId.reset();
    }
  }

  bool Session::send(std::string text) {
    if (m_phase == Phase::Starting || m_phase == Phase::Failed || text.empty()) {
      return false;
    }
    if (m_phase == Phase::Idle) {
      startPrompt(std::move(text));
    } else {
      m_queued.push_back(std::move(text));
    }
    return true;
  }

  void Session::startPrompt(std::string text) {
    if (!transition(Phase::Working)) {
      return;
    }
    if (m_title.empty()) {
      m_title = text.substr(0, 120);
    }
    m_lastStop.reset();
    const auto id = m_nextId++;
    m_promptId = id;
    m_outbox.push_back({{"jsonrpc", "2.0"},
                        {"id", id},
                        {"method", "session/prompt"},
                        {"params", {{"sessionId", m_sessionId}, {"prompt", Json::array({{{"type", "text"}, {"text", text}}})}}}});
    m_entries.emplace_back(Turn{.text = std::move(text), .started = std::chrono::system_clock::now()});
  }

  void Session::cancel() {
    if (!transition(Phase::Cancelling)) {
      return;
    }
    m_queued.clear();
    closeRequests("cancelled");
    m_outbox.push_back({{"jsonrpc", "2.0"}, {"method", "session/cancel"}, {"params", {{"sessionId", m_sessionId}}}});
  }

  bool Session::answer(std::size_t option) {
    if (m_pending.empty()) {
      return false;
    }
    auto& request = std::get<Request>(m_entries[m_pending.front()]);
    if (option >= request.options.size()) {
      return false;
    }
    const auto chosen = request.options[option];
    respond(request, chosen.id);
    m_pending.erase(m_pending.begin());
    if (auto* action = findAction(request.toolCallId)) {
      const bool allow = chosen.kind.starts_with("allow");
      action->status = allow ? ActionStatus::Running : ActionStatus::Rejected;
    }
    return true;
  }

  void Session::respond(Request& request, const std::string& optionId) {
    request.answer = optionId;
    Json outcome = optionId == "cancelled" ? Json{{"outcome", "cancelled"}}
                                           : Json{{"outcome", "selected"}, {"optionId", optionId}};
    m_outbox.push_back({{"jsonrpc", "2.0"}, {"id", request.rpcId}, {"result", {{"outcome", outcome}}}});
  }

  void Session::closeRequests(const char* answer) {
    for (const auto index : m_pending) {
      auto& request = std::get<Request>(m_entries[index]);
      respond(request, answer);
      if (auto* action = findAction(request.toolCallId)) {
        action->status = ActionStatus::Cancelled;
      }
    }
    m_pending.clear();
  }

  Action* Session::findAction(const std::string& id) {
    for (auto& entry : m_entries) {
      if (auto* action = std::get_if<Action>(&entry); action != nullptr && action->id == id) {
        return action;
      }
    }
    return nullptr;
  }

  void Session::applyTool(Action& action, const Json& fields) {
    if (const auto title = str(fields, "title"); !title.empty()) {
      action.title = title;
    }
    if (const auto kind = str(fields, "kind"); !kind.empty()) {
      action.kind = kind;
    }
    if (const auto status = str(fields, "status"); !status.empty()) {
      action.status = toStatus(status, action.status);
    }
    if (const auto it = fields.find("rawInput"); it != fields.end() && it->is_object()) {
      action.command = str(*it, "command");
    }
    if (const auto it = fields.find("locations"); it != fields.end() && it->is_array()) {
      action.paths.clear();
      for (const auto& location : *it) {
        if (auto path = str(location, "path"); !path.empty()) {
          action.paths.push_back(std::move(path));
        }
      }
    }
    if (const auto it = fields.find("content"); it != fields.end() && it->is_array()) {
      action.diffs.clear();
      for (const auto& content : *it) {
        if (str(content, "type") == "diff") {
          DiffHunk hunk{.path = str(content, "path"), .oldText = std::nullopt, .newText = str(content, "newText")};
          if (const auto old = content.find("oldText"); old != content.end() && old->is_string()) {
            hunk.oldText = old->get<std::string>();
          }
          action.diffs.push_back(std::move(hunk));
        }
      }
    }
    if (const auto it = fields.find("rawOutput"); it != fields.end() && it->is_object()) {
      action.output = str(*it, "output");
    }
  }

  void Session::update(const Json& params) {
    const auto it = params.find("update");
    if (it == params.end() || !it->is_object() || m_phase == Phase::Starting || m_phase == Phase::Failed) {
      return;
    }
    const Json& u = *it;
    const auto kind = str(u, "sessionUpdate");
    const bool inTurn = m_phase == Phase::Working || m_phase == Phase::Cancelling;
    if (kind == "agent_message_chunk" && inTurn) {
      const auto text = blockText(u.value("content", Json::object()));
      if (auto* prose = m_entries.empty() ? nullptr : std::get_if<Prose>(&m_entries.back());
          prose != nullptr && prose->streaming) {
        prose->text += text;
      } else if (!text.empty()) {
        m_entries.emplace_back(Prose{text});
      }
    } else if (kind == "agent_thought_chunk" && inTurn) {
      const auto text = blockText(u.value("content", Json::object()));
      if (auto* note = m_entries.empty() ? nullptr : std::get_if<Note>(&m_entries.back())) {
        note->text += text;
      } else if (!text.empty()) {
        m_entries.emplace_back(Note{text});
      }
    } else if (kind == "tool_call" || kind == "tool_call_update") {
      const auto id = str(u, "toolCallId");
      if (id.empty()) {
        return;
      }
      Action* action = findAction(id);
      if (action == nullptr) {
        if (!inTurn) {
          return;
        }
        m_entries.emplace_back(Action{.id = id});
        action = &std::get<Action>(m_entries.back());
      }
      applyTool(*action, u);
    } else if (kind == "plan") {
      m_plan.clear();
      for (const auto& entry : u.value("entries", Json::array())) {
        m_plan.push_back({str(entry, "content"), str(entry, "status")});
      }
    } else if (kind == "available_commands_update") {
      m_commands.clear();
      for (const auto& command : u.value("availableCommands", Json::array())) {
        if (auto name = str(command, "name"); !name.empty()) {
          m_commands.push_back(std::move(name));
        }
      }
    } else if (kind == "current_mode_update") {
      m_mode = str(u, "currentModeId");
    } else if (kind == "usage_update") {
      m_used = u.value("used", std::uint64_t{0});
      m_size = u.value("size", std::uint64_t{0});
    } else if (kind == "config_option_update") {
      configure(u.value("configOptions", Json::array()));
    } else if (kind == "session_info_update") {
      if (auto title = str(u, "title"); !title.empty()) {
        m_title = std::move(title);
      }
    }
  }

  void Session::configure(const Json& options) {
    if (!options.is_array()) {
      return;
    }
    for (const Json& option : options) {
      if (!option.is_object()) {
        continue;
      }
      if (str(option, "id") == "mode") {
        m_mode = str(option, "currentValue");
        m_modes.clear();
        for (const Json& choice : option.value("options", Json::array())) {
          if (choice.is_object() && !str(choice, "value").empty()) {
            m_modes.push_back({str(choice, "value"), str(choice, "name"), str(choice, "description")});
          }
        }
        continue;
      }
      if (str(option, "category") != "model" && str(option, "id") != "model") {
        continue;
      }
      const std::string current = str(option, "currentValue");
      m_model = current;
      for (const Json& choice : option.value("options", Json::array())) {
        if (choice.is_object() && str(choice, "value") == current && !str(choice, "name").empty()) {
          m_model = str(choice, "name");
        }
      }
    }
  }

  const Session::Mode* Session::currentMode() const {
    const auto it = std::ranges::find(m_modes, m_mode, &Mode::value);
    return it != m_modes.end() ? &*it : nullptr;
  }

  void Session::cycleMode() {
    if (m_modes.size() < 2 || m_sessionId.empty()) {
      return;
    }
    const auto it = std::ranges::find(m_modes, m_mode, &Mode::value);
    const Mode& next = it == m_modes.end() || it + 1 == m_modes.end() ? m_modes.front() : *(it + 1);
    m_outbox.push_back({{"jsonrpc", "2.0"},
                        {"id", m_nextId++},
                        {"method", "session/set_config_option"},
                        {"params", {{"sessionId", m_sessionId}, {"configId", "mode"}, {"value", next.value}}}});
  }

  void Session::permission(Json rpcId, const Json& params) {
    const auto call = params.value("toolCall", Json::object());
    const auto id = str(call, "toolCallId");
    Request request{.rpcId = std::move(rpcId), .toolCallId = id, .options = {}, .answer = std::nullopt};
    for (const auto& option : params.value("options", Json::array())) {
      request.options.push_back({str(option, "optionId"), str(option, "name"), str(option, "kind")});
    }
    if (m_phase != Phase::Working || request.options.empty()) {
      // Outside a running turn nothing can show the request: it is cancelled, never approved.
      respond(request, "cancelled");
      return;
    }
    Action* action = findAction(id);
    if (action == nullptr && !id.empty()) {
      m_entries.emplace_back(Action{.id = id});
      action = &std::get<Action>(m_entries.back());
    }
    if (action != nullptr) {
      applyTool(*action, call);
      action->status = ActionStatus::Pending;
    }
    m_entries.emplace_back(std::move(request));
    m_pending.push_back(m_entries.size() - 1);
  }

  bool Session::response(const Json& message) {
    const auto it = message.find("id");
    if (!m_promptId || it == message.end() || !it->is_number_integer() || it->get<std::int64_t>() != *m_promptId) {
      return false;
    }
    if (const auto error = message.find("error"); error != message.end()) {
      promptFailed(error->is_object() ? str(*error, "message") : std::string{"The agent reported an error."});
    } else {
      promptDone(str(message.value("result", Json::object()), "stopReason"));
    }
    return true;
  }

  void Session::promptDone(const std::string& stopReason) {
    std::optional<Stop> stop;
    if (stopReason == "max_tokens") {
      stop = Stop{StopKind::MaxTokens, {}};
    } else if (stopReason == "max_turn_requests") {
      stop = Stop{StopKind::MaxTurns, {}};
    } else if (stopReason == "refusal") {
      stop = Stop{StopKind::Refusal, {}};
    } else if (stopReason == "cancelled" || m_phase == Phase::Cancelling) {
      stop = Stop{StopKind::Cancelled, {}};
    }
    endTurn(std::move(stop));
  }

  void Session::promptFailed(const std::string& message) { endTurn(Stop{StopKind::Failed, message}); }

  void Session::endTurnTime() {
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
      if (auto* turn = std::get_if<Turn>(&*it)) {
        if (!turn->ended) {
          turn->ended = std::chrono::system_clock::now();
        }
        return;
      }
    }
  }

  void Session::endTurn(std::optional<Stop> stop) {
    if (m_phase != Phase::Working && m_phase != Phase::Cancelling) {
      return;
    }
    closeRequests("cancelled");
    m_promptId.reset();
    endTurnTime();
    for (auto& entry : m_entries) {
      if (auto* prose = std::get_if<Prose>(&entry)) {
        prose->streaming = false;
      } else if (auto* action = std::get_if<Action>(&entry);
                 action != nullptr && (action->status == ActionStatus::Running || action->status == ActionStatus::Pending)) {
        action->status = stop && stop->kind == StopKind::Cancelled ? ActionStatus::Cancelled : ActionStatus::Done;
      }
    }
    if (stop) {
      m_lastStop = stop->kind;
      m_entries.emplace_back(std::move(*stop));
    } else {
      m_lastStop.reset();
    }
    transition(Phase::Idle);
    if (!m_queued.empty()) {
      auto next = std::move(m_queued.front());
      m_queued.erase(m_queued.begin());
      startPrompt(std::move(next));
    }
  }

  void Session::exited(int status, std::string stderrTail) {
    if (m_phase == Phase::Failed) {
      return;
    }
    const bool inTurn = m_phase == Phase::Working || m_phase == Phase::Cancelling;
    // The agent is gone: open requests get no reply, they only close.
    for (const auto index : m_pending) {
      std::get<Request>(m_entries[index]).answer = "cancelled";
    }
    m_pending.clear();
    m_queued.clear();
    m_promptId.reset();
    endTurnTime();
    for (auto& entry : m_entries) {
      if (auto* prose = std::get_if<Prose>(&entry)) {
        prose->streaming = false;
      } else if (auto* action = std::get_if<Action>(&entry);
                 action != nullptr && (action->status == ActionStatus::Running || action->status == ActionStatus::Pending)) {
        action->status = ActionStatus::Cancelled;
      }
    }
    if (inTurn || m_phase == Phase::Starting || status != 0) {
      m_entries.emplace_back(Stop{StopKind::Failed, "The agent exited with status " + std::to_string(status) + ".\n" + stderrTail});
    }
    m_lastStop = StopKind::Failed;
    m_outbox.clear();
    transition(Phase::Failed);
  }

  RowStatus Session::rowStatus() const noexcept {
    switch (m_phase) {
    case Phase::Starting:
      return RowStatus::Starting;
    case Phase::Failed:
      return RowStatus::Failed;
    case Phase::Working:
      return m_pending.empty() ? RowStatus::Working : RowStatus::NeedsPermission;
    case Phase::Cancelling:
      return RowStatus::Working;
    case Phase::Idle:
      break;
    }
    if (!m_lastStop) {
      return std::ranges::any_of(m_entries, [](const Entry& e) { return std::holds_alternative<Turn>(e); })
                 ? RowStatus::Done
                 : RowStatus::Ready;
    }
    return *m_lastStop == StopKind::Failed ? RowStatus::Failed : RowStatus::Stopped;
  }

  std::vector<Json> Session::takeOutbox() { return std::exchange(m_outbox, {}); }

} // namespace term::agent
