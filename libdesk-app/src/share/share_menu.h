#pragma once

#include "ui/controls/context_menu.h"

#include <filesystem>
#include <string>
#include <vector>

class LinkShare;

namespace share {

  // The Share › submenu (mockup §4): paired phones, then Email. Each row's id is its 1-based index in `ids`, which
  // gets "share.device:<id>" or "share.email"; the caller may append more rows the same way.
  [[nodiscard]] ContextMenuControlEntry menuEntry(const LinkShare* link, std::vector<std::string>& ids);

  // Opens the mail app with the regular files attached (xdg-email). Returns an error sentence, empty on success.
  [[nodiscard]] std::string sendByEmail(const std::vector<std::filesystem::path>& paths);

} // namespace share
