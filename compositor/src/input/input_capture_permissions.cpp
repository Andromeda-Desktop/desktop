#include "input/input_capture_permissions.h"

#include <algorithm>
#include <sys/stat.h>
#include <fstream>
#include <array>
#include <cstdlib>
#include <unistd.h>
#include <nlohmann/json.hpp>

#include "protocols/xdg_dirs.h"
#include "util/private_state_file.h"

namespace andromeda {

  namespace {
    constexpr size_t kMaxGrants = 128;
    constexpr size_t kMaxBinaries = 64;
    constexpr size_t kMaxBinaryPath = 4096;
    // Programs whose process runs code from another file: their exe stays the same for every script or program they
    // run, so a grant to one of them goes to all of that code. Matched on the file name without a version suffix.
    constexpr std::string_view kRunners[] = {
        "bash",  "busybox", "bun",  "dash",  "deno",   "dotnet", "electron", "env",    "fish",           "gjs",
        "guile", "java",    "ksh",  "ld.so", "lua",    "luajit", "mksh",     "mono",   "node",           "nodejs",
        "perl",  "php",     "pwsh", "pypy",  "python", "racket", "Rscript",  "ruby",   "sbcl",           "sh",
        "tclsh", "wine",    "wish", "zsh",   "wine-preloader", "wine64-preloader",
    };

    bool runsOtherCode(const std::filesystem::path& binary) {
      const std::string name = binary.filename().native();
      if (name.starts_with("ld-linux") || name.starts_with("ld-musl") || name.starts_with("qemu-")) {
        return true;
      }
      const auto end = name.find_last_not_of("0123456789.-");
      const std::string_view stem = std::string_view(name).substr(0, end == std::string::npos ? 0 : end + 1);
      return std::ranges::contains(kRunners, stem);
    }
    // A grant holds keyboard or pointer; a remote input grant may add clipboard.
    constexpr bool validCapabilities(uint64_t capabilities, InputPermissionKind kind) {
      return (capabilities & 3U) != 0 && capabilities <= (kind == InputPermissionKind::Emulate ? 7U : 3U);
    }
    constexpr size_t kMaxStoreBytes = 256 * 1024;
    constexpr std::string_view kLockName = ".input-capture-permissions.lock";

    bool hexString(std::string_view value, size_t length) {
      return value.size() == length
          && std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    bool validRequest(
        std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
        InputPermissionKind kind
    ) {
      if (application.size() > 256 || !validCapabilities(capabilities, kind) || persistMode > 2 || peer == 0) {
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
      m_binaries.clear();
      return true;
    }
    const auto data = nlohmann::json::parse(std::get<std::string>(file), nullptr, false);
    // Version 1 has no binaries.
    const bool v2 = data.is_object() && data.contains("version") && data["version"] == 2;
    if (!data.is_object()
        || data.size() != (v2 ? 3U : 2U)
        || !data.contains("version")
        || !data["version"].is_number_unsigned()
        || (data["version"] != 1 && !v2)
        || !data.contains("grants")
        || !data["grants"].is_array()
        || data["grants"].size() > kMaxGrants
        || (v2 && (!data.contains("binaries") || !data["binaries"].is_array() || data["binaries"].size() > kMaxBinaries))) {
      return false;
    }
    std::vector<std::string> binaries;
    if (v2) {
      for (const auto& item : data["binaries"]) {
        if (!item.is_string()) {
          return false;
        }
        auto path = item.get<std::string>();
        if (path.empty() || path.size() > kMaxBinaryPath || path.front() != '/' || std::ranges::contains(binaries, path)) {
          return false;
        }
        binaries.push_back(std::move(path));
      }
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
          || !validCapabilities(capabilities, emulate ? InputPermissionKind::Emulate : InputPermissionKind::Capture)
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
    m_binaries = std::move(binaries);
    return true;
  }

  bool InputCapturePermissions::save(const std::vector<Record>& records, const std::vector<std::string>& binaries) const {
    if (m_path.empty() || !m_path.is_absolute()) {
      return false;
    }
    // Version 1 while no binary is listed, so an older compositor sharing the store still reads it.
    auto data = binaries.empty()
                    ? nlohmann::json{{"version", 1}, {"grants", nlohmann::json::array()}}
                    : nlohmann::json{{"version", 2}, {"grants", nlohmann::json::array()}, {"binaries", binaries}};
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
    if (!validRequest(application, capabilities, persistMode, peer, kind) || persistMode == 0) {
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
    const auto id = private_state::randomHex(16);
    const auto token = private_state::randomHex(32);
    if (!id || !token) {
      return std::nullopt;
    }
    auto records = m_records;
    if (records.size() >= kMaxGrants) {
      // Records are in use order (a restore moves its record last), so a full store drops the grant of the same
      // persist mode used least recently: for example one whose token the portal or the app no longer holds.
      const auto oldest = std::ranges::find(records, persistMode, [](const Record& item) {
        return item.permission.persistMode;
      });
      if (oldest == records.end()) {
        return std::nullopt;
      }
      records.erase(oldest);
    }
    records.push_back(
        {{*id, std::string(application), capabilities, persistMode, kind}, *token, persistMode == 1 ? peer : 0}
    );
    if (persistMode == 2 && !save(records, m_binaries)) {
      return std::nullopt;
    }
    m_records = std::move(records);
    return InputCaptureGrant{*id, *token, persistMode};
  }

  std::optional<InputCaptureGrant> InputCapturePermissions::restore(
      std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
      InputPermissionKind kind
  ) {
    if (!validRequest(application, capabilities, persistMode, peer, kind) || !hexString(token, 64)) {
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
      std::rotate(records.begin() + static_cast<ptrdiff_t>(index), records.begin() + static_cast<ptrdiff_t>(index) + 1,
                  records.end());
    }
    if (durable && !save(records, m_binaries)) {
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
    if (durable && !save(records, m_binaries)) {
      return false;
    }
    m_records = std::move(records);
    if (all) {
      m_loaded = true;
    }
    return true;
  }

  std::vector<std::string> InputCapturePermissions::binaries() {
    // Asked per new connection: parse the store again only when the file changed (another compositor instance may
    // have written it). A bad file keeps the last list read.
    struct stat st{};
    const std::array<int64_t, 4> stamp = ::lstat(m_path.c_str(), &st) == 0
                                             ? std::array<int64_t, 4>{static_cast<int64_t>(st.st_ino), st.st_size,
                                                                      st.st_mtim.tv_sec, st.st_mtim.tv_nsec}
                                             : std::array<int64_t, 4>{};
    if (!m_loaded || stamp != m_stamp) {
      m_loaded = load();
      m_stamp = stamp;
    }
    return m_binaries;
  }

  std::expected<std::string, std::string> InputCapturePermissions::addBinary(std::string_view path) {
    if (path.empty() || path.front() != '/') {
      return std::unexpected("not-absolute");
    }
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(std::filesystem::path(path), ec);
    if (ec || !std::filesystem::is_regular_file(canonical, ec) || ::access(canonical.c_str(), X_OK) != 0) {
      return std::unexpected("not-executable");
    }
    // A client's identity is the ELF the kernel ran: for a script that is its interpreter, so a script never matches.
    std::array<char, 4> magic{};
    std::ifstream(canonical, std::ios::binary).read(magic.data(), magic.size());
    if (magic[0] == '#' && magic[1] == '!') {
      return std::unexpected("script");
    }
    if (magic != std::array<char, 4>{'\x7f', 'E', 'L', 'F'}) {
      return std::unexpected("not-executable");
    }
    if (runsOtherCode(canonical)) {
      return std::unexpected("runs-other-code");
    }
    std::string binary = canonical.native();
    if (binary.size() > kMaxBinaryPath) {
      return std::unexpected("too-long");
    }
    private_state::Lock lock(m_path, kLockName);
    if (!lock || !(m_loaded = load())) {
      return std::unexpected("store-failed");
    }
    if (std::ranges::contains(m_binaries, binary)) {
      return binary;
    }
    if (m_binaries.size() >= kMaxBinaries) {
      return std::unexpected("too-many");
    }
    auto binaries = m_binaries;
    binaries.push_back(binary);
    if (!save(m_records, binaries)) {
      return std::unexpected("store-failed");
    }
    m_binaries = std::move(binaries);
    return binary;
  }

  bool InputCapturePermissions::removeBinary(std::string_view path) {
    private_state::Lock lock(m_path, kLockName);
    if (!lock || !(m_loaded = load())) {
      return false;
    }
    // The list holds canonical paths; a symlink to a listed binary names it too, also after the binary is gone.
    std::string listed(path);
    if (!std::ranges::contains(m_binaries, listed)) {
      std::error_code ec;
      listed = std::filesystem::weakly_canonical(std::filesystem::path(path), ec).native();
      if (ec || !std::ranges::contains(m_binaries, listed)) {
        return false;
      }
    }
    auto binaries = m_binaries;
    std::erase(binaries, listed);
    if (!save(m_records, binaries)) {
      return false;
    }
    m_binaries = std::move(binaries);
    return true;
  }

  void InputCapturePermissions::expirePeer(uint64_t peer) {
    std::erase_if(m_records, [peer](const auto& item) {
      return item.permission.persistMode == 1 && item.peer == peer;
    });
  }

} // namespace andromeda
