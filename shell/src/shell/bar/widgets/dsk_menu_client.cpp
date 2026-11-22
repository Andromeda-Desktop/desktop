#include "shell/bar/widgets/dsk_menu_client.h"

#include "compositors/andromeda/andromeda_runtime.h"
#include "core/log.h"

#include <format>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

namespace {
  constexpr Logger kLog("dsk-menu");

  // dsk_app_menu_v1 flags.
  constexpr std::uint32_t kSeparator = 1;
  constexpr std::uint32_t kDisabled = 2;
  constexpr std::uint32_t kCheckable = 4;
  constexpr std::uint32_t kRadio = 8;
  constexpr std::uint32_t kChecked = 16;

  struct Row {
    std::uint32_t id = 0;
    std::uint32_t flags = 0;
    std::string label;
    std::string shortcut;
  };

  void addChildren(DbusMenuNode& node, std::uint32_t parent, const std::unordered_map<std::uint32_t, std::vector<Row>>& rows) {
    const auto found = rows.find(parent);
    if (found == rows.end()) {
      return;
    }
    for (const Row& row : found->second) {
      const bool mark = (row.flags & (kCheckable | kRadio)) != 0;
      DbusMenuNode child{
          .id = static_cast<std::int32_t>(row.id),
          .label = row.label,
          .shortcut = row.shortcut,
          .enabled = (row.flags & kDisabled) == 0,
          .separator = (row.flags & kSeparator) != 0,
          .checkmark = (row.flags & kCheckable) != 0,
          .radio = (row.flags & kRadio) != 0,
          .toggleState = mark ? ((row.flags & kChecked) != 0 ? 1 : 0) : -1,
      };
      addChildren(child, row.id, rows);
      node.children.push_back(std::move(child));
    }
  }
} // namespace

DskMenuClient::DskMenuClient(
    const compositors::andromeda::AndromedaRuntime& runtime, std::string windowId, std::uint64_t serial
)
    : m_runtime(runtime), m_windowId(std::move(windowId)), m_serial(serial) {
  const auto reply = m_runtime.requestCommand("app-menu", m_windowId);
  if (!reply.has_value()) {
    kLog.debug("app-menu {} failed", m_windowId);
    return;
  }
  const nlohmann::json& ok = (*reply)["ok"];
  // The window sent a newer tree since the windows event: that event follows and makes a new client.
  if (ok.value("serial", std::uint64_t{0}) != m_serial || !ok.contains("items") || !ok["items"].is_array()) {
    return;
  }
  std::unordered_map<std::uint32_t, std::vector<Row>> rows;
  for (const nlohmann::json& item : ok["items"]) {
    rows[item.value("parent", 0U)].push_back(Row{
        .id = item.value("id", 0U),
        .flags = item.value("flags", 0U),
        .label = item.value("label", std::string{}),
        .shortcut = item.value("shortcut", std::string{}),
    });
  }
  addChildren(m_root, 0, rows);
  m_loaded = true;
}

void DskMenuClient::activate(std::int32_t id) {
  if (!m_runtime.requestCommand("app-menu-activate", std::format("{} {} {}", m_windowId, m_serial, id)).has_value()) {
    kLog.debug("app-menu-activate {} {} {} refused", m_windowId, m_serial, id);
  }
}
