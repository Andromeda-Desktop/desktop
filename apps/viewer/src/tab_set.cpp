#include "tab_set.h"

#include <algorithm>

namespace viewer {

  namespace {
    template <class... F> struct Overload : F... {
      using F::operator()...;
    };
  } // namespace

  bool TabSet::contains(std::uint64_t id) const { return std::ranges::find(m_tabs, id) != m_tabs.end(); }

  const std::vector<std::uint64_t>* TabSet::splitOf(std::uint64_t id) const {
    const auto it = std::ranges::find_if(m_splits, [&](const auto& s) { return std::ranges::find(s, id) != s.end(); });
    return it != m_splits.end() ? &*it : nullptr;
  }

  std::vector<std::uint64_t> TabSet::visible() const {
    if (!m_focused) {
      return {};
    }
    const auto* split = splitOf(*m_focused);
    return split != nullptr ? *split : std::vector<std::uint64_t>{*m_focused};
  }

  void TabSet::removeFromSplit(std::uint64_t id) {
    for (auto& split : m_splits) {
      std::erase(split, id);
    }
    std::erase_if(m_splits, [](const auto& s) { return s.size() < 2; });
  }

  bool TabSet::apply(const TabEvent& event) {
    using namespace tab_event;
    return std::visit(
        Overload{
            [&](const Open& e) {
              if (contains(e.id)) {
                return false;
              }
              const auto at = m_focused ? std::ranges::find(m_tabs, *m_focused) + 1 : m_tabs.end();
              m_tabs.insert(at, e.id);
              m_focused = e.id;
              return true;
            },
            [&](const Close& e) {
              const auto it = std::ranges::find(m_tabs, e.id);
              if (it == m_tabs.end()) {
                return false;
              }
              if (m_focused == e.id) {
                std::optional<std::uint64_t> next;
                if (const auto* split = splitOf(e.id)) {
                  const auto at = std::ranges::find(*split, e.id);
                  next = at != split->begin() ? *(at - 1) : *(at + 1);
                } else if (m_tabs.size() > 1) {
                  next = it != m_tabs.begin() ? *(it - 1) : *(it + 1);
                }
                m_focused = next;
              }
              m_tabs.erase(it);
              removeFromSplit(e.id);
              return true;
            },
            [&](const Focus& e) {
              if (!contains(e.id)) {
                return false;
              }
              m_focused = e.id;
              return true;
            },
            [&](const Join& e) {
              if (e.target == e.joining || !contains(e.target) || !contains(e.joining) || splitOf(e.joining) != nullptr) {
                return false;
              }
              auto split = std::ranges::find_if(m_splits, [&](const auto& s) { return std::ranges::find(s, e.target) != s.end(); });
              if (split == m_splits.end()) {
                m_splits.push_back({e.target});
                split = m_splits.end() - 1;
              } else if (split->size() >= kMaxPanes) {
                return false;
              }
              split->insert(std::ranges::find(*split, e.target) + (e.before ? 0 : 1), e.joining);
              m_focused = e.joining;
              return true;
            },
            [&](const Leave& e) {
              if (splitOf(e.id) == nullptr) {
                return false;
              }
              removeFromSplit(e.id);
              return true;
            },
        },
        event);
  }

} // namespace viewer
