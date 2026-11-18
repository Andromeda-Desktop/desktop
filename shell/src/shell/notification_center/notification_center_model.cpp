#include "shell/notification_center/notification_center_model.h"

#include "i18n/i18n.h"
#include "notification/notification_display_name.h"

#include <algorithm>
#include <chrono>
#include <map>

namespace notification_center {

  std::string groupKeyOf(const Notification& n) { return n.group.has_value() ? "g:" + *n.group : "a:" + notificationDisplayAppName(n); }

  std::string groupLabelOf(const Notification& n) {
    if (!n.group.has_value()) {
      return notificationDisplayAppName(n);
    }
    std::string label = *n.group;
    if (const auto slash = label.rfind('/'); slash != std::string::npos) {
      label.replace(slash, 1, " · ");
    }
    return label;
  }

  std::string ageText(const Notification& n) {
    if (!n.receivedWallClock.has_value()) {
      return {};
    }
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - *n.receivedWallClock).count();
    if (minutes < 1) {
      return i18n::tr("control-center.detail.notifications.now");
    }
    if (minutes < 60) {
      return i18n::tr("control-center.detail.notifications.minutes", "count", static_cast<int>(minutes));
    }
    if (minutes < 24 * 60) {
      return i18n::tr("control-center.detail.notifications.hours", "count", static_cast<int>(minutes / 60));
    }
    return i18n::tr("control-center.detail.notifications.days", "count", static_cast<int>(minutes / (24 * 60)));
  }

  std::vector<Group> buildGroups(const std::deque<NotificationHistoryEntry>& history, std::set<std::string>& openKeys) {
    std::vector<Group> groups;
    std::map<std::string, std::size_t> index;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      std::string key = groupKeyOf(it->notification);
      auto [slot, added] = index.try_emplace(key, groups.size());
      if (added) {
        groups.push_back({.key = std::move(key), .label = groupLabelOf(it->notification)});
      }
      groups[slot->second].items.push_back(&it->notification);
    }
    std::erase_if(openKeys, [&](const std::string& key) { return !index.contains(key); });
    for (Group& group : groups) {
      group.open = group.items.size() == 1 || openKeys.contains(group.key);
    }
    return groups;
  }

  GroupView viewOf(const Group& group, std::size_t stackVisible) {
    const std::size_t n = group.items.size();
    if (group.open) {
      return {.cards = n, .peeks = 0, .footer = n > 1};
    }
    return {.cards = 1, .peeks = std::min(n, std::max<std::size_t>(stackVisible, 1)) - 1, .footer = true};
  }

} // namespace notification_center
