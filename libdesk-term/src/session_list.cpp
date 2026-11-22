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

  const std::vector<std::uint64_t>* SessionList::splitOf(std::uint64_t id) const noexcept {
    const auto it = std::ranges::find_if(m_splits, [&](const auto& s) { return std::ranges::contains(s, id); });
    return it == m_splits.end() ? nullptr : &*it;
  }

  std::vector<std::uint64_t> SessionList::visible() const {
    const auto id = shown();
    if (!id) {
      return {};
    }
    const auto* split = splitOf(*id);
    return split != nullptr ? *split : std::vector<std::uint64_t>{*id};
  }

  void SessionList::show(std::uint64_t id) {
    std::erase(m_recent, id);
    m_recent.insert(m_recent.begin(), id);
    for (const auto pane : visible()) {
      get(pane)->seen = Seen::Seen;
    }
  }

  void SessionList::removeFromSplit(std::uint64_t id) {
    for (auto& split : m_splits) {
      std::erase(split, id);
    }
    std::erase_if(m_splits, [](const auto& s) { return s.size() < 2; });
  }

  ApplyResult SessionList::apply(const SessionListEvent& event) {
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
              removeFromSplit(e.id);
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
              if (!std::ranges::contains(visible(), e.id)) {
                s->seen = Seen::Unseen;
              }
              return true;
            },
            [&](const Output& e) {
              SessionInfo* s = get(e.id);
              auto* running = s == nullptr ? nullptr : std::get_if<CommandRunning>(&s->command);
              if (running == nullptr || running->output == e.output) {
                return false;
              }
              running->output = e.output;
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
            [&](const Title& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr || s->title == e.title) {
                return false;
              }
              s->title = e.title;
              return true;
            },
            [&](const Kind& e) {
              SessionInfo* s = get(e.id);
              if (s == nullptr) {
                return false;
              }
              s->kind = e.kind;
              s->host = e.host;
              return true;
            },
            [&](const Join& e) {
              if (e.target == e.joining || find(e.target) == nullptr || find(e.joining) == nullptr ||
                  splitOf(e.joining) != nullptr) {
                return false;
              }
              const auto* existing = splitOf(e.target);
              if (existing != nullptr && existing->size() >= kMaxPanes) {
                return false;
              }
              if (existing == nullptr) {
                existing = &m_splits.emplace_back(std::vector<std::uint64_t>{e.target});
              }
              auto& panes = *const_cast<std::vector<std::uint64_t>*>(existing);
              const auto at = std::ranges::find(panes, e.target) + (e.before ? 0 : 1);
              panes.insert(at, e.joining);
              show(e.joining);
              return true;
            },
            [&](const Leave& e) {
              if (splitOf(e.id) == nullptr) {
                return false;
              }
              removeFromSplit(e.id);
              return true;
            },
            [&](const Move& e) {
              const auto from = std::ranges::find(m_sessions, e.id, &SessionInfo::id);
              if (from == m_sessions.end() || e.id == e.before || (e.before != 0 && get(e.before) == nullptr)) {
                return false;
              }
              SessionInfo moved = std::move(*from);
              m_sessions.erase(from);
              const auto to = e.before == 0 ? m_sessions.end() : std::ranges::find(m_sessions, e.before, &SessionInfo::id);
              m_sessions.insert(to, std::move(moved));
              return true;
            },
            [&](const Swap& e) {
              const auto* split = splitOf(e.a);
              if (e.a == e.b || split == nullptr || split != splitOf(e.b)) {
                return false;
              }
              auto& panes = *const_cast<std::vector<std::uint64_t>*>(split);
              std::iter_swap(std::ranges::find(panes, e.a), std::ranges::find(panes, e.b));
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
