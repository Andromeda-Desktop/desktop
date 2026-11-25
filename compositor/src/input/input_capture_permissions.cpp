#include "input/input_capture_permissions.h"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>

#include "protocols/xdg_dirs.h"
#include "util/private_state_file.h"

namespace andromeda {

  namespace {
    constexpr size_t kMaxGrants = 128;
    constexpr size_t kMaxStoreBytes = 256 * 1024;
    constexpr std::string_view kLockName = ".input-capture-permissions.lock";

    bool hexString(std::string_view value, size_t length) {
      return value.size() == length
          && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    bool validRequest(std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer) {
      if (application.size() > 256 || capabilities == 0 || (capabilities & ~3U) != 0 || persistMode > 2 || peer == 0) {
        return false;
      }
      try {
        nlohmann::json(application).dump();
      } catch (const nlohmann::json::exception&) {
        return false;
      }
      return true;
    }
  } // namespace

  InputCapturePermissions::InputCapturePermissions(std::filesystem::path path) : m_path(std::move(path)) {
    m_loaded = load();
  }

  std::filesystem::path InputCapturePermissions::defaultPath() {
    if (const std::string state = xdg::stateHome(); !state.empty()) {
      return std::filesystem::path(state) / "andromeda/input-capture-permissions.json";
    }
    return {};
  }

  bool InputCapturePermissions::load() {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    const auto file = private_state::read(m_path, kMaxStoreBytes);
    if (std::holds_alternative<private_state::Unsafe>(file)) {
      return false;
    }
    if (std::holds_alternative<private_state::Missing>(file)) {
      std::erase_if(m_records, [](const auto& item) { return item.permission.persistMode == 2; });
      return true;
    }
    const auto data = nlohmann::json::parse(std::get<std::string>(file), nullptr, false);
    if (!data.is_object()
        || data.size() != 2
        || !data.contains("version")
        || !data["version"].is_number_unsigned()
        || data["version"] != 1
        || !data.contains("grants")
        || !data["grants"].is_array()
        || data["grants"].size() > kMaxGrants) {
      return false;
    }
    std::vector<Record> records;
    for (const auto& item : data["grants"]) {
      // A capture grant has 4 keys; an emulation grant adds "kind": "emulate".
      const bool emulate = item.is_object() && item.size() == 5 && item.contains("kind") && item["kind"] == "emulate";
      if (!item.is_object()
          || (item.size() != 4 && !emulate)
          || !item.contains("id")
          || !item["id"].is_string()
          || !item.contains("token")
          || !item["token"].is_string()
          || !item.contains("application")
          || !item["application"].is_string()
          || !item.contains("capabilities")
          || !item["capabilities"].is_number_unsigned()) {
        return false;
      }
      const auto id = item["id"].get<std::string>();
      const auto token = item["token"].get<std::string>();
      const auto application = item["application"].get<std::string>();
      const auto capabilities = item["capabilities"].get<uint64_t>();
      if (!hexString(id, 32)
          || !hexString(token, 64)
          || application.size() > 256
          || capabilities == 0
          || capabilities > 3
          || std::ranges::any_of(records, [&](const auto& record) {
               return record.permission.id == id || record.token == token;
             })) {
        return false;
      }
      records.push_back(
          {{id, application, static_cast<uint32_t>(capabilities), 2,
            emulate ? InputPermissionKind::Emulate : InputPermissionKind::Capture},
           token, 0}
      );
    }
    for (const auto& record : m_records) {
      if (record.permission.persistMode == 1) {
        records.push_back(record);
      }
    }
    m_records = std::move(records);
    return true;
  }

  bool InputCapturePermissions::save(const std::vector<Record>& records) const {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    auto data = nlohmann::json{{"version", 1}, {"grants", nlohmann::json::array()}};
    for (const auto& record : records) {
      if (record.permission.persistMode == 2) {
        data["grants"].push_back({
            {"id", record.permission.id},
            {"application", record.permission.application},
            {"capabilities", record.permission.capabilities},
            {"token", record.token},
        });
        if (record.permission.kind == InputPermissionKind::Emulate) {
          data["grants"].back()["kind"] = "emulate";
        }
      }
    }
    std::string bytes;
    try {
      bytes = data.dump() + '\n';
    } catch (const nlohmann::json::exception&) {
      return false;
    }
    if (bytes.size() > kMaxStoreBytes) {
      return false;
    }
    return private_state::writeAtomic(m_path, bytes);
  }

  std::optional<InputCaptureGrant> InputCapturePermissions::issue(
      std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer, InputPermissionKind kind
  ) {
    if (!validRequest(application, capabilities, persistMode, peer) || persistMode == 0) {
      return std::nullopt;
    }
    // Serialize durable credentials across compositor instances. Refresh while
    // holding the lock so another instance cannot restore a consumed token.
    std::optional<private_state::Lock> lock;
    if (persistMode == 2) {
      lock.emplace(m_path, kLockName);
      if (!*lock || !(m_loaded = load())) {
        return std::nullopt;
      }
    }
    if (m_records.size() >= kMaxGrants) {
      return std::nullopt;
    }
    const auto id = private_state::randomHex(16);
    const auto token = private_state::randomHex(32);
    if (!id || !token) {
      return std::nullopt;
    }
    auto records = m_records;
    records.push_back(
        {{*id, std::string(application), capabilities, persistMode, kind}, *token, persistMode == 1 ? peer : 0}
    );
    if (persistMode == 2 && !save(records)) {
      return std::nullopt;
    }
    m_records = std::move(records);
    return InputCaptureGrant{*id, *token, persistMode};
  }

  std::optional<InputCaptureGrant> InputCapturePermissions::restore(
      std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
      InputPermissionKind kind
  ) {
    if (!validRequest(application, capabilities, persistMode, peer) || !hexString(token, 64)) {
      return std::nullopt;
    }
    std::optional<private_state::Lock> lock;
    const bool ephemeral = std::ranges::any_of(m_records, [token](const auto& record) {
      return record.permission.persistMode == 1 && record.token == token;
    });
    if (!ephemeral) {
      lock.emplace(m_path, kLockName);
      if (!*lock || !(m_loaded = load())) {
        return std::nullopt;
      }
    }
    const auto found = std::ranges::find(m_records, token, &Record::token);
    if (found == m_records.end()
        || found->permission.kind != kind
        || found->permission.application != application
        || (capabilities & ~found->permission.capabilities) != 0
        || persistMode > found->permission.persistMode
        || (found->permission.persistMode == 1 && found->peer != peer)) {
      return std::nullopt;
    }
    const std::string id = found->permission.id;
    const auto nextToken = persistMode != 0 ? private_state::randomHex(32) : std::optional<std::string>{std::string{}};
    if (!nextToken) {
      return std::nullopt;
    }
    auto records = m_records;
    const size_t index = static_cast<size_t>(found - m_records.begin());
    const bool durable = found->permission.persistMode == 2;
    if (persistMode == 0) {
      records.erase(records.begin() + static_cast<ptrdiff_t>(index));
    } else {
      records[index].token = *nextToken;
      records[index].permission.persistMode = persistMode;
      records[index].peer = persistMode == 1 ? peer : 0;
    }
    if (durable && !save(records)) {
      return std::nullopt;
    }
    m_records = std::move(records);
    return InputCaptureGrant{id, *nextToken, persistMode};
  }

  std::vector<InputCapturePermission> InputCapturePermissions::list() {
    private_state::Lock lock(m_path, kLockName);
    if (lock) {
      m_loaded = load();
      if (!m_loaded) {
        std::erase_if(m_records, [](const auto& item) { return item.permission.persistMode == 2; });
      }
    }
    std::vector<InputCapturePermission> result;
    result.reserve(m_records.size());
    for (const auto& record : m_records) {
      result.push_back(record.permission);
    }
    return result;
  }

  bool InputCapturePermissions::revoke(std::string_view id) {
    const bool all = id == "all";
    if (!all && !hexString(id, 32)) {
      return false;
    }
    std::optional<private_state::Lock> lock;
    const bool ephemeral = !all && std::ranges::any_of(m_records, [id](const auto& record) {
      return record.permission.persistMode == 1 && record.permission.id == id;
    });
    if (!ephemeral) {
      lock.emplace(m_path, kLockName);
      if (!*lock) {
        return false;
      }
      m_loaded = load();
      if (!m_loaded && !all) {
        return false;
      }
    }
    if (!all && std::ranges::none_of(m_records, [id](const auto& item) { return item.permission.id == id; })) {
      // A restored-once session still carries this ID after consuming the
      // stored permission. Revoking it again must remain successful.
      return true;
    }
    auto records = m_records;
    std::erase_if(records, [id, all](const auto& item) { return all || item.permission.id == id; });
    const bool durable = all || std::ranges::any_of(m_records, [id](const auto& item) {
                           return item.permission.id == id && item.permission.persistMode == 2;
                         });
    if (durable && !save(records)) {
      return false;
    }
    m_records = std::move(records);
    if (all) {
      m_loaded = true;
    }
    return true;
  }

  void InputCapturePermissions::expirePeer(uint64_t peer) {
    std::erase_if(m_records, [peer](const auto& item) {
      return item.permission.persistMode == 1 && item.peer == peer;
    });
  }

} // namespace andromeda
