#include "shell/notification_center/notification_center_model.h"
#include "tests/test_check.h"

#include <cstdint>
#include <deque>
#include <random>
#include <set>
#include <string>

namespace nc = notification_center;

namespace {

  // Random add / remove / toggle / clear sequences; every reachable state keeps the group invariants.
  void testRandomSequencesKeepInvariants() {
    std::mt19937 rng(7);
    const std::string apps[] = {"Messages", "Mail", "Calendar", "Files"};
    std::deque<NotificationHistoryEntry> history;
    std::set<std::string> openKeys;
    std::uint32_t nextId = 1;
    for (int step = 0; step < 4000; ++step) {
      const int event = static_cast<int>(rng() % 10);
      if (event < 5) {
        NotificationHistoryEntry entry{};
        entry.notification.id = nextId++;
        entry.notification.appName = apps[rng() % 4];
        history.push_back(std::move(entry));
      } else if (event < 7 && !history.empty()) {
        history.erase(history.begin() + static_cast<long>(rng() % history.size()));
      } else if (event < 9 && !history.empty()) {
        const std::string key = nc::groupKeyOf(history[rng() % history.size()].notification);
        if (!openKeys.erase(key)) {
          openKeys.insert(key);
        }
      } else if (rng() % 8 == 0) {
        history.clear();
      }

      const auto groups = nc::buildGroups(history, openKeys);
      std::size_t total = 0;
      std::set<std::string> keys;
      for (std::size_t g = 0; g < groups.size(); ++g) {
        const nc::Group& group = groups[g];
        TEST_CHECK(!group.items.empty());
        TEST_CHECK(keys.insert(group.key).second);
        total += group.items.size();
        for (std::size_t i = 1; i < group.items.size(); ++i) {
          TEST_CHECK(group.items[i - 1]->id > group.items[i]->id);
        }
        if (g > 0) {
          TEST_CHECK(groups[g - 1].items.front()->id > group.items.front()->id);
        }
        const nc::GroupView view = nc::viewOf(group, 3);
        if (group.items.size() == 1) {
          TEST_CHECK(view.cards == 1 && view.peeks == 0 && !view.footer);
        } else if (group.open) {
          TEST_CHECK(view.cards == group.items.size() && view.peeks == 0 && view.footer);
        } else {
          TEST_CHECK(view.cards == 1 && view.peeks == std::min<std::size_t>(group.items.size() - 1, 2) && view.footer);
        }
      }
      TEST_CHECK(total == history.size());
      for (const std::string& key : openKeys) {
        TEST_CHECK(keys.contains(key));
      }
    }
  }

} // namespace

int main() {
  testRandomSequencesKeepInvariants();
  return 0;
}
