#pragma once

#include "system/desktop_entry.h"

#include <string>
#include <string_view>
#include <vector>

namespace shell::dock::pinned_apps {

  [[nodiscard]] bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  [[nodiscard]] bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  // Pins at the end or unpins; returns whether the list changed. Callers pass the live config list, never a copy
  // taken when a menu opened, so a pin from another path is kept.
  [[nodiscard]] bool setEntryPinned(std::vector<std::string>& pinned, const DesktopEntry& entry, bool pin);
  [[nodiscard]] std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);

} // namespace shell::dock::pinned_apps
