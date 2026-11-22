#include "share/share_menu.h"

#include "share/link_share.h"

#include <format>
#include <glib.h>

namespace share {

  ContextMenuControlEntry menuEntry(const LinkShare* link, std::vector<std::string>& ids) {
    ContextMenuControlEntry share{.label = "Share", .hasSubmenu = true, .glyph = "share"};
    const auto child = [&](std::string id, std::string label, std::string glyph, bool enabled, std::string caption) {
      ids.push_back(std::move(id));
      share.children.push_back({.id = static_cast<std::int32_t>(ids.size()), .label = std::move(label),
                                .shortcut = std::move(caption), .enabled = enabled, .glyph = std::move(glyph)});
    };
    std::string names;
    for (const LinkShare::Device& device : link != nullptr ? link->devices() : std::vector<LinkShare::Device>{}) {
      child("share.device:" + device.id, device.name, "device-mobile", device.connected,
            device.connected ? std::string() : std::string("Not connected"));
      names += (names.empty() ? "" : ", ") + device.name;
    }
    if (!share.children.empty()) {
      share.children.push_back({.separator = true});
      // The row names the phones (mockup §4 "Share › Pixel 9, Link…").
      share.shortcut = names + ", Link…";
    }
    child("share.email", "Email…", "mail", true, {});
    return share;
  }

  std::string sendByEmail(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::string> args{"xdg-email"};
    for (const auto& path : paths) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(path, ec)) {
        args.push_back("--attach");
        args.push_back(path.string());
      }
    }
    if (args.size() == 1) {
      return {};
    }
    std::vector<char*> argv;
    for (auto& a : args) {
      argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
      std::string message = std::format("Could not open the mail app: {}", error->message);
      g_error_free(error);
      return message;
    }
    return {};
  }

} // namespace share
