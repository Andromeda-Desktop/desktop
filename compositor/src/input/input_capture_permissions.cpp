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

    using Record = InputCapturePermissions::Record;
    using Stored = InputCapturePermissions::Stored;

    // Missing = empty; nullopt = unsafe or malformed.
    std::optional<Stored> parse(const private_state::ReadResult& file) {
      if (std::holds_alternative<private_state::Unsafe>(file)) {
        return std::nullopt;
      }
      if (std::holds_alternative<private_state::Missing>(file)) {
        return Stored{};
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
          || (v2
              && (!data.contains("binaries")
                  || !data["binaries"].is_array()
                  || data["binaries"].size() > kMaxBinaries))) {
        return std::nullopt;
      }
      std::vector<std::string> binaries;
      if (v2) {
        for (const auto& item : data["binaries"]) {
          if (!item.is_string()) {
            return std::nullopt;
          }
          auto path = item.get<std::string>();
          if (path.empty()
              || path.size() > kMaxBinaryPath
              || path.front() != '/'
              || std::ranges::contains(binaries, path)) {
            return std::nullopt;
          }
          binaries.push_back(std::move(path));
        }
      }
      std::vector<InputCapturePermissions::Record> records;
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
          return std::nullopt;
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
          return std::nullopt;
        }
        records.push_back(
            {{id, application, static_cast<uint32_t>(capabilities), 2,
              emulate ? InputPermissionKind::Emulate : InputPermissionKind::Capture},
             token,
             0}
        );
      }
      return Stored{std::move(records), std::move(binaries)};
    }

    std::optional<std::string> serialize(const Stored& stored) {
      // Version 1 while no binary is listed, so an older compositor sharing the store still reads it.
      auto data = stored.binaries.empty()
          ? nlohmann::json{{"version", 1}, {"grants", nlohmann::json::array()}}
          : nlohmann::json{{"version", 2}, {"grants", nlohmann::json::array()}, {"binaries", stored.binaries}};
      for (const auto& record : stored.grants) {
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
      std::string bytes;
      try {
        bytes = data.dump() + '\n';
      } catch (const nlohmann::json::exception&) {
        return std::nullopt;
      }
      if (bytes.size() > kMaxStoreBytes) {
        return std::nullopt;
      }
      return bytes;
    }

    // The canonical binary `path` names, or why it cannot be trusted. Reads the file: saver thread only.
    std::expected<std::string, std::string> trustedBinary(std::string_view path) {
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
      return binary;
    }

    // One change on the saver thread: the file after it (nullopt = unreadable) and the change's own answer.
    template <class Answer> struct Applied {
      std::optional<Stored> stored;
      Answer answer{};
    };

    template <class Answer> private_state::Merge<Applied<Answer>> keep(std::optional<Stored> stored, Answer answer) {
      return {std::nullopt, {std::move(stored), std::move(answer)}};
    }

    template <class Answer> private_state::Merge<Applied<Answer>> write(Stored stored, Answer answer, Answer failed) {
      auto bytes = serialize(stored);
      if (!bytes) {
        return {std::nullopt, {std::nullopt, std::move(failed)}};
      }
      return {std::move(bytes), {std::move(stored), std::move(answer)}};
    }

    // A restore that may use the token: same kind and app, a subset of its capabilities, no longer kept than it was.
    bool restorable(
        const Record& record, InputPermissionKind kind, std::string_view application, uint32_t capabilities,
        uint32_t persistMode
    ) {
      return record.permission.kind == kind
          && record.permission.application == application
          && (capabilities & ~record.permission.capabilities) == 0
          && persistMode <= record.permission.persistMode;
    }
  } // namespace

  InputCapturePermissions::InputCapturePermissions(wl_event_loop* loop, std::filesystem::path path)
      : m_saver(loop, std::move(path), std::string(kLockName), kMaxStoreBytes, "andro-inputperm") {
    m_loaded = load();
  }

  std::filesystem::path InputCapturePermissions::defaultPath() {
    if (const std::string state = xdg::stateHome(); !state.empty()) {
      return std::filesystem::path(state) / "andromeda/input-capture-permissions.json";
    }
    return {};
  }

  bool InputCapturePermissions::load() {
    const auto& file = m_saver.file();
    if (file.empty() || !file.is_absolute()) {
      return false;
    }
    auto stored = parse(private_state::read(file, kMaxStoreBytes));
    if (!stored) {
      return false;
    }
    m_durable = std::move(stored->grants);
    m_binaries = std::move(stored->binaries);
    return true;
  }

  void InputCapturePermissions::commit(Stored stored) {
    m_durable = std::move(stored.grants);
    m_binaries = std::move(stored.binaries);
    m_loaded = true;
  }

  void InputCapturePermissions::issue(
      std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
      InputPermissionKind kind, GrantDone done
  ) {
    const auto id = private_state::randomHex(16);
    const auto token = private_state::randomHex(32);
    if (!validRequest(application, capabilities, persistMode, peer, kind) || persistMode == 0 || !id || !token) {
      m_saver.then([done = std::move(done)] { done(std::nullopt); });
      return;
    }
    Record record{
        {*id, std::string(application), capabilities, persistMode, kind}, *token, persistMode == 1 ? peer : 0
    };
    const InputCaptureGrant grant{*id, *token, persistMode};
    if (persistMode == 1) {
      m_saver.then([this, record = std::move(record), grant, done = std::move(done)] {
        if (m_durable.size() + m_ephemeral.size() >= kMaxGrants) {
          // Records are in use order, so a full store drops the least recently used grant of the same persist mode:
          // for example one whose token the portal or the app no longer holds.
          if (m_ephemeral.empty()) {
            done(std::nullopt);
            return;
          }
          m_ephemeral.erase(m_ephemeral.begin());
        }
        m_ephemeral.push_back(record);
        if (!done(grant)) {
          std::erase_if(m_ephemeral, [&](const Record& kept) { return kept.permission.id == grant.id; });
        }
      });
      return;
    }
    // Saved under the lock from a fresh read, so another instance cannot restore a consumed token.
    m_saver.save<Applied<bool>>(
        [record = std::move(record), ephemeral = m_ephemeral.size()](const private_state::ReadResult& file) {
          auto stored = parse(file);
          if (!stored) {
            return keep<bool>(std::nullopt, false);
          }
          if (stored->grants.size() + ephemeral >= kMaxGrants) {
            if (stored->grants.empty()) {
              return keep<bool>(std::move(stored), false);
            }
            stored->grants.erase(stored->grants.begin());
          }
          stored->grants.push_back(record);
          return write<bool>(std::move(*stored), true, false);
        },
        [this, grant, done = std::move(done)](std::optional<Applied<bool>> result) {
          if (result && result->stored) {
            commit(std::move(*result->stored));
            trimEphemeral();
          } else if (result) {
            m_loaded = false;
          }
          if (!(result && result->answer)) {
            done(std::nullopt);
          } else if (!done(grant)) {
            revoke(grant.id, [](bool) {});
          }
        }
    );
  }

  void InputCapturePermissions::restore(
      std::string_view token, std::string_view application, uint32_t capabilities, uint32_t persistMode, uint64_t peer,
      InputPermissionKind kind, GrantDone done
  ) {
    const auto nextToken = persistMode != 0 ? private_state::randomHex(32) : std::optional<std::string>{std::string{}};
    if (!validRequest(application, capabilities, persistMode, peer, kind) || !hexString(token, 64) || !nextToken) {
      m_saver.then([done = std::move(done)] { done(std::nullopt); });
      return;
    }
    const bool ephemeral = std::ranges::contains(m_ephemeral, token, &Record::token);
    if (ephemeral) {
      m_saver.then([this, token = std::string(token), application = std::string(application), capabilities, persistMode,
                    peer, kind, nextToken = *nextToken, done = std::move(done)] {
        const auto found = std::ranges::find(m_ephemeral, token, &Record::token);
        if (found == m_ephemeral.end()
            || !restorable(*found, kind, application, capabilities, persistMode)
            || found->peer != peer) {
          done(std::nullopt);
          return;
        }
        const InputCaptureGrant grant{found->permission.id, nextToken, persistMode};
        const Record original = *found;
        if (persistMode == 0) {
          m_ephemeral.erase(found);
        } else {
          found->token = nextToken;
          std::rotate(found, found + 1, m_ephemeral.end());
        }
        if (!done(grant)) {
          std::erase_if(m_ephemeral, [&](const Record& kept) { return kept.permission.id == original.permission.id; });
          m_ephemeral.push_back(original);
        }
      });
      return;
    }
    struct Restored {
      std::optional<InputCaptureGrant> grant;
      // Restored for one app instance: the record leaves the file and lives in memory.
      std::optional<Record> ephemeral;
      // The file's record before the restore, to put back when the grant reaches no client.
      std::optional<Record> original;
    };
    m_saver.save<Applied<Restored>>(
        [token = std::string(token), application = std::string(application), capabilities, persistMode, peer, kind,
         nextToken = *nextToken](const private_state::ReadResult& file) {
          auto stored = parse(file);
          if (!stored) {
            return keep<Restored>(std::nullopt, {});
          }
          auto& grants = stored->grants;
          const auto found = std::ranges::find(grants, token, &Record::token);
          if (found == grants.end() || !restorable(*found, kind, application, capabilities, persistMode)) {
            return keep<Restored>(std::move(stored), {});
          }
          Restored restored{InputCaptureGrant{found->permission.id, nextToken, persistMode}, std::nullopt, *found};
          if (persistMode == 1) {
            restored.ephemeral = Record{found->permission, nextToken, peer};
            restored.ephemeral->permission.persistMode = 1;
          }
          if (persistMode == 2) {
            found->token = nextToken;
            std::rotate(found, found + 1, grants.end());
          } else {
            grants.erase(found);
          }
          return write<Restored>(std::move(*stored), std::move(restored), {});
        },
        [this, done = std::move(done)](std::optional<Applied<Restored>> result) {
          if (!result || !result->stored) {
            if (result) {
              m_loaded = false;
            }
            done(std::nullopt);
            return;
          }
          commit(std::move(*result->stored));
          auto& answer = result->answer;
          if (answer.ephemeral) {
            m_ephemeral.push_back(*answer.ephemeral);
          }
          if (!answer.grant) {
            done(std::nullopt);
          } else if (!done(answer.grant)) {
            if (answer.ephemeral) {
              std::erase_if(m_ephemeral, [&](const Record& kept) { return kept.token == answer.ephemeral->token; });
            }
            putBack(std::move(*answer.original), answer.grant->token);
          }
        }
    );
  }

  void InputCapturePermissions::putBack(Record original, std::string used) {
    m_saver.save<Applied<bool>>(
        [original = std::move(original), used = std::move(used)](const private_state::ReadResult& file) {
          auto stored = parse(file);
          if (!stored) {
            return keep<bool>(std::nullopt, false);
          }
          auto& grants = stored->grants;
          const auto found = std::ranges::find(grants, original.permission.id, [](const Record& record) {
            return record.permission.id;
          });
          // A grant gone from the file stays gone: a revoke queued after the restore may have removed it.
          if (found == grants.end() || found->token != used) {
            return keep<bool>(std::move(stored), false);
          }
          found->token = original.token;
          return write<bool>(std::move(*stored), true, false);
        },
        [this](std::optional<Applied<bool>> result) {
          if (result && result->stored) {
            commit(std::move(*result->stored));
            trimEphemeral();
          }
        }
    );
  }

  void InputCapturePermissions::trimEphemeral() {
    while (m_durable.size() + m_ephemeral.size() > kMaxGrants && !m_ephemeral.empty()) {
      m_ephemeral.erase(m_ephemeral.begin());
    }
  }

  std::vector<InputCapturePermission> InputCapturePermissions::list() {
    private_state::Lock lock(m_saver.file(), kLockName);
    if (lock) {
      m_loaded = load();
      if (!m_loaded) {
        m_durable.clear();
      }
    }
    std::vector<InputCapturePermission> result;
    result.reserve(m_durable.size() + m_ephemeral.size());
    for (const auto* records : {&m_durable, &m_ephemeral}) {
      for (const auto& record : *records) {
        result.push_back(record.permission);
      }
    }
    return result;
  }

  void InputCapturePermissions::revoke(std::string_view id, Done done) {
    const bool all = id == "all";
    if (!all && !hexString(id, 32)) {
      m_saver.then([done = std::move(done)] { done(false); });
      return;
    }
    // Ids are unique across both lists; a revoke also drops the id from memory when an earlier queued restore moved
    // the grant there.
    const auto forget = [this, all, id = std::string(id)] {
      std::erase_if(m_ephemeral, [&](const Record& record) { return all || record.permission.id == id; });
    };
    if (!all && std::ranges::contains(m_ephemeral, id, [](const Record& record) { return record.permission.id; })) {
      m_saver.then([forget, done = std::move(done)] {
        forget();
        done(true);
      });
      return;
    }
    m_saver.save<Applied<bool>>(
        [all, id = std::string(id), binaries = m_binaries](const private_state::ReadResult& file) {
          auto stored = parse(file);
          if (all) {
            // Revoking everything replaces an unreadable file; the binaries this instance holds stay.
            return write<bool>(Stored{{}, stored ? std::move(stored->binaries) : binaries}, true, false);
          }
          if (!stored) {
            return keep<bool>(std::nullopt, false);
          }
          // A restored-once session still carries this ID after consuming the stored permission: revoking it again
          // still succeeds.
          if (std::erase_if(stored->grants, [&](const Record& record) { return record.permission.id == id; }) == 0) {
            return keep<bool>(std::move(stored), true);
          }
          return write<bool>(std::move(*stored), true, false);
        },
        [this, forget, done = std::move(done)](std::optional<Applied<bool>> result) {
          if (!result || !result->answer) {
            if (result && !result->stored) {
              m_loaded = false;
            }
            done(false);
            return;
          }
          commit(std::move(*result->stored));
          forget();
          done(true);
        }
    );
  }

  std::vector<std::string> InputCapturePermissions::binaries() {
    // Asked per new connection: parse the store again only when the file changed (another compositor instance may
    // have written it). A bad file keeps the last list read.
    struct stat st{};
    const std::array<int64_t, 4> stamp = ::lstat(m_saver.file().c_str(), &st) == 0
        ? std::array<int64_t, 4>{static_cast<int64_t>(st.st_ino), st.st_size, st.st_mtim.tv_sec, st.st_mtim.tv_nsec}
        : std::array<int64_t, 4>{};
    if (!m_loaded || stamp != m_stamp) {
      m_loaded = load();
      m_stamp = stamp;
    }
    return m_binaries;
  }

  void InputCapturePermissions::addBinary(std::string_view path, BinaryDone done) {
    using Added = std::expected<std::string, std::string>;
    m_saver.save<Applied<Added>>(
        [path = std::string(path)](const private_state::ReadResult& file) {
          auto binary = trustedBinary(path);
          if (!binary) {
            return keep<Added>(std::nullopt, std::move(binary));
          }
          auto stored = parse(file);
          if (!stored) {
            return keep<Added>(std::nullopt, std::unexpected("store-failed"));
          }
          if (std::ranges::contains(stored->binaries, *binary)) {
            return keep<Added>(std::move(stored), std::move(binary));
          }
          if (stored->binaries.size() >= kMaxBinaries) {
            return keep<Added>(std::move(stored), std::unexpected("too-many"));
          }
          stored->binaries.push_back(*binary);
          return write<Added>(std::move(*stored), std::move(binary), std::unexpected("store-failed"));
        },
        [this, done = std::move(done)](std::optional<Applied<Added>> result) {
          if (!result) {
            done(std::unexpected("store-failed"));
            return;
          }
          if (result->stored) {
            commit(std::move(*result->stored));
          }
          done(std::move(result->answer));
        }
    );
  }

  void InputCapturePermissions::removeBinary(std::string_view path, Done done) {
    m_saver.save<Applied<bool>>(
        [path = std::string(path)](const private_state::ReadResult& file) {
          auto stored = parse(file);
          if (!stored) {
            return keep<bool>(std::nullopt, false);
          }
          // The list holds canonical paths; a symlink to a listed binary names it too, also after the binary is gone.
          std::string listed = path;
          if (!std::ranges::contains(stored->binaries, listed)) {
            std::error_code ec;
            listed = std::filesystem::weakly_canonical(std::filesystem::path(path), ec).native();
            if (ec || !std::ranges::contains(stored->binaries, listed)) {
              return keep<bool>(std::move(stored), false);
            }
          }
          std::erase(stored->binaries, listed);
          return write<bool>(std::move(*stored), true, false);
        },
        [this, done = std::move(done)](std::optional<Applied<bool>> result) {
          if (result && result->stored) {
            commit(std::move(*result->stored));
          }
          done(result && result->answer);
        }
    );
  }

  void InputCapturePermissions::expirePeer(uint64_t peer) {
    // Queued: a restore for this peer that is still saving adds its record first.
    m_saver.then([this, peer] {
      std::erase_if(m_ephemeral, [peer](const Record& record) { return record.peer == peer; });
    });
  }

} // namespace andromeda
