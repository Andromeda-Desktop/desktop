#include "session_list.h"

#include <algorithm>

namespace term {

  namespace {
    template <class... F> struct Overload : F... {
      using F::operator()...;
    };
  } // namespace

  std::optional<std::uint64_t> SessionList::shown() const noexcept {
    if (m_recent.empty()) {
      return std::nullopt;
    }
    return m_recent.front();
  }

  const SessionInfo* SessionList::find(std::uint64_t id) const noexcept {
    const auto it = std::ranges::find(m_sessions, id, &SessionInfo::id);
    return it == m_sessions.end() ? nullptr : &*it;
  }

  SessionInfo* SessionList::get(std::uint64_t id) noexcept { return const_cast<SessionInfo*>(find(id)); }

  void SessionList::show(std::uint64_t id) {
    std::erase(m_recent, id);
    m_recent.insert(m_recent.begin(), id);
    get(id)->seen = Seen::Seen;
  }

  ApplyResult SessionList::apply(const SessionEvent& event) {
    using namespace session_event;
    const bool ok = std::visit(
        Overload{
            [&](const Open& e) {
              if (find(e.id) != nullptr) {
                return false;
              }
              m_sessions.push_back({.id = e.id, .kind = e.kind, .name = e.name, .cwd = e.cwd});
              show(e.id);
              return true;
            },
            [&](const Close& e) {
              if (find(e.id) == nullptr) {
                return false;
              }
              std::erase_if(m_sessions, [&](const SessionInfo& s) { return s.id == e.id; });
              std::erase(m_recent, e.id);
              if (!m_recent.empty()) {
                show(m_recent.front());
              }
              return true;
            },
            [&](const Show& e) {
              if (find(e.id) == nullptr) {
                return false;
              }
              show(e.id);
              return true;
            },
            [&](const CommandStart& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr || std::holds_alternative<CommandRunning>(s->command)) {
                return false;
              }
              s->command = CommandRunning{.command = e.command, .start = e.at};
              // A new command replaces the finish the user missed.
              s->seen = Seen::Seen;
              return true;
            },
            [&](const CommandEnd& e) {
              SessionInfo* s = get(e.id);
              const auto* running = s == nullptr ? nullptr : std::get_if<CommandRunning>(&s->command);
              if (running == nullptr) {
                return false;
              }
              s->command = CommandFinished{
                  .command = running->command, .exitCode = e.exitCode, .duration = e.at - running->start};
              if (shown() != e.id) {
                s->seen = Seen::Unseen;
              }
              return true;
            },
            [&](const Rename& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr) {
                return false;
              }
              s->name = e.name;
              return true;
            },
            [&](const Cwd& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr) {
                return false;
              }
              s->cwd = e.cwd;
              return true;
            },
            [&](const Kind& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr) {
                return false;
              }
              s->kind = e.kind;
              return true;
            },
        },
        event);
    if (!ok) {
      return ApplyResult::Rejected;
    }
    return std::holds_alternative<Close>(event) && m_sessions.empty() ? ApplyResult::LastClosed
                                                                      : ApplyResult::Applied;
  }

} // namespace term
