#pragma once

#include "notification/notification_manager.h"

#include <cstddef>
#include <deque>
#include <set>
#include <string>
#include <vector>

// The notification center's groups: history newest first, one group per app or x-andromeda-group hint, in order of
// each group's newest notification (proto overlays.js renderNC).
namespace notification_center {

  struct Group {
    std::string key;
    std::string label;
    std::vector<const Notification*> items; // newest first
    bool open = false;                      // a group of one is always open and has no footer
  };

  // What a group shows: `cards` laid out cards, `peeks` back cards under a collapsed front card (content hidden).
  struct GroupView {
    std::size_t cards = 0;
    std::size_t peeks = 0;
    bool footer = false;
  };

  [[nodiscard]] std::string groupKeyOf(const Notification& n);
  [[nodiscard]] std::string groupLabelOf(const Notification& n);
  // "now", "N min", "N h", "N d" since the notification arrived; empty without a wall clock time.
  [[nodiscard]] std::string ageText(const Notification& n);

  // Builds the groups and drops open keys whose group no longer exists.
  [[nodiscard]] std::vector<Group> buildGroups(const std::deque<NotificationHistoryEntry>& history, std::set<std::string>& openKeys);
  // stackVisible = cards a collapsed stack shows, front included (tokens component.notification.stack_visible).
  [[nodiscard]] GroupView viewOf(const Group& group, std::size_t stackVisible);

} // namespace notification_center
